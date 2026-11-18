"""Tiny PPU register tracker for naming passes.

Walks a function linearly, tracking constant values of GPRs built by
lis/addi/ori/addis and TOC loads (lwz/ld rD,d(r2)), and reports each `bl`
with the argument registers r3..r8 known at that point.
"""
import json
import os
import struct

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


class Image:
    def __init__(self, elf=None):
        elf = elf or os.path.join(ROOT, "input", "EBOOT.ELF")
        self.data = open(elf, "rb").read()
        self.segs = []
        for s in json.load(open(os.path.join(ROOT, "out", "EBOOT.image.json"))):
            self.segs.append((int(s["vaddr"], 16), int(s["filesz"], 16),
                              int(s["file_offset"], 16)))
        ld = json.load(open(os.path.join(ROOT, "out", "EBOOT.loader.json")))
        self.toc = int(ld["module_toc"], 16)

    def read(self, va, n):
        for base, size, off in self.segs:
            if base <= va and va + n <= base + size:
                return self.data[off + va - base: off + va - base + n]
        return None

    def u32(self, va):
        b = self.read(va, 4)
        return struct.unpack(">I", b)[0] if b else None

    def cstr(self, va, limit=256):
        b = self.read(va, 1)
        if b is None:
            return None
        out = bytearray()
        while len(out) < limit:
            c = self.read(va + len(out), 1)
            if not c or c == b"\0":
                break
            out += c
        try:
            s = out.decode("ascii")
        except UnicodeDecodeError:
            return None
        return s if s and s.isprintable() else None

    def opd_code(self, opd):
        """OPD descriptor -> code address (first word)."""
        return self.u32(opd)


def sext16(v):
    return v - 0x10000 if v & 0x8000 else v


def scan(img, start, size):
    """Yield (insn_addr, call_target, regs_dict) for every bl in [start, start+size)."""
    regs = {}
    for pc in range(start, start + size, 4):
        w = img.u32(pc)
        if w is None:
            break
        op = w >> 26
        rd = (w >> 21) & 31
        ra = (w >> 16) & 31
        imm = w & 0xFFFF
        if op == 15:  # addis
            base = 0 if ra == 0 else regs.get(ra, img.toc if ra == 2 else None)
            regs[rd] = None if base is None else (base + (sext16(imm) << 16)) & 0xFFFFFFFF
        elif op == 14:  # addi
            base = 0 if ra == 0 else regs.get(ra, img.toc if ra == 2 else None)
            regs[rd] = None if base is None else (base + sext16(imm)) & 0xFFFFFFFF
        elif op == 24:  # ori rA,rS,imm (rS is rd field)
            v = regs.get(rd)
            regs[ra] = None if v is None else v | imm
        elif op == 32 or (op == 58 and (w & 3) == 0):  # lwz / ld
            base = img.toc if ra == 2 else regs.get(ra)
            if base is None:
                regs[rd] = None
            else:
                d = sext16(imm & ~3) + 4 if op == 58 else sext16(imm)
                regs[rd] = img.u32((base + d) & 0xFFFFFFFF)
        elif op == 31 and ((w >> 1) & 0x3FF) == 444:  # or (mr)
            rs, rb = rd, (w >> 11) & 31
            regs[ra] = regs.get(rs) if rs == rb else None
        elif op == 18:  # b / bl
            li = w & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            tgt = (li if w & 2 else pc + li) & 0xFFFFFFFF
            if w & 1:
                yield pc, tgt, {r: regs.get(r) for r in range(3, 9)}
                for r in (0, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12):  # volatile
                    regs.pop(r, None)
        elif op in (16, 19):
            continue
        else:
            # any other write to rd: forget it (conservative)
            if op not in (36, 37, 38, 44, 45, 52, 53, 54, 55, 62):  # stores keep regs
                regs.pop(rd, None)
