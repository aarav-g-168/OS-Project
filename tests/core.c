#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "chronofs.h"

static void write_file(const char *path, const void *data, size_t n)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    assert(fd >= 0);
    assert(write(fd, data, n) == (ssize_t)n);
    assert(close(fd) == 0);
}

int main(int argc, char **argv)
{
    struct store st, writer;
    struct lru *cache;
    struct lru_stats stats;
    char path[PATH_MAX], hex[65], normalized[32], data[BLOCK_SIZE * 2 + 17];
    uint8_t hash[HASH_LEN], key[HASH_LEN] = {1}, second[HASH_LEN] = {2};
    uint8_t (*blocks)[HASH_LEN];
    uint64_t size;
    size_t n, len;
    int64_t duration, t;
    int fd;

    /* Small fixture writer for CLI integration tests. */
    if (argc == 4 && !strcmp(argv[1], "snapshot")) {
        int r = store_open(&st, argv[2], 1);
        if (r < 0)
            return 1;
        r = bs_snapshot_file(&st, argv[3]);
        store_close(&st);
        return r < 0;
    }
    assert(argc == 2);
    sha256("abc", 3, hash);
    hash_to_hex(hash, hex);
    assert(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    sha256("", 0, hash);
    hash_to_hex(hash, hex);
    assert(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    assert(normalize_path("//a/./b//", normalized, sizeof(normalized)) == 0);
    assert(!strcmp(normalized, "/a/b"));
    assert(normalize_path("a/../outside", normalized, sizeof(normalized)) < 0);
    assert(normalize_path("long", normalized, 3) < 0);
    assert(parse_duration("1h30min", &duration) == 0 && duration == 5400 * NS_PER_SEC);
    assert(parse_duration("1000000000w", &duration) < 0);
    assert(parse_duration("-1s", &duration) < 0);
    assert(parse_when("@999999999999999999999", 0, &t) < 0);
    assert(parse_when("@2026-02-30", 0, &t) < 0);
    assert(parse_when("@2024-02-29", 0, &t) == 0);
    assert(parse_when("2min-ago", 200 * NS_PER_SEC, &t) == 0 && t == 80 * NS_PER_SEC);

    cache = lru_create(1);
    assert(cache);
    lru_put(cache, key, "one", 3);
    assert(lru_get(cache, key, data, &len) && len == 3 && !memcmp(data, "one", 3));
    lru_put(cache, second, "two", 3);
    assert(!lru_get(cache, key, data, &len));
    lru_get_stats(cache, &stats);
    assert(stats.hits == 1 && stats.misses == 1 && stats.evictions == 1);
    lru_destroy(cache);

    assert(store_open(&st, argv[1], 1) == 0);
    snprintf(path, sizeof(path), "%s/current/file", argv[1]);
    memset(data, 'A', sizeof(data));
    write_file(path, data, sizeof(data));
    assert(bs_snapshot_file(&st, "/file") == 0);
    assert(st.idx.nrecords == 1);
    assert(bs_snapshot_file(&st, "/file") == 0 && st.idx.nrecords == 1);
    t = st.last_ts;
    assert(store_real_path(&st, path, sizeof(path), "/../outside") < 0);
    assert(store_open(&writer, argv[1], 0) == 0);
    assert(store_append(&writer, OP_UNLINK, "/file", 0, now_ns(), 0, NULL) == 0);
    store_close(&writer);
    assert(version_live(index_at(&st.idx, "/file", INT64_MAX)));
    assert(store_reload(&st) == 0);
    assert(!version_live(index_at(&st.idx, "/file", INT64_MAX)));
    assert(version_live(index_at(&st.idx, "/file", t)));

    memcpy(hash, index_at(&st.idx, "/file", t)->manifest, HASH_LEN);
    assert(bs_get_manifest(&st, hash, &size, &blocks, &n) == 0);
    assert(size == sizeof(data) && n == 3);
    assert(bs_read_version(&st, NULL, size, (const uint8_t (*)[HASH_LEN])blocks,
                           n, data, 20, BLOCK_SIZE - 5) == 20);
    assert(!memcmp(data, "AAAAAAAAAAAAAAAAAAAA", 20));
    assert(bs_object_path(&st, "objects", blocks[0], path, sizeof(path)) == 0);
    write_file(path, "damaged", 7);
    assert(bs_get_block(&st, blocks[0], data, &len) == -EIO);
    free(blocks);
    assert(bs_object_path(&st, "manifests", hash, path, sizeof(path)) == 0);
    write_file(path, "damaged", 7);
    assert(bs_get_manifest(&st, hash, &size, &blocks, &n) == -EIO);
    /* A failed reload must preserve the prior usable in-memory index. */
    snprintf(path, sizeof(path), "%s/journal.log", argv[1]);
    fd = open(path, O_WRONLY | O_APPEND);
    assert(fd >= 0 && write(fd, "x", 1) == 1);
    close(fd);
    assert(store_reload(&st) == -EIO && st.idx.nrecords == 2);
    store_close(&st);
    assert(store_open(&st, argv[1], 0) == -EIO);
    fd = open(path, O_WRONLY);
    assert(fd >= 0 && ftruncate(fd, 2 * sizeof(struct jrec)) == 0);
    assert(write(fd, "x", 1) == 1);
    close(fd);
    assert(store_open(&st, argv[1], 0) == -EIO);
    puts("PASS: hashing, paths, time bounds, LRU, snapshots, historical lookup, reload, corruption");
    return 0;
}
