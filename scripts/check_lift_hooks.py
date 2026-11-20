#!/usr/bin/env python3
"""Bring a lift's hooks up to date, and check that every expected hook is there.

    check_lift_hooks.py RECOMP --profile PROFILE [--elf EBOOT.ELF] [--patch] [--stamp FILE]

A lift (game/src/recomp, out/<profile>/recomp) is generated once and patched
by scripts/patch_lift.py. A lift patched before a hook was added builds
without it, silently. Every hook listed in versions/<profile>-hook-symbols.txt
must be called in the lifted sources at least as often as listed, or this stops
with the list of missing hooks. Calls are counted in code only: comments,
declarations and definitions don't count, so a commented-out hook is missing.

--patch (game/CMakeLists.txt runs it at configure time): first, when
patch_lift.py, one of its inputs or the lift itself is newer than the stamp (or
there is no stamp), re-run patch_lift.py on the lift (it is idempotent: hooks
already in place are left alone). Without --patch (the build step) it only
checks, and never writes a source. The stamp is written only when the check passes.
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


def code_only(text):
    """text with its comments and string and character literals blanked out (newlines kept)."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(''.join(ch if ch == '\n' else ' ' for ch in text[i:j])); i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i)); i = j
        elif c in '"\'':
            j = i + 1
            while j < n and text[j] != c and text[j] != '\n':
                j += 2 if text[j] == '\\' else 1
            out.append(c + ' ' * (min(j, n) - i - 1) + (c if j < n else '')); i = j + 1
        else:
            out.append(c); i += 1
    return ''.join(out)


# A name after one of these is a call ("return racit_x(...)"); after any other word, a
# type ("int racit_x(...)"), so a declaration or a definition.
CALL_KEYWORDS = {'return', 'else', 'do', 'case', 'throw', 'co_return'}


LIFTED_FUNCTION = re.compile(r'void func_[0-9A-F]{8}\(ppu_context\* ctx\) \{$')


def lifted_body_lines(code):
    """The lines inside lifted function bodies ("void func_XXXXXXXX(ppu_context* ctx) {"
    to the "}" that closes it at the start of a line), without preprocessor lines and
    what a constant-false #if / #elif leaves out. A condition that isn't a plain 0 or 1
    counts as true (the lifted code only uses #if 0 to switch code off)."""
    def value(expr):
        expr = expr.split('//', 1)[0].strip()
        return False if expr == '0' else True
    # One entry per open #if: [code here is live, a branch was taken, the enclosing was live]
    stack = []
    live = True
    inside = False
    for line in code.split('\n'):
        stripped = line.strip()
        if stripped.startswith('#'):
            word = stripped[1:].strip()
            name = re.match(r'[a-z]*', word).group(0)
            rest = word[len(name):]
            if name in ('if', 'ifdef', 'ifndef'):
                cond = value(rest) if name == 'if' else True
                stack.append([live and cond, cond, live])
            elif name == 'elif' and stack:
                top = stack[-1]
                cond = not top[1] and value(rest)
                top[0] = top[2] and cond
                top[1] = top[1] or cond
            elif name == 'else' and stack:
                top = stack[-1]
                top[0] = top[2] and not top[1]
                top[1] = True
            elif name == 'endif' and stack:
                stack.pop()
            live = stack[-1][0] if stack else True
            continue
        if not live:
            continue
        if not inside:
            inside = bool(LIFTED_FUNCTION.match(line))
            continue
        if line.startswith('}'):
            inside = False
            continue
        yield line


def count_calls(sources, hooks):
    """How often each hook is called in the lifted sources: real calls only."""
    counts = dict.fromkeys(hooks, 0)
    if not hooks:
        return counts
    call = re.compile(r'\b(' + '|'.join(map(re.escape, hooks)) + r')\s*\(')
    before = re.compile(r'([A-Za-z_][A-Za-z0-9_]*|[*&])\s*$')
    for path in sources:
        with open(path, encoding='utf-8', errors='replace') as f:
            code = code_only(f.read())
        for line in lifted_body_lines(code):
            for match in call.finditer(line):
                prior = before.search(line, 0, match.start())
                if prior and prior.group(1) not in CALL_KEYWORDS:
                    continue   # "int name(" / "void* name(": a declaration or a definition
                counts[match.group(1)] += 1
    return counts


def main():
    # Paths in the messages can be anything (a Windows checkout under C:\Users\李): never let the
    # console's code page (cp1252) stop a build whose hooks are fine.
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, 'reconfigure'):
            stream.reconfigure(encoding='utf-8', errors='replace')
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('recomp')
    parser.add_argument('--profile', required=True)
    parser.add_argument('--elf', default='')
    parser.add_argument('--stamp', default='')
    parser.add_argument('--patch', action='store_true', help='re-patch a stale lift first (configure time)')
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
    # Every run, patching or not: another version's lift must not pass as this one.
    check_identity(sources, options.profile, functions)
    stale = options.patch and (not options.stamp or not os.path.exists(options.stamp) or os.path.getmtime(options.stamp) < newest)
    if stale:
        command = [sys.executable, '-I', '-X', 'utf8', os.path.join(ROOT, 'scripts', 'patch_lift.py'), options.recomp]
        if options.elf:
            command += ['--elf', options.elf, '--profile', options.profile]
        elif options.profile != 'BCUS98124-1.00':
            sys.exit('check_lift_hooks: re-patching the %s lift needs its executable: '
                     'configure with -DRACACIT_ELF=<its decrypted EBOOT.ELF>' % options.profile)
        print('check_lift_hooks: re-running patch_lift.py on %s' % options.recomp, flush=True)
        subprocess.run(command, check=True)

    counts = count_calls(sources, hooks)
    missing = ['%s (%d of %d)' % (n, counts[n], want) for n, want in hooks.items() if counts[n] < want]
    if missing:
        sys.exit('check_lift_hooks: the %s lift in %s lacks hooks that %s expects:\n  %s\n'
                 'Re-run scripts/patch_lift.py on it (or regenerate it) and rebuild; '
                 'a build without them must not be staged.'
                 % (options.profile, options.recomp, os.path.relpath(hook_list, ROOT), '\n  '.join(missing)))
    print('check_lift_hooks: %s: all %d expected hooks present in %s' % (options.profile, len(hooks), options.recomp))
    if options.stamp:
        with open(options.stamp, 'w', encoding='utf-8') as f:
            f.write('ok\n')


if __name__ == '__main__':
    main()
