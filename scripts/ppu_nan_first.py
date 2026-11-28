#!/usr/bin/env python3
"""ppu_nan_first.py [--check]: the lifted PPU files' float helpers (ppu_fp_bin, ppu_fmadd_core) test the
result for NaN first. 2026-11-26.

Any NaN operand makes the result of +, -, *, / and fma a NaN, so a result that is not a NaN proves no
operand was one: one compare where there were three (four for the fused forms), and the operand order
that picks which NaN propagates (Book I 4.6.5.2) runs only when the result is a NaN. Same value in every
case. In func_006417C0 (a hot polynomial on the main thread) the common path went from 457 to 313 ARM
instructions. Applies to every game/src/recomp/*.cpp and game/prx/*/*.cpp that carries the helpers
(the lifter's preamble), keeping each file's line endings; tools/ps3recomp/tools/ppu_lifter.py has the
same text, so a new lift emits it.
"""
import glob, os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OLD = [("""static inline double ppu_fp_bin(double a, double b, double r)
{
    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    if (r != r) return ppu_fp_default_qnan();
    return r;
}""", """static inline double ppu_fp_bin(double a, double b, double r)
{
    /* A NaN operand makes r a NaN: r not a NaN, no operand was one (scripts/ppu_nan_first.py). */
    if (__builtin_expect(r == r, 1)) return r;
    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    return ppu_fp_default_qnan();
}"""), ("""    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    if (c != c) return ppu_fp_quiet(c);
    double r = fma(a, c, neg_b ? -b : b);
    if (r != r) return ppu_fp_default_qnan();
    return neg_res ? -r : r;""", """    double r = fma(a, c, neg_b ? -b : b);
    if (__builtin_expect(r == r, 1)) return neg_res ? -r : r;
    if (a != a) return ppu_fp_quiet(a);
    if (b != b) return ppu_fp_quiet(b);
    if (c != c) return ppu_fp_quiet(c);
    return ppu_fp_default_qnan();""")]


def fix(path, check):
    raw = open(path, 'rb').read().decode('utf-8', 'surrogateescape')
    s = raw; n = 0
    for old, new in OLD:
        for nl in ('\r\n', '\n'):
            o = old.replace('\n', nl); w = new.replace('\n', nl)
            c = s.count(o)
            if c: s = s.replace(o, w); n += c
    if n and not check:
        open(path, 'wb').write(s.encode('utf-8', 'surrogateescape'))
    return n


def main():
    check = '--check' in sys.argv
    files = sorted(glob.glob(os.path.join(ROOT, 'game', 'src', 'recomp', '*.cpp')) +
                   glob.glob(os.path.join(ROOT, 'game', 'prx', '*', '*.cpp')) +
                   [os.path.join(ROOT, 'tools', 'ps3recomp', 'tools', 'ppu_lifter.py')])
    total = 0; nf = 0
    for f in files:
        n = fix(f, check)
        if n: nf += 1; total += n
    print(f'{nf} files, {total} helpers rewritten' + (' [check only]' if check else ''))


if __name__ == '__main__':
    main()
