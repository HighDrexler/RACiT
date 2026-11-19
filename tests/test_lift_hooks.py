"""The build refuses a lift that lacks a hook, and the hook lists cover every hook.

Twice a player build shipped without hooks (no mouse follow camera, no menu
keycaps) because its lift was patched before they were added.
scripts/check_lift_hooks.py, run by game/CMakeLists.txt, re-patches a stale lift
and stops the build if a hook in versions/<profile>-hook-symbols.txt is missing.
This needs no game files.

    python -m unittest discover -s tests
"""
import json
import os
import re
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / 'scripts' / 'check_lift_hooks.py'
HOOK = re.compile(r'\b((?:racacit|racit)_[a-z0-9_]+)\(')


def hook_list(profile):
    hooks, functions = {}, []
    for line in (ROOT / 'versions' / (profile + '-hook-symbols.txt')).read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if line.startswith('@function '):
            functions.append(line.split()[1])
        elif line:
            name, count = line.split()
            hooks[name] = int(count)
    return hooks, functions


def inserted_by_patch_lift():
    # Hook calls patch_lift.py writes into lifted functions: string literals that call a
    # racacit_/racit_ function with the guest context or guest registers.
    source = (ROOT / 'scripts' / 'patch_lift.py').read_text(encoding='utf-8')
    names = set()
    for literal in re.findall(r"'((?:\\.|[^'\\\n])*)'|\"((?:\\.|[^\"\\\n])*)\"", source):
        text = literal[0] or literal[1]
        if 'extern' in text or 'void racacit' in text or 'void racit' in text or 'int racacit' in text:
            continue
        for name in HOOK.findall(text):
            if 'ctx' in text:
                names.add(name)
    return names


def inserted_by_update120_recipe():
    recipe = json.loads((ROOT / 'versions' / 'BCUS98124-1.20-hooks.json').read_text(encoding='utf-8'))
    names = set()
    for row in recipe['hooks']:
        for edit in row.get('edits', []):
            names.update(HOOK.findall(edit[2]))
    return names


class HookLists(unittest.TestCase):
    def test_disc_list_has_every_patch_lift_hook(self):
        hooks, _ = hook_list('BCUS98124-1.00')
        missing = sorted(inserted_by_patch_lift() - set(hooks))
        self.assertEqual(missing, [], 'add these to versions/BCUS98124-1.00-hook-symbols.txt')

    def test_update_list_has_every_recipe_hook(self):
        hooks, _ = hook_list('BCUS98124-1.20')
        missing = sorted(inserted_by_update120_recipe() - set(hooks))
        self.assertEqual(missing, [], 'add these to versions/BCUS98124-1.20-hook-symbols.txt')

    def test_menu_and_camera_hooks_are_checked_on_both_versions(self):
        for profile in ('BCUS98124-1.00', 'BCUS98124-1.20'):
            hooks, functions = hook_list(profile)
            for name in ('racit_camera_take', 'racit_publish_menu_keyboard', 'racacit_front_end_cards'):
                self.assertIn(name, hooks, profile)
            self.assertTrue(functions, profile + ' needs @function lines to recognise its lift')


class CheckLiftHooks(unittest.TestCase):
    def lift(self, directory, hooks, functions, leave_out=()):
        body = ['#include "ppu_recomp.h"', 'extern "C" void racit_camera_take(ppu_context* ctx);']
        for function in functions:
            body.append('void func_%s(ppu_context* ctx) {' % function)
            body.append('}')
        body.append('void func_00010000(ppu_context* ctx) {')
        for name, count in hooks.items():
            if name not in leave_out:
                body.extend(['    %s(ctx);' % name] * count)
        body.append('}')
        (Path(directory) / 'ppu_recomp_000.cpp').write_text('\n'.join(body) + '\n', encoding='utf-8')
        # A stamp newer than the lift and every patcher input: check only, no re-patch.
        stamp = Path(directory) / 'lift-hooks.ok'
        stamp.write_text('ok\n')
        future = time.time() + 3600
        os.utime(stamp, (future, future))
        return stamp

    def run_check(self, directory, profile, stamp):
        return subprocess.run([sys.executable, '-I', str(CHECK), directory, '--profile', profile, '--stamp', str(stamp)],
                              capture_output=True, text=True)

    def test_complete_lift_passes(self):
        for profile in ('BCUS98124-1.00', 'BCUS98124-1.20'):
            hooks, functions = hook_list(profile)
            with tempfile.TemporaryDirectory() as directory:
                stamp = self.lift(directory, hooks, functions)
                result = self.run_check(directory, profile, stamp)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_hook_stops_the_build(self):
        for profile in ('BCUS98124-1.00', 'BCUS98124-1.20'):
            hooks, functions = hook_list(profile)
            with tempfile.TemporaryDirectory() as directory:
                stamp = self.lift(directory, hooks, functions, leave_out=('racit_publish_menu_keyboard',))
                os.remove(stamp)
                stamp.write_text('ok\n')
                future = time.time() + 3600
                os.utime(stamp, (future, future))
                result = self.run_check(directory, profile, stamp)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('racit_publish_menu_keyboard', result.stdout + result.stderr)


if __name__ == '__main__':
    unittest.main()
