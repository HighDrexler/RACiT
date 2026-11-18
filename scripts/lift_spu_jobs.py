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
# Into the Nexus (2026-11-10): the raw jobs start right after the igPm policy
# module (0xC14800) and its runtime block (0xC14A80 + 0x580), and run up to the
# first embedded SPU ELF image (file 0xD09B00 = EA 0xD19B00). Every job the game
# ran up to the first level (SPU_DUMP_JOBS, out/spu_job_dumps_20261110: 18 GUIDs)
# has its header in this range. (ACiT's range was 0xAD9780..0xBE3C80.) More
# GUID-shaped headers sit in the data segment at 0xE52280..0xE7D880: not lifted,
# not seen running yet.
SCAN_LO, SCAN_HI = 0xC15000, 0xD19B00
MAX_SIZE = 0x30000
# SPU ELF jobs already lifted by build_spu_workloads (image id, text EA, size,
# LS base) that the WWS job manager stages into its code buffer at LS 0x4000,
# so they must also be found by content (sealed at boot from guest memory).
# Into the Nexus (2026-11-10): the two embedded ELFs that load at 0x4000, the
# audio synth and reverb jobs (same sizes as ACiT's nextsynth 0xDC30 and lr1);
# text at ELF + 0x80. Unregistered, they ran in the SPU interpreter: the audio
# job lists finished late (the game's jobarraycontainer.cpp(105) / joblist.cpp
# asserts), frames hitched and audio starved once the job manager ran.
ELF_OVERLAYS = [
    (2, 0xD23080, 0xDC30, 0x4000),   # ELF at EA 0xD23000 (file 0xD13000): synth
    (3, 0xD31780, 0xB550, 0x4000),   # ELF at EA 0xD31700 (file 0xD21700): reverb
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
# Into the Nexus: the igPm runtime sits right after the igPm policy module
# (0xC14800 + 560, at 0xC14A80; ACiT's was 0x580 before its module, 0xAD8F80).
# Same 0x580 layout and LS base; its code ends at LS 0x3F9B0 (ACiT 0x3FA00),
# variables at 0x3FA80. Found by matching 16-byte pieces of ACiT's (80 hits).
EXTRA_BLOCKS = [
    ("igpm_rt", 0xC14A80, 0x580, 0x3F580, 150),
]
# Lifter hints per block: job-style entry after the GUID header (not found by
# --auto-functions), and where code ends (variables follow).
EXTRA_HINTS = {
    "igpm_rt": ["--extra-funcs", "0x3F5A0", "--code-end", "0x3F9B0"],
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


def patch_igpm_clear(path):
    """igPm clears local store 16 bytes a turn before each job: a plain loop, not
    two trampoline hops per turn (a large part of a busy lane's time).

    The loop is `stqd r5,0(r3); ai r3,r3,16` then `ai r4,r4,-1; ceqi rT,r4,-1;
    brz rT` back. A Crack in Time has the decrement at 0x3F90C (rT = r66, store at
    0x3F904); Into the Nexus at 0x3F8C0 (rT = r56, store at 0x3F8B8). Nexus ran
    the lifted two-hop form until 2026-11-16 (this matched ACiT's addresses only):
    190-490 us before job 42C62A02 started, ~200 us before 42A41602, every frame,
    while the main thread waited for those jobs (Phase 4r)."""
    with open(path, newline="") as f:
        text = f.read()
    eol = chr(13) + chr(10) if chr(13) in text else chr(10)
    for dec, reg, store in (("0003F90C", 66, "0003F904"), ("0003F8C0", 56, "0003F8B8")):
        old = [
            f"void igpm_rt_spu_func_{dec}(spu_context* SPU_RESTRICT ctx) {{",
            "        ctx->gpr[4] = spu_ai(ctx->gpr[4], -1);",
            f"        ctx->gpr[{reg}] = spu_ceqi(ctx->gpr[4], -1);",
            f"        if (ctx->gpr[{reg}]._u32[0] == 0) {{ ctx->pc = 0x{int(store, 16):X}; "
            f"g_spu_trampoline_fn = igpm_rt_spu_func_{store}; return; }}",
        ]
        new = [
            old[0],
            "        /* The job launch clears local store, 16 bytes a turn: as a plain loop here",
            "         * (scripts/lift_spu_jobs.py), not two trampoline hops per turn. */",
            "        for (;;) {",
            "            ctx->gpr[4] = spu_ai(ctx->gpr[4], -1);",
            f"            ctx->gpr[{reg}] = spu_ceqi(ctx->gpr[4], -1);",
            f"            if (ctx->gpr[{reg}]._u32[0] != 0) break;",
            "            spu_ls_write128(ctx, ctx->gpr[3]._u32[0] + 0x0, ctx->gpr[5]);",
            "            ctx->gpr[3] = spu_ai(ctx->gpr[3], 16);",
            "        }",
        ]
        a, b = eol.join(old) + eol, eol.join(new) + eol
        if a in text:
            with open(path, "w", newline="") as f:
                f.write(text.replace(a, b, 1))
            print(f"igpm_rt: local store clear loop at 0x{dec[3:]} made a plain loop")
            return
    # Only a speed-up; the plain lift is correct.
    print("igpm_rt: no known local store clear loop: left as lifted (review patch_igpm_clear)")


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
        if name == "igpm_rt":
            patch_igpm_clear(os.path.join(spu_dir, name, "spu_recomp.c"))
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
# Into the Nexus: its igPm runtime block (lifted by --extra as igpm_rt).
SUB_SKIP = {(0xC14A80, 0x3F580)}


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
    # How many modules each parent loads at each base (a "slot").
    slot_n = {}
    for (ea, base), parents in mods.items():
        for g in parents - {"reloc"}:
            slot_n[(base, g)] = slot_n.get((base, g), 0) + 1
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
        # A module seen at several bases: the parent's recorded starts are LS
        # addresses with no module attached, so translated through every base
        # the module was ever seen at, most of them are other code shifted into
        # it. 0xCDBA80 (55 bases) took 1,466 such "entries" -- one C function
        # per instruction with a trampoline between each, and job 430b3802 ran
        # ~14 ms a frame while streaming (Nexus 2026-11-11). There, keep only
        # return points (the instruction before is a call); other entries come
        # as exact offsets from spu_submodule_entries.txt. The same holds for a
        # module whose parent loads other modules at the same base (439ad202
        # swaps five through 0x20800 and five through 0x24E80): each took the
        # whole slot's starts, mostly mid-block in its own code (0xD0B600: 35
        # hints, 6 return points).
        multi = (len({b for b, _ in seen}) > 1 or
                 any(slot_n.get((b, g), 0) > 1 for b, ps in seen for g in ps - {"reloc"}))
        def is_ret_point(o):
            if o < 4:
                return False
            w, = struct.unpack(">I", raw[o - 4:o])
            return (w >> 23) in (0x066, 0x062) or (w >> 21) in (0x1A9, 0x1AB)   # brsl brasl bisl bisled
        for b, parents in seen:
            hints |= {a - b + base for g in parents for a in starts.get(g, ())
                      if b + SUB_ANCHOR <= a < b + size and (not multi or is_ret_point(a - b))}
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
                "extern char* getenv(const char*);\nextern int atoi(const char*);\n"
                "void racacit_register_elf_overlays(void)\n{\n"
                "    /* NEXUS_ELF_OVL_INTERP=<image> (diagnostic): leave that one to the SPU\n"
                "     * interpreter -- an A/B of the lifted synth (2) or reverb (3). */\n"
                "    const char* ie = getenv(\"NEXUS_ELF_OVL_INTERP\"); const int interp = ie ? atoi(ie) : -1;\n"
                + "".join(f"    if (interp != {i}) spu_register_overlay_image({i}, 0x{b:X}, vm_base + 0x{ea:X}u, 0x{sz:X});\n"
                          for i, ea, sz, b in ELF_OVERLAYS) + "}\n\n"
                "__attribute__((constructor(200))) static void racacit_jobs_register_ctor(void)\n"
                "{\n    racacit_jobs_register_all();\n}\n")


if __name__ == "__main__":
    main()
