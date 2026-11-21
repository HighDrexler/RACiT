"""patch_lift.py step 25 puts every skip-puzzle hook in once, and stops the build when one can't go in.

Builds a tiny disc lift (test_mouse_repatch.py's cameras plus each puzzle function with only its
anchor line) and runs scripts/patch_lift.py on it. No game files.

    python -m unittest discover -s tests
"""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import test_mouse_repatch as mouse   # noqa: E402  (its MouseRepatch is not imported, so it runs once)
from test_mouse_repatch import PATCH_LIFT, camera_function, recipe   # noqa: E402


class PuzzleHooks(unittest.TestCase):
    def lift(self, directory):
        names = recipe()
        follow = camera_function('func_000459D4', names['MOUSE_CAMERAS']['follow'])
        return mouse.MouseRepatch('test_fresh_lift_is_patched_once').lift(directory, follow), names

    def run_patch(self, directory):
        return subprocess.run([sys.executable, '-I', str(PATCH_LIFT), directory], capture_output=True, text=True)

    def test_every_puzzle_hooked_once(self):
        with tempfile.TemporaryDirectory() as directory:
            path, names = self.lift(directory)
            result = self.run_patch(directory)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('puzzle hooks in all %d puzzles (BCUS98124-1.00)' % len(names['PUZZLE_HOOKS']), result.stdout)
            text = path.read_text(encoding='utf-8')
            for name in names['PUZZLE_HOOKS']:
                self.assertEqual(text.count('/* racacit puzzle: %s */' % name), 1, name)
                self.assertEqual(text.count('racit_skip_puzzle_now("%s")' % name), 1, name)
            self.assertEqual(text.count('extern "C" int racit_skip_puzzle_now(const char* puzzle);'), 1)
            # Again: nothing more goes in.
            self.assertEqual(self.run_patch(directory).returncode, 0)
            self.assertEqual(path.read_text(encoding='utf-8'), text)

    def test_missing_anchor_stops(self):
        with tempfile.TemporaryDirectory() as directory:
            path, names = self.lift(directory)
            for name, (anchor, _) in sorted(names['PUZZLE_HOOKS'].items()):
                text = path.read_text(encoding='utf-8')
                path.write_text(text.replace('        ' + anchor + '\n', '        ctx->gpr[5] = 0;\n', 1), encoding='utf-8')
                result = self.run_patch(directory)
                self.assertNotEqual(result.returncode, 0, name)
                self.assertIn('puzzle hooks missing', result.stdout + result.stderr)
                self.assertIn(name, result.stdout + result.stderr)
                path.write_text(text, encoding='utf-8')


if __name__ == '__main__':
    unittest.main()
