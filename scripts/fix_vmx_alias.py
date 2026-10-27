#!/usr/bin/env python3
"""fix_vmx_alias.py [recomp_dir] [--check]: two fixes to lifted AltiVec code
(idempotent). (1) Vector instructions whose destination is also a source read
that source from a snapshot. (2) vpkuwus, vpkuhus, vadduws and vmulouh/vmuleuh
compose their big-endian lanes from bytes (ENDIAN_RULES below).

The lifter lowered most AltiVec instructions as a loop writing vD lane by lane
through a pointer into ctx->vr. When vD is also vA, vB or vC, lanes of the
source are overwritten before they are read -- the hardware reads every
source first. The case that showed it is vcmpequh. in A Crack in Time's
texture streamer (func_005E3584, a vectorised search of a u16 id table):

    d[i] = a[i]==b[i] ? ~0 : 0;  t += a[i]==b[i];      (vcmpequh. v0,v1,v0)

re-read b[i] after storing the mask into it, so a matching lane never counted,
CR6 said "no lane equal" and the search found no key but 0. High-resolution
texture upgrades were never switched in, and VRAM compaction then moved
low-resolution texture headers onto high-resolution data: the striped textures.

ppu_lifter.py now snapshots aliased sources itself (_translate_vmx_aliased).
This applies the same rewrite to an existing lift, statement by statement:

    { T* d=(T*)&ctx->vr[N]; T* a=(T*)&ctx->vr[N]; ... }
 -> { alignas(16) uint8_t _vsnap[16]; memcpy(_vsnap, &ctx->vr[N], 16); T* d=(T*)&ctx->vr[N]; T* a=(T*)&_vsnap; ... }

Only the source pointers (a, b, c) of a statement that also has a destination
pointer d into the same register are redirected. --check reports without
writing. Run by patch_lift.py after every lift.
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
args = [a for a in sys.argv[1:] if not a.startswith("--")]
RECOMP = args[0] if args else os.path.join(ROOT, "game", "src", "recomp")
CHECK = "--check" in sys.argv

DEST = re.compile(r"\bd\s*=\s*\(\s*(?:const\s+)?[a-z0-9_]+\s*\*\s*\)\s*&ctx->vr\[(\d+)\]")
SRC = r"\b([abc])\s*=\s*\(\s*(?:const\s+)?([a-z0-9_]+)\s*\*\s*\)\s*&ctx->vr\[{n}\]"


def fix_line(line):
    if "_vsnap" in line or "&ctx->vr[" not in line:
        return line, 0
    m = DEST.search(line)
    if not m:
        return line, 0
    n = m.group(1)
    src = re.compile(SRC.format(n=n))
    if not src.search(line):
        return line, 0
    # The statement is the innermost { ... } that holds the destination pointer.
    start = line.rfind("{", 0, m.start())
    if start < 0:
        return line, 0
    body = line[start + 1:]
    new_body = src.sub(lambda mm: f"{mm.group(1)}=({mm.group(2)}*)&_vsnap", body)
    return (line[:start + 1] + f" alignas(16) uint8_t _vsnap[16]; memcpy(_vsnap, &ctx->vr[{n}], 16);" + new_body), 1


# Lowerings that did lane arithmetic through host uint16_t*/uint32_t* reads of
# big-endian lanes -- byte-reversed values, so wrong sums, products and
# saturation (vpkuwus clamped nearly every word to 0xFFFF). ppu_lifter.py now
# composes the lanes from bytes; these rewrite the statements of an existing
# lift the same way. SRC is "&ctx->vr[N]" or "&_vsnap".
SRC_RE = r"(&ctx->vr\[\d+\]|&_vsnap)"
ENDIAN_RULES = [
    ("vpkuwus",
     re.compile(r"uint16_t\* d=\(uint16_t\*\)&ctx->vr\[(\d+)\]; uint32_t\* a=\(uint32_t\*\)" + SRC_RE +
                r"; uint32_t\* b=\(uint32_t\*\)" + SRC_RE +
                r"; for\(int i=0;i<4;i\+\+\)\{uint32_t v=a\[i\]; d\[i\]=\(uint16_t\)\(v>65535u\?65535u:v\);\} "
                r"for\(int i=0;i<4;i\+\+\)\{uint32_t v=b\[i\]; d\[4\+i\]=\(uint16_t\)\(v>65535u\?65535u:v\);\}"),
     lambda m: (f"const uint8_t* a=(const uint8_t*){m.group(2)}; const uint8_t* b=(const uint8_t*){m.group(3)}; uint8_t o[16]; "
                f"for(int i=0;i<4;i++){{ uint32_t v=((uint32_t)a[4*i]<<24)|((uint32_t)a[4*i+1]<<16)|((uint32_t)a[4*i+2]<<8)|a[4*i+3]; "
                f"uint32_t r=v>65535u?65535u:v; o[2*i]=(uint8_t)(r>>8); o[2*i+1]=(uint8_t)r; }} "
                f"for(int i=0;i<4;i++){{ uint32_t v=((uint32_t)b[4*i]<<24)|((uint32_t)b[4*i+1]<<16)|((uint32_t)b[4*i+2]<<8)|b[4*i+3]; "
                f"uint32_t r=v>65535u?65535u:v; o[8+2*i]=(uint8_t)(r>>8); o[8+2*i+1]=(uint8_t)r; }} "
                f"memcpy(&ctx->vr[{m.group(1)}], o, 16);")),
    ("vpkuhus",
     re.compile(r"uint8_t\* d=\(uint8_t\*\)&ctx->vr\[(\d+)\]; uint16_t\* a=\(uint16_t\*\)" + SRC_RE +
                r"; uint16_t\* b=\(uint16_t\*\)" + SRC_RE +
                r"; for\(int i=0;i<8;i\+\+\)\{uint32_t v=a\[i\]; d\[i\]=\(uint8_t\)\(v>255u\?255u:v\);\} "
                r"for\(int i=0;i<8;i\+\+\)\{uint32_t v=b\[i\]; d\[8\+i\]=\(uint8_t\)\(v>255u\?255u:v\);\}"),
     lambda m: (f"const uint8_t* a=(const uint8_t*){m.group(2)}; const uint8_t* b=(const uint8_t*){m.group(3)}; uint8_t o[16]; "
                f"for(int i=0;i<8;i++){{ uint32_t v=(uint32_t)((a[2*i]<<8)|a[2*i+1]); o[i]=(uint8_t)(v>255u?255u:v); }} "
                f"for(int i=0;i<8;i++){{ uint32_t v=(uint32_t)((b[2*i]<<8)|b[2*i+1]); o[8+i]=(uint8_t)(v>255u?255u:v); }} "
                f"memcpy(&ctx->vr[{m.group(1)}], o, 16);")),
    ("vadduws",
     re.compile(r"uint32_t\* d=\(uint32_t\*\)&ctx->vr\[(\d+)\]; uint32_t\* a=\(uint32_t\*\)" + SRC_RE +
                r"; uint32_t\* b=\(uint32_t\*\)" + SRC_RE +
                r"; for\(int i=0;i<4;i\+\+\)\{uint64_t r=\(uint64_t\)a\[i\]\+\(uint64_t\)b\[i\]; "
                r"d\[i\]=\(uint32_t\)\(r>0xFFFFFFFFu\?0xFFFFFFFFu:r\);\}"),
     lambda m: (f"const uint8_t* a=(const uint8_t*){m.group(2)}; const uint8_t* b=(const uint8_t*){m.group(3)}; uint8_t o[16]; "
                f"for(int i=0;i<4;i++){{ uint64_t x=0,y=0; for(int k=0;k<4;k++){{ x=(x<<8)|a[4*i+k]; y=(y<<8)|b[4*i+k]; }} "
                f"uint64_t r=x+y; uint32_t v=(uint32_t)(r>0xFFFFFFFFull?0xFFFFFFFFull:r); "
                f"for(int k=0;k<4;k++) o[4*i+k]=(uint8_t)(v>>(8*(3-k))); }} "
                f"memcpy(&ctx->vr[{m.group(1)}], o, 16);")),
    ("vmulouh/vmuleuh",
     re.compile(r"uint32_t\* d=\(uint32_t\*\)&ctx->vr\[(\d+)\]; uint16_t\* a=\(uint16_t\*\)" + SRC_RE +
                r"; uint16_t\* b=\(uint16_t\*\)" + SRC_RE +
                r"; d\[0\]=\(uint32_t\)a\[([01])\]\*\(uint32_t\)b\[\4\]; d\[1\]=\(uint32_t\)a\[[23]\]\*\(uint32_t\)b\[[23]\]; "
                r"d\[2\]=\(uint32_t\)a\[[45]\]\*\(uint32_t\)b\[[45]\]; d\[3\]=\(uint32_t\)a\[[67]\]\*\(uint32_t\)b\[[67]\];"),
     lambda m: (f"const uint8_t* a=(const uint8_t*){m.group(2)}; const uint8_t* b=(const uint8_t*){m.group(3)}; uint8_t o[16]; "
                f"for(int j=0;j<4;j++){{ int l=2*j+{m.group(4)}; "
                f"uint32_t x=(uint32_t)((a[l*2]<<8)|a[l*2+1]); uint32_t y=(uint32_t)((b[l*2]<<8)|b[l*2+1]); "
                f"uint32_t r=x*y; for(int k=0;k<4;k++) o[j*4+k]=(uint8_t)(r>>(8*(3-k))); }} "
                f"memcpy(&ctx->vr[{m.group(1)}], o, 16);")),
]


def fix_endian(line):
    n = 0
    for name, rx, rep in ENDIAN_RULES:
        line, k = rx.subn(rep, line)
        n += k
    return line, n


def main():
    files = sorted(glob.glob(os.path.join(RECOMP, "ppu_recomp_*.cpp")))
    total = 0
    for path in files:
        with open(path, newline="") as f:
            text = f.read()
        nl = "\r\n" if "\r\n" in text else "\n"
        lines = text.split(nl)
        count = 0
        for i, line in enumerate(lines):
            new, c = fix_line(line)
            new, e = fix_endian(new)
            if c or e:
                lines[i] = new
                count += c + e
        if count:
            total += count
            print(f"{os.path.basename(path)}: {count} vector statement(s)")
            if not CHECK:
                with open(path, "w", newline="") as f:
                    f.write(nl.join(lines))
    print(f"{total} statement(s) {'would be ' if CHECK else ''}rewritten")


if __name__ == "__main__":
    main()
