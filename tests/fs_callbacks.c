/* Exercise real FUSE handlers without requiring a kernel mount. */
#include <assert.h>
#include "../src/fs.c"

int main(int argc, char **argv)
{
    struct store st;
    struct fuse_file_info live = {.flags = O_RDWR}, historical = {.flags = O_RDONLY};
    struct fuse_file_info denied = {.flags = O_WRONLY};
    struct version v;
    char tagpath[PATH_MAX], buf[32];
    FILE *tags;
    int saved_fd;
    int64_t first;
    assert(argc == 2 && store_open(&st, argv[1], 1) == 0);
    G.st = &st;
    G.cache = lru_create(4);
    assert(G.cache);
    assert(cfs_create("/file", 0644, &live) == 0);
    /* Shell dup/close before writing must not create a spurious empty version. */
    assert(cfs_flush("/file", &live) == 0 && st.idx.nrecords == 0);
    assert(cfs_write("/file", "first", 5, 0, &live) == 5);
    assert(cfs_flush("/file", &live) == 0 && st.idx.nrecords == 1);
    first = st.last_ts;
    assert(cfs_release("/file", &live) == 0);
    assert(snap_lookup(first, "/file", &v) == SNAP_FILE && v.size == 5);
    snprintf(tagpath, sizeof(tagpath), "%s/tags", argv[1]);
    tags = fopen(tagpath, "w");
    assert(tags);
    fprintf(tags, "first %lld\n", (long long)first);
    assert(fclose(tags) == 0);
    assert(cfs_open("/.snapshots/first/file", &historical) == 0);
    assert(cfs_read("/.snapshots/first/file", buf, sizeof(buf), 0, &historical) == 5);
    assert(!memcmp(buf, "first", 5));
    assert(cfs_release("/.snapshots/first/file", &historical) == 0);
    assert(cfs_open("/.snapshots/first/file", &denied) == -EROFS);
    assert(cfs_unlink("/.snapshots/first/file") == -EROFS);
    assert(cfs_mkdir("/.snapshots/first/dir", 0755) == -EROFS);

    live.flags = O_RDWR;
    assert(cfs_open("/file", &live) == 0);
    assert(cfs_write("/file", "later", 5, 0, &live) == 5);
    saved_fd = st.jfd;
    st.jfd = -1;
    assert(cfs_flush("/file", &live) == -EBADF);
    assert(get_fh(&live)->wrote); /* failure must remain retryable */
    st.jfd = saved_fd;
    assert(cfs_fsync("/file", 0, &live) == 0);
    assert(cfs_release("/file", &live) == 0);
    assert(cfs_rename("/file", "/renamed", 0) == 0);
    assert(snap_lookup(INT64_MAX, "/file", &v) == SNAP_NONE);
    assert(snap_lookup(INT64_MAX, "/renamed", &v) == SNAP_FILE);
    assert(snap_lookup(first, "/file", &v) == SNAP_FILE);
    assert(cfs_unlink("/renamed") == 0);
    assert(snap_lookup(INT64_MAX, "/renamed", &v) == SNAP_NONE);
    assert(cfs_mkdir("/dir", 0755) == 0);
    assert(cfs_rmdir("/dir") == 0);
    assert(cfs_chmod("/", 0755, NULL) == 0);
    lru_destroy(G.cache);
    store_close(&st);
    puts("PASS: FUSE callbacks, write sessions, read-only history, snapshot errors, rename, deletion");
    return 0;
}
