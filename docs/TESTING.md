# Testing ChronoFS

## Repeatable checks

Install the dependencies in the README, then run:

```bash
make clean
make -j2 test
make clean
make -j2 test HAVE_NCURSES=0
```

`make test` runs two C executables and a Python integration suite. Temporary stores
are isolated and removed after each test. Python uses only its standard library.

| Suite | Coverage |
|---|---|
| `tests/core.c` | SHA-256 known answers, path validation, duration overflow, invalid dates, LRU eviction, unchanged snapshots, cross-block reads, historical lookup, store reload, malformed objects and journal tails |
| `tests/fs_callbacks.c` | FUSE callbacks invoked directly: shell-style write sessions, historical reads, read-only enforcement, snapshot failures and retry, fsync, rename, deletion, directories |
| `tests/integration.py` | CLI history, tags, exported and offline restores, deduplication, retention and dry run, GC abort on corrupt manifests, invalid input, terminal reload of externally added files |

The callback tests do not exercise the kernel FUSE protocol. The terminal test
uses a pseudoterminal and is skipped when building without ncurses.

## Sanitizers

Use a clean build when changing compiler flags:

```bash
make clean
make -j2 test \
  CFLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -fno-pie' \
  LDLIBS='-fsanitize=address,undefined -no-pie'
```

## Mounted verification

On Linux with a usable `/dev/fuse` and permission to mount:

```bash
make clean
make
make test-mount
./scripts/demo.sh --pause
```

The mounted test checks real historical reads, denied historical writes, directory
rename, deleted-file restore, four independent concurrent writers, mount-time
reconciliation, and GC. It exits with status 77 when `/dev/fuse` is missing.
The demo preserves its unique temporary store and prints its location.

For a presentation, capture the demo's snapshot reads, restoration, storage
statistics and timeline browser. Include actual screenshots from the target
machine; repository documentation does not stand in for these measurements.

## Verification in this development environment

The environment is Debian 13 with a local dependency prefix providing libfuse3
and ncurses headers. Its dependency settings are environment-specific and are
not needed on a machine with the packages installed normally.

The core, direct callback, CLI and terminal suites were run here. The machine has
no `/dev/fuse`, so kernel mounting and the full mounted demo require verification
on a FUSE-capable host. GitHub Actions is configured in the repository; its remote
result is available only after the changes are pushed and the workflow runs.

AddressSanitizer and UndefinedBehaviorSanitizer checks also passed with ncurses
disabled. Leak detection had to be disabled with `ASAN_OPTIONS=detect_leaks=0`
because LeakSanitizer cannot run under this environment's tracing. This run does
not establish freedom from memory leaks; use the command above on an ordinary
host to include that check.

## Corruption behavior

Historical reads report errors when a block or manifest does not match its hash.
GC aborts before deleting objects when a retained manifest cannot be decoded or
verified. Incomplete or malformed journal records prevent opening the store.
Preserve a copy of a damaged store before diagnosing it. Automatic destructive
repair is intentionally not provided.
