"""patch_lift.py re-patches a lift that v1.0.0 already patched, without a second mouse_angles.

v1.0.0's step 22 (and its update 1.20 recipe) put racit_camera_take into the follow
camera, func_000459D4. The mouse recipe for every camera declares mouse_angles there
too, so on such a lift it must take the old patch out first, or the lift doesn't compile.
This builds a tiny lift with the ten camera functions (only the lines the recipe looks
for), the follow camera carrying the v1.0.0 patch, and runs scripts/patch_lift.py on it
twice. No game files.

    python -m unittest discover -s tests
"""
import ast
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PATCH_LIFT = ROOT / 'scripts' / 'patch_lift.py'


def recipe():
    """MOUSE_CAMERAS, _DISC_CAMERAS and OLD_FOLLOW_MOUSE, read without running patch_lift.py."""
    tree = ast.parse(PATCH_LIFT.read_text(encoding='utf-8'))
    wanted = {'MOUSE_CAMERAS', 'OLD_FOLLOW_MOUSE', 'OLD_MOUSE_DECL', '_DISC_CAMERAS'}
    names = {}
    for node in tree.body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and getattr(node.targets[0], 'id', '') in wanted:
            names[node.targets[0].id] = ast.literal_eval(node.value)
    return names


def camera_function(func, edits):
    start = int(func[5:], 16)
    lines = ['void %s(ppu_context* ctx) {' % func, '        ctx->gpr[3] = 0;']
    for kind, where, _ in edits:
        if kind == 'text':
            lines.append('        ' + where)
        else:
            lines.append('        ctx->lr = 0x%08X; func_00633C34(ctx); DRAIN_TRAMPOLINE(ctx);' % (start + where))
        lines.append('        ctx->gpr[4] = 1;')
    lines.append('}')
    return '\n'.join(lines) + '\n'


def old_patched_follow(names, update120=False):
    """func_000459D4 as v1.0.0's step 22 (or its update 1.20 recipe) left it."""
    old = names['OLD_FOLLOW_MOUSE'][3:] if update120 else names['OLD_FOLLOW_MOUSE']
    if update120:
        old = (old[0], old[2], old[1])   # head; after the yaw call; before the pitch line
    fn = camera_function('func_000459D4', names['MOUSE_CAMERAS']['follow'])
    head = 'void func_000459D4(ppu_context* ctx) {\n'
    fn = fn.replace(head, head + old[0], 1)
    call = '        ctx->lr = 0x00045D6C;'
    fn = fn.replace(call, old[1] + call, 1)
    pitch = '        ctx->fpr[3] = ppu_fp_single(ppu_fsub(ctx->fpr[31], ctx->fpr[1]));'
    fn = fn.replace(pitch, old[2] + pitch, 1)
    return fn


class MouseRepatch(unittest.TestCase):
    def lift(self, directory, follow):
        names = recipe()
        cameras = names['_DISC_CAMERAS']
        parts = ['#include "ppu_recomp.h"\n', names['OLD_MOUSE_DECL'] if 'racit_camera_take' in follow else '']
        for name, func in sorted(cameras.items()):
            parts.append(follow if name == 'follow' else camera_function(func, names['MOUSE_CAMERAS'][name]))
        path = Path(directory) / 'ppu_recomp_000.cpp'
        path.write_text(''.join(parts), encoding='utf-8')
        return path

    def patch(self, directory):
        result = subprocess.run([sys.executable, '-I', str(PATCH_LIFT), directory], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def follow_of(self, path):
        text = path.read_text(encoding='utf-8')
        start = text.index('void func_000459D4(ppu_context* ctx) {')
        return text, text[start:text.index('\n}\n', start)]

    def test_v100_patch_is_replaced(self):
        names = recipe()
        with tempfile.TemporaryDirectory() as directory:
            path = self.lift(directory, old_patched_follow(names))
            out = self.patch(directory)
            self.assertIn('v1.0.0 follow camera patch taken out', out)
            self.assertIn('mouse hooks in all 10 cameras', out)
            text, follow = self.follow_of(path)
            self.assertEqual(len(re.findall(r'double mouse_angles\[2\]', follow)), 1, follow)
            self.assertNotIn('racit_camera_take', text)
            self.assertIn('racit_camera_turn("follow"', follow)
            # And again: nothing more goes in.
            self.patch(directory)
            self.assertEqual(path.read_text(encoding='utf-8'), text)

    def test_v100_update120_patch_is_replaced(self):
        names = recipe()
        with tempfile.TemporaryDirectory() as directory:
            path = self.lift(directory, old_patched_follow(names, update120=True))
            self.assertIn('taken out', self.patch(directory))
            _, follow = self.follow_of(path)
            self.assertEqual(len(re.findall(r'double mouse_angles\[2\]', follow)), 1, follow)
            self.assertNotIn('racit_camera_take', follow)

    def test_fresh_lift_is_patched_once(self):
        names = recipe()
        with tempfile.TemporaryDirectory() as directory:
            path = self.lift(directory, camera_function('func_000459D4', names['MOUSE_CAMERAS']['follow']))
            out = self.patch(directory)
            self.assertNotIn('taken out', out)
            _, follow = self.follow_of(path)
            self.assertEqual(len(re.findall(r'double mouse_angles\[2\]', follow)), 1, follow)

    def test_unknown_old_patch_stops(self):
        names = recipe()
        with tempfile.TemporaryDirectory() as directory:
            follow = old_patched_follow(names).replace('mouse_angles[0];   /* racacit: added', 'mouse_angles[0] * 2;   /* racacit: added')
            self.lift(directory, follow)
            result = subprocess.run([sys.executable, '-I', str(PATCH_LIFT), directory], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('regenerate the lift', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
