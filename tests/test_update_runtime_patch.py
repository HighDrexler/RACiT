"""scripts/update_runtime_patch.sh swaps the runtime patch without losing anything.

A small git repository stands in for tools/ps3recomp, with two versions of a patch:
the first setup applies it (over unrelated local edits too), a changed patch is
swapped in keeping the builder's edits, a swap that conflicts changes nothing (the old
patch and the stamp stay) and works when run again after the conflict is resolved,
and --reset starts over from the pinned commit. Needs git and bash.

    python -m unittest discover -s tests
"""
import hashlib
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / 'scripts' / 'update_runtime_patch.sh'
BASE = {'a.c': 'one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\n', 'b.c': 'alpha\nbeta\ngamma\ndelta\n'}


def git(cwd, *args):
    return subprocess.run(['git', '-C', str(cwd), *args], check=True, capture_output=True, text=True).stdout


@unittest.skipUnless(shutil.which('git') and shutil.which('bash'), 'needs git and bash')
class UpdateRuntimePatch(unittest.TestCase):
    def setUp(self):
        self.dir = Path(tempfile.mkdtemp())
        self.rt = self.dir / 'runtime'
        self.rt.mkdir()
        git(self.rt, 'init', '-q')
        for name, text in BASE.items():
            (self.rt / name).write_text(text)
        git(self.rt, 'add', '.')
        git(self.rt, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-qm', 'pinned')
        # Patch 1 changes "two"; patch 2 changes "two" differently and also "seven".
        self.p1 = self.patch('p1', {'a.c': BASE['a.c'].replace('two', 'TWO')})
        self.p2 = self.patch('p2', {'a.c': BASE['a.c'].replace('two', 'Two').replace('seven', 'SEVEN')})

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def patch(self, name, files):
        for f, text in files.items():
            (self.rt / f).write_text(text)
        out = git(self.rt, 'diff')
        git(self.rt, 'checkout', '-q', '--', '.')
        path = self.dir / (name + '.patch')
        path.write_text(out)
        return path

    def run_script(self, patch, *extra):
        return subprocess.run(['bash', str(SCRIPT), str(self.rt), str(patch), *extra], capture_output=True, text=True)

    def stamp(self):
        return (self.rt / '.racit-patch').read_text().strip()

    def a(self):
        return (self.rt / 'a.c').read_text()

    def test_first_setup_over_unrelated_edits(self):
        (self.rt / 'b.c').write_text(BASE['b.c'].replace('gamma', 'mine'))
        r = self.run_script(self.p1)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('TWO', self.a())
        self.assertIn('mine', (self.rt / 'b.c').read_text())
        self.assertEqual(self.stamp(), hashlib.sha256(self.p1.read_bytes()).hexdigest())

    def test_swap_keeps_own_edits(self):
        self.assertEqual(self.run_script(self.p1).returncode, 0)
        (self.rt / 'b.c').write_text(BASE['b.c'].replace('gamma', 'mine'))
        r = self.run_script(self.p2)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('Two', self.a()); self.assertIn('SEVEN', self.a()); self.assertNotIn('TWO', self.a())
        self.assertIn('mine', (self.rt / 'b.c').read_text())

    def test_conflicting_swap_changes_nothing_then_retry_works(self):
        self.assertEqual(self.run_script(self.p1).returncode, 0)
        old_stamp = self.stamp()
        # The builder's own edit where patch 2 changes "seven".
        (self.rt / 'a.c').write_text(self.a().replace('seven', 'my seven'))
        before = self.a()
        r = self.run_script(self.p2)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn('stash', r.stderr)
        self.assertEqual(self.a(), before, 'the runtime changed although the swap failed')
        self.assertEqual(self.stamp(), old_stamp)
        # Resolved: the builder takes the edit back out. Running again swaps.
        (self.rt / 'a.c').write_text(before.replace('my seven', 'seven'))
        r = self.run_script(self.p2)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn('Two', self.a()); self.assertIn('SEVEN', self.a())
        # And again: nothing to do.
        r = self.run_script(self.p2)
        self.assertEqual(r.returncode, 0); self.assertIn('already applied', r.stdout)

    def test_stale_stamp_after_a_restore(self):
        self.assertEqual(self.run_script(self.p1).returncode, 0)
        git(self.rt, 'checkout', '-q', '--', 'a.c')   # the patch is gone, the stamp stays
        r = self.run_script(self.p1)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertNotIn('already applied', r.stdout)
        self.assertIn('TWO', self.a())

    def test_reset_when_already_applied(self):
        self.assertEqual(self.run_script(self.p1).returncode, 0)
        (self.rt / 'b.c').write_text(BASE['b.c'].replace('gamma', 'mine'))
        r = self.run_script(self.p1, '--reset')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual((self.rt / 'b.c').read_text(), BASE['b.c'])
        self.assertIn('TWO', self.a())

    def test_reset_starts_from_the_pinned_commit(self):
        self.assertEqual(self.run_script(self.p1).returncode, 0)
        (self.rt / 'a.c').write_text(self.a().replace('seven', 'my seven'))
        r = self.run_script(self.p2, '--reset')
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.a(), BASE['a.c'].replace('two', 'Two').replace('seven', 'SEVEN'))


if __name__ == '__main__':
    unittest.main()
