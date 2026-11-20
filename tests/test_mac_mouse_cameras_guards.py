"""tests/mac_mouse_cameras.py checks its inputs and --work before it clears or starts anything:
a --work that is not empty and not its own, one holding --save, or a missing input each stop
it, and nothing is deleted. The fake player records it if it ever runs. No game files.

    python -m unittest discover -s tests
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'tests' / 'mac_mouse_cameras.py'


class Guards(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        t = Path(self.tmp.name)
        (t / 'game' / 'PS3_GAME').mkdir(parents=True)
        (t / 'save').mkdir()
        self.ran = t / 'ran'
        self.player = t / 'player'
        self.player.write_text('#!/bin/sh\ntouch "%s"\n' % self.ran, encoding='utf-8'); self.player.chmod(0o755)
        (t / 'EBOOT.ELF').write_bytes(b'\x7fELF')

    def tearDown(self):
        self.tmp.cleanup()

    def run_script(self, work, save=None):
        t = Path(self.tmp.name)
        return subprocess.run([sys.executable, '-I', str(SCRIPT), '--player', str(self.player), '--elf', str(t / 'EBOOT.ELF'),
                               '--game', str(t / 'game'), '--save', str(save or t / 'save'), '--work', str(work)],
                              capture_output=True, text=True, cwd=self.tmp.name)

    def assertStopped(self, result, words):
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(words, result.stdout + result.stderr)
        self.assertFalse(self.ran.exists(), 'the game was started')

    def test_unmarked_folder_is_kept(self):
        work = Path(self.tmp.name) / 'mine'
        work.mkdir(); (work / 'keep.txt').write_text('x')
        self.assertStopped(self.run_script(work), 'not made by this script')
        self.assertTrue((work / 'keep.txt').exists())

    def test_work_holding_the_save(self):
        work = Path(self.tmp.name) / 'w'
        (work / 'save').mkdir(parents=True)
        self.assertStopped(self.run_script(work, save=work / 'save'), 'holds --save')
        self.assertTrue((work / 'save').exists())

    def test_missing_input(self):
        self.assertStopped(self.run_script(Path(self.tmp.name) / 'new', save=Path(self.tmp.name) / 'nope'), '--save')


if __name__ == '__main__':
    unittest.main()
