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


# patch_lift.py's definitions (everything before it reads its arguments), for the update
# 1.20 path: that profile is only chosen for its own executable, which a test doesn't
# have, so the test calls the camera pass with the profile itself.
PREFIX_END = "if len(sys.argv) > 1 and sys.argv[1] == '--check-profile':"
U120_DRIVER = r'''
import sys
path, recomp = sys.argv[1], sys.argv[2]
source = open(path, encoding='utf-8').read()
namespace = {'__file__': path, '__name__': 'patch_lift'}
sys.path.insert(0, __import__('os').path.dirname(path))
exec(compile(source[:source.index(sys.argv[3])], path, 'exec'), namespace)
if sys.argv[4] == 'table':
    print(repr(namespace['MOUSE_CAMERA_FUNCS']['BCUS98124-1.20']))
else:
    namespace['apply_mouse_cameras'](recomp, 'BCUS98124-1.20')
'''


def update120_cameras():
    out = subprocess.run([sys.executable, '-I', '-c', U120_DRIVER, str(PATCH_LIFT), '', PREFIX_END, 'table'],
                         check=True, capture_output=True, text=True).stdout
    return ast.literal_eval(out.strip())


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
    def lift(self, directory, follow, cameras=None):
        names = recipe()
        cameras = cameras or names['_DISC_CAMERAS']
        parts = ['#include "ppu_recomp.h"\n', names['OLD_MOUSE_DECL'] if 'racit_camera_take' in follow else '']
        for name, func in sorted(cameras.items()):
            parts.append(follow if name == 'follow' else self.lock_function(func) if name == 'lock'
                         else camera_function(func, names['MOUSE_CAMERAS'][name]))
        path = Path(directory) / 'ppu_recomp_000.cpp'
        path.write_text(''.join(parts), encoding='utf-8')
        return path

    def patch_update120(self, directory):
        result = subprocess.run([sys.executable, '-I', '-c', U120_DRIVER, str(PATCH_LIFT), directory, PREFIX_END, 'patch'],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

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
            # The 1.20 lift: its own camera table (two cameras sit elsewhere), patched
            # by the 1.20 camera pass.
            cameras = update120_cameras()
            self.assertNotEqual(cameras, names['_DISC_CAMERAS'])
            path = self.lift(directory, old_patched_follow(names, update120=True), cameras)
            out = self.patch_update120(directory)
            self.assertIn('taken out', out)
            self.assertIn('mouse hooks in all 10 cameras (BCUS98124-1.20)', out)
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

    STICK_READ = '        ctx->lr = 0x%08X; func_006417C0(ctx); DRAIN_TRAMPOLINE(ctx);\n'   # at +0x104
    GATE = '        ctx->fpr[12] = (ctx->fpr[1] >= 0.0) ? ctx->fpr[13] : ctx->fpr[0];\n'
    TAKE = '        if (!mouse_taken) { mouse_taken = 1; mouse_direct = racit_camera_turn("lock", mouse_angles, racit_mouse_now()); }\n'

    def lock_function(self, func):
        """The lock camera's lines the recipes look for: the stick read (+0x104), the tilt gate,
        and the yaw step's call (+0x4C8)."""
        names = recipe()
        fn = camera_function(func, names['MOUSE_CAMERAS']['lock'])
        head = '        ctx->gpr[3] = 0;\n'
        return fn.replace(head, head + self.STICK_READ % (int(func[5:], 16) + 0x104), 1)

    def old_lock(self, func, tilt_after_read=False):
        """The lock camera as an earlier recipe left it: the yaw only (round 6), or with the
        tilt written into [r1+0x78] right after the stick read (round 7)."""
        fn = self.lock_function(func)
        head = 'void %s(ppu_context* ctx) {\n' % func
        fn = fn.replace(head, head + '        double mouse_angles[2] = {0, 0}; int mouse_taken = 0, mouse_direct = 0;   /* racacit */\n', 1)
        call = '        ctx->lr = 0x%08X;' % (int(func[5:], 16) + 0x4C8)
        fn = fn.replace(call, self.TAKE + '        if (mouse_direct) ctx->fpr[1] -= mouse_angles[0];   /* racacit mouse: lock */\n' + call, 1)
        if tilt_after_read:
            read = self.STICK_READ % (int(func[5:], 16) + 0x104)
            fn = fn.replace(read, read + self.TAKE +
                            '        if (mouse_direct) { uint32_t yb = vm_read32(ctx->gpr[1] + 0x78); float y; memcpy(&y, &yb, 4);\n'
                            '          y = (float)fmin(fmax(y + mouse_angles[1] * 20.0, -1.0), 1.0);\n'
                            '          memcpy(&yb, &y, 4); vm_write32(ctx->gpr[1] + 0x78, yb); }   /* racacit mouse: lock */\n', 1)
        return fn

    def check_lock(self, path, func):
        text = path.read_text(encoding='utf-8')
        start = text.index('void %s(ppu_context* ctx) {' % func)
        lock = text[start:text.index('\n}\n', start)]
        self.assertEqual(lock.count('/* racacit mouse: lock */'), 2, lock)
        self.assertEqual(lock.count('double mouse_angles[2]'), 1, lock)
        self.assertEqual(lock.count('ctx->fpr[1] -= mouse_angles[0];'), 1, lock)
        self.assertNotIn('vm_write32(ctx->gpr[1] + 0x78', lock)
        # The tilt goes on the stick input right before the game's gate, the other way up from mouse_angles[1].
        tilt = lock.index('ctx->fpr[13] = (float)fmin(fmax(ctx->fpr[13] - mouse_angles[1] * 20.0, -1.0), 1.0);')
        self.assertLess(tilt, lock.index(self.GATE.strip()))
        self.assertLess(lock.index('ctx->lr = 0x%08X;' % (int(func[5:], 16) + 0x104)), tilt)
        return text

    def test_old_lock_patch_gets_tilt(self):
        names = recipe()
        for update120 in (False, True):
            cameras = update120_cameras() if update120 else names['_DISC_CAMERAS']
            func = cameras['lock']
            with self.subTest(update120=update120), tempfile.TemporaryDirectory() as directory:
                follow = camera_function(cameras['follow'], names['MOUSE_CAMERAS']['follow'])
                path = self.lift(directory, follow, cameras)
                text = path.read_text(encoding='utf-8')
                fresh = self.lock_function(func)
                path.write_text(text.replace(fresh, self.old_lock(func, tilt_after_read=update120), 1), encoding='utf-8')
                out = self.patch_update120(directory) if update120 else self.patch(directory)
                self.assertIn('earlier lock camera patch replaced', out)
                text = self.check_lock(path, func)
                # And again: nothing more goes in.
                self.patch_update120(directory) if update120 else self.patch(directory)
                self.assertEqual(path.read_text(encoding='utf-8'), text)

    def test_fresh_lock_gets_yaw_and_tilt(self):
        names = recipe()
        with tempfile.TemporaryDirectory() as directory:
            path = self.lift(directory, camera_function('func_000459D4', names['MOUSE_CAMERAS']['follow']))
            self.patch(directory)
            self.check_lock(path, names['_DISC_CAMERAS']['lock'])

    def test_strafe_pitch_the_other_way_up_is_replaced(self):
        """A lift patched when strafe's pitch went the other way up (+=) gets it the way up of the
        recipe now, once, and nothing else changes."""
        names = recipe()
        with tempfile.TemporaryDirectory() as directory:
            path = self.lift(directory, camera_function('func_000459D4', names['MOUSE_CAMERAS']['follow']))
            self.patch(directory)
            now = path.read_text(encoding='utf-8')
            want = [e for e in names['MOUSE_CAMERAS']['strafe'] if 'mouse_angles[1]' in e[2]][0][2]
            self.assertIn(want, now)
            line = want + '   /* racacit mouse: strafe */'
            self.assertEqual(now.count(line), 1)
            other = want.replace('-=', '+=') if '-=' in want else want.replace('+=', '-=')
            path.write_text(now.replace(line, other + '   /* racacit mouse: strafe */'), encoding='utf-8')
            out = self.patch(directory)
            self.assertIn('earlier strafe camera patch replaced', out)
            self.assertEqual(path.read_text(encoding='utf-8'), now)

    def test_494cfa6_follow_patch_is_replaced(self):
        """The lift the shipped v1.0.0 USA Windows exes came from: patched by 494cfa6's recipe
        (text as it was there), with its three declarations at the top."""
        names = recipe()
        fn = camera_function('func_000459D4', names['MOUSE_CAMERAS']['follow'])
        changes = (   # 494cfa6:scripts/patch_lift.py, the follow-camera distances step
            ('void func_000459D4(ppu_context* ctx) {',
             'void func_000459D4(ppu_context* ctx) {\n        double mouse_angles[2] = {};\n        const int mouse_direct = racit_camera_take(mouse_angles, SDL_GetPerformanceCounter() * (1000000000.0 / SDL_GetPerformanceFrequency()));'),
            ('        ctx->lr = 0x00045D6C; func_00633C34(ctx); DRAIN_TRAMPOLINE(ctx);',
             '        if (mouse_direct) ctx->fpr[1] = -mouse_angles[0];\n        ctx->lr = 0x00045D6C; func_00633C34(ctx); DRAIN_TRAMPOLINE(ctx);'),
            ('        ctx->fpr[3] = ppu_fp_single(ppu_fsub(ctx->fpr[31], ctx->fpr[1]));',
             '''        if (mouse_direct) {
            uint32_t lb=vm_read32(ctx->gpr[30]-0x7F24), hb=vm_read32(ctx->gpr[30]-0x7F1C);
            float lo,hi; memcpy(&lo,&lb,4); memcpy(&hi,&hb,4);
            const double target=mouse_angles[1]-ctx->fpr[1];
            ctx->fpr[31]=fmin(fmax(target,lo),hi)+ctx->fpr[1];
        }
        ctx->fpr[3] = ppu_fp_single(ppu_fsub(ctx->fpr[31], ctx->fpr[1]));'''),
        )
        for old, new in changes:
            self.assertEqual(fn.count(old), 1, old)
            fn = fn.replace(old, new, 1)
        decls = ('extern "C" int racit_camera_take(double*, unsigned long long);\n'
                 'extern "C" unsigned long long SDL_GetPerformanceCounter(void);\n'
                 'extern "C" unsigned long long SDL_GetPerformanceFrequency(void);\n')
        with tempfile.TemporaryDirectory() as directory:
            path = self.lift(directory, fn)
            text = path.read_text(encoding='utf-8')
            i = text.index('\n') + 1
            path.write_text(text[:i] + decls + text[i:], encoding='utf-8')
            out = self.patch(directory)
            self.assertIn('v1.0.0 follow camera patch taken out', out)
            text, follow = self.follow_of(path)
            self.assertEqual(len(re.findall(r'double mouse_angles\[2\]', follow)), 1, follow)
            self.assertNotIn('racit_camera_take', text)
            self.assertEqual(text.count('extern "C" unsigned long long SDL_GetPerformanceCounter(void);'), 1)
            self.assertIn('racit_camera_turn("follow"', follow)
            self.patch(directory)
            self.assertEqual(path.read_text(encoding='utf-8'), text)

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
