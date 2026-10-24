/*
 * blockstore.c - content-addressed storage for file versions.
 *
 * A file version is split into 4 KiB blocks.  Each block is stored once under
 * objects/<first 2 hex>/<remaining 62 hex> of its SHA-256, so unchanged blocks
 * are shared by every version that contains them (deduplication).  A manifest
 * lists the block hashes of one version - it is an index block, exactly like
 * indexed file allocation - and is itself stored by hash under manifests/.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "chronofs.h"

struct manifest_hdr {
    uint32_t magic;
    uint32_t nblocks;
    uint64_t size;
};

int bs_object_path(const struct store *st, const char *kind, const uint8_t h[HASH_LEN],
                   char *out, size_t n)
{
    char hex[HASH_HEX_LEN + 1];
    int r;
    hash_to_hex(h, hex);
    r = snprintf(out, n, "%s/%s/%.2s/%s", st->root, kind, hex, hex + 2);
    return (r < 0 || (size_t)r >= n) ? -ENAMETOOLONG : 0;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

static ssize_t read_full(int fd, void *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t r = read(fd, (char *)buf + got, len - got);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return -errno;
        }
        if (r == 0)
            break;
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/* Stores data under kind/<hash> unless it already exists (write-temp-then-rename). */
static int put_object(struct store *st, const char *kind, const uint8_t h[HASH_LEN],
                      const void *data, size_t len)
{
    char path[PATH_MAX], tmp[PATH_MAX], *slash;
    int fd, r;

    if ((r = bs_object_path(st, kind, h, path, sizeof(path))) < 0)
        return r;
    if (access(path, F_OK) == 0)
        return 0;                       /* already stored: dedup hit */

    strcpy(tmp, path);
    slash = strrchr(tmp, '/');
    *slash = '\0';
    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -errno;
    strcat(tmp, "/.tmpXXXXXX");
    fd = mkstemp(tmp);
    if (fd < 0)
        return -errno;
    r = write_all(fd, data, len);
    if (r == 0 && fsync(fd) < 0)
        r = -errno;
    if (close(fd) < 0 && r == 0)
        r = -errno;
    if (r == 0 && rename(tmp, path) < 0)
        r = -errno;
    if (r < 0)
        unlink(tmp);
    return r;
}

int bs_put_block(struct store *st, const void *data, size_t len, uint8_t hash[HASH_LEN])
{
    sha256(data, len, hash);
    return put_object(st, "objects", hash, data, len);
}

int bs_get_block(struct store *st, const uint8_t hash[HASH_LEN], void *buf, size_t *len)
{
    char path[PATH_MAX];
    ssize_t r;
    int fd;

    if (bs_object_path(st, "objects", hash, path, sizeof(path)) < 0)
        return -ENAMETOOLONG;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    r = read_full(fd, buf, BLOCK_SIZE);
    close(fd);
    if (r < 0)
        return (int)r;
    *len = (size_t)r;
    {
        uint8_t actual[HASH_LEN];
        sha256(buf, *len, actual);
        if (memcmp(actual, hash, HASH_LEN))
            return -EIO;
    }
    return 0;
}

int bs_put_manifest(struct store *st, uint64_t size, const uint8_t (*blocks)[HASH_LEN],
                    size_t n, uint8_t mhash[HASH_LEN])
{
    size_t total = sizeof(struct manifest_hdr) + n * HASH_LEN;
    struct manifest_hdr *hdr = malloc(total);
    int r;

    if (!hdr)
        return -ENOMEM;
    hdr->magic = MANIFEST_MAGIC;
    hdr->nblocks = (uint32_t)n;
    hdr->size = size;
    if (n)
        memcpy(hdr + 1, blocks, n * HASH_LEN);
    sha256(hdr, total, mhash);
    r = put_object(st, "manifests", mhash, hdr, total);
    free(hdr);
    return r;
}

int bs_get_manifest(struct store *st, const uint8_t mhash[HASH_LEN], uint64_t *size,
                    uint8_t (**blocks)[HASH_LEN], size_t *n)
{
    char path[PATH_MAX];
    struct manifest_hdr hdr;
    struct stat sb;
    uint8_t (*b)[HASH_LEN] = NULL;
    ssize_t r;
    int fd;

    if (bs_object_path(st, "manifests", mhash, path, sizeof(path)) < 0)
        return -ENAMETOOLONG;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    r = read_full(fd, &hdr, sizeof(hdr));
    if (r != (ssize_t)sizeof(hdr) || hdr.magic != MANIFEST_MAGIC ||
        fstat(fd, &sb) < 0 ||
        (uint64_t)sb.st_size != sizeof(hdr) + (uint64_t)hdr.nblocks * HASH_LEN ||
        hdr.nblocks != hdr.size / BLOCK_SIZE + (hdr.size % BLOCK_SIZE != 0)) {
        close(fd);
        return -EIO;
    }
    if (hdr.nblocks) {
        b = malloc((size_t)hdr.nblocks * HASH_LEN);
        if (!b) {
            close(fd);
            return -ENOMEM;
        }
        r = read_full(fd, b, (size_t)hdr.nblocks * HASH_LEN);
        if (r != (ssize_t)hdr.nblocks * HASH_LEN) {
            free(b);
            close(fd);
            return -EIO;
        }
    }
    close(fd);
    {
        size_t total = sizeof(hdr) + (size_t)hdr.nblocks * HASH_LEN;
        uint8_t actual[HASH_LEN];
        char *encoded = malloc(total);
        if (!encoded) {
            free(b);
            return -ENOMEM;
        }
        memcpy(encoded, &hdr, sizeof(hdr));
        if (hdr.nblocks)
            memcpy(encoded + sizeof(hdr), b, (size_t)hdr.nblocks * HASH_LEN);
        sha256(encoded, total, actual);
        free(encoded);
        if (memcmp(actual, mhash, HASH_LEN)) {
            free(b);
            return -EIO;
        }
    }
    *size = hdr.size;
    *blocks = b;
    *n = hdr.nblocks;
    return 0;
}

int bs_snapshot_file(struct store *st, const char *fpath)
{
    char real[PATH_MAX];
    char *buf;
    uint8_t (*blocks)[HASH_LEN] = NULL;
    uint8_t mhash[HASH_LEN];
    size_t n = 0, cap = 0;
    uint64_t total = 0;
    struct stat sb;
    const struct version *last;
    int fd, r = 0;

    if ((r = store_real_path(st, real, sizeof(real), fpath)) < 0)
        return r;
    fd = open(real, O_RDONLY);
    if (fd < 0)
        return -errno;
    if (fstat(fd, &sb) < 0) {
        r = -errno;
        close(fd);
        return r;
    }
    if (!S_ISREG(sb.st_mode)) {
        close(fd);
        return 0;
    }
    buf = malloc(BLOCK_SIZE);
    if (!buf) {
        close(fd);
        return -ENOMEM;
    }

    /* Split the file into blocks; only blocks not already stored hit the disk. */
    for (;;) {
        ssize_t got = read_full(fd, buf, BLOCK_SIZE);
        if (got < 0) {
            r = (int)got;
            goto out;
        }
        if (got == 0)
            break;
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 16;
            void *nb = realloc(blocks, nc * HASH_LEN);
            if (!nb) {
                r = -ENOMEM;
                goto out;
            }
            blocks = nb;
            cap = nc;
        }
        if ((r = bs_put_block(st, buf, (size_t)got, blocks[n])) < 0)
            goto out;
        n++;
        total += (uint64_t)got;
        if (got < BLOCK_SIZE)
            break;
    }
    if (fstat(fd, &sb) < 0) {
        r = -errno;
        goto out;
    }
    if ((r = bs_put_manifest(st, total, (const uint8_t (*)[HASH_LEN])blocks, n, mhash)) < 0)
        goto out;

    pthread_rwlock_wrlock(&st->lock);
    last = index_at(&st->idx, fpath, INT64_MAX);
    if (last && last->op == OP_WRITE && last->mode == sb.st_mode &&
        last->mtime_ns == (int64_t)sb.st_mtim.tv_sec * NS_PER_SEC + sb.st_mtim.tv_nsec &&
        memcmp(last->manifest, mhash, HASH_LEN) == 0) {
        r = 0;                          /* nothing changed: no new version */
    } else {
        r = store_append_locked(st, OP_WRITE, fpath, sb.st_mode,
                                (int64_t)sb.st_mtim.tv_sec * NS_PER_SEC + sb.st_mtim.tv_nsec,
                                total, mhash);
    }
    pthread_rwlock_unlock(&st->lock);
out:
    free(buf);
    free(blocks);
    close(fd);
    return r;
}

ssize_t bs_read_version(struct store *st, struct lru *cache, uint64_t size,
                        const uint8_t (*blocks)[HASH_LEN], size_t nblocks,
                        char *buf, size_t len, off_t off)
{
    char *blk;
    size_t done = 0;

    if (off < 0 || (uint64_t)off >= size)
        return 0;
    if ((uint64_t)off + len > size)
        len = (size_t)(size - (uint64_t)off);
    blk = malloc(BLOCK_SIZE);
    if (!blk)
        return -ENOMEM;

    while (done < len) {
        uint64_t pos = (uint64_t)off + done;
        size_t bi = (size_t)(pos / BLOCK_SIZE);
        size_t bo = (size_t)(pos % BLOCK_SIZE);
        size_t blen = 0, take;
        int r;

        if (bi >= nblocks)
            break;
        if (!cache || !lru_get(cache, blocks[bi], blk, &blen)) {
            if ((r = bs_get_block(st, blocks[bi], blk, &blen)) < 0) {
                free(blk);
                return r;
            }
            if (cache)
                lru_put(cache, blocks[bi], blk, blen);
        }
        if (bo >= blen)
            break;
        take = blen - bo;
        if (take > len - done)
            take = len - done;
        memcpy(buf + done, blk + bo, take);
        done += take;
    }
    free(blk);
    return (ssize_t)done;
}
