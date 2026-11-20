"""scripts/update_runtime_patch.sh: the runtime checkout gets this repository's patch, safely.

The rule: a runtime holding exactly the current patch is up to date; one holding nothing
gets it; one holding exactly an earlier patch of ours is reset to the pin (the files that
patch created removed) and gets the new one; anything else stops and asks for --reset,
which resets to the pin, cleans and applies. A small git repository stands in for
tools/ps3recomp with patches shaped like the real one (they edit files and create one).
Needs git and bash.

With RACIT_RUNTIME_CLONE set to a ps3recomp checkout that has the pinned commit, the same
cases also run with the real patch.

    python -m unittest discover -s tests
"""
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts' / 'update_runtime_patch.sh'
REAL_PATCH = ROOT / 'patches' / 'ps3recomp-0001-racacit-runtime-fixes.patch'
PIN = '5244db92669b54831c6d3ebb7a96d58c4dbfa6df'
BASE = {'a.c': 'one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n', 'b.c': 'alpha\nbeta\ngamma\ndelta\n'}


def git(cwd, *args):
    return subprocess.run(['git', '-C', str(cwd), *args], check=True, capture_output=True, text=True).stdout


class Cases:
    """The cases, for a runtime self.rt, patches self.p1 (older) and self.p2 (current),
    self.created (a file p1 creates) and self.edited (a file both change)."""

    def run_script(self, patch, *extra):
        return subprocess.run(['bash', str(SCRIPT), str(self.rt), str(patch), *extra], capture_output=True, text=True)

    def status(self):
        return git(self.rt, 'status', '--porcelain', '--untracked-files=all')

    def assert_holds(self, patch):
        r = self.run_script(patch)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('already applied', r.stdout)

    def test_clean_runtime_gets_the_patch_then_is_up_to_date(self):
        r = self.run_script(self.p2)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assert_holds(self.p2)

    def test_earlier_patch_is_swapped(self):
        self.assertEqual(self.run_script(self.p1).returncode, 0)
        self.assertTrue((self.rt / self.created).exists())
        (self.rt / '.git' / 'racit-patch.applied').unlink()   # found from history or not: both work
        shutil.copy(self.p1, self.rt / '.git' / 'racit-patch.applied')
        r = self.run_script(self.p2)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('previous runtime patch is out', r.stdout)
        self.assertFalse((self.rt / self.created).exists(), 'the earlier patch\'s own file was left behind')
        self.assert_holds(self.p2)

    def test_patch_left_half_after_a_restore_stops_then_reset_works(self):
        self.assertEqual(self.run_script(self.p1).returncode, 0)
        git(self.rt, 'restore', '.')            # tracked files back; the created file stays
        self.assertTrue((self.rt / self.created).exists())
        r = self.run_script(self.p2)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('--reset', r.stderr)
        r = self.run_script(self.p2, '--reset')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assert_holds(self.p2)

    def test_own_edits_and_staged_changes_stop_and_reset_clears_them(self):
        self.assertEqual(self.run_script(self.p2).returncode, 0)
        path = self.rt / self.edited
        path.write_bytes(path.read_bytes() + b'\n/* mine */\n')
        r = self.run_script(self.p2)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('local changes', r.stderr)
        self.assertIn('/* mine */', path.read_text(errors='replace'), 'stopping must not touch the edit')
        git(self.rt, 'add', self.edited)        # staged now
        self.assertNotEqual(self.run_script(self.p2).returncode, 0)
        r = self.run_script(self.p2, '--reset')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn('/* mine */', path.read_text(errors='replace'))
        self.assertEqual(git(self.rt, 'diff', '--cached'), '')
        self.assert_holds(self.p2)

    def test_old_stamp_files_in_the_tree_are_ignored(self):
        self.assertEqual(self.run_script(self.p2).returncode, 0)
        (self.rt / '.racit-patch').write_text('stale\n')
        (self.rt / '.racit-patch.applied').write_text('stale\n')
        self.assert_holds(self.p2)
        self.assertFalse((self.rt / '.racit-patch').exists())


@unittest.skipUnless(shutil.which('git') and shutil.which('bash'), 'needs git and bash')
class FakeRuntime(Cases, unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.rt = self.dir / 'runtime'
        self.rt.mkdir()
        git(self.rt, 'init', '-q')
        for name, text in BASE.items():
            (self.rt / name).write_text(text)
        git(self.rt, 'add', '.')
        git(self.rt, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-qm', 'pinned')
        self.created, self.edited = 'new/made.c', 'a.c'
        self.p1 = self.patch('p1', {'a.c': BASE['a.c'].replace('two', 'TWO'), 'new/made.c': 'made\n'})
        self.p2 = self.patch('p2', {'a.c': BASE['a.c'].replace('two', 'Two').replace('seven', 'SEVEN'),
                                    'b.c': BASE['b.c'].replace('beta', 'BETA')})

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def patch(self, name, files):
        for f, text in files.items():
            (self.rt / f).parent.mkdir(parents=True, exist_ok=True)
            (self.rt / f).write_text(text)
        git(self.rt, 'add', '-N', '.')
        out = git(self.rt, 'diff')
        git(self.rt, 'reset', '-q', '--hard')
        git(self.rt, 'clean', '-fdq')
        path = self.dir / (name + '.patch')
        path.write_text(out)
        return path


CLONE = os.environ.get('RACIT_RUNTIME_CLONE')


@unittest.skipUnless(CLONE and shutil.which('git'), 'set RACIT_RUNTIME_CLONE to a ps3recomp checkout with the pinned commit')
class RealPatch(Cases, unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.rt = self.dir / 'ps3recomp'
        subprocess.run(['git', 'clone', '-q', '--no-checkout', CLONE, str(self.rt)], check=True)
        git(self.rt, '-c', 'core.autocrlf=false', 'checkout', '-q', '--detach', PIN)
        self.p2 = REAL_PATCH
        # The earlier patch: the real one with one more file of its own, which the
        # swap must remove.
        text = REAL_PATCH.read_text(encoding='utf-8', errors='surrogateescape')
        extra = ('diff --git a/libs/video/racit_test_left.c b/libs/video/racit_test_left.c\nnew file mode 100644\n'
                 'index 0000000..1111111\n--- /dev/null\n+++ b/libs/video/racit_test_left.c\n@@ -0,0 +1 @@\n+/* made by the earlier patch */\n')
        self.p1 = self.dir / 'p1.patch'
        self.p1.write_text(text + extra, encoding='utf-8', errors='surrogateescape')
        self.created = 'libs/video/racit_test_left.c'
        self.edited = 'libs/input/cellPad.c'

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)


if __name__ == '__main__':
    unittest.main()
