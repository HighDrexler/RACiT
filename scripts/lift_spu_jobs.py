#!/usr/bin/env python3
"""Lift raw SPU job binaries embedded in the EBOOT as content-matched overlays.

Insomniac's igPm SPURS policy module DMAs each job binary into LS 0x100 and
branches into it, so dozens of different programs share the same local-store
addresses. Each job starts with a 16-byte GUID header (four `ila $r2,...`).
This script finds those headers, slices each job up to the next header, lifts
it at base 0x100 under its own image id, and writes game/spu/spu_jobs.c, which
registers the functions and declares each image an overlay
(spu_register_overlay_image): the runtime serves a job's lifted code only when
the local store holds that job's GUID and code words.

Usage: scripts/lift_spu_jobs.py [input/EBOOT.ELF] [--only 0xB9B580,0xB8A980,...]
"""
import os, struct, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PS3R = os.path.join(ROOT, "tools", "ps3recomp", "tools")
JOB_BASE = 0x100
IMAGE_ID_BASE = 200
# Raw job region (ELF images follow). Starts just past igPm's own runtime and
# policy module (0xAD8F80 / 0xAD9500, handled by EXTRA_BLOCKS / the PM lift):
# Bink's SPU video decoder lives at 0xADB700 and the old 0xB00000 start missed
# it, so every movie frame was decoded by the SPU interpreter.
SCAN_LO, SCAN_HI = 0xAD9780, 0xBE3C80
MAX_SIZE = 0x30000
# SPU ELF jobs already lifted by build_spu_workloads (image id, text EA, size,
# LS base): the WWS job manager stages them into a code buffer at LS 0x4000,
# so they too must be found by content. Sealed at boot from guest memory.
ELF_OVERLAYS = [
    (2, 0xBFD480, 0xDC30, 0x4000),   # nextsynth (SCREAM synth job)
    (3, 0xC0BB80, 0xB5D0, 0x4000),   # lr1 (reverb job)
]


def segs(d):
    phoff, = struct.unpack(">Q", d[0x20:0x28])
    phentsize, phnum = struct.unpack(">HH", d[0x36:0x3A])
    out = []
    for i in range(phnum):
        p = d[phoff + i * phentsize: phoff + (i + 1) * phentsize]
        ptype, _, off, vaddr, _, filesz, _, _ = struct.unpack(">IIQQQQQQ", p)
        if ptype == 1 and filesz:
            out.append((vaddr, off, filesz))
    return out


def rd(d, sg, a, n):
    for va, off, fs in sg:
        if va <= a and a + n <= va + fs:
            return d[off + a - va: off + a - va + n]
    return None


def find_headers(d, sg):
    hs = []
    for a in range(SCAN_LO, SCAN_HI, 16):
        b = rd(d, sg, a, 16)
        if not b:
            continue
        ws = struct.unpack(">4I", b)
        if all((w >> 25) == 0x21 and (w & 0x7F) == 2 for w in ws):
            hs.append(a)
    return hs


# Code blocks that a policy module copies somewhere in LS at run time, lifted
# straight from the EBOOT (name, EBOOT EA, size, LS base, image id). igPm puts
# the 0x580 bytes in front of its own image at LS 0x3F580 and calls into them
# (its job-loading/lock-line polling runtime); unlifted, every SPURS worker
# spun in the interpreter there. Written to game/spu/spu_extra.c by --extra.
EXTRA_BLOCKS = [
    ("igpm_rt", 0xAD8F80, 0x580, 0x3F580, 150),
]
# Lifter hints per block: job-style entry after the GUID header (not found by
# --auto-functions), and where code ends (variables follow).
EXTRA_HINTS = {
    "igpm_rt": ["--extra-funcs", "0x3F5A0", "--code-end", "0x3FA00"],
}


def lift_block(name, raw, base, out_raw, spu_dir, hints=()):
    bin_p = os.path.join(out_raw, name + ".bin")
    elf_p = os.path.join(out_raw, name + ".elf")
    open(bin_p, "wb").write(raw)
    subprocess.run([sys.executable, "-I", os.path.join(PS3R, "wrap_spu_elf.py"), bin_p,
                    "--out", elf_p, "--base", hex(base), "--entry", hex(base)], check=True,
                   capture_output=True)
    dst = os.path.join(spu_dir, name)
    os.makedirs(dst, exist_ok=True)
    r = subprocess.run([sys.executable, "-I", os.path.join(PS3R, "spu_lifter.py"),
                        "--auto-functions", elf_p, "--base", hex(base),
                        "--symbol-prefix", name + "_", "--output", dst, *hints],
                       capture_output=True, text=True)
    open(os.path.join(out_raw, name + ".lift.log"), "w").write(r.stdout + r.stderr)
    if r.returncode:
        return False
    c = os.path.join(dst, "spu_recomp.c")
    src = open(c).read()
    for h in ("spu_context.h", "spu_helpers.h"):
        src = src.replace(f'"../../runtime/spu/{h}"', f'"{h}"')
    open(c, "w").write(src)
    return True


def main_extra(d, sg):
    out_raw = os.path.join(ROOT, "out", "spu_jobs")
    spu_dir = os.path.join(ROOT, "game", "spu")
    os.makedirs(out_raw, exist_ok=True)
    decls, regs = [], []
    for name, ea, size, base, img in EXTRA_BLOCKS:
        raw = rd(d, sg, ea, size)
        if not lift_block(name, raw, base, out_raw, spu_dir, EXTRA_HINTS.get(name, ())):
            print(f"{name}: lifter FAILED, see out/spu_jobs/{name}.lift.log")
            continue
        decls.append(f"extern void {name}_spu_recomp_register(void);")
        regs.append(f"    spu_begin_image({img}); {name}_spu_recomp_register(); spu_begin_image(0);")
        print(f"{name}: 0x{ea:X} size 0x{size:X} at LS 0x{base:X} -> image {img}")
    with open(os.path.join(spu_dir, "spu_extra.c"), "w") as f:
        f.write("/* Generated by scripts/lift_spu_jobs.py --extra -- LS-relocated policy-module code. */\n"
                "extern void spu_begin_image(int image_id);\n"
                "extern void spu_register_overlay_image(int image_id, unsigned base, const unsigned char* bytes, unsigned size);\n"
                "extern unsigned char* vm_base;\n\n" + "\n".join(decls) + "\n\n"
                "__attribute__((constructor(201))) static void racacit_extra_register_ctor(void)\n{\n"
                + "\n".join(regs) + "\n}\n\n"
                "/* Content signatures come from the EBOOT in guest memory (main() calls this). */\n"
                "void racacit_register_extra_overlays(void)\n{\n"
                + "".join(f"    spu_register_overlay_image({img}, 0x{base:X}, vm_base + 0x{ea:X}u, 0x{size:X});\n"
                          for _, ea, size, base, img in EXTRA_BLOCKS) + "}\n")


# Entry points the interpreter was entered at (SPU_INTERP_STARTS=<file>,
# "<parent guid> <LS addr>" per line), merged into scripts/spu_starts.txt by
# --merge-starts: indirect-call targets --auto-functions cannot see.
STARTS = os.path.join(ROOT, "scripts", "spu_starts.txt")
SUBMODULES = os.path.join(ROOT, "scripts", "spu_submodules.txt")
SUB_IMAGE_ID_BASE = 400
SUB_ANCHOR = 0x30
SUB_ENTRIES = os.path.join(ROOT, "scripts", "spu_submodule_entries.txt")
# Modules found at a second base that have their own lift already.
SUB_SKIP = {(0xC17B00, 0xA00), (0xAD8F80, 0x3F580)}


def read_starts():
    """{parent guid hex: set(LS addr)}"""
    out = {}
    if os.path.exists(STARTS):
        for line in open(STARTS):
            p = line.split("#")[0].split()
            if len(p) >= 2:
                out.setdefault(p[0], set()).add(int(p[1], 16))
    return out


def merge_starts(path):
    cur = read_starts()
    n0 = sum(len(v) for v in cur.values())
    for line in open(path):
        p = line.split()
        if len(p) >= 2:
            cur.setdefault(p[0], set()).add(int(p[1], 16))
    with open(STARTS, "w", newline="\n") as f:
        f.write("# Addresses the SPU interpreter was entered at: <job GUID at LS 0x100> <LS addr>.\n"
                "# From SPU_INTERP_STARTS=<file>, merged by scripts/lift_spu_jobs.py --merge-starts <file>.\n")
        for g in sorted(cur):
            for a in sorted(cur[g]):
                f.write("%s %05X\n" % (g, a))
    n1 = sum(len(v) for v in cur.values())
    print("%d start(s) in scripts/spu_starts.txt (%d new)" % (n1, n1 - n0))


def all_headers(d, sg):
    hs = []
    for va, off, fs in sg:
        for a in range((va + 15) & ~15, va + fs - 16, 16):
            ws = struct.unpack(">4I", d[off + a - va: off + a - va + 16])
            if all((w >> 25) == 0x21 and (w & 0x7F) == 2 for w in ws):
                hs.append(a)
    return sorted(hs)


def main_sub(d, sg):
    """Job modules loaded above a parent job (scripts/spu_submodules.txt), each
    lifted at the LS base it was seen at -> game/spu/spu_sub.c."""
    out_raw = os.path.join(ROOT, "out", "spu_jobs")
    spu_dir = os.path.join(ROOT, "game", "spu")
    os.makedirs(out_raw, exist_ok=True)
    heads = all_headers(d, sg)
    starts = read_starts()
    mods = {}                                  # (ea, base) -> {parent guid}
    if os.path.exists(SUBMODULES):
        for line in open(SUBMODULES):
            p = line.split("#")[0].split()
            if len(p) >= 3:
                mods.setdefault((int(p[0], 16), int(p[1], 16)), set()).add(p[2])
    # Entry points as module offsets (scripts/dev/miss_bases.py --write).
    offsets = {}
    if os.path.exists(SUB_ENTRIES):
        for line in open(SUB_ENTRIES):
            p = line.split("#")[0].split()
            if len(p) >= 2:
                offsets.setdefault(int(p[0], 16), set()).add(int(p[1], 16))
    # One relocatable lift per module: a parent may load it at any base, and
    # the lifted code runs wherever the runtime finds it (ls_delta). Lift at
    # the first base recorded from a snapshot (else the lowest seen), and
    # translate every base's interpreter starts into that frame.
    by_ea = {}
    for (ea, base), parents in mods.items():
        if (ea, base) in SUB_SKIP or base == JOB_BASE:
            continue
        by_ea.setdefault(ea, []).append((base, parents))
    decls, regs, blobs = [], [], []
    img = SUB_IMAGE_ID_BASE
    for ea, seen in sorted(by_ea.items()):
        snap = sorted(b for b, ps in seen if ps - {"reloc"})
        base = snap[0] if snap else min(b for b, _ in seen)
        nxt = next((h for h in heads if h > ea), ea + MAX_SIZE)
        size = min(nxt - ea, MAX_SIZE, 0x40000 - base)
        raw = rd(d, sg, ea, size)
        if raw is None:
            print(f"sub {ea:06X}: not in a loaded segment, skipped")
            continue
        name = f"sub_{ea:06X}_{base:05X}"
        hints = set()
        for b, parents in seen:
            hints |= {a - b + base for g in parents for a in starts.get(g, ()) if b + SUB_ANCHOR <= a < b + size}
        # Recorded entries from 0x20 on: a GUID-headed module is entered at
        # (header word 6 & 0xFFFF) << 2, often 0x20, and the microstepper hands
        # its prologue to the interpreter at about 0x2C unless a lift starts
        # there. Never 0..0x1F: that "function" runs the header words to a stop.
        hints |= {o + base for o in offsets.get(ea, ()) if 0x20 <= o < size}
        hints = sorted(hints)
        args = ["--reloc"] + (["--extra-funcs", ",".join(hex(a) for a in hints)] if hints else [])
        if not lift_block(name, raw, base, out_raw, spu_dir, args):
            print(f"{name}: lifter FAILED (skipped), see out/spu_jobs/{name}.lift.log")
            continue
        decls.append(f"extern void {name}_spu_recomp_register(void);")
        blobs.append(f"static const unsigned char {name}_bytes[{size}] = {{" + ",".join(str(x) for x in raw) + "};")
        regs.append(f"    spu_begin_image({img}); {name}_spu_recomp_register();\n"
                    f"    spu_register_overlay_image_at({img}, 0x{base:X}, {name}_bytes, {size}, 0x{SUB_ANCHOR:X});\n"
                    f"    spu_mark_overlay_relocatable({img});")
        print(f"{name}: size 0x{size:X}, {len(seen)} base(s) seen, {len(hints)} entry hint(s) -> image {img}")
        img += 1
    with open(os.path.join(spu_dir, "spu_sub.c"), "w") as f:
        f.write("/* Generated by scripts/lift_spu_jobs.py --sub -- job modules a parent job loads above itself,\n"
                " * lifted relocatable (--reloc): served at whatever base the parent picks. */\n"
                "extern void spu_begin_image(int image_id);\n"
                "extern void spu_register_overlay_image_at(int image_id, unsigned base, const unsigned char* bytes, unsigned size, unsigned anchor);\n"
                "extern void spu_mark_overlay_relocatable(int image_id);\n\n"
                + "\n".join(decls) + "\n\n" + "\n".join(blobs) + "\n\n"
                "__attribute__((constructor(202))) static void racacit_sub_register_ctor(void)\n{\n"
                + "\n".join(regs) + "\n    spu_begin_image(0);\n}\n")
    print(f"{img - SUB_IMAGE_ID_BASE} module(s) -> game/spu/spu_sub.c")


def main():
    if "--merge-starts" in sys.argv:
        return merge_starts(sys.argv[sys.argv.index("--merge-starts") + 1])
    if "--sub" in sys.argv:
        args = [a for a in sys.argv[1:] if not a.startswith("--")]
        elf = args[0] if args else os.path.join(ROOT, "input", "EBOOT.ELF")
        d = open(elf, "rb").read()
        return main_sub(d, segs(d))
    if "--extra" in sys.argv:
        args = [a for a in sys.argv[1:] if not a.startswith("--")]
        elf = args[0] if args else os.path.join(ROOT, "input", "EBOOT.ELF")
        d = open(elf, "rb").read()
        return main_extra(d, segs(d))
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    only = None
    if "--only" in sys.argv:
        only = {int(x, 16) for x in sys.argv[sys.argv.index("--only") + 1].split(",")}
        args = [a for a in args if not a.startswith("0x")]
    elf = args[0] if args else os.path.join(ROOT, "input", "EBOOT.ELF")
    d = open(elf, "rb").read()
    sg = segs(d)
    heads = find_headers(d, sg)
    out_raw = os.path.join(ROOT, "out", "spu_jobs")
    spu_dir = os.path.join(ROOT, "game", "spu")
    os.makedirs(out_raw, exist_ok=True)
    decls, regs, blobs = [], [], []
    starts = read_starts()
    for i, a in enumerate(heads):
        if only and a not in only:
            continue
        nxt = heads[i + 1] if i + 1 < len(heads) else SCAN_HI
        size = min(nxt - a, MAX_SIZE)
        raw = rd(d, sg, a, size)
        name = f"job_{a:06X}"
        bin_p = os.path.join(out_raw, name + ".bin")
        elf_p = os.path.join(out_raw, name + ".elf")
        open(bin_p, "wb").write(raw)
        subprocess.run([sys.executable, "-I", os.path.join(PS3R, "wrap_spu_elf.py"), bin_p,
                        "--out", elf_p, "--base", hex(JOB_BASE), "--entry", hex(JOB_BASE)], check=True,
                       capture_output=True)
        dst = os.path.join(spu_dir, name)
        os.makedirs(dst, exist_ok=True)
        prefix = name + "_"
        # igPm enters a job at (header word 6 & 0xFFFF) << 2 -- an indirect
        # call --auto-functions cannot see, so name it, or the job's whole
        # top level runs in the interpreter.
        entry = (struct.unpack(">I", raw[0x18:0x1C])[0] & 0xFFFF) << 2
        extra = {entry} if JOB_BASE < entry < JOB_BASE + size else set()
        # ...and so are the places the interpreter was seen entering this job.
        extra |= {s for s in starts.get(raw[:16].hex(), ()) if JOB_BASE + 0x20 <= s < JOB_BASE + size}
        hints = ["--extra-funcs", ",".join(hex(x) for x in sorted(extra))] if extra else []
        r = subprocess.run([sys.executable, "-I", os.path.join(PS3R, "spu_lifter.py"),
                            "--auto-functions", elf_p, "--base", hex(JOB_BASE),
                            "--symbol-prefix", prefix, "--output", dst, *hints,
                            # One C function per run of blocks between calls:
                            # loops keep SPU registers in host registers. Not
                            # for --sub modules, which a parent may patch past
                            # an entry's validated span (see spu_lookup_fixed).
                            *([] if os.environ.get("RACACIT_SPU_REGIONS") == "0" else ["--regions"]),
                            # SPU registers in C locals where no call sits in a
                            # loop: opt-in (RACACIT_SPU_REGCACHE=1) -- slower
                            # in Tombli, no gain in Galacton.
                            *(["--reg-cache"] if os.environ.get("RACACIT_SPU_REGCACHE") == "1" else [])],
                           capture_output=True, text=True)
        open(os.path.join(out_raw, name + ".lift.log"), "w").write(r.stdout + r.stderr)
        if r.returncode:
            print(f"{name}: lifter FAILED (skipped), see out/spu_jobs/{name}.lift.log")
            continue
        c = os.path.join(dst, "spu_recomp.c")
        src = open(c).read()
        for h in ("spu_context.h", "spu_helpers.h"):
            src = src.replace(f'"../../runtime/spu/{h}"', f'"{h}"')
        open(c, "w").write(src)
        img = IMAGE_ID_BASE + i
        decls.append(f"extern void {prefix}spu_recomp_register(void);")
        blobs.append(f"static const unsigned char {name}_bytes[{size}] = {{"
                     + ",".join(str(x) for x in raw) + "};")
        regs.append(f"    spu_begin_image({img}); {prefix}spu_recomp_register();\n"
                    f"    spu_register_overlay_image({img}, 0x{JOB_BASE:X}, {name}_bytes, {size});")
        print(f"{name}: 0x{a:X} size 0x{size:X} -> image {img}")
    with open(os.path.join(spu_dir, "spu_jobs.c"), "w") as f:
        f.write("/* Generated by scripts/lift_spu_jobs.py -- igPm job binaries as content-matched overlays. */\n"
                "extern void spu_begin_image(int image_id);\n"
                "extern void spu_register_overlay_image(int image_id, unsigned base, const unsigned char* bytes, unsigned size);\n\n"
                + "\n".join(decls) + "\n\n" + "\n".join(blobs) + "\n\n"
                "void racacit_jobs_register_all(void)\n{\n" + "\n".join(regs) + "\n    spu_begin_image(0);\n}\n\n"
                "/* Called from main() once the EBOOT is in guest memory. */\n"
                "extern unsigned char* vm_base;\n"
                "void racacit_register_elf_overlays(void)\n{\n"
                + "".join(f"    spu_register_overlay_image({i}, 0x{b:X}, vm_base + 0x{ea:X}u, 0x{sz:X});\n"
                          for i, ea, sz, b in ELF_OVERLAYS) + "}\n\n"
                "__attribute__((constructor(200))) static void racacit_jobs_register_ctor(void)\n"
                "{\n    racacit_jobs_register_all();\n}\n")


if __name__ == "__main__":
    main()
