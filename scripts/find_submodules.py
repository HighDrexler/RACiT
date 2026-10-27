#!/usr/bin/env python3
"""find_submodules.py <dump dir> -- which job modules sit where in local store?

igPm loads a job at LS 0x100, and a job may load further job modules above
itself at a base of its own choosing (39 of them live in the EBOOT's data
segment, 0xDB4180..0xDDD380, outside lift_spu_jobs.py's main scan). Those ran
in the SPU interpreter: one second per frame in the Great Clock.

Reads the local-store snapshots the runtime writes with SPU_DUMP_JOBS=<dir>
(one per parent job the first time it reaches unlifted code), finds every job
module from the EBOOT at an address other than 0x100, and merges the
(module EA, LS base, parent GUID) rows into scripts/spu_submodules.txt, which
`lift_spu_jobs.py --sub` lifts as content-matched overlays at those bases.
Run the game again afterwards: a parent that loads several modules only dumps
at its first miss, so repeat until SPU_INTERP_REPORT=1 is quiet.

A module is recognised by its code, not its header: once a module is in place
its first 0x30 bytes are rewritten (the GUID reads DEADDEAD... or zero).
"""
import glob
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TABLE = os.path.join(ROOT, "scripts", "spu_submodules.txt")
ANCHOR = 0x30     # where a loaded module still matches the EBOOT
SPAN = 0x100      # bytes that must match from the anchor


def load_elf(path):
    d = open(path, "rb").read()
    phoff, = struct.unpack(">Q", d[0x20:0x28])
    phentsize, phnum = struct.unpack(">HH", d[0x36:0x3A])
    sg = []
    for i in range(phnum):
        p = d[phoff + i * phentsize: phoff + (i + 1) * phentsize]
        ptype, _, off, vaddr, _, filesz, _, _ = struct.unpack(">IIQQQQQQ", p)
        if ptype == 1 and filesz:
            sg.append((vaddr, off, filesz))
    return d, sg


def all_headers(d, sg):
    """EA of every 16-byte job header (four `ila $r2, imm`) in the image."""
    hs = []
    for va, off, fs in sg:
        for a in range((va + 15) & ~15, va + fs - 16, 16):
            ws = struct.unpack(">4I", d[off + a - va: off + a - va + 16])
            if all((w >> 25) == 0x21 and (w & 0x7F) == 2 for w in ws):
                hs.append(a)
    return sorted(hs)


def rd(d, sg, a, n):
    for va, off, fs in sg:
        if va <= a and a + n <= va + fs:
            return d[off + a - va: off + a - va + n]
    return None


def read_table():
    """{(module EA, LS base, parent GUID hex)}"""
    rows = set()
    if os.path.exists(TABLE):
        for line in open(TABLE):
            p = line.split("#")[0].split()
            if len(p) >= 3:
                rows.add((int(p[0], 16), int(p[1], 16), p[2]))
    return rows


def main():
    dump_dir = sys.argv[1]
    elf = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "input", "EBOOT.ELF")
    d, sg = load_elf(elf)
    heads = all_headers(d, sg)
    by_code = {}
    for h in heads:
        c = rd(d, sg, h + ANCHOR, 16)
        if c and c.count(0) < 12:
            by_code.setdefault(c, []).append(h)

    def score(ls, base, ea):
        """Matching 16-byte blocks over the module's length (to the next header)."""
        nxt = next((h for h in heads if h > ea), ea + 0x30000)
        size = min(nxt - ea, 0x30000, len(ls) - base)
        raw = rd(d, sg, ea, size)
        if raw is None:
            return 0
        return sum(1 for o in range(ANCHOR, size - 15, 16) if ls[base + o:base + o + 16] == raw[o:o + 16])

    rows = read_table()
    before = len(rows)
    for path in sorted(glob.glob(os.path.join(dump_dir, "*.ls"))):
        ls = open(path, "rb").read()
        parent = ls[0x100:0x110].hex()
        for base in range(0, len(ls) - ANCHOR - SPAN, 16):
            if base == 0x100:
                continue                      # the parent itself (lifted by the main scan)
            cands = [ea for ea in by_code.get(ls[base + ANCHOR:base + ANCHOR + 16], ())
                     if rd(d, sg, ea + ANCHOR, SPAN) == ls[base + ANCHOR:base + ANCHOR + SPAN]]
            if not cands:
                continue
            # modules can share their opening code: keep the one that matches furthest
            ea = max(cands, key=lambda e: score(ls, base, e))
            if (ea, base, parent) not in rows:
                rows.add((ea, base, parent))
                print("%s: module %06X at LS %05X" % (parent[:8], ea, base))
    with open(TABLE, "w", newline="\n") as f:
        f.write("# Job modules a parent job loads above itself:\n"
                "#   <module EBOOT EA> <LS base> <parent job GUID (LS 0x100)>   (hex)\n"
                "# Generated and merged by scripts/find_submodules.py from SPU_DUMP_JOBS snapshots;\n"
                "# lifted by scripts/lift_spu_jobs.py --sub as content-matched overlays at these bases.\n")
        for ea, base, parent in sorted(rows):
            f.write("%06X %05X %s\n" % (ea, base, parent))
    print("%d row(s) in %s (%d new)" % (len(rows), os.path.relpath(TABLE, ROOT), len(rows) - before))


if __name__ == "__main__":
    main()
