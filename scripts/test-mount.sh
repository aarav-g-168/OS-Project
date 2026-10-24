#!/usr/bin/env bash
# Kernel-level integration checks. Each run owns a fresh temporary store.
set -euo pipefail
cd "$(dirname "$0")/.."
if [ ! -c /dev/fuse ]; then
    echo 'test-mount: /dev/fuse is unavailable; mounted checks cannot run.' >&2
    exit 77
fi
make
BIN="$PWD/chronofs"
BASE=$(mktemp -d "${TMPDIR:-/tmp}/chronofs-mount-test.XXXXXX")
S="$BASE/store"
M="$BASE/mnt"
mkdir -p "$S" "$M"
cleanup() {
    if mountpoint -q "$M"; then
        "$BIN" umount "$M" || return
    fi
    rm -rf "$BASE"
}
trap cleanup EXIT
"$BIN" mount "$S" "$M"
mountpoint -q "$M"
printf first > "$M/file"
"$BIN" tag "$S" first
printf second > "$M/file"
test "$(cat "$M/.snapshots/first/file")" = first
test "$(cat "$M/file")" = second
if (printf forbidden > "$M/.snapshots/first/file") 2>/dev/null; then
    echo 'test-mount: historical write unexpectedly succeeded' >&2
    exit 1
fi
mkdir "$M/dir"
printf nested > "$M/dir/child"
mv "$M/dir" "$M/moved"
test "$(cat "$M/moved/child")" = nested
rm "$M/file"
"$BIN" restore "$S" file --at first
test "$(cat "$M/file")" = first
pids=()
for writer in 1 2 3 4; do
    (
        for revision in {1..10}; do
            printf '%s:%s' "$writer" "$revision" > "$M/writer-$writer"
        done
    ) &
    pids+=("$!")
done
for pid in "${pids[@]}"; do wait "$pid"; done
for writer in 1 2 3 4; do test "$(cat "$M/writer-$writer")" = "$writer:10"; done
"$BIN" umount "$M"
printf offline > "$S/current/offline"
"$BIN" mount "$S" "$M"
test "$(cat "$M/.snapshots/now/offline")" = offline
"$BIN" umount "$M"
"$BIN" gc "$S" --keep 1h
test "$("$BIN" cat "$S" file --at first)" = first
echo 'PASS: mounted history, read-only snapshots, rename, restore, concurrent writers, reconciliation, GC'
