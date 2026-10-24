/*
 * chronofs.h - shared definitions for ChronoFS, a time-travel FUSE filesystem.
 *
 * On-disk layout of a store directory:
 *
 *   <store>/current/          live files (the mount passes through to here)
 *   <store>/objects/ab/cd...  4 KiB data blocks, named by their SHA-256
 *   <store>/manifests/ab/...  one manifest per file version (size + block list)
 *   <store>/journal.log       append-only array of struct jrec
 *   <store>/tags              "name ts_ns" lines (named snapshots)
 *   <store>/lock              flock()ed by the mount daemon
 *   <store>/mounted           mountpoint path while mounted
 */
#ifndef CHRONOFS_H
#define CHRONOFS_H

#include <stdint.h>
#include <stddef.h>
#include <limits.h>
#include <pthread.h>
#include <sys/types.h>

#define CHRONOFS_VERSION "1.0"

#define BLOCK_SIZE      4096
#define HASH_LEN        32              /* SHA-256 digest bytes      */
#define HASH_HEX_LEN    (HASH_LEN * 2)  /* hex string length          */
#define JPATH_MAX       1024            /* max path stored in journal */

#define JREC_MAGIC      0x4a524843u     /* "CHRJ" little endian */
#define MANIFEST_MAGIC  0x4d524843u     /* "CHRM" little endian */

#define SNAP_DIR        ".snapshots"
#define CTL_DIR         ".chronofs"

#define NS_PER_SEC      1000000000LL

/* Journal operations. */
enum jop {
    OP_WRITE  = 1,  /* a new version of a regular file   */
    OP_UNLINK = 2,  /* file deleted (tombstone)           */
    OP_MKDIR  = 3,  /* directory created                  */
    OP_RMDIR  = 4,  /* directory removed (tombstone)      */
};

/* One fixed-size journal record (1096 bytes). */
struct jrec {
    uint32_t magic;
    uint16_t op;
    uint16_t reserved;
    uint32_t mode;
    uint32_t reserved2;
    int64_t  ts_ns;               /* when this version was recorded   */
    int64_t  mtime_ns;            /* file mtime at that moment        */
    uint64_t size;                /* file size in bytes               */
    uint8_t  manifest[HASH_LEN];  /* manifest hash (OP_WRITE only)    */
    char     path[JPATH_MAX];     /* "/dir/file", NUL terminated      */
};

/* In-memory copy of a journal record, minus the path. */
struct version {
    int64_t  ts_ns;
    int64_t  mtime_ns;
    uint64_t size;
    uint32_t mode;
    uint16_t op;
    uint8_t  manifest[HASH_LEN];
};

/* All versions of one path, sorted by ts_ns. */
struct pentry {
    char           *path;
    struct version *v;
    size_t          n, cap;
    struct pentry  *next;         /* hash chain */
};

/* Path -> history hash table. */
struct index {
    struct pentry **buckets;
    size_t          nbuckets;
    struct pentry **all;          /* every entry, for iteration */
    size_t          nall, capall;
    size_t          nrecords;
};

/* An open store. */
struct store {
    char             root[PATH_MAX];
    int              jfd;         /* journal fd (O_APPEND)            */
    int              lockfd;      /* held while mounted, -1 otherwise */
    int64_t          last_ts;     /* ts of the newest record          */
    struct index     idx;
    pthread_rwlock_t lock;        /* protects idx, last_ts, journal   */
};

/* ---- sha256.c ---- */
void sha256(const void *data, size_t len, uint8_t out[HASH_LEN]);
void hash_to_hex(const uint8_t h[HASH_LEN], char out[HASH_HEX_LEN + 1]);

/* ---- journal.c : store + index ---- */
int  store_open(struct store *st, const char *dir, int create);
int  store_reload(struct store *st);
void store_close(struct store *st);
int  store_path(const struct store *st, char *out, size_t n, const char *sub);
int  store_real_path(const struct store *st, char *out, size_t n, const char *fpath);
int64_t now_ns(void);

/* Appends a record. Caller must NOT hold st->lock. */
int  store_append(struct store *st, int op, const char *path, uint32_t mode,
                  int64_t mtime_ns, uint64_t size, const uint8_t *manifest);
/* Same, but caller holds st->lock for writing. */
int  store_append_locked(struct store *st, int op, const char *path, uint32_t mode,
                         int64_t mtime_ns, uint64_t size, const uint8_t *manifest);

void index_init(struct index *idx);
void index_free(struct index *idx);
int  index_add(struct index *idx, const char *path, const struct version *v);
struct pentry *index_find(const struct index *idx, const char *path);
const struct version *pentry_at(const struct pentry *p, int64_t t);
const struct version *index_at(const struct index *idx, const char *path, int64_t t);
int  version_live(const struct version *v);
int  journal_read_all(const char *store_root, struct jrec **out, size_t *n);

/* ---- blockstore.c ---- */
int  bs_put_block(struct store *st, const void *data, size_t len, uint8_t hash[HASH_LEN]);
int  bs_get_block(struct store *st, const uint8_t hash[HASH_LEN], void *buf, size_t *len);
int  bs_put_manifest(struct store *st, uint64_t size, const uint8_t (*blocks)[HASH_LEN],
                     size_t n, uint8_t mhash[HASH_LEN]);
int  bs_get_manifest(struct store *st, const uint8_t mhash[HASH_LEN], uint64_t *size,
                     uint8_t (**blocks)[HASH_LEN], size_t *n);
int  bs_object_path(const struct store *st, const char *kind, const uint8_t h[HASH_LEN],
                    char *out, size_t n);
/* Hashes the real file behind fpath and records a new version if it changed. */
int  bs_snapshot_file(struct store *st, const char *fpath);
/* Reads [off, off+len) of a stored version; uses the block cache if given. */
struct lru;
ssize_t bs_read_version(struct store *st, struct lru *cache, uint64_t size,
                        const uint8_t (*blocks)[HASH_LEN], size_t nblocks,
                        char *buf, size_t len, off_t off);

/* ---- lru.c : LRU cache of data blocks ---- */
struct lru_stats {
    uint64_t hits, misses, evictions;
    size_t   used, capacity;
};
struct lru *lru_create(size_t capacity);
void lru_destroy(struct lru *c);
int  lru_get(struct lru *c, const uint8_t key[HASH_LEN], void *buf, size_t *len);
void lru_put(struct lru *c, const uint8_t key[HASH_LEN], const void *data, size_t len);
void lru_get_stats(struct lru *c, struct lru_stats *out);

/* ---- timeparse.c ---- */
int  parse_when(const char *spec, int64_t now, int64_t *out);
int  resolve_when(const struct store *st, const char *spec, int64_t *out);
int  tag_lookup(const struct store *st, const char *name, int64_t *out);
int  parse_duration(const char *s, int64_t *out_ns);
void fmt_time(int64_t ts_ns, char *buf, size_t n);
void fmt_time_spec(int64_t ts_ns, char *buf, size_t n);
void fmt_size(uint64_t bytes, char *buf, size_t n);

/* ---- fs.c ---- */
int  fs_main(struct store *st, const char *mountpoint, int argc, char **argv,
             size_t cache_blocks);

/* ---- cmd.c / gc.c ---- */
int cmd_tui(int argc, char **argv);
int cmd_log(int argc, char **argv);
int cmd_tag(int argc, char **argv);
int cmd_ls(int argc, char **argv);
int cmd_cat(int argc, char **argv);
int cmd_restore(int argc, char **argv);
int cmd_stats(int argc, char **argv);
int cmd_gc(int argc, char **argv);

/* Normalises "a/b" or "/a/b/" into "/a/b". Returns -1 if too long. */
int normalize_path(const char *in, char *out, size_t n);

#endif
