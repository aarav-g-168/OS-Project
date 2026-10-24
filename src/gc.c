/*
 * gc.c - retention policy + mark-and-sweep garbage collection.
 *
 *   chronofs gc <store> --keep 1h [--dry-run]
 *
 * 1. Retention: keep every journal record newer than (now - keep), plus, for
 *    each path, the newest older record if it is still live (the "base"
 *    state), so every snapshot inside the window stays exact.
 * 2. Mark: every manifest referenced by a kept record, and every block those
 *    manifests list, is marked reachable.
 * 3. Commit the retained journal before sweeping unmarked manifests and blocks.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "chronofs.h"

/* Open-addressing hash set of 32-byte hashes. */
struct hset {
    uint8_t (*keys)[HASH_LEN];
    uint8_t  *used;
    size_t    cap, n;
};

static int hset_init(struct hset *s, size_t cap)
{
    s->cap = 64;
    while (s->cap < cap * 2)
        s->cap <<= 1;
    s->n = 0;
    s->keys = malloc(s->cap * HASH_LEN);
    s->used = calloc(s->cap, 1);
    if (!s->keys || !s->used) {
        free(s->keys);
        free(s->used);
        s->keys = NULL;
        s->used = NULL;
        return -ENOMEM;
    }
    return 0;
}

static void hset_free(struct hset *s)
{
    free(s->keys);
    free(s->used);
}

static size_t hset_slot(const struct hset *s, const uint8_t k[HASH_LEN])
{
    uint64_t h;
    size_t i;
    memcpy(&h, k, sizeof(h));
    i = (size_t)(h & (s->cap - 1));
    while (s->used[i] && memcmp(s->keys[i], k, HASH_LEN) != 0)
        i = (i + 1) & (s->cap - 1);
    return i;
}

static int hset_has(const struct hset *s, const uint8_t k[HASH_LEN])
{
    return s->used[hset_slot(s, k)];
}

static int hset_add(struct hset *s, const uint8_t k[HASH_LEN])
{
    size_t i;
    if ((s->n + 1) * 2 > s->cap) {
        struct hset bigger;
        size_t j;
        if (hset_init(&bigger, s->cap) < 0)
            return -ENOMEM;
        for (j = 0; j < s->cap; j++)
            if (s->used[j])
                hset_add(&bigger, s->keys[j]);
        hset_free(s);
        *s = bigger;
    }
    i = hset_slot(s, k);
    if (s->used[i])
        return 0;
    memcpy(s->keys[i], k, HASH_LEN);
    s->used[i] = 1;
    s->n++;
    return 1;
}

static int hex_to_hash(const char *hex, uint8_t out[HASH_LEN])
{
    int i;
    for (i = 0; i < HASH_LEN; i++) {
        unsigned v;
        if (sscanf(hex + i * 2, "%2x", &v) != 1)
            return -1;
        out[i] = (uint8_t)v;
    }
    return 0;
}

/* Deletes every object under <store>/<sub>/ that is not in keep. */
static void sweep(const struct store *st, const char *sub, const struct hset *keep, int dry,
                  size_t *removed, uint64_t *bytes)
{
    char base[PATH_MAX], p[PATH_MAX], q[PATH_MAX + 300], hex[HASH_HEX_LEN + 1];
    uint8_t h[HASH_LEN];
    DIR *d, *e;
    struct dirent *de, *fe;
    struct stat sb;

    store_path(st, base, sizeof(base), sub);
    if (!(d = opendir(base)))
        return;
    while ((de = readdir(d))) {
        if (strlen(de->d_name) != 2)
            continue;
        snprintf(p, sizeof(p), "%s/%s", base, de->d_name);
        if (!(e = opendir(p)))
            continue;
        while ((fe = readdir(e))) {
            int orphan_tmp = strncmp(fe->d_name, ".tmp", 4) == 0;
            if (fe->d_name[0] == '.' && !orphan_tmp)
                continue;
            snprintf(q, sizeof(q), "%s/%s", p, fe->d_name);
            if (!orphan_tmp) {
                if (strlen(fe->d_name) != HASH_HEX_LEN - 2)
                    continue;
                snprintf(hex, sizeof(hex), "%s%s", de->d_name, fe->d_name);
                if (hex_to_hash(hex, h) < 0 || hset_has(keep, h))
                    continue;
            }
            if (stat(q, &sb) == 0)
                *bytes += (uint64_t)sb.st_size;
            if (!dry)
                unlink(q);
            (*removed)++;
        }
        closedir(e);
        if (!dry)
            rmdir(p);                   /* only succeeds if now empty */
    }
    closedir(d);
}

int cmd_gc(int argc, char **argv)
{
    struct store st;
    struct jrec *recs = NULL;
    struct hset mset = {0}, bset = {0};
    char *keep_s = NULL, p[PATH_MAX], tmp[PATH_MAX], tbuf[64], b1[32], b2[32];
    char line[512], name[256];
    size_t n, i, kept = 0, rm_blocks = 0, rm_mans = 0;
    uint64_t blk_bytes = 0, man_bytes = 0;
    int64_t keep_ns, cutoff;
    int dry = 0, lockfd, fd, r = 1, j;
    unsigned char *keepmask;
    FILE *in, *outf;

    for (j = 0; j < argc; j++) {
        if (!strcmp(argv[j], "--keep") && j + 1 < argc)
            keep_s = argv[++j];
        else if (!strcmp(argv[j], "--dry-run") || !strcmp(argv[j], "-n"))
            dry = 1;
    }
    if (argc < 1 || !keep_s || argv[0][0] == '-') {
        fprintf(stderr, "usage: chronofs gc <store> --keep DURATION [--dry-run]\n"
                        "  e.g. --keep 1h  keeps full history for the last hour\n");
        return 2;
    }
    if (parse_duration(keep_s, &keep_ns) < 0) {
        fprintf(stderr, "chronofs: bad duration '%s' (e.g. 30s, 10min, 2h, 7d)\n", keep_s);
        return 2;
    }
    if ((j = store_open(&st, argv[0], 0)) < 0) {
        fprintf(stderr, "chronofs: cannot open store: %s\n", strerror(-j));
        return 1;
    }

    /* Refuse to run while mounted: the daemon holds this lock. */
    store_path(&st, p, sizeof(p), "lock");
    lockfd = open(p, O_RDWR | O_CREAT, 0644);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "chronofs: store is mounted - unmount it before running gc\n");
        if (lockfd >= 0)
            close(lockfd);
        store_close(&st);
        return 1;
    }

    cutoff = now_ns() - keep_ns;
    fmt_time(cutoff, tbuf, sizeof(tbuf));
    if (journal_read_all(st.root, &recs, &n) < 0 || !(keepmask = calloc(n + 1, 1)))
        goto out;

    /* 1. Retention. Timestamps are unique, so ts identifies a record. */
    for (i = 0; i < n; i++) {
        if (recs[i].ts_ns >= cutoff) {
            keepmask[i] = 1;
        } else {
            const struct version *base = index_at(&st.idx, recs[i].path, cutoff - 1);
            keepmask[i] = base && base->ts_ns == recs[i].ts_ns && version_live(base);
        }
        kept += keepmask[i];
    }

    /* 2. Mark. */
    if (hset_init(&mset, kept + 16) < 0 || hset_init(&bset, 1024) < 0)
        goto out_sets;
    for (i = 0; i < n; i++) {
        uint8_t (*blocks)[HASH_LEN];
        uint64_t size;
        size_t nb, k;
        int added;
        if (!keepmask[i] || recs[i].op != OP_WRITE)
            continue;
        added = hset_add(&mset, recs[i].manifest);
        if (added < 0)
            goto out_sets;
        if (!added)
            continue;
        if (bs_get_manifest(&st, recs[i].manifest, &size, &blocks, &nb) < 0) {
            fprintf(stderr, "chronofs: cannot read retained manifest for %s; gc aborted\n", recs[i].path);
            goto out_sets;
        }
        for (k = 0; k < nb; k++) {
            if (hset_add(&bset, blocks[k]) < 0) {
                free(blocks);
                goto out_sets;
            }
        }
        free(blocks);
    }

    if (!dry) {
        /* Rewrite the journal with only the kept records. */
        store_path(&st, p, sizeof(p), "journal.log");
        snprintf(tmp, sizeof(tmp), "%s.tmp", p);
        fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0)
            goto out_sets;
        for (i = 0; i < n; i++) {
            if (keepmask[i] && write(fd, &recs[i], sizeof(recs[i])) != (ssize_t)sizeof(recs[i])) {
                close(fd);
                unlink(tmp);
                goto out_sets;
            }
        }
        if (fsync(fd) < 0) {
            close(fd);
            unlink(tmp);
            goto out_sets;
        }
        if (close(fd) < 0) {
            unlink(tmp);
            goto out_sets;
        }
        if (rename(tmp, p) < 0)
            goto out_sets;
        /* Persist the new journal name before freeing its old objects. */
        fd = open(st.root, O_RDONLY | O_DIRECTORY);
        if (fd < 0)
            goto out_sets;
        if (fsync(fd) < 0) {
            close(fd);
            goto out_sets;
        }
        close(fd);

        /* Tags older than the window can no longer be shown exactly. */
        store_path(&st, p, sizeof(p), "tags");
        snprintf(tmp, sizeof(tmp), "%s.tmp", p);
        if ((in = fopen(p, "r"))) {
            if ((outf = fopen(tmp, "w"))) {
                long long ts;
                while (fgets(line, sizeof(line), in)) {
                    if (sscanf(line, "%255s %lld", name, &ts) == 2 && ts < cutoff)
                        printf("dropping tag %s (older than retention window)\n", name);
                    else
                        fputs(line, outf);
                }
                fclose(outf);
                rename(tmp, p);
            }
            fclose(in);
        }
    }

    /* Commit retention before deleting anything: interruption leaves only orphans. */
    sweep(&st, "objects", &bset, dry, &rm_blocks, &blk_bytes);
    sweep(&st, "manifests", &mset, dry, &rm_mans, &man_bytes);

    fmt_size(blk_bytes, b1, sizeof(b1));
    fmt_size(man_bytes, b2, sizeof(b2));
    printf("%sretention window: history since %s\n", dry ? "[dry run] " : "", tbuf);
    printf("journal records : %zu kept, %zu dropped\n", kept, n - kept);
    printf("blocks          : %zu reachable, %zu %s (%s)\n", bset.n, rm_blocks,
           dry ? "would be freed" : "freed", b1);
    printf("manifests       : %zu reachable, %zu %s (%s)\n", mset.n, rm_mans,
           dry ? "would be freed" : "freed", b2);
    r = 0;
out_sets:
    hset_free(&mset);
    hset_free(&bset);
    free(keepmask);
out:
    free(recs);
    close(lockfd);
    store_close(&st);
    return r;
}
