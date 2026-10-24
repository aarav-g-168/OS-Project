/*
 * main.c - the `chronofs` command line tool.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>
#include "chronofs.h"

static void usage(FILE *f)
{
    fprintf(f,
        "ChronoFS %s - a time-travel filesystem (FUSE)\n"
        "\n"
        "usage: chronofs <command> [args]\n"
        "\n"
        "  mount   <store> <mountpoint> [-f] [-d] [--cache-blocks N] [-o opts]\n"
        "              mount the store; every write is versioned.\n"
        "              browse history at <mountpoint>/.snapshots/<when>/\n"
        "  umount  <mountpoint>                  unmount\n"
        "  tui     <store> [--at WHEN]           interactive timeline browser\n"
        "  log     <store> [path] [-n N]         show version history\n"
        "  ls      <store> [dir] [--at WHEN]     list a directory as it was\n"
        "  cat     <store> <path> [--at WHEN]    print a file as it was\n"
        "  restore <store> <path> --at WHEN [-o dest]\n"
        "              bring back a file or directory from the past\n"
        "  tag     <store> [name [--at WHEN]]    name a moment (or list tags)\n"
        "  stats   <store>                       storage and dedup statistics\n"
        "  gc      <store> --keep DURATION [--dry-run]\n"
        "              drop history older than DURATION and free unused blocks\n"
        "\n"
        "WHEN is one of:  now | 30s-ago | 2min-ago | 1h30min-ago | 1d-ago\n"
        "                 @19:53:12 | @2026-10-05T19:53:12 | @<epoch> | <tag>\n"
        "\n"
        "examples:\n"
        "  chronofs mount ~/store ~/mnt\n"
        "  ls -t ~/mnt/.snapshots/2min-ago/\n"
        "  cat ~/mnt/.snapshots/@19:53:12/notes.txt\n"
        "  chronofs restore ~/store notes.txt --at 5min-ago\n",
        CHRONOFS_VERSION);
}

static int cmd_mount(int argc, char **argv)
{
    struct store st;
    char *fargv[32], lockp[PATH_MAX];
    int fargc = 0, i, r, lockfd;
    size_t cache_blocks = 1024;         /* 1024 x 4 KiB = 4 MiB */
    const char *store_dir = NULL, *mnt = NULL;

    fargv[fargc++] = "chronofs";
    fargv[fargc++] = "-o";
    fargv[fargc++] = "fsname=chronofs,subtype=chronofs,default_permissions";
    for (i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--cache-blocks") && i + 1 < argc) {
            char *end;
            unsigned long parsed;
            errno = 0;
            parsed = strtoul(argv[++i], &end, 10);
            if (errno || !argv[i][0] || argv[i][0] == '-' || *end ||
                parsed == 0 || parsed > INT_MAX / 2 || parsed > SIZE_MAX / (BLOCK_SIZE * 2)) {
                fprintf(stderr, "chronofs: --cache-blocks requires a positive, bounded integer\n");
                return 2;
            }
            cache_blocks = (size_t)parsed;
        } else if ((!strcmp(argv[i], "-f") || !strcmp(argv[i], "-d") || !strcmp(argv[i], "-s"))
                   && fargc < 28) {
            fargv[fargc++] = argv[i];
        } else if (!strcmp(argv[i], "-o") && i + 1 < argc && fargc < 27) {
            fargv[fargc++] = argv[i];
            fargv[fargc++] = argv[++i];
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "chronofs: unknown mount option %s\n", argv[i]);
            return 2;
        } else if (!store_dir) {
            store_dir = argv[i];
        } else if (!mnt) {
            mnt = argv[i];
        } else {
            fprintf(stderr, "chronofs: too many arguments\n");
            return 2;
        }
    }
    if (!store_dir || !mnt) {
        fprintf(stderr, "usage: chronofs mount <store> <mountpoint> [-f] [-d] "
                        "[--cache-blocks N] [-o opts]\n");
        return 2;
    }
    if (access(mnt, F_OK) < 0) {
        fprintf(stderr, "chronofs: mountpoint %s does not exist\n", mnt);
        return 1;
    }
    if ((r = store_open(&st, store_dir, 1)) < 0) {
        fprintf(stderr, "chronofs: cannot open store %s: %s\n", store_dir, strerror(-r));
        return 1;
    }

    /* One daemon per store.  The lock is inherited by the daemon after fork. */
    store_path(&st, lockp, sizeof(lockp), "lock");
    lockfd = open(lockp, O_RDWR | O_CREAT, 0644);
    if (lockfd < 0 || flock(lockfd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "chronofs: %s is already mounted\n", st.root);
        if (lockfd >= 0)
            close(lockfd);
        store_close(&st);
        return 1;
    }
    st.lockfd = lockfd;

    fargv[fargc++] = (char *)mnt;
    fargv[fargc] = NULL;
    r = fs_main(&st, mnt, fargc, fargv, cache_blocks);
    store_close(&st);
    return r;
}

static int cmd_umount(int argc, char **argv)
{
    if (argc < 1) {
        fprintf(stderr, "usage: chronofs umount <mountpoint>\n");
        return 2;
    }
    execlp("fusermount3", "fusermount3", "-u", argv[0], (char *)NULL);
    execlp("fusermount", "fusermount", "-u", argv[0], (char *)NULL);
    execlp("umount", "umount", argv[0], (char *)NULL);
    perror("chronofs: umount");
    return 1;
}

int main(int argc, char **argv)
{
    static const struct {
        const char *name;
        int (*fn)(int, char **);
    } cmds[] = {
        { "mount", cmd_mount },   { "umount", cmd_umount }, { "unmount", cmd_umount },
        { "tui", cmd_tui },       { "log", cmd_log },       { "ls", cmd_ls },
        { "cat", cmd_cat },
        { "restore", cmd_restore }, { "tag", cmd_tag },     { "stats", cmd_stats },
        { "gc", cmd_gc },
    };
    size_t i;

    if (argc < 2) {
        usage(stderr);
        return 2;
    }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help") || !strcmp(argv[1], "help")) {
        usage(stdout);
        return 0;
    }
    if (!strcmp(argv[1], "-V") || !strcmp(argv[1], "--version")) {
        printf("chronofs %s\n", CHRONOFS_VERSION);
        return 0;
    }
    for (i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        if (!strcmp(argv[1], cmds[i].name))
            return cmds[i].fn(argc - 2, argv + 2);
    }
    fprintf(stderr, "chronofs: unknown command '%s' (try: chronofs help)\n", argv[1]);
    return 2;
}
