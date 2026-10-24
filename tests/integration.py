"""CLI and terminal regressions; no kernel FUSE mount required."""
import os
import pathlib
import pty
import select
import subprocess
import tempfile
import time
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
BIN = ROOT / "chronofs"
HELPER = ROOT / "build/test-core"


class Integration(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="chronofs-test-")
        self.base = pathlib.Path(self.tmp.name)
        self.store = self.base / "store"
        (self.store / "current").mkdir(parents=True)

    def tearDown(self):
        self.tmp.cleanup()

    def cli(self, *args, ok=True):
        result = subprocess.run([str(BIN), *map(str, args)], capture_output=True, text=True)
        if ok:
            self.assertEqual(result.returncode, 0, result.stderr)
        else:
            self.assertNotEqual(result.returncode, 0)
        return result.stdout

    def snapshot(self, text, path="notes.txt"):
        file = self.store / "current" / path
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(text)
        subprocess.run([str(HELPER), "snapshot", str(self.store), "/" + path], check=True)

    def test_history_and_restore(self):
        self.snapshot("first\n")
        self.cli("tag", self.store, "first")
        self.snapshot("second\n")
        self.assertEqual(self.cli("cat", self.store, "notes.txt", "--at", "first"), "first\n")
        self.assertEqual(self.cli("cat", self.store, "notes.txt"), "second\n")
        self.assertIn("notes.txt", self.cli("ls", self.store))
        exported = self.base / "exported"
        self.cli("restore", self.store, "notes.txt", "--at", "first", "-o", exported)
        self.assertEqual(exported.read_text(), "first\n")
        self.cli("restore", self.store, "notes.txt", "--at", "first")
        self.assertEqual(self.cli("cat", self.store, "notes.txt"), "first\n")
        self.assertIn("3  (3 writes", self.cli("stats", self.store))

    def test_dedup_and_gc(self):
        self.snapshot("A" * 4096 + "C" * 4096)
        self.snapshot("B" + "A" * 4095 + "C" * 4096)
        self.assertIn("3 blocks", self.cli("stats", self.store))
        before = (self.store / "journal.log").read_bytes()
        self.cli("gc", self.store, "--keep", "0s", "--dry-run")
        self.assertEqual((self.store / "journal.log").read_bytes(), before)
        self.cli("gc", self.store, "--keep", "0s")
        self.assertEqual(self.cli("cat", self.store, "notes.txt"), "B" + "A" * 4095 + "C" * 4096)
        self.assertIn("2 blocks", self.cli("stats", self.store))

    def test_gc_aborts_without_deleting_on_corrupt_manifest(self):
        self.snapshot("first")
        self.snapshot("second")
        manifest = next((self.store / "manifests").glob("*/*"))
        manifest.write_bytes(b"corrupt")
        before = {p.relative_to(self.store): p.read_bytes()
                  for p in self.store.rglob("*") if p.is_file()}
        self.cli("gc", self.store, "--keep", "1h", ok=False)
        for path, content in before.items():
            self.assertEqual((self.store / path).read_bytes(), content)

    def test_invalid_paths_times_and_journal(self):
        self.snapshot("safe")
        self.cli("cat", self.store, "../notes.txt", ok=False)
        self.cli("restore", self.store, "../notes.txt", "--at", "now", ok=False)
        self.cli("cat", self.store, "notes.txt", "--at", "@2026-02-30", ok=False)
        self.cli("gc", self.store, "--keep", "1000000000w", ok=False)
        self.cli("tag", self.store, "", ok=False)
        for invalid in ("0", "-1", "junk", "999999999999999999999"):
            self.cli("mount", self.store, self.base, "--cache-blocks", invalid, ok=False)
        with (self.store / "journal.log").open("ab") as f:
            f.write(b"torn")
        self.cli("stats", self.store, ok=False)
        self.cli("gc", self.store, "--keep", "1h", ok=False)

    def test_tui_reload(self):
        if os.environ.get("CHRONOFS_TEST_NO_TUI"):
            self.skipTest("ncurses intentionally disabled")
        self.snapshot("first")
        master, slave = pty.openpty()
        env = dict(os.environ, TERM="xterm", LINES="30", COLUMNS="100")
        proc = subprocess.Popen([str(BIN), "tui", str(self.store)],
                                stdin=slave, stdout=slave, stderr=slave, env=env)
        os.close(slave)
        output = bytearray()

        def until(needle):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if select.select([master], [], [], 0.1)[0]:
                    try:
                        output.extend(os.read(master, 65536))
                    except OSError:
                        break
                if needle in output:
                    return
            self.fail(f"terminal did not display {needle!r}: {bytes(output)!r}")

        try:
            until(b"Timeline")
            self.snapshot("UPDATED content", "new.txt")
            os.write(master, b"R")
            until(b"new.txt")
            os.write(master, b"\t")
            until(b"UPDATED content")
            os.write(master, b"q")
            self.assertEqual(proc.wait(timeout=5), 0)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            os.close(master)


if __name__ == "__main__":
    unittest.main(verbosity=2)
