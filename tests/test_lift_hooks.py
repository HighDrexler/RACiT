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
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CHECK = ROOT / 'scripts' / 'check_lift_hooks.py'
PROFILES = ('BCUS98124-1.00', 'BCUS98124-1.20')
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

    def test_menu_and_camera_hooks_are_checked_on_every_version(self):
        for profile in PROFILES:
            hooks, functions = hook_list(profile)
            for name in ('racit_camera_take', 'racit_publish_menu_keyboard', 'racacit_front_end_cards'):
                self.assertIn(name, hooks, profile)
            self.assertTrue(functions, profile + ' needs @function lines to recognise its lift')


class CheckLiftHooks(unittest.TestCase):
    """The checker on small lifts. No stamp tricks: --patch re-patches when there is no
    stamp, and without --patch nothing is ever patched."""

    def lift(self, directory, hooks, functions, leave_out=(), chunks=1, extra=''):
        """A lift with every hook called as often as listed, spread over `chunks` files.
        Each file declares and defines the hooks the way patch_lift.py's preambles do."""
        calls = [name for name, count in hooks.items() if name not in leave_out for _ in range(count)]
        files = [[] for _ in range(chunks)]
        for i, name in enumerate(calls):
            files[i % chunks].append(name)
        for function in functions:
            files[0].append('@' + function)
        for index, names in enumerate(files):
            body = ['#include "ppu_recomp.h"']
            for name in (n for n in hooks if n not in leave_out):   # in every chunk; not calls
                body.append('extern "C" void %s(ppu_context* ctx);' % name)
                body.append('static inline unsigned long long %s_now(void) { return 0; }' % name)
            body.append('static inline void %s(ppu_context* ctx) { }' % 'racit_unused_helper')
            body.append('void func_%08X(ppu_context* ctx) {' % (0x10000 + index))
            for name in names:
                if not name.startswith('@'):
                    body.append('    %s(ctx);' % name)
            body.append('}')
            for name in names:
                if name.startswith('@'):
                    # Named in a comment (which the check doesn't count), so patch_lift.py
                    # takes these as patched already and leaves them be.
                    body.extend(['void func_%s(ppu_context* ctx) {' % name[1:],
                                 '        /* patched: %s */' % ' '.join(n for n in hooks if n not in leave_out), '}'])
            if index == 0:
                body.append(extra)
            (Path(directory) / ('ppu_recomp_%03d.cpp' % index)).write_text('\n'.join(body) + '\n', encoding='utf-8')

    def run_check(self, directory, profile, *flags):
        return subprocess.run([sys.executable, '-I', str(CHECK), directory, '--profile', profile] + list(flags),
                              capture_output=True, text=True)

    def test_complete_lift_passes(self):
        for profile in PROFILES:
            hooks, functions = hook_list(profile)
            with tempfile.TemporaryDirectory() as directory:
                self.lift(directory, hooks, functions)
                result = self.run_check(directory, profile)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_hook_stops_the_build(self):
        for profile in PROFILES:
            hooks, functions = hook_list(profile)
            with tempfile.TemporaryDirectory() as directory:
                self.lift(directory, hooks, functions, leave_out=('racit_publish_menu_keyboard',))
                result = self.run_check(directory, profile)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('racit_publish_menu_keyboard', result.stdout + result.stderr)

    def test_verify_only_never_patches(self):
        hooks, functions = hook_list('BCUS98124-1.00')
        with tempfile.TemporaryDirectory() as directory:
            self.lift(directory, hooks, functions, leave_out=('racacit_lua_throw_hook',),
                      extra='void func_0050A83C(ppu_context* ctx) {\n        return;\n}')
            before = {p.name: p.read_text() for p in Path(directory).glob('*.cpp')}
            result = self.run_check(directory, 'BCUS98124-1.00', '--stamp', str(Path(directory) / 'ok'))
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('re-running', result.stdout)
            self.assertEqual({p.name: p.read_text() for p in Path(directory).glob('*.cpp')}, before)
            self.assertFalse((Path(directory) / 'ok').exists())

    def test_patch_mode_repatches_a_stale_lift(self):
        # A lift patched before racacit_lua_throw_hook existed: --patch runs patch_lift.py,
        # which puts it in, and the check passes; a second run with the stamp leaves it be.
        hooks, functions = hook_list('BCUS98124-1.00')
        with tempfile.TemporaryDirectory() as directory:
            self.lift(directory, hooks, functions, leave_out=('racacit_lua_throw_hook',),
                      extra='void func_0050A83C(ppu_context* ctx) {\n        return;\n}')
            stamp = Path(directory) / 'patched.ok'
            result = self.run_check(directory, 'BCUS98124-1.00', '--patch', '--stamp', str(stamp))
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('re-running patch_lift.py', result.stdout)
            self.assertIn('racacit_lua_throw_hook(ctx);', (Path(directory) / 'ppu_recomp_000.cpp').read_text())
            self.assertTrue(stamp.exists())
            again = self.run_check(directory, 'BCUS98124-1.00', '--patch', '--stamp', str(stamp))
            self.assertEqual(again.returncode, 0, again.stdout + again.stderr)
            self.assertNotIn('re-running', again.stdout)



if __name__ == '__main__':
    unittest.main()
