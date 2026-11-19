#!/usr/bin/env python3
"""Bring a lift's hooks up to date and check that every expected hook is there.

    check_lift_hooks.py RECOMP --profile PROFILE [--elf EBOOT.ELF] [--stamp FILE]

A lift (game/src/recomp, out/<profile>/recomp) is generated once and patched
by scripts/patch_lift.py. A lift patched before a hook was added builds
without it, silently. So, when patch_lift.py, one of its inputs or the lift itself is
newer than the stamp (or there is no stamp), this re-runs patch_lift.py on the lift
(it is idempotent: hooks already in place are left alone). Then every hook
listed in versions/<profile>-hook-symbols.txt must be called in the lifted
sources at least as often as listed, or the build stops with the list of
missing hooks. The stamp is written only when the check passes.
"""
import argparse
import glob
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
# What patch_lift.py reads: a change to any of them re-patches the lift.
PATCHER_INPUTS = ['scripts/patch_lift.py', 'scripts/update120_hooks.py', 'scripts/fix_vmx_alias.py',
                  'scripts/game_version.py', 'versions/profiles.json']


def expected_hooks(profile):
    path = os.path.join(ROOT, 'versions', profile + '-hook-symbols.txt')
    if not os.path.exists(path):
        sys.exit('check_lift_hooks: no expected-hook list for %s (%s)' % (profile, path))
    hooks, functions = {}, []
    with open(path, encoding='utf-8') as f:
        for line in f:
            line = line.split('#', 1)[0].strip()
            if line.startswith('@function '):
                functions.append(line.split()[1])
            elif line:
                name, count = line.split()
                hooks[name] = int(count)
    return path, hooks, functions


def check_identity(sources, profile, functions):
    # Patching another version's lift with this profile's hooks would corrupt it.
    wanted = {'void func_%s(ppu_context* ctx) {' % f for f in functions}
    for path in sources:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith('void func_'):
                    wanted.discard(line.rstrip('\n'))
        if not wanted:
            return
    sys.exit('check_lift_hooks: %s is not a %s lift (it has no %s). Check RECOMP_DIR, '
             'RACACIT_GAME_PROFILE and HOST_DIR; nothing was patched.'
             % (os.path.dirname(sources[0]), profile, ', '.join(sorted(w[5:w.index('(')] for w in wanted))))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('recomp')
    parser.add_argument('--profile', required=True)
    parser.add_argument('--elf', default='')
    parser.add_argument('--stamp', default='')
    options = parser.parse_args()
    sources = sorted(glob.glob(os.path.join(options.recomp, 'ppu_recomp_*.cpp')))
    if not sources:
        sys.exit('check_lift_hooks: no lifted sources in %s' % options.recomp)
    hook_list, hooks, functions = expected_hooks(options.profile)

    inputs = [os.path.join(ROOT, p) for p in PATCHER_INPUTS] + [hook_list]
    inputs += glob.glob(os.path.join(ROOT, 'versions', options.profile + '-hooks.json'))
    newest = max(os.path.getmtime(p) for p in inputs if os.path.exists(p))
    # A regenerated lift (sources newer than the stamp) is patched again too.
    newest = max([newest] + [os.path.getmtime(p) for p in sources])
    stale = not options.stamp or not os.path.exists(options.stamp) or os.path.getmtime(options.stamp) < newest
    if stale:
        check_identity(sources, options.profile, functions)
        command = [sys.executable, '-I', os.path.join(ROOT, 'scripts', 'patch_lift.py'), options.recomp]
        if options.elf:
            command += ['--elf', options.elf, '--profile', options.profile]
        elif options.profile != 'BCUS98124-1.00':
            sys.exit('check_lift_hooks: re-patching the %s lift needs its executable: '
                     'configure with -DRACACIT_ELF=<its decrypted EBOOT.ELF>' % options.profile)
        print('check_lift_hooks: re-running patch_lift.py on %s' % options.recomp, flush=True)
        subprocess.run(command, check=True)

    counts = dict.fromkeys(hooks, 0)
    call = re.compile(r'\b(' + '|'.join(map(re.escape, hooks)) + r')\(')
    for path in sources:
        with open(path, encoding='utf-8', errors='replace') as f:
            for line in f:
                if line.startswith(('extern ', '#', 'typedef ')):
                    continue
                for match in call.finditer(line):
                    counts[match.group(1)] += 1
    missing = ['%s (%d of %d)' % (n, counts[n], want) for n, want in hooks.items() if counts[n] < want]
    if missing:
        sys.exit('check_lift_hooks: the %s lift in %s lacks hooks that %s expects:\n  %s\n'
                 'Re-run scripts/patch_lift.py on it (or regenerate it) and rebuild; '
                 'a build without them must not be staged.'
                 % (options.profile, options.recomp, os.path.relpath(hook_list, ROOT), '\n  '.join(missing)))
    print('check_lift_hooks: %s: all %d expected hooks present in %s' % (options.profile, len(hooks), options.recomp))
    if options.stamp:
        with open(options.stamp, 'w') as f:
            f.write('ok\n')


if __name__ == '__main__':
    main()
