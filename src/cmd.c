/*
 * cmd.c - offline subcommands that read a store directly (no mount needed):
 *   log, tag, ls, cat, restore, stats
 */
#include <ctype.h>
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

/* Pulls "--at X" (or "-t X") out of argv; returns the remaining positional args. */
static int take_option(int *argc, char **argv, const char *lng, const char *shrt, char **val)
{
    int i, j;
    for (i = 0; i < *argc; i++) {
        if ((strcmp(argv[i], lng) == 0 || (shrt && strcmp(argv[i], shrt) == 0)) && i + 1 < *argc) {
            *val = argv[i + 1];
            for (j = i; j + 2 <= *argc; j++)
                argv[j] = argv[j + 2];
            *argc -= 2;
            return 1;
        }
    }
    return 0;
}

static int open_store(struct store *st, const char *dir)
{
    int r = store_open(st, dir, 0);
    if (r < 0 && r != -ENOENT)
        fprintf(stderr, "chronofs: cannot open store %s: %s\n", dir, strerror(-r));
    return r;
}

static int get_time(const struct store *st, const char *spec, int64_t *t)
{
    if (!spec) {
        *t = now_ns();
        return 0;
    }
    if (resolve_when(st, spec, t) < 0) {
        fprintf(stderr, "chronofs: cannot understand time '%s'\n"
                "  try: now, 30s-ago, 2min-ago, 1h-ago, @19:53:12, @2026-10-05T19:53, or a tag\n",
                spec);
        return -1;
    }
    return 0;
}

static const char *op_name(int op)
{
    switch (op) {
    case OP_WRITE:  return "write";
    case OP_UNLINK: return "delete";
    case OP_MKDIR:  return "mkdir";
    case OP_RMDIR:  return "rmdir";
    }
    return "?";
}

/* Is the store currently mounted?  If so, copy the mountpoint into mp. */
static int store_mounted(const struct store *st, char *mp, size_t n)
{
    char p[PATH_MAX];
    int fd, mounted = 0;
    FILE *f;

    store_path(st, p, sizeof(p), "lock");
    fd = open(p, O_RDONLY);
    if (fd < 0)
        return 0;
    if (flock(fd, LOCK_EX | LOCK_NB) < 0 && errno == EWOULDBLOCK)
        mounted = 1;
    close(fd);
    if (mounted && mp) {
        mp[0] = '\0';
        store_path(st, p, sizeof(p), "mounted");
        if ((f = fopen(p, "r"))) {
            if (fgets(mp, (int)n, f))
                mp[strcspn(mp, "\n")] = '\0';
            fclose(f);
        }
    }
    return mounted;
}

/* ------------------------------------------------------------------ */

int cmd_log(int argc, char **argv)
{
    struct store st;
    struct jrec *recs;
    char filter[JPATH_MAX] = "", tbuf[64], sbuf[32], hex[HASH_HEX_LEN + 1];
    char *limit_s = NULL;
    size_t n, i, start = 0, shown = 0, fl = 0;
    long limit = 0;

    take_option(&argc, argv, "--limit", "-n", &limit_s);
    if (argc < 1) {
        fprintf(stderr, "usage: chronofs log <store> [path] [-n N]\n");
        return 2;
    }
    if (limit_s)
        limit = strtol(limit_s, NULL, 10);
    if (open_store(&st, argv[0]) < 0)
        return 1;
    if (argc > 1) {
        if (normalize_path(argv[1], filter, sizeof(filter)) < 0) {
            fprintf(stderr, "chronofs: invalid path (too long or contains ..)\n");
            store_close(&st);
            return 2;
        }
        fl = strlen(filter);
    }
    if (journal_read_all(st.root, &recs, &n) < 0) {
        store_close(&st);
        return 1;
    }

    /* With -n, show only the last N matching records. */
    if (limit > 0) {
        size_t matches = 0;
        for (i = n; i-- > 0;) {
            if (fl > 1 && !(strncmp(recs[i].path, filter, fl) == 0 &&
                            (recs[i].path[fl] == '\0' || recs[i].path[fl] == '/')))
                continue;
            if (++matches == (size_t)limit) {
                start = i;
                break;
            }
        }
    }

    printf("%-23s  %-6s  %10s  %-8s  %s\n", "TIME", "OP", "SIZE", "MANIFEST", "PATH");
    for (i = start; i < n; i++) {
        struct jrec *r = &recs[i];
        if (fl > 1 && !(strncmp(r->path, filter, fl) == 0 &&
                        (r->path[fl] == '\0' || r->path[fl] == '/')))
            continue;
        fmt_time(r->ts_ns, tbuf, sizeof(tbuf));
        if (r->op == OP_WRITE) {
            fmt_size(r->size, sbuf, sizeof(sbuf));
            hash_to_hex(r->manifest, hex);
            hex[8] = '\0';
        } else {
            strcpy(sbuf, "-");
            strcpy(hex, "-");
        }
        printf("%-23s  %-6s  %10s  %-8s  %s\n", tbuf, op_name(r->op), sbuf, hex, r->path);
        shown++;
    }
    if (shown == 0)
        printf("(no history%s%s)\n", fl > 1 ? " for " : "", fl > 1 ? filter : "");
    free(recs);
    store_close(&st);
    return 0;
}

int cmd_tag(int argc, char **argv)
{
    struct store st;
    char *at = NULL, p[PATH_MAX], tbuf[64], line[512], name[256];
    const char *s;
    long long ts;
    int64_t t, dummy;
    FILE *f;

    take_option(&argc, argv, "--at", "-t", &at);
    if (argc < 1) {
        fprintf(stderr, "usage: chronofs tag <store> [<name> [--at TIME]]\n");
        return 2;
    }
    if (open_store(&st, argv[0]) < 0)
        return 1;
    store_path(&st, p, sizeof(p), "tags");

    if (argc == 1) {                    /* list tags */
        if ((f = fopen(p, "r"))) {
            while (fgets(line, sizeof(line), f)) {
                if (sscanf(line, "%255s %lld", name, &ts) == 2 &&
                    tag_lookup(&st, name, &dummy) == 0 && dummy == ts) {
                    fmt_time(ts, tbuf, sizeof(tbuf));
                    printf("%-20s %s\n", name, tbuf);
                }
            }
            fclose(f);
        }
        store_close(&st);
        return 0;
    }

    for (s = argv[1]; *s; s++) {
        if (!isalnum((unsigned char)*s) && *s != '-' && *s != '_' && *s != '.')
            break;
    }
    if (!argv[1][0] || *s || argv[1][0] == '.' || parse_when(argv[1], now_ns(), &dummy) == 0 ||
        strlen(argv[1]) > 64) {
        fprintf(stderr, "chronofs: bad tag name '%s' (use letters, digits, - _ . ; "
                "must not look like a time)\n", argv[1]);
        store_close(&st);
        return 1;
    }
    if (get_time(&st, at, &t) < 0) {
        store_close(&st);
        return 1;
    }
    if (!(f = fopen(p, "a"))) {
        perror("chronofs: tags");
        store_close(&st);
        return 1;
    }
    fprintf(f, "%s %lld\n", argv[1], (long long)t);
    fclose(f);
    fmt_time(t, tbuf, sizeof(tbuf));
    printf("tagged %s = %s  (browse: <mnt>/%s/%s/)\n", argv[1], tbuf, SNAP_DIR, argv[1]);
    store_close(&st);
    return 0;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

int cmd_ls(int argc, char **argv)
{
    struct store st;
    char *at = NULL, dir[JPATH_MAX] = "/", tbuf[64], sbuf[32];
    char **names = NULL;
    size_t i, n = 0, dl;
    int64_t t;
    const char *prefix;

    take_option(&argc, argv, "--at", "-t", &at);
    if (argc < 1) {
        fprintf(stderr, "usage: chronofs ls <store> [dir] [--at TIME]\n");
        return 2;
    }
    if (open_store(&st, argv[0]) < 0)
        return 1;
    if (argc > 1 && normalize_path(argv[1], dir, sizeof(dir)) < 0) {
        fprintf(stderr, "chronofs: invalid path (too long or contains ..)\n");
        store_close(&st);
        return 2;
    }
    if (get_time(&st, at, &t) < 0) {
        store_close(&st);
        return 1;
    }
    fmt_time(t, tbuf, sizeof(tbuf));
    printf("%s as of %s\n", dir, tbuf);

    prefix = strcmp(dir, "/") == 0 ? "" : dir;
    dl = strlen(prefix);
    names = calloc(st.idx.nall + 1, sizeof(*names));
    for (i = 0; names && i < st.idx.nall; i++) {
        struct pentry *p = st.idx.all[i];
        const char *rest;
        size_t len;
        if (strncmp(p->path, prefix, dl) != 0 || p->path[dl] != '/' ||
            !version_live(pentry_at(p, t)))
            continue;
        rest = p->path + dl + 1;
        len = strcspn(rest, "/");
        names[n++] = strndup(rest, len);
    }
    if (n > 1)
        qsort(names, n, sizeof(*names), cmp_str);
    for (i = 0; i < n; i++) {
        char full[JPATH_MAX];
        const struct version *v;
        if (i > 0 && strcmp(names[i], names[i - 1]) == 0)
            continue;
        snprintf(full, sizeof(full), "%s/%s", prefix, names[i]);
        v = index_at(&st.idx, full, t);
        if (v && v->op == OP_WRITE) {
            fmt_size(v->size, sbuf, sizeof(sbuf));
            fmt_time(v->mtime_ns, tbuf, sizeof(tbuf));
            printf("  %o  %10s  %s  %s\n", v->mode & 07777, sbuf, tbuf, names[i]);
        } else {
            printf("  dir  %10s  %-23s  %s/\n", "-", "", names[i]);
        }
    }
    if (n == 0)
        printf("  (empty)\n");
    for (i = 0; i < n; i++)
        free(names[i]);
    free(names);
    store_close(&st);
    return 0;
}

/* Writes version v to fd. */
static int dump_version(struct store *st, const struct version *v, int fd)
{
    uint64_t size, off = 0;
    uint8_t (*blocks)[HASH_LEN] = NULL;
    size_t nb;
    char *buf;
    int r;

    if ((r = bs_get_manifest(st, v->manifest, &size, &blocks, &nb)) < 0)
        return r;
    buf = malloc(64 * 1024);
    if (!buf) {
        free(blocks);
        return -ENOMEM;
    }
    while (off < size) {
        ssize_t got = bs_read_version(st, NULL, size, (const uint8_t (*)[HASH_LEN])blocks,
                                      nb, buf, 64 * 1024, (off_t)off);
        ssize_t w = 0;
        if (got <= 0) {
            r = got < 0 ? (int)got : -EIO;
            break;
        }
        while (w < got) {
            ssize_t k = write(fd, buf + w, (size_t)(got - w));
            if (k < 0) {
                r = -errno;
                goto out;
            }
            w += k;
        }
        off += (uint64_t)got;
    }
out:
    free(buf);
    free(blocks);
    return r;
}

int cmd_cat(int argc, char **argv)
{
    struct store st;
    char *at = NULL, path[JPATH_MAX];
    const struct version *v;
    int64_t t;
    int r;

    take_option(&argc, argv, "--at", "-t", &at);
    if (argc < 2) {
        fprintf(stderr, "usage: chronofs cat <store> <path> [--at TIME]\n");
        return 2;
    }
    if (open_store(&st, argv[0]) < 0)
        return 1;
    if (normalize_path(argv[1], path, sizeof(path)) < 0) {
        fprintf(stderr, "chronofs: invalid path (too long or contains ..)\n");
        store_close(&st);
        return 2;
    }
    if (get_time(&st, at, &t) < 0) {
        store_close(&st);
        return 1;
    }
    v = index_at(&st.idx, path, t);
    if (!v || v->op != OP_WRITE) {
        fprintf(stderr, "chronofs: %s did not exist as a file at that time\n", path);
        store_close(&st);
        return 1;
    }
    r = dump_version(&st, v, STDOUT_FILENO);
    if (r < 0)
        fprintf(stderr, "chronofs: read failed: %s\n", strerror(-r));
    store_close(&st);
    return r < 0;
}

static int mkdirs_for(const char *file)
{
    char tmp[PATH_MAX], *p;
    snprintf(tmp, sizeof(tmp), "%s", file);
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -errno;
            *p = '/';
        }
    }
    return 0;
}

/* Restores one file version to dest (a real filesystem path). */
static int restore_one(struct store *st, const struct version *v, const char *dest)
{
    int fd, r;

    if ((r = mkdirs_for(dest)) < 0)
        return r;
    fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, v->mode & 07777);
    if (fd < 0)
        return -errno;
    r = dump_version(st, v, fd);
    if (close(fd) < 0 && r == 0)
        r = -errno;
    if (r == 0)
        chmod(dest, v->mode & 07777);
    return r;
}

int cmd_restore(int argc, char **argv)
{
    struct store st;
    char *at = NULL, *out = NULL, path[JPATH_MAX], mp[PATH_MAX], tbuf[64];
    const struct version *v;
    int64_t t;
    int mounted, restored = 0, failed = 0;
    size_t i, pl;

    take_option(&argc, argv, "--at", "-t", &at);
    take_option(&argc, argv, "--output", "-o", &out);
    if (argc < 2 || !at) {
        fprintf(stderr, "usage: chronofs restore <store> <path> --at TIME [-o dest]\n");
        return 2;
    }
    if (open_store(&st, argv[0]) < 0)
        return 1;
    if (normalize_path(argv[1], path, sizeof(path)) < 0) {
        fprintf(stderr, "chronofs: invalid path (too long or contains ..)\n");
        store_close(&st);
        return 2;
    }
    if (get_time(&st, at, &t) < 0) {
        store_close(&st);
        return 1;
    }
    mounted = store_mounted(&st, mp, sizeof(mp));
    fmt_time(t, tbuf, sizeof(tbuf));

    /* A single file, or every file under a directory, as of time t. */
    pl = strlen(path);
    for (i = 0; i < st.idx.nall; i++) {
        struct pentry *p = st.idx.all[i];
        char dest[PATH_MAX];
        const char *suffix;
        struct version copy;
        int r;

        if (strncmp(p->path, path, pl) != 0 || (p->path[pl] != '\0' && p->path[pl] != '/' &&
                                                 strcmp(path, "/") != 0))
            continue;
        v = pentry_at(p, t);
        if (!v || v->op != OP_WRITE)
            continue;
        copy = *v;
        suffix = p->path + (strcmp(path, "/") == 0 ? 0 : pl);

        if (out)
            snprintf(dest, sizeof(dest), "%s%s", out, suffix);
        else if (mounted && mp[0])
            snprintf(dest, sizeof(dest), "%s%s", mp, p->path);  /* goes through FUSE */
        else
            store_real_path(&st, dest, sizeof(dest), p->path);

        r = restore_one(&st, &copy, dest);
        if (r < 0) {
            fprintf(stderr, "chronofs: %s: %s\n", dest, strerror(-r));
            failed++;
            continue;
        }
        /* Offline restore into the store: record it as a new version ourselves. */
        if (!out && !(mounted && mp[0])) {
            char saved[JPATH_MAX];
            snprintf(saved, sizeof(saved), "%s", p->path);
            if ((r = bs_snapshot_file(&st, saved)) < 0) {
                fprintf(stderr, "chronofs: cannot record restored file: %s\n", strerror(-r));
                failed++;
                continue;
            }
        }
        printf("restored %s (as of %s) -> %s\n", p->path, tbuf, dest);
        restored++;
    }
    if (restored == 0 && failed == 0)
        fprintf(stderr, "chronofs: nothing to restore: %s did not exist at %s\n", path, tbuf);
    store_close(&st);
    return (restored == 0 || failed) ? 1 : 0;
}

/* Counts files and bytes under <store>/<sub>/xx/... */
static void count_objects(const struct store *st, const char *sub, size_t *count, uint64_t *bytes)
{
    char base[PATH_MAX], p[PATH_MAX], q[PATH_MAX + 300];
    DIR *d, *e;
    struct dirent *de, *fe;
    struct stat sb;

    *count = 0;
    *bytes = 0;
    store_path(st, base, sizeof(base), sub);
    if (!(d = opendir(base)))
        return;
    while ((de = readdir(d))) {
        if (de->d_name[0] == '.')
            continue;
        snprintf(p, sizeof(p), "%s/%s", base, de->d_name);
        if (!(e = opendir(p)))
            continue;
        while ((fe = readdir(e))) {
            if (fe->d_name[0] == '.')
                continue;
            snprintf(q, sizeof(q), "%s/%s", p, fe->d_name);
            if (stat(q, &sb) == 0) {
                (*count)++;
                *bytes += (uint64_t)sb.st_size;
            }
        }
        closedir(e);
    }
    closedir(d);
}

int cmd_stats(int argc, char **argv)
{
    struct store st;
    struct jrec *recs;
    size_t n, i, nobj, nman, writes = 0, deletes = 0, dirs = 0, live_files = 0, live_dirs = 0;
    uint64_t obj_bytes, man_bytes, logical = 0, live_bytes = 0;
    char b1[32], b2[32], b3[32], t1[64], t2[64], mp[PATH_MAX];
    int64_t now = now_ns();

    if (argc < 1) {
        fprintf(stderr, "usage: chronofs stats <store>\n");
        return 2;
    }
    if (open_store(&st, argv[0]) < 0)
        return 1;
    if (journal_read_all(st.root, &recs, &n) < 0) {
        store_close(&st);
        return 1;
    }
    for (i = 0; i < n; i++) {
        switch (recs[i].op) {
        case OP_WRITE:  writes++; logical += recs[i].size; break;
        case OP_UNLINK:
        case OP_RMDIR:  deletes++; break;
        case OP_MKDIR:  dirs++; break;
        }
    }
    for (i = 0; i < st.idx.nall; i++) {
        const struct version *v = pentry_at(st.idx.all[i], now);
        if (v && v->op == OP_WRITE) {
            live_files++;
            live_bytes += v->size;
        } else if (v && v->op == OP_MKDIR) {
            live_dirs++;
        }
    }
    count_objects(&st, "objects", &nobj, &obj_bytes);
    count_objects(&st, "manifests", &nman, &man_bytes);

    printf("store              %s\n", st.root);
    if (store_mounted(&st, mp, sizeof(mp)))
        printf("status             mounted on %s\n", mp[0] ? mp : "(unknown)");
    else
        printf("status             not mounted\n");
    if (n) {
        fmt_time(recs[0].ts_ns, t1, sizeof(t1));
        fmt_time(recs[n - 1].ts_ns, t2, sizeof(t2));
        printf("history            %s  ->  %s\n", t1, t2);
    }
    printf("journal records    %zu  (%zu writes, %zu deletes, %zu mkdirs)\n",
           n, writes, deletes, dirs);
    printf("tracked paths      %zu  (now live: %zu files, %zu dirs)\n",
           st.idx.nall, live_files, live_dirs);
    fmt_size(live_bytes, b1, sizeof(b1));
    printf("live data          %s\n", b1);
    fmt_size(logical, b1, sizeof(b1));
    printf("all versions       %s  (if every version were a full copy)\n", b1);
    fmt_size(obj_bytes, b2, sizeof(b2));
    fmt_size(man_bytes, b3, sizeof(b3));
    printf("stored blocks      %zu blocks, %s\n", nobj, b2);
    printf("manifests          %zu, %s\n", nman, b3);
    if (obj_bytes > 0)
        printf("dedup ratio        %.2fx  (%s of history in %s of blocks)\n",
               (double)logical / (double)obj_bytes, b1, b2);
    free(recs);
    store_close(&st);
    return 0;
}
