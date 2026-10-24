# ChronoFS: A Userspace Time-Travel Filesystem

Course: BITE303P Operating Systems Lab

## Abstract

ChronoFS is a C filesystem implemented with libfuse 3. It records file versions
at write-session boundaries and exposes historical views through ordinary paths,
such as `.snapshots/2min-ago/notes.txt`. Content-addressed 4 KiB blocks reduce
duplicate storage, an append-only journal describes changes, and a bounded LRU
cache accelerates historical reads. A command-line interface and optional ncurses
browser provide history inspection and restoration without requiring a mount.
The project demonstrates filesystem interfaces, indexed allocation, hashing,
synchronization, caching and storage reclamation in one working implementation.

## Problem and scope

An ordinary file save overwrites the previous contents; deletion removes the
normal path to the data. Applications can implement their own undo, but that
does not provide a common history across a directory tree. ChronoFS addresses
this by interposing a userspace filesystem between applications and a backing
directory. Applications continue using normal file operations, while historical
data remains accessible through a virtual read-only namespace.

The intended setting is a trusted local demonstration or project-sized directory
tree. ChronoFS does not implement a production backup service, hostile-user
isolation, distributed storage, or transactional snapshots of concurrent writers.

## Architecture

An application issues a system call through the Linux VFS. The FUSE kernel
interface forwards it to the ChronoFS daemon. Normal paths map into the store's
`current/` directory. Historical paths are resolved through the journal index and
read from immutable objects.

```text
Application -> Linux VFS -> FUSE -> ChronoFS callbacks
                                      |
                       +--------------+---------------+
                       |                              |
                  Live operations                Historical reads
                       |                              |
                   current/                    path/time index
                       |                              |
                 Snapshot file                    manifest
                       |                              |
                blocks + manifest              LRU -> objects/
                       |
                append journal
```

The CLI entry point is `src/main.c`. `src/fs.c` implements the mounted namespace.
`src/journal.c` manages stores and history indexes. `src/blockstore.c` and
`src/sha256.c` implement content-addressed data. `src/lru.c` implements the cache,
`src/timeparse.c` interprets requested times, `src/gc.c` handles retention, and
`src/cmd.c` and `src/tui.c` expose inspection and recovery interfaces.

## Persistent representation

| Store entry | Contents |
|---|---|
| `current/` | Live files and directories |
| `objects/` | Data blocks, addressed by SHA-256 |
| `manifests/` | File size and ordered block hashes, also addressed by SHA-256 |
| `journal.log` | Fixed 1096-byte change records |
| `tags` | Named timestamps |
| `lock` | Advisory lock used by the mounted daemon and GC |
| `mounted` | Active mountpoint used for restoration through FUSE |

A journal record stores the operation, path, timestamp, mode, file size, mtime
and manifest hash. Deletions are tombstones. The in-memory hash table maps each
path to a sorted array of versions. Binary search selects the latest version
whose timestamp is at or before the requested instant.

## Version creation and deduplication

The daemon marks an open file dirty when it is written or truncated. A flush
after actual writes records a version. Creates or truncates without a write are
handled on final release; fsync also records dirty data. This avoids generating
an empty version when a shell duplicates and closes a descriptor before writing.

Snapshotting reads the file in 4096-byte blocks. SHA-256 identifies each block,
so equal contents share a stored object. An ordered list of hashes and the file
size form the manifest. A journal entry then associates that manifest with a path
and time. Identical content, mode and mtime do not create a redundant record.

For example, a file containing two different blocks needs two objects. Changing
one byte in its first block creates one additional object; the unchanged second
block is shared by both versions. The integration suite verifies this case.
Every snapshot still reads and hashes the whole file, so storage savings do not
eliminate CPU and read overhead.

## Historical namespace and recovery

`/.snapshots/<when>/` is virtual: the requested time is decoded from the path.
Supported forms include relative durations, absolute dates and clock times,
Unix epoch seconds, `now`, and named tags. Historical files expose saved contents
and read-only permissions. Directory listings collect children that existed at
the chosen time. This requires scanning tracked paths.

Restoration reconstructs files from manifests. For a mounted store it writes
through the mount, recording the recovery as a new version. Offline restoration
writes into `current/` and explicitly records the restored file. An output path
can export historical contents without modifying the live store. Restoration
recovers file contents and permissions; empty directories and all metadata are
not reproduced.

The ncurses browser shows a timeline, directory tree and content preview. Moving
through the timeline updates the other panes. Reload builds a fresh index before
replacing the current one, so externally recorded changes become browsable and a
failed reload preserves the previous index.

## Caching and synchronization

Historical data uses a fixed-capacity cache. Hash buckets locate blocks and a
doubly linked list maintains recency. A hit moves a block to the front; a full
cache evicts the tail. Cache statistics expose hits, misses and evictions.
Sequential scans larger than the cache can cause repeated misses.

A reader-writer lock protects the path index and journal updates in the daemon.
Mutexes protect cache operations and statistics. An advisory file lock prevents
two daemons from mounting the same store and prevents GC while mounted.
Separate offline writers must be serialized by the operator. Concurrent writes
to the same file do not provide an atomic point-in-time snapshot.

## Retention and integrity

GC retains all records inside the requested retention window and the last live
state before the cutoff for each path. It marks referenced manifests and blocks,
commits the retained journal, syncs its directory entry, then sweeps unreachable
objects. A dry run reports the proposed changes without deleting history.

If a retained manifest is unreadable or marking runs out of memory, GC stops
before sweeping. Reads verify hashes and manifest dimensions. Malformed or torn
journals are rejected instead of silently discarding history. Snapshot errors
propagate from flush and fsync so callers can observe failed version creation.
New object files are synced before publication, but the overall system is not
a fully transactional guarantee against every crash or power-loss scenario.

## Relationship to operating systems concepts

| Concept | Implementation |
|---|---|
| Kernel/userspace boundary | Linux VFS and FUSE request handling |
| File system calls | open, read, write, stat, rename, unlink, fsync and directory traversal |
| Indexed allocation | A manifest maps logical file blocks to stored object hashes |
| Synchronization | Reader-writer locks, mutexes and advisory file locks |
| Page replacement | Bounded LRU cache with hit/miss/eviction statistics |
| Dynamic allocation | Growing version arrays, hash tables and cache frames |
| Journaling | Persistent change records replayed into an index |
| Garbage collection | Reachability marking and sweeping under a retention policy |

## Validation

`make test` runs core unit tests, direct FUSE callback tests and CLI/terminal
integration tests. These cover known SHA-256 answers, time overflow and calendar
validation, paths, LRU eviction, historical lookup, unchanged snapshot suppression,
cross-block reads, restores, deduplication, GC, corruption and browser reload.
`docs/TESTING.md` contains the commands and explains the boundary between callback
tests and actual kernel integration.

The development environment lacks `/dev/fuse`. The mounted workflow therefore
requires a separate Linux host exposing that device and permitting FUSE mounts.
`make test-mount` supplies a repeatable real-mount test; `scripts/demo.sh` supplies
the presentation walkthrough. Screenshots, throughput figures and mounted test
results should be recorded on that host rather than inferred from source code.

## Limitations and future work

Symlinks pass through without historical versioning; hard links and extended
attributes are unsupported. File ownership is not versioned. Snapshots rehash
whole files, and fixed boundaries deduplicate insertions poorly. Store files
use native C layout and byte order. The journal index is rebuilt at startup,
and historical directory listing scans all paths.

Possible extensions include content-defined chunking, compression, dirty-block
tracking, persistent index checkpoints, richer retention policies, complete
metadata restoration, and stronger transaction and crash recovery guarantees.
These extend the educational implementation rather than prerequisites for its
current demonstration scope.

## References

- libfuse documentation and examples: https://github.com/libfuse/libfuse
- Vangoor, Tarasov and Zadok, “To FUSE or Not to FUSE: Performance of User-Space
  File Systems,” USENIX FAST, 2017.
- Quinlan and Dorward, “Venti: a new approach to archival storage,” USENIX FAST, 2002.
- NIST FIPS 180-4, Secure Hash Standard.
- Silberschatz, Galvin and Gagne, *Operating System Concepts*, filesystem and
  virtual memory chapters.
