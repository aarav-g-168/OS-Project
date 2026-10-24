/*
 * fs.c - the FUSE filesystem.
 *
 * Normal paths pass straight through to <store>/current/.  When a file that
 * was written to is closed (flush/release) or fsync()ed, its contents are
 * hashed into the block store and a new version is journaled.  Deletes,
 * renames and directory changes are journaled too.
 *
 * Two virtual, read-only directories exist at the root of the mount:
 *
 *   /.snapshots/<when>/...   the whole tree as it was at <when>
 *                            (e.g. 2min-ago, @19:53:12, a tag name)
 *   /.chronofs/stats         live statistics (cache hit rate etc.)
 */
#define FUSE_USE_VERSION 31
#include <fuse.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <unistd.h>
#include "chronofs.h"

#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1 << 0)
#endif

static struct {
    struct store *st;
    struct lru   *cache;
    char          mountpoint[PATH_MAX];
    int64_t       mounted_at;
    uint64_t      versions_this_mount;
    uint64_t      snapshot_reads;
    pthread_mutex_t stat_mu;
} G = { .stat_mu = PTHREAD_MUTEX_INITIALIZER };

/* ------------------------------------------------------------------ */
/* Path classification                                                */
/* ------------------------------------------------------------------ */

enum vkind { V_REAL, V_SNAPROOT, V_SNAP, V_CTLDIR, V_CTLSTATS };

struct vpath {
    enum vkind  k;
    int64_t     t;              /* V_SNAP: the moment being viewed       */
    const char *rel;            /* V_SNAP: path inside the snapshot ("/")*/
};

static int classify(const char *path, struct vpath *vp)
{
    static const char snap[] = "/" SNAP_DIR, ctl[] = "/" CTL_DIR;
    size_t sl = sizeof(snap) - 1, cl = sizeof(ctl) - 1;

    vp->k = V_REAL;
    vp->rel = path;
    if (strncmp(path, snap, sl) == 0 && (path[sl] == '\0' || path[sl] == '/')) {
        const char *spec = path + sl + 1, *end;
        char buf[256];
        size_t len;

        if (path[sl] == '\0' || path[sl + 1] == '\0') {
            vp->k = V_SNAPROOT;
            return 0;
        }
        end = strchr(spec, '/');
        len = end ? (size_t)(end - spec) : strlen(spec);
        if (len >= sizeof(buf))
            return -ENOENT;
        memcpy(buf, spec, len);
        buf[len] = '\0';
        if (resolve_when(G.st, buf, &vp->t) < 0)
            return -ENOENT;
        vp->k = V_SNAP;
        vp->rel = (end && end[1]) ? end : "/";
        return 0;
    }
    if (strncmp(path, ctl, cl) == 0 && (path[cl] == '\0' || path[cl] == '/')) {
        if (path[cl] == '\0' || strcmp(path + cl, "/") == 0)
            vp->k = V_CTLDIR;
        else if (strcmp(path + cl, "/stats") == 0)
            vp->k = V_CTLSTATS;
        else
            return -ENOENT;
        return 0;
    }
    return 0;
}

static int real_path(const char *path, char *out)
{
    return store_real_path(G.st, out, PATH_MAX, path);
}

/* ------------------------------------------------------------------ */
/* Looking things up inside a snapshot                                */
/* ------------------------------------------------------------------ */

enum { SNAP_NONE = 0, SNAP_FILE = 1, SNAP_DIRECTORY = 2 };

/* Does any path under dir/ exist at time t?  Caller holds the read lock. */
static int has_live_child(int64_t t, const char *dir)
{
    size_t dl = strlen(dir), i;
    for (i = 0; i < G.st->idx.nall; i++) {
        struct pentry *p = G.st->idx.all[i];
        if (strncmp(p->path, dir, dl) == 0 && p->path[dl] == '/' &&
            version_live(pentry_at(p, t)))
            return 1;
    }
    return 0;
}

static int snap_lookup(int64_t t, const char *rel, struct version *out)
{
    const struct version *v;
    int kind = SNAP_NONE;

    if (strcmp(rel, "/") == 0)
        return SNAP_DIRECTORY;
    pthread_rwlock_rdlock(&G.st->lock);
    v = index_at(&G.st->idx, rel, t);
    if (v && v->op == OP_WRITE) {
        *out = *v;
        kind = SNAP_FILE;
    } else if (v && v->op == OP_MKDIR) {
        *out = *v;
        kind = SNAP_DIRECTORY;
    } else if (has_live_child(t, rel)) {
        memset(out, 0, sizeof(*out));   /* implied by a file inside it */
        out->mode = S_IFDIR | 0755;
        out->mtime_ns = t;
        kind = SNAP_DIRECTORY;
    }
    pthread_rwlock_unlock(&G.st->lock);
    return kind;
}

static void fill_ts(struct timespec *ts, int64_t ns)
{
    ts->tv_sec = (time_t)(ns / NS_PER_SEC);
    ts->tv_nsec = (long)(ns % NS_PER_SEC);
}

static void virt_dir_stat(struct stat *sb, int64_t mtime)
{
    memset(sb, 0, sizeof(*sb));
    sb->st_mode = S_IFDIR | 0555;
    sb->st_nlink = 2;
    sb->st_uid = getuid();
    sb->st_gid = getgid();
    fill_ts(&sb->st_mtim, mtime);
    sb->st_atim = sb->st_ctim = sb->st_mtim;
}

/* ------------------------------------------------------------------ */
/* /.chronofs/stats                                                    */
/* ------------------------------------------------------------------ */

static char *gen_stats(size_t *len)
{
    struct lru_stats ls;
    char started[64], cap[32], *buf;
    uint64_t lookups, vers, sreads;
    size_t records, paths;
    int n;

    lru_get_stats(G.cache, &ls);
    pthread_rwlock_rdlock(&G.st->lock);
    records = G.st->idx.nrecords;
    paths = G.st->idx.nall;
    pthread_rwlock_unlock(&G.st->lock);
    pthread_mutex_lock(&G.stat_mu);
    vers = G.versions_this_mount;
    sreads = G.snapshot_reads;
    pthread_mutex_unlock(&G.stat_mu);

    fmt_time(G.mounted_at, started, sizeof(started));
    fmt_size((uint64_t)ls.capacity * BLOCK_SIZE, cap, sizeof(cap));
    lookups = ls.hits + ls.misses;

    buf = malloc(2048);
    if (!buf)
        return NULL;
    n = snprintf(buf, 2048,
        "ChronoFS %s\n"
        "store            : %s\n"
        "mounted at       : %s\n"
        "mounted since    : %s\n"
        "journal records  : %zu\n"
        "tracked paths    : %zu\n"
        "versions (mount) : %llu\n"
        "snapshot reads   : %llu\n"
        "\n"
        "LRU block cache\n"
        "  capacity       : %zu blocks (%s)\n"
        "  in use         : %zu blocks\n"
        "  hits           : %llu\n"
        "  misses         : %llu\n"
        "  evictions      : %llu\n"
        "  hit ratio      : %.1f%%\n",
        CHRONOFS_VERSION, G.st->root, G.mountpoint, started, records, paths,
        (unsigned long long)vers, (unsigned long long)sreads,
        ls.capacity, cap, ls.used,
        (unsigned long long)ls.hits, (unsigned long long)ls.misses,
        (unsigned long long)ls.evictions,
        lookups ? 100.0 * (double)ls.hits / (double)lookups : 0.0);
    *len = (size_t)n;
    return buf;
}

/* ------------------------------------------------------------------ */
/* Open file handles                                                   */
/* ------------------------------------------------------------------ */

enum fhkind { FH_REAL, FH_SNAP, FH_STATS };

struct fh {
    enum fhkind kind;
    int         fd;                 /* FH_REAL                         */
    int         dirty;              /* FH_REAL: changed since snapshot  */
    int         wrote;              /* FH_REAL: write() since snapshot  */
    uint64_t    size;               /* FH_SNAP                         */
    uint8_t   (*blocks)[HASH_LEN];  /* FH_SNAP                         */
    size_t      nblocks;            /* FH_SNAP                         */
    char       *text;               /* FH_STATS                        */
    size_t      textlen;
};

static struct fh *get_fh(struct fuse_file_info *fi)
{
    return (struct fh *)(uintptr_t)fi->fh;
}

/* Turn the current contents of path into a new version. */
static int record_version(const char *path)
{
    int r;
    size_t before;

    if (!path)
        return 0;                   /* file was deleted while open */
    pthread_rwlock_rdlock(&G.st->lock);
    before = G.st->idx.nrecords;
    pthread_rwlock_unlock(&G.st->lock);

    r = bs_snapshot_file(G.st, path);
    if (r < 0) {
        fprintf(stderr, "chronofs: snapshot of %s failed: %s\n", path, strerror(-r));
        return r;
    }
    pthread_rwlock_rdlock(&G.st->lock);
    if (G.st->idx.nrecords != before) {
        pthread_mutex_lock(&G.stat_mu);
        G.versions_this_mount++;
        pthread_mutex_unlock(&G.stat_mu);
    }
    pthread_rwlock_unlock(&G.st->lock);
    return 0;
}

/* ------------------------------------------------------------------ */
/* FUSE operations                                                     */
/* ------------------------------------------------------------------ */

static void *cfs_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    char p[PATH_MAX];
    FILE *f;

    /* Snapshot views change with the clock, so don't let the kernel cache them. */
    cfg->entry_timeout = 0;
    cfg->attr_timeout = 0;
    cfg->negative_timeout = 0;
    cfg->hard_remove = 1;
    if (conn->capable & FUSE_CAP_ATOMIC_O_TRUNC)
        conn->want |= FUSE_CAP_ATOMIC_O_TRUNC;

    store_path(G.st, p, sizeof(p), "mounted");
    if ((f = fopen(p, "w"))) {
        fprintf(f, "%s\n", G.mountpoint);
        fclose(f);
    }
    return NULL;
}

static void cfs_destroy(void *priv)
{
    char p[PATH_MAX];
    (void)priv;
    store_path(G.st, p, sizeof(p), "mounted");
    unlink(p);
}

static int cfs_getattr(const char *path, struct stat *sb, struct fuse_file_info *fi)
{
    struct vpath vp;
    struct version v;
    char real[PATH_MAX];
    int r, kind;

    (void)fi;
    if ((r = classify(path, &vp)) < 0)
        return r;

    switch (vp.k) {
    case V_REAL:
        if ((r = real_path(path, real)) < 0)
            return r;
        return lstat(real, sb) < 0 ? -errno : 0;

    case V_SNAPROOT:
    case V_CTLDIR:
        virt_dir_stat(sb, G.st->last_ts ? G.st->last_ts : G.mounted_at);
        return 0;

    case V_CTLSTATS: {
        size_t len = 0;
        char *t = gen_stats(&len);
        free(t);
        memset(sb, 0, sizeof(*sb));
        sb->st_mode = S_IFREG | 0444;
        sb->st_nlink = 1;
        sb->st_size = (off_t)len;
        sb->st_uid = getuid();
        sb->st_gid = getgid();
        fill_ts(&sb->st_mtim, now_ns());
        sb->st_atim = sb->st_ctim = sb->st_mtim;
        return 0;
    }

    case V_SNAP:
        kind = snap_lookup(vp.t, vp.rel, &v);
        if (kind == SNAP_NONE)
            return -ENOENT;
        if (kind == SNAP_DIRECTORY) {
            int64_t mt = strcmp(vp.rel, "/") == 0 ? vp.t : v.mtime_ns;
            if (mt > now_ns())
                mt = now_ns();
            virt_dir_stat(sb, mt);
            return 0;
        }
        memset(sb, 0, sizeof(*sb));
        sb->st_mode = (mode_t)(v.mode & ~0222u);    /* history is read-only */
        sb->st_nlink = 1;
        sb->st_size = (off_t)v.size;
        sb->st_blocks = (blkcnt_t)((v.size + 511) / 512);
        sb->st_uid = getuid();
        sb->st_gid = getgid();
        fill_ts(&sb->st_mtim, v.mtime_ns);
        sb->st_atim = sb->st_mtim;
        fill_ts(&sb->st_ctim, v.ts_ns);
        return 0;
    }
    return -ENOENT;
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int cmp_i64_desc(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return (x < y) - (x > y);
}

/* Lists the children of dir as it was at time t. */
static int readdir_snapshot(int64_t t, const char *dir, void *buf, fuse_fill_dir_t filler)
{
    char **names = NULL;
    size_t n = 0, cap = 0, i, dl;
    const char *prefix = strcmp(dir, "/") == 0 ? "" : dir;

    dl = strlen(prefix);
    pthread_rwlock_rdlock(&G.st->lock);
    for (i = 0; i < G.st->idx.nall; i++) {
        struct pentry *p = G.st->idx.all[i];
        const char *rest, *slash;
        size_t len;

        if (strncmp(p->path, prefix, dl) != 0 || p->path[dl] != '/' || p->path[dl + 1] == '\0')
            continue;
        if (!version_live(pentry_at(p, t)))
            continue;
        rest = p->path + dl + 1;
        slash = strchr(rest, '/');
        len = slash ? (size_t)(slash - rest) : strlen(rest);
        if (n == cap) {
            size_t nc = cap ? cap * 2 : 32;
            char **nn = realloc(names, nc * sizeof(*nn));
            if (!nn)
                break;
            names = nn;
            cap = nc;
        }
        names[n] = strndup(rest, len);
        if (names[n])
            n++;
    }
    pthread_rwlock_unlock(&G.st->lock);

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    if (n > 1)
        qsort(names, n, sizeof(*names), cmp_str);
    for (i = 0; i < n; i++) {
        if (i == 0 || strcmp(names[i], names[i - 1]) != 0)
            filler(buf, names[i], NULL, 0, 0);
    }
    for (i = 0; i < n; i++)
        free(names[i]);
    free(names);
    return 0;
}

/* .snapshots/ lists handy relative times, tags, and one entry per recent change. */
static int readdir_snaproot(void *buf, fuse_fill_dir_t filler)
{
    static const char *presets[] = {
        "now", "30s-ago", "1min-ago", "2min-ago", "5min-ago", "10min-ago",
        "1h-ago", "1d-ago",
    };
    char p[PATH_MAX], line[512], name[256], spec[64], last[64] = "";
    int64_t *ts = NULL;
    size_t n = 0, cap = 0, i, j;
    FILE *f;

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    for (i = 0; i < sizeof(presets) / sizeof(presets[0]); i++)
        filler(buf, presets[i], NULL, 0, 0);

    store_path(G.st, p, sizeof(p), "tags");
    if ((f = fopen(p, "r"))) {
        while (fgets(line, sizeof(line), f)) {
            int64_t dummy;
            if (sscanf(line, "%255s", name) == 1 && tag_lookup(G.st, name, &dummy) == 0)
                filler(buf, name, NULL, 0, 0);
        }
        fclose(f);
    }

    pthread_rwlock_rdlock(&G.st->lock);
    for (i = 0; i < G.st->idx.nall; i++) {
        struct pentry *pe = G.st->idx.all[i];
        for (j = 0; j < pe->n; j++) {
            if (n == cap) {
                size_t nc = cap ? cap * 2 : 128;
                int64_t *nt = realloc(ts, nc * sizeof(*nt));
                if (!nt)
                    goto done;
                ts = nt;
                cap = nc;
            }
            ts[n++] = pe->v[j].ts_ns;
        }
    }
done:
    pthread_rwlock_unlock(&G.st->lock);

    /* Newest 50 distinct seconds at which something changed. */
    if (n > 1)
        qsort(ts, n, sizeof(*ts), cmp_i64_desc);
    for (i = 0, j = 0; i < n && j < 50; i++) {
        fmt_time_spec(ts[i], spec, sizeof(spec));
        if (strcmp(spec, last) != 0) {
            filler(buf, spec, NULL, 0, 0);
            strcpy(last, spec);
            j++;
        }
    }
    free(ts);
    return 0;
}

static int cfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t off,
                       struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    struct vpath vp;
    struct version v;
    char real[PATH_MAX];
    DIR *d;
    struct dirent *de;
    int r;

    (void)off; (void)fi; (void)flags;
    if ((r = classify(path, &vp)) < 0)
        return r;

    switch (vp.k) {
    case V_SNAPROOT:
        return readdir_snaproot(buf, filler);
    case V_CTLDIR:
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        filler(buf, "stats", NULL, 0, 0);
        return 0;
    case V_CTLSTATS:
        return -ENOTDIR;
    case V_SNAP:
        r = snap_lookup(vp.t, vp.rel, &v);
        if (r == SNAP_NONE)
            return -ENOENT;
        if (r == SNAP_FILE)
            return -ENOTDIR;
        return readdir_snapshot(vp.t, vp.rel, buf, filler);
    case V_REAL:
        break;
    }

    if ((r = real_path(path, real)) < 0)
        return r;
    if (!(d = opendir(real)))
        return -errno;
    while ((de = readdir(d))) {
        if (strcmp(path, "/") == 0 &&
            (strcmp(de->d_name, SNAP_DIR) == 0 || strcmp(de->d_name, CTL_DIR) == 0))
            continue;
        filler(buf, de->d_name, NULL, 0, 0);
    }
    closedir(d);
    if (strcmp(path, "/") == 0) {
        filler(buf, SNAP_DIR, NULL, 0, 0);
        filler(buf, CTL_DIR, NULL, 0, 0);
    }
    return 0;
}

static int cfs_open(const char *path, struct fuse_file_info *fi)
{
    struct vpath vp;
    struct version v;
    struct fh *h;
    char real[PATH_MAX];
    int r, fd;

    if ((r = classify(path, &vp)) < 0)
        return r;
    if (vp.k != V_REAL && (fi->flags & O_ACCMODE) != O_RDONLY)
        return -EROFS;

    h = calloc(1, sizeof(*h));
    if (!h)
        return -ENOMEM;
    h->fd = -1;

    switch (vp.k) {
    case V_REAL:
        if ((r = real_path(path, real)) < 0)
            goto fail;
        fd = open(real, fi->flags);
        if (fd < 0) {
            r = -errno;
            goto fail;
        }
        h->kind = FH_REAL;
        h->fd = fd;
        h->dirty = (fi->flags & O_TRUNC) != 0;
        break;

    case V_SNAP:
        if (snap_lookup(vp.t, vp.rel, &v) != SNAP_FILE) {
            r = -ENOENT;
            goto fail;
        }
        if ((r = bs_get_manifest(G.st, v.manifest, &h->size, &h->blocks, &h->nblocks)) < 0)
            goto fail;
        h->kind = FH_SNAP;
        pthread_mutex_lock(&G.stat_mu);
        G.snapshot_reads++;
        pthread_mutex_unlock(&G.stat_mu);
        break;

    case V_CTLSTATS:
        h->kind = FH_STATS;
        h->text = gen_stats(&h->textlen);
        if (!h->text) {
            r = -ENOMEM;
            goto fail;
        }
        fi->direct_io = 1;
        break;

    default:
        r = -EISDIR;
        goto fail;
    }
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
fail:
    free(h);
    return r;
}

static int cfs_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct vpath vp;
    struct fh *h;
    char real[PATH_MAX];
    int r, fd;

    if ((r = classify(path, &vp)) < 0)
        return r == -ENOENT ? -EROFS : r;
    if (vp.k != V_REAL)
        return -EROFS;
    if ((r = real_path(path, real)) < 0)
        return r;
    fd = open(real, fi->flags | O_CREAT, mode);
    if (fd < 0)
        return -errno;
    h = calloc(1, sizeof(*h));
    if (!h) {
        close(fd);
        return -ENOMEM;
    }
    h->kind = FH_REAL;
    h->fd = fd;
    h->dirty = 1;                       /* a new file is a new version */
    fi->fh = (uint64_t)(uintptr_t)h;
    return 0;
}

static int cfs_read(const char *path, char *buf, size_t size, off_t off,
                    struct fuse_file_info *fi)
{
    struct fh *h = get_fh(fi);
    ssize_t r;

    (void)path;
    switch (h->kind) {
    case FH_REAL:
        r = pread(h->fd, buf, size, off);
        return r < 0 ? -errno : (int)r;
    case FH_SNAP:
        r = bs_read_version(G.st, G.cache, h->size, (const uint8_t (*)[HASH_LEN])h->blocks,
                            h->nblocks, buf, size, off);
        return (int)r;
    case FH_STATS:
        if ((size_t)off >= h->textlen)
            return 0;
        if (size > h->textlen - (size_t)off)
            size = h->textlen - (size_t)off;
        memcpy(buf, h->text + off, size);
        return (int)size;
    }
    return -EIO;
}

static int cfs_write(const char *path, const char *buf, size_t size, off_t off,
                     struct fuse_file_info *fi)
{
    struct fh *h = get_fh(fi);
    ssize_t r;

    (void)path;
    if (h->kind != FH_REAL)
        return -EROFS;
    r = pwrite(h->fd, buf, size, off);
    if (r < 0)
        return -errno;
    h->dirty = h->wrote = 1;
    return (int)r;
}

/*
 * Called on every close(): this is where a write session becomes a version.
 * Only data actually written triggers it here - a shell's `> file` dup()s and
 * closes the fd before writing, which would otherwise record an empty file.
 * Pure creates/truncates are recorded on the final release instead.
 */
static int cfs_flush(const char *path, struct fuse_file_info *fi)
{
    struct fh *h = get_fh(fi);
    if (h->kind == FH_REAL && h->wrote) {
        int r = record_version(path);
        if (r < 0)
            return r;
        h->dirty = h->wrote = 0;
    }
    return 0;
}

static int cfs_release(const char *path, struct fuse_file_info *fi)
{
    struct fh *h = get_fh(fi);
    int r = 0;
    if (h->kind == FH_REAL) {
        if (h->dirty)
            r = record_version(path);
        if (close(h->fd) < 0 && r == 0)
            r = -errno;
    }
    free(h->blocks);
    free(h->text);
    free(h);
    return r;
}

static int cfs_fsync(const char *path, int datasync, struct fuse_file_info *fi)
{
    struct fh *h = get_fh(fi);
    if (h->kind != FH_REAL)
        return 0;
    if ((datasync ? fdatasync(h->fd) : fsync(h->fd)) < 0)
        return -errno;
    if (h->dirty) {
        int r = record_version(path);
        if (r < 0)
            return r;
        h->dirty = h->wrote = 0;
    }
    return fsync(G.st->jfd) < 0 ? -errno : 0;
}

static int cfs_truncate(const char *path, off_t size, struct fuse_file_info *fi)
{
    struct vpath vp;
    char real[PATH_MAX];
    int r;

    if ((r = classify(path, &vp)) < 0 || vp.k != V_REAL)
        return r < 0 ? r : -EROFS;
    if (fi) {
        struct fh *h = get_fh(fi);
        if (ftruncate(h->fd, size) < 0)
            return -errno;
        h->dirty = 1;
        return 0;
    }
    if ((r = real_path(path, real)) < 0)
        return r;
    if (truncate(real, size) < 0)
        return -errno;
    return record_version(path);
}

static int cfs_unlink(const char *path)
{
    struct vpath vp;
    char real[PATH_MAX];
    int r;

    if ((r = classify(path, &vp)) < 0 || vp.k != V_REAL)
        return r < 0 ? r : -EROFS;
    if ((r = real_path(path, real)) < 0)
        return r;
    if (unlink(real) < 0)
        return -errno;
    pthread_rwlock_wrlock(&G.st->lock);
    r = 0;
    if (version_live(index_at(&G.st->idx, path, INT64_MAX)))
        r = store_append_locked(G.st, OP_UNLINK, path, 0, now_ns(), 0, NULL);
    pthread_rwlock_unlock(&G.st->lock);
    return r;
}

static int cfs_mkdir(const char *path, mode_t mode)
{
    struct vpath vp;
    char real[PATH_MAX];
    int r;

    if ((r = classify(path, &vp)) < 0 || vp.k != V_REAL)
        return -EROFS;
    if ((r = real_path(path, real)) < 0)
        return r;
    if (mkdir(real, mode) < 0)
        return -errno;
    return store_append(G.st, OP_MKDIR, path, S_IFDIR | (mode & 07777), now_ns(), 0, NULL);
}

static int cfs_rmdir(const char *path)
{
    struct vpath vp;
    char real[PATH_MAX];
    int r;

    if ((r = classify(path, &vp)) < 0 || vp.k != V_REAL)
        return r < 0 ? r : -EROFS;
    if ((r = real_path(path, real)) < 0)
        return r;
    if (rmdir(real) < 0)
        return -errno;
    return store_append(G.st, OP_RMDIR, path, 0, now_ns(), 0, NULL);
}

/*
 * Rename = "the old name stops existing, the new name starts existing with
 * the same content".  Data blocks are shared, so no file content is copied:
 * we just journal the existing manifest under the new path.
 */
static int cfs_rename(const char *from, const char *to, unsigned int flags)
{
    struct vpath a, b;
    char rfrom[PATH_MAX], rto[PATH_MAX], **olds = NULL;
    struct stat sb;
    size_t fl = strlen(from), n = 0, i;
    int r, need_snapshot = 0;

    if (classify(from, &a) < 0 || classify(to, &b) < 0 || a.k != V_REAL || b.k != V_REAL)
        return -EROFS;
    if (flags & ~RENAME_NOREPLACE)
        return -EINVAL;
    if ((r = real_path(from, rfrom)) < 0 || (r = real_path(to, rto)) < 0)
        return r;
    if ((flags & RENAME_NOREPLACE) && lstat(rto, &sb) == 0)
        return -EEXIST;
    if (rename(rfrom, rto) < 0)
        return -errno;
    if (lstat(rto, &sb) < 0)
        return 0;

    pthread_rwlock_wrlock(&G.st->lock);
    /* Collect "from" and everything below it that currently exists. */
    for (i = 0; i < G.st->idx.nall; i++) {
        struct pentry *p = G.st->idx.all[i];
        if (strncmp(p->path, from, fl) == 0 && (p->path[fl] == '\0' || p->path[fl] == '/') &&
            version_live(pentry_at(p, INT64_MAX))) {
            char **no = realloc(olds, (n + 1) * sizeof(*no));
            if (!no)
                break;
            olds = no;
            olds[n++] = strdup(p->path);
        }
    }
    for (i = 0; i < n; i++) {
        const struct version *v;
        struct version copy;
        char newp[JPATH_MAX];

        if (!olds[i])
            continue;
        v = index_at(&G.st->idx, olds[i], INT64_MAX);
        if (!v)
            continue;
        copy = *v;  /* index_add may realloc the version array */
        if (snprintf(newp, sizeof(newp), "%s%s", to, olds[i] + fl) >= (int)sizeof(newp))
            continue;
        store_append_locked(G.st, copy.op, newp, copy.mode, copy.mtime_ns, copy.size,
                            copy.manifest);
        store_append_locked(G.st, copy.op == OP_MKDIR ? OP_RMDIR : OP_UNLINK, olds[i],
                            0, now_ns(), 0, NULL);
    }
    if (S_ISDIR(sb.st_mode) && !version_live(index_at(&G.st->idx, to, INT64_MAX)))
        store_append_locked(G.st, OP_MKDIR, to, sb.st_mode, now_ns(), 0, NULL);
    if (S_ISREG(sb.st_mode) && n == 0)
        need_snapshot = 1;              /* never versioned before: do it now */
    pthread_rwlock_unlock(&G.st->lock);

    for (i = 0; i < n; i++)
        free(olds[i]);
    free(olds);
    if (need_snapshot)
        return record_version(to);
    return 0;
}

static int cfs_chmod(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    struct vpath vp;
    char real[PATH_MAX];
    struct stat sb;
    int r;

    (void)fi;
    if ((r = classify(path, &vp)) < 0 || vp.k != V_REAL)
        return r < 0 ? r : -EROFS;
    if ((r = real_path(path, real)) < 0)
        return r;
    if (chmod(real, mode) < 0)
        return -errno;
    if (lstat(real, &sb) == 0 && S_ISREG(sb.st_mode))
        return record_version(path);
    return 0;
}

static int cfs_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *fi)
{
    struct vpath vp;
    char real[PATH_MAX];
    int r;

    (void)fi;
    if ((r = classify(path, &vp)) < 0 || vp.k != V_REAL)
        return r < 0 ? r : -EROFS;
    if ((r = real_path(path, real)) < 0)
        return r;
    return lchown(real, uid, gid) < 0 ? -errno : 0;
}

static int cfs_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *fi)
{
    struct vpath vp;
    char real[PATH_MAX];
    struct stat sb;
    int r;

    if ((r = classify(path, &vp)) < 0 || vp.k != V_REAL)
        return r < 0 ? r : -EROFS;
    if ((r = real_path(path, real)) < 0)
        return r;
    if (utimensat(AT_FDCWD, real, tv, AT_SYMLINK_NOFOLLOW) < 0)
        return -errno;
    /* If the file is still open for writing, the version is taken on close. */
    if (fi && get_fh(fi)->dirty)
        return 0;
    if (lstat(real, &sb) == 0 && S_ISREG(sb.st_mode))
        return record_version(path);
    return 0;
}

static int cfs_access(const char *path, int mask)
{
    struct vpath vp;
    char real[PATH_MAX];
    int r;

    if ((r = classify(path, &vp)) < 0)
        return r;
    if (vp.k != V_REAL)
        return (mask & W_OK) ? -EROFS : 0;
    if ((r = real_path(path, real)) < 0)
        return r;
    return access(real, mask) < 0 ? -errno : 0;
}

static int cfs_statfs(const char *path, struct statvfs *sv)
{
    char real[PATH_MAX];
    (void)path;
    real_path("/", real);
    return statvfs(real, sv) < 0 ? -errno : 0;
}

static int cfs_symlink(const char *target, const char *link)
{
    struct vpath vp;
    char real[PATH_MAX];
    int r;

    if ((r = classify(link, &vp)) < 0 || vp.k != V_REAL)
        return -EROFS;
    if ((r = real_path(link, real)) < 0)
        return r;
    return symlink(target, real) < 0 ? -errno : 0;
}

static int cfs_readlink(const char *path, char *buf, size_t size)
{
    struct vpath vp;
    char real[PATH_MAX];
    ssize_t n;
    int r;

    if ((r = classify(path, &vp)) < 0)
        return r;
    if (vp.k != V_REAL)
        return -EINVAL;
    if ((r = real_path(path, real)) < 0)
        return r;
    n = readlink(real, buf, size - 1);
    if (n < 0)
        return -errno;
    buf[n] = '\0';
    return 0;
}

static const struct fuse_operations cfs_ops = {
    .init     = cfs_init,
    .destroy  = cfs_destroy,
    .getattr  = cfs_getattr,
    .readdir  = cfs_readdir,
    .open     = cfs_open,
    .create   = cfs_create,
    .read     = cfs_read,
    .write    = cfs_write,
    .flush    = cfs_flush,
    .release  = cfs_release,
    .fsync    = cfs_fsync,
    .truncate = cfs_truncate,
    .unlink   = cfs_unlink,
    .mkdir    = cfs_mkdir,
    .rmdir    = cfs_rmdir,
    .rename   = cfs_rename,
    .chmod    = cfs_chmod,
    .chown    = cfs_chown,
    .utimens  = cfs_utimens,
    .access   = cfs_access,
    .statfs   = cfs_statfs,
    .symlink  = cfs_symlink,
    .readlink = cfs_readlink,
};

/* ------------------------------------------------------------------ */
/* Startup: reconcile the journal with current/ (changes made offline) */
/* ------------------------------------------------------------------ */

static int reconcile_dir(const char *fpath, int *changes)
{
    char real[PATH_MAX], child[PATH_MAX];
    DIR *d;
    struct dirent *de;
    struct stat sb;
    int r;

    if ((r = real_path(fpath, real)) < 0)
        return r;
    if (!(d = opendir(real)))
        return -errno;
    while ((de = readdir(d))) {
        const struct version *v;
        int64_t mt;

        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (!strcmp(fpath, "/") && (!strcmp(de->d_name, SNAP_DIR) || !strcmp(de->d_name, CTL_DIR)))
            continue;
        snprintf(child, sizeof(child), "%s/%s", strcmp(fpath, "/") ? fpath : "", de->d_name);
        if (real_path(child, real) < 0 || lstat(real, &sb) < 0)
            continue;
        v = index_at(&G.st->idx, child, INT64_MAX);
        if (S_ISDIR(sb.st_mode)) {
            if (!v || v->op != OP_MKDIR) {
                r = store_append(G.st, OP_MKDIR, child, sb.st_mode, now_ns(), 0, NULL);
                if (r < 0) {
                    closedir(d);
                    return r;
                }
                (*changes)++;
            }
            if ((r = reconcile_dir(child, changes)) < 0) {
                closedir(d);
                return r;
            }
        } else if (S_ISREG(sb.st_mode)) {
            mt = (int64_t)sb.st_mtim.tv_sec * NS_PER_SEC + sb.st_mtim.tv_nsec;
            if (!v || v->op != OP_WRITE || v->size != (uint64_t)sb.st_size ||
                v->mtime_ns != mt || v->mode != sb.st_mode) {
                size_t before = G.st->idx.nrecords;
                if ((r = bs_snapshot_file(G.st, child)) < 0) {
                    closedir(d);
                    return r;
                }
                if (G.st->idx.nrecords != before)
                    (*changes)++;
            }
        }
    }
    closedir(d);
    return 0;
}

static int reconcile(void)
{
    int changes = 0, r;
    size_t i, n;
    char real[PATH_MAX];
    struct stat sb;

    if ((r = reconcile_dir("/", &changes)) < 0)
        return r;

    /* Paths the journal thinks exist but which are gone from current/. */
    n = G.st->idx.nall;
    for (i = 0; i < n; i++) {
        struct pentry *p = G.st->idx.all[i];
        const struct version *v = pentry_at(p, INT64_MAX);
        char path[JPATH_MAX];
        if (!version_live(v))
            continue;
        snprintf(path, sizeof(path), "%s", p->path);
        if (real_path(path, real) < 0)
            continue;
        if (lstat(real, &sb) < 0 ||
            (v->op == OP_MKDIR) != (S_ISDIR(sb.st_mode) != 0)) {
            r = store_append(G.st, v->op == OP_MKDIR ? OP_RMDIR : OP_UNLINK, path, 0, now_ns(), 0, NULL);
            if (r < 0)
                return r;
            changes++;
        }
    }
    return changes;
}

int fs_main(struct store *st, const char *mountpoint, int argc, char **argv, size_t cache_blocks)
{
    int changes, r;

    G.st = st;
    G.mounted_at = now_ns();
    if (!realpath(mountpoint, G.mountpoint))
        snprintf(G.mountpoint, sizeof(G.mountpoint), "%s", mountpoint);
    G.cache = lru_create(cache_blocks);
    if (!G.cache) {
        fprintf(stderr, "chronofs: cannot allocate block cache\n");
        return 1;
    }

    changes = reconcile();
    if (changes < 0) {
        fprintf(stderr, "chronofs: reconciliation failed: %s\n", strerror(-changes));
        lru_destroy(G.cache);
        return 1;
    }
    if (changes)
        printf("chronofs: recorded %d change(s) made while unmounted\n", changes);
    printf("chronofs: %s mounted on %s (%zu versions in journal)\n",
           st->root, G.mountpoint, st->idx.nrecords);
    fflush(stdout);

    r = fuse_main(argc, argv, &cfs_ops, NULL);
    lru_destroy(G.cache);
    return r;
}
