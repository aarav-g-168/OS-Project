# ChronoFS — Project Report Outline

This is the original planning outline. Its sample checkmarks and measurements
are illustrative and are not evidence of verification in the current environment.
Use `PROJECT_REPORT.md` for the implementation report and `TESTING.md` for
repeatable checks and the remaining mounted-validation requirement.

**Course:** BITE303P Operating Systems Lab
**Title:** ChronoFS: A Userspace Time-Travel Filesystem with Transparent Versioning (FUSE)

Suggested length is 15–25 pages. Each section lists what to write and which files or demo steps
back it up.

---

## 1. Abstract (½ page)
- The problem: ordinary filesystems overwrite data in place, so an accidental `rm` or a bad save is permanent.
- The solution: a FUSE filesystem that versions every write session and exposes the past as ordinary
  directories (`.snapshots/2min-ago/`), with deduplicated block storage, an LRU cache and garbage collection.
- The result: a working C implementation (~3,500 lines), tested on Linux (Ubuntu 24.04, libfuse 3.14).

## 2. Introduction (1–2 pages)
- Motivation: undo for the whole filesystem, like Time Machine, ZFS/Btrfs snapshots, NILFS, or Plan 9's Venti/Fossil.
- Why FUSE: you write the filesystem as a normal process, with no kernel module, but still go through the real VFS.
- Objectives:
  1. Transparent versioning, so no application changes are needed.
  2. Time-based browsing using paths.
  3. Space-efficient storage.
  4. Bounded memory caching.
  5. Retention / reclamation.
- Scope and limitations (see README).

## 3. Background (2–3 pages)
- **The VFS layer and FUSE architecture:** a user process makes a syscall, it goes VFS → `fuse.ko` → `/dev/fuse` → the
  libfuse daemon → back. Draw the diagram from the README. Discuss the cost of the user/kernel context switches.
- **File system concepts:** inodes vs paths, directories, metadata (`stat`), the open-file lifecycle
  (`open` → `write`* → `flush` on every `close` → `release`).
- **Copy-on-write and content addressing:** hashing blocks with SHA-256 and why identical blocks dedup.
- **Journaling:** append-only logs and why a single `O_APPEND` write is effectively atomic.
- **Related work:** ZFS/Btrfs snapshots, NILFS2 (log-structured with continuous snapshots), Git's
  object store, rsnapshot, Apple Time Machine.

## 4. System Design (4–5 pages)
### 4.1 Architecture
- Components: CLI (`main.c`, `cmd.c`), FUSE layer (`fs.c`), journal and index (`journal.c`),
  block store (`blockstore.c`), cache (`lru.c`), GC (`gc.c`).
- The on-disk layout of a store (`current/`, `objects/`, `manifests/`, `journal.log`, `tags`, `lock`).

### 4.2 Data structures (include the C structs)
- `struct jrec`: the fixed 1096-byte journal record, and why fixed-size records make recovery easy.
- `struct version` / `struct pentry` / `struct index`: a chained hash table of path → time-sorted versions.
- The manifest format: header plus an array of 32-byte block hashes.
- The LRU: a node array (frames) + hash buckets + doubly linked recency list.

### 4.3 Key algorithms
- **Snapshot on close:** read the file in 4 KiB blocks → hash each → store it if new → write the manifest →
  append to the journal. Skip the version if the manifest, mode and mtime are all unchanged.
- **Time lookup:** binary search for the latest version with `ts ≤ T`. A tombstone means "absent".
- **Snapshot directory listing:** collect the live children at T; directories can be implied by the files inside them.
- **Rename:** journal the existing manifest under the new path and tombstone the old one, so no data is copied.
- **Garbage collection:** retention cutoff → keep the base state → mark manifests and blocks → sweep → atomically
  rewrite the journal.
- **Mount-time reconciliation:** walk `current/` and diff it against the index.

### 4.4 Design decisions & trade-offs
- Per-session versions, not per-`write()`: the kernel splits writes into ≤128 KiB requests,
  so per-call versions would be meaningless and huge.
- A trap we hit: a shell's `> file` `dup()`s and closes the fd before writing, which would record an empty
  version on `flush`. So versions on flush happen only after a real `write()`; creates and truncates are recorded on `release`.
- Kernel caching is turned off (`entry/attr_timeout = 0`), because `2min-ago` means a different moment on every access.
- Fixed-size blocks vs content-defined chunking.
- SHA-256 for naming blocks, and why collisions are not a practical concern.

## 5. Mapping to the OS Lab syllabus (1–2 pages, important for evaluation)

| Syllabus experiment | Where it appears in ChronoFS |
|---|---|
| **Exp 1** – Linux commands, `/proc` | `mount`, `ls -t`, `stat`, `/proc/self/mountinfo`, `fusermount3`, monitoring the daemon with `ps`/`top` |
| **Exp 2** – system calls (`open`, `read`, `write`, `close`, `stat`, `opendir`, `readdir`, `lseek`/`pread`, `fcntl`) | every FUSE callback in `src/fs.c` is a thin wrapper over these. Also used: `flock`, `fsync`, `rename`, `mkstemp`, `utimensat` |
| **Exp 2** – `fork`/`exec`/`wait` | libfuse daemonizes with `fork()` (the parent exits, the child serves). `chronofs umount` uses `execlp` to run `fusermount3` |
| **Exp 3** – IPC | the kernel ↔ daemon protocol over the `/dev/fuse` character device. Shared state between the daemon and the CLI goes through the journal file and an advisory lock |
| **Exp 4** – Pthreads | libfuse's multithreaded loop serves concurrent requests on a thread pool |
| **Exp 6** – synchronization | `pthread_rwlock_t` on the index (many readers, one writer), `pthread_mutex_t` in the LRU cache and statistics, `flock()` so only one daemon runs per store. Compare these with the semaphore solutions to readers–writers |
| **Exp 8** – memory allocation | a fixed pool of cache frames, analogous to fixed partitions. Dynamic growth (`realloc` doubling) of version arrays |
| **Exp 9** – page replacement (FIFO/LRU/LFU) | `src/lru.c` is LRU replacement for disk blocks. Show the hits/misses/evictions from `.chronofs/stats`, and the sequential-flooding weakness (§7) |
| **Exp 10** – disk scheduling | reads go block by block in offset order. Discuss how the kernel's readahead and I/O scheduler sit below FUSE |
| **Exp 11** – file allocation (sequential / indexed / linked) | each version's **manifest is an index block** (indexed allocation). Blocks are scattered across `objects/` and located only through the manifest. Contrast with contiguous and linked allocation |
| Course outcome 3 – gcc & kernel structure | built with gcc/Make. FUSE shows the VFS boundary between userspace and the kernel module |

## 6. Implementation (3–4 pages)
- Build environment: Docker (Ubuntu 24.04) on macOS, `--device /dev/fuse --cap-add SYS_ADMIN`.
- Walk through the important functions, with short code excerpts:
  - `cfs_flush` / `record_version` / `bs_snapshot_file`
  - `classify()`: how a path like `/.snapshots/2min-ago/a/b` is decoded
  - `snap_lookup` and `readdir_snapshot`
  - `cfs_rename` (directory rename as journal rewrite)
  - `lru_get` / `lru_put`
  - `cmd_gc` (mark and sweep)
- Error handling: negative-errno returns (`-ENOENT`, `-EROFS`); write-to-temp-then-rename.

## 7. Testing & Results (3–4 pages)
Use the output of `./scripts/demo.sh`, with screenshots of each step.

| Test | Expected | Observed |
|---|---|---|
| write v1, v2, v3; read `.snapshots/@T1/` | v1 | ✔ |
| `ls -lt .snapshots/now/` | sorted by the historical mtime | ✔ |
| `rm -rf` then `cp -r .snapshots/<tag>/…` | files recovered | ✔ |
| write into `.snapshots/` | `EROFS` | ✔ |
| change 1 byte in a 2 MiB file | +1 block stored, dedup 2.00x | ✔ (516 → 517 blocks) |
| read an old version 3× | cache hit ratio ≈ 66% | ✔ (1026 hits / 515 misses) |
| 8 parallel writers × 20 writes | 160 versions, no corruption | ✔ |
| edit `current/` while unmounted, then remount | changes picked up | ✔ |
| `gc --keep 5s` | unreachable blocks freed, recent history intact | ✔ |
| AddressSanitizer + UBSan build | no errors or leaks | ✔ |

- Measurements you can add:
  - Throughput of `dd` to ChronoFS vs. directly to disk, which shows FUSE overhead.
  - Dedup ratio as the number of edits grows.
  - Cache hit ratio for cache sizes 64/256/1024 blocks (`--cache-blocks`).
- **LRU sequential flooding** (measured with `--cache-blocks 100`, each file read 3 times):

  | file size | hits | misses | evictions |
  |---|---|---|---|
  | 99 blocks (fits) | 198 | 99 | 0 |
  | 101 blocks (one block too big) | **0** | 303 | 203 |

  Exceeding the cache by a single block takes the hit ratio from 67% to 0%: LRU evicts each block just
  before it's needed again. This is a good comparison with FIFO/LFU/optimal from Exp 9.

## 8. Conclusion & Future Work (1 page)
- Content-defined chunking (Rabin fingerprints) to dedup insertions.
- Hard-link and symlink versioning, and xattrs.
- Compression of blocks (zlib/zstd).
- Incremental hashing: rehash only the blocks dirtied by `write()`.
- Retention policies like "hourly for a day, daily for a month".
- A persistent index or checkpoint, so mount doesn't replay the whole journal.

## 9. References
- libfuse documentation and examples (`passthrough.c`): github.com/libfuse/libfuse
- Vangoor, Tarasov, Zadok, "To FUSE or Not to FUSE: Performance of User-Space File Systems", USENIX FAST 2017.
- Quinlan & Dorward, "Venti: a new approach to archival storage", USENIX FAST 2002.
- Konishi et al., "The Linux implementation of a log-structured file system" (NILFS), ACM SIGOPS OSR 2006.
- Silberschatz, Galvin, Gagne, *Operating System Concepts*, chapters on file-system implementation and virtual memory.
- FIPS 180-4, Secure Hash Standard (SHA-256).

## Appendix: likely viva questions
1. Why does a version get created at `close()` and not at each `write()`?
2. What happens if the daemon crashes halfway through a snapshot? (Temp-file + rename for objects. A torn
   journal tail is discarded because each record carries a magic number.)
3. How does `.snapshots/2min-ago` exist if it was never created? (The path is parsed in `getattr`;
   it's a virtual directory.)
4. Why turn off the kernel's attribute cache? What does that cost?
5. How does rename avoid copying data?
6. Why a reader–writer lock instead of a mutex for the index?
7. What are the worst cases for your dedup and for your LRU cache?
8. How would you make listing a snapshot faster than O(number of paths)?
