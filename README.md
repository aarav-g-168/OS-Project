# ChronoFS — a time-travel filesystem in C (FUSE)

ChronoFS is a userspace filesystem. It **versions every write**. It mounts as a normal
directory. The past is always one path away:

```bash
ls -t  mnt/.snapshots/2min-ago/
cat    mnt/.snapshots/@19:53:12/notes.txt
cp -r  mnt/.snapshots/before-disaster/src  mnt/
```

ChronoFS uses [libfuse 3](https://github.com/libfuse/libfuse). It stores data in a
content-addressed block store with deduplication. It writes an append-only journal. It
caches blocks in an LRU cache. It reclaims space with mark-and-sweep garbage collection.
The core is plain C and needs only libfuse. The optional timeline browser also needs ncurses.

## Quick start

FUSE needs a Linux kernel and a usable `/dev/fuse` device with permission to mount.
On macOS or Windows, use a Linux VM with FUSE support. Docker requires a Linux host
or VM that exposes `/dev/fuse`; a container alone cannot supply the missing device.

```bash
./scripts/dev.sh            # opens a Linux shell with FUSE enabled, repo at /src
make                        # builds ./chronofs
./scripts/demo.sh           # scripted end-to-end demo (add --pause to step through)
./scripts/demo.sh --interactive   # same demo, then browse the history in a TUI
```

On a Linux machine, you can skip Docker. Install `libfuse3-dev fuse3 pkg-config build-essential`.
Then run `make`.

On Debian/Ubuntu:

```bash
sudo apt-get update
sudo apt-get install build-essential pkg-config libfuse3-dev fuse3 libncurses-dev python3
make
make test                    # core, CLI, FUSE callback and terminal regressions
make test-mount              # additionally requires /dev/fuse and mount permission
```

The automated suite works without a mounted filesystem. GitHub Actions builds and
tests both with and without the optional ncurses browser. The mounted test uses a
fresh temporary directory and removes it after unmounting. The demo also uses a
unique temporary directory, but leaves its store for further exploration.

See `docs/PROJECT_REPORT.md` for the implementation report and `docs/TESTING.md`
for verification details and commands.

The interactive timeline browser (`chronofs tui`) is optional. It uses ncurses. If the build
finds `ncursesw`, it compiles the browser in. If not, `make` builds everything else, and
`chronofs tui` tells you how to enable the browser. Install `libncurses-dev` (Debian/Ubuntu)
or `ncurses-devel` (Fedora). Then run `make clean && make`.

## Using it

```bash
mkdir -p /tmp/store /tmp/mnt
./chronofs mount /tmp/store /tmp/mnt      # -f to stay in the foreground, -d for FUSE debug

cd /tmp/mnt
echo "v1" > notes.txt
sleep 60
echo "v2" > notes.txt

cat .snapshots/1min-ago/notes.txt         # -> v1
ls -lt .snapshots/                        # every moment something changed
cat .chronofs/stats                       # live cache / version statistics

./chronofs umount /tmp/mnt
```

### Naming a point in time

You can use a time in two places: `.snapshots/<WHEN>/` and `--at <WHEN>`.

| Form | Example | Meaning |
|---|---|---|
| relative | `30s-ago`, `2min-ago`, `1h30min-ago`, `1d-ago` | that long before now |
| clock time | `@19:53:12`, `@19:53` | today, local time (end of that second/minute) |
| date + time | `@2026-10-05T19:53:12` | absolute local time |
| epoch | `@1759674000` | Unix seconds |
| tag | `before-disaster` | a moment named with `chronofs tag` |
| `now` | `now` | the current state, read-only |

### Commands

| Command | What it does |
|---|---|
| `chronofs mount <store> <mnt> [-f] [-d] [--cache-blocks N]` | Mount the store. History appears under `<mnt>/.snapshots/`. |
| `chronofs umount <mnt>` | Unmount the store. |
| `chronofs tui <store> [--at WHEN]` | Open the interactive timeline browser (ncurses, optional). |
| `chronofs log <store> [path] [-n N]` | Show the version history (writes, deletes, mkdirs). |
| `chronofs ls <store> [dir] --at WHEN` | List a directory as it was, without a mount. |
| `chronofs cat <store> <path> --at WHEN` | Print a file as it was. |
| `chronofs restore <store> <path> --at WHEN [-o dest]` | Restore a file or a whole directory. If the store is mounted, the restore uses the mount. The restore then becomes a new version too. |
| `chronofs tag <store> [name] [--at WHEN]` | Name a moment, or list the tags. |
| `chronofs stats <store>` | Show the versions, the blocks on disk, and the dedup ratio. |
| `chronofs gc <store> --keep 1h [--dry-run]` | Remove history older than 1h. Free the unreferenced blocks. The store must be unmounted. |

### Interactive timeline browser

`chronofs tui <store>` opens a browser with three panes. The browser uses ncurses and reads
the store directly. It does not need a mount. Move the timeline cursor to rewind the tree and
the preview to that instant:

```
+---------------------------------------------------------------+
| Timeline: every journal change, newest first                  |
+--------------------------+------------------------------------+
| Tree at the chosen moment| Preview of the highlighted file    |
+--------------------------+------------------------------------+
| hints / messages                                              |
+---------------------------------------------------------------+
```

| Key | Action |
|---|---|
| Up / Down | move in the focused pane (timeline rewinds the tree) |
| PgUp / PgDn | page (scrolls the preview when the tree pane is focused) |
| Tab | switch focus: timeline ↔ tree |
| Enter / Backspace | open a directory / go to the parent |
| Home / End | newest / oldest moment |
| `n` | jump to the live tree |
| `t` | tag the selected moment |
| `r` | restore the highlighted file or directory at that moment |
| `/` | filter the tree by substring |
| `?` | help · `R` reload from disk · `q` quit |

If the store is mounted, a restore uses the mount. The restore then becomes a new version
too. If the store is not mounted, a restore writes into `current/`, and the journal records
the new version.

## How it works

```
 user process ──syscall──▶ Linux VFS ──▶ fuse.ko ──/dev/fuse──▶ chronofs daemon (libfuse, multithreaded)
                                                                  │
                         normal paths ─ passthrough ──────────────┼──▶ <store>/current/
                         on close/fsync of a written file ────────┼──▶ split into 4 KiB blocks
                                                                  │      SHA-256 each → objects/ab/cd…  (stored once)
                                                                  │      block list → manifests/…       (index block)
                                                                  │      append record → journal.log
                         .snapshots/<WHEN>/path ──────────────────┴──▶ index lookup at time T
                                                                         → manifest → blocks (via LRU cache)
```

- **Store layout:** `current/` holds the live files. `objects/` holds the deduplicated data
  blocks. `manifests/` holds the block list of each version. `journal.log` is an append-only
  array of fixed-size 1096-byte records.
- **When versions are taken:** at `close()` (that is, `flush`), after a write to the file.
  Also on `fsync()`, and after `truncate`, `chmod`, and `utime`. A version is taken for each
  write session, not for each `write()` syscall. A 1 MB `cp` makes about 8 kernel write
  requests. This rule keeps those 8 requests from becoming 8 versions.
- **Deletes and renames** are journaled too. A delete is a tombstone. A rename records the
  new path and the same manifest, so the rename copies no data. Past snapshots therefore
  still show deleted and moved files.
- **The in-memory index** is a hash table. It maps each path to its versions, sorted by time.
  A lookup at time T is a hash lookup and a binary search. A `pthread_rwlock_t` protects the
  index, because libfuse serves requests from several threads.
- **Error handling:** the code writes and syncs each new object to a temp file, then renames it.
  Reads verify block and manifest hashes. Each journal record is a single `O_APPEND`
  `write()`. A malformed or incomplete journal is rejected rather than silently losing
  records; back up the store before attempting manual recovery. At mount time, the code reconciles
  the journal with `current/`. The code records anything that changed while the store was
  unmounted. This step works like a small fsck.
- **One daemon per store:** `flock()` enforces this limit. `gc` refuses to run while the
  store is mounted.

## Source map

| File | Purpose |
|---|---|
| `src/main.c` | CLI entry point and the `mount`/`umount` subcommands |
| `src/fs.c` | all FUSE callbacks, the `.snapshots/` and `.chronofs/` virtual trees, and reconciliation at mount time |
| `src/journal.c` | journal file, path→versions hash index, store open/close |
| `src/blockstore.c` | block and manifest storage, file snapshotting, reading old versions |
| `src/lru.c` | LRU cache of 4 KiB blocks (hash map + doubly linked list) |
| `src/timeparse.c` | `2min-ago` / `@19:53:12` / tag parsing, formatting helpers |
| `src/gc.c` | retention plus mark-and-sweep garbage collection |
| `src/cmd.c` | `log`, `ls`, `cat`, `restore`, `tag`, `stats` |
| `src/tui.c` | optional ncurses timeline browser (`chronofs tui`) |
| `src/sha256.c` | self-contained SHA-256 |

## Limitations

- ChronoFS passes symlinks through, but does not version them. It does not support hard links.
- A version is the content of the file at close or fsync. A writer that runs for a long time
  and never closes or fsyncs the file produces no intermediate versions.
- Each snapshot rehashes the whole file. Only *new* blocks are written. An edit in the middle
  of a file shifts every later block, so fixed-size blocks deduplicate those blocks poorly.
  Content-defined chunking would fix this.
- A snapshot directory listing scans every tracked path. The cost is O(paths). This cost is
  acceptable for project-sized trees.
- This is an educational filesystem for trusted, local users. It is not a security
  sandbox or a substitute for backups. Symlinks can lead outside the store.
- Use one writer per file at a time. Concurrent writes to the same file are not
  transactional snapshots. Do not run offline restore, edit `current/`, or run GC
  concurrently with other offline writers. GC refuses to run while mounted.
- File restores recreate file contents and permissions; they do not restore ownership,
  extended attributes, original timestamps, or empty directories.
- Object syncing and journal validation improve failure handling, but the store is not
  a fully transactional, power-loss-proof filesystem. Journal records use native C
  layout and byte order, so stores are not a portable interchange format.
