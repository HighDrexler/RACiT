"""tests/mac_user_launch.py checks its inputs and its --work folder before it touches anything,
since it clears --work on every run: a missing input, a --work that is not empty and not its
own, or a --work holding an input or the current folder each stop it before it clears or
starts anything. The fake build's RaCACiT records it if it ever runs. No game files.

    python -m unittest discover -s tests
"""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tests' / 'mac_user_launch.py'


@unittest.skipUnless(sys.platform == 'darwin', 'the macOS launch smoke uses /usr/bin/pgrep')
class Guards(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        self.build, self.game, self.elf = t / 'build', t / 'game', t / 'EBOOT.ELF'
        (self.game / 'PS3_GAME').mkdir(parents=True)
        self.build.mkdir()
        self.ran = t / 'ran'
        prog = self.build / 'RaCACiT'
        prog.write_text('#!/bin/sh\ntouch "%s"\n' % self.ran, encoding='utf-8')
        prog.chmod(0o755)
        self.elf.write_bytes(b'\x7fELF')

    def tearDown(self):
        self.tmp.cleanup()

    def run_script(self, work, *extra, cwd=None):
        return subprocess.run([sys.executable, '-I', str(SCRIPT), '--build', str(self.build), '--elf', str(self.elf),
                               '--game', str(self.game), '--work', str(work), '--seconds', '1'] + list(extra),
                              capture_output=True, text=True, cwd=cwd or self.tmp.name)

    def assertStopped(self, result, words):
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(words, result.stdout + result.stderr)
        self.assertFalse(self.ran.exists(), 'the game was started')

    def test_unmarked_folder_is_kept(self):
        work = Path(self.tmp.name) / 'mine'
        work.mkdir(); (work / 'keep.txt').write_text('x')
        self.assertStopped(self.run_script(work), 'not made by this script')
        self.assertTrue((work / 'keep.txt').exists())

    def test_work_holding_an_input_or_the_current_folder(self):
        self.assertStopped(self.run_script(self.tmp.name), 'holds')
        sub = Path(self.tmp.name) / 'w'; sub.mkdir()
        self.assertStopped(self.run_script(sub, cwd=str(sub)), 'holds')

    def test_missing_inputs_stop_it_first(self):
        work = Path(self.tmp.name) / 'mine'
        work.mkdir(); (work / 'keep.txt').write_text('x')
        self.elf.unlink()
        self.assertStopped(self.run_script(work), '--elf')
        self.assertTrue((work / 'keep.txt').exists())
        self.elf.write_bytes(b'\x7fELF')
        self.assertStopped(self.run_script(work, '--update', str(Path(self.tmp.name) / 'nope')), '--update')


DRAWS = 'echo "[RSX vulkan] register-file draw engine path"\n'
STATS = 'echo "[rsx-stats] methods=1 draws=1 flips=0 gcmflips=$n clears=0 texups=5 texMB=0 us=1"'


class Verdicts(Guards):
    """What it makes of a game that keeps drawing, hangs after its first frames, or crashes:
    a fake RaCACiT that prints the log lines the check reads."""

    def fake(self, body):
        prog = self.build / 'RaCACiT'
        prog.write_text('#!/bin/sh\n' + DRAWS + body, encoding='utf-8')
        prog.chmod(0o755)

    def verdict(self, *extra):
        result = self.run_script(Path(self.tmp.name) / 'w', '--seconds', '8', '--alive', '7', '--cap', '40', *extra)
        return result.returncode, result.stdout + result.stderr

    def test_keeps_drawing_passes(self):
        self.fake('n=0; while :; do n=$((n+10)); %s; sleep 1; done\n' % STATS)
        code, out = self.verdict()
        self.assertEqual(code, 0, out)

    def test_hang_after_first_frames_fails(self):
        self.fake('n=10; %s; n=20; %s; sleep 60\n' % (STATS, STATS))
        code, out = self.verdict()
        self.assertEqual(code, 1, out)

    def test_freeze_near_the_end_fails(self):
        # Draws for about 12 s, then stops 10 s before the end of a 22 s watch.
        self.fake('n=0; for i in $(seq 12); do n=$((n+10)); %s; sleep 1; done; sleep 60\n' % STATS)
        code, out = self.verdict('--seconds', '22', '--alive', '14')
        self.assertEqual(code, 1, out)

    def test_crash_fails(self):
        self.fake('n=0; for i in 1 2 3; do n=$((n+10)); %s; sleep 1; done; kill -SEGV $$\n' % STATS)
        code, out = self.verdict('--seconds', '20')
        self.assertEqual(code, 1, out)
        self.assertIn('stopped by itself', out)

    def test_never_drawing_is_inconclusive_at_the_cap(self):
        prog = self.build / 'RaCACiT'
        prog.write_text('#!/bin/sh\nwhile :; do echo "[fs] read fd=3"; sleep 1; done\n', encoding='utf-8')
        prog.chmod(0o755)
        result = self.run_script(Path(self.tmp.name) / 'w', '--seconds', '4', '--cap', '6')
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)

    def test_drawing_during_loading_is_inconclusive_at_the_cap(self):
        self.fake('n=0; while :; do n=$((n+10)); %s; echo "[fs] read fd=3"; sleep 1; done\n' % STATS)
        code, out = self.verdict('--cap', '10')
        self.assertEqual(code, 2, out)


if __name__ == '__main__':
    unittest.main()
