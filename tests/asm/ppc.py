#!/usr/bin/env python3
"""A tiny PowerPC64 assembler that writes PS3-shaped ELF executables.

Enough of the ISA for lv2 test programs: integer ops, loads/stores, branches,
`sc`, and the OPD function descriptors a PPU ELF entry points at. One RWX
PT_LOAD segment at the base address, no sections. Two passes over a plain
text syntax:

    label:            ; a label
    li r3, 42         ; instructions as in the PowerPC books (r0..r31)
    la r4, buf        ; macro: 32-bit address of a label (lis + ori)
    li64 r5, 0x123    ; macro: 64-bit immediate (5 instructions)
    .quad expr ...    ; data: 8/4/1-byte values, labels allowed
    .long expr ...
    .byte expr ...
    .ascii "text\n"
    .align 8
    .opd name, func   ; a function descriptor {u32 func, u32 toc} named `name`

Usage: ppc.py [--base 0x10000] [--entry symbol] in.s out.elf
"""
import re
import struct
import sys


class Asm:
    def __init__(self, base):
        self.base = base
        self.labels = {}
        self.out = bytearray()
        self.pc = base
        self.pass_no = 0

    # ---- expressions -------------------------------------------------
    def val(self, tok):
        tok = tok.strip()
        if re.fullmatch(r"-?0x[0-9a-fA-F]+", tok):
            return int(tok, 16)
        if re.fullmatch(r"-?\d+", tok):
            return int(tok)
        m = re.fullmatch(r"(\w+)\s*([+-])\s*(\w+)", tok)
        if m:
            a = self.val(m.group(1))
            b = self.val(m.group(3))
            return a + b if m.group(2) == "+" else a - b
        if tok in self.labels:
            return self.labels[tok]
        if self.pass_no == 0:
            return 0
        raise SystemExit(f"undefined symbol: {tok}")

    @staticmethod
    def reg(tok):
        tok = tok.strip()
        m = re.fullmatch(r"r(\d+)", tok)
        if not m or not 0 <= int(m.group(1)) <= 31:
            raise SystemExit(f"bad register: {tok}")
        return int(m.group(1))

    def mem(self, tok):
        # d(rA)
        m = re.fullmatch(r"\s*(.*)\((r\d+)\)\s*", tok)
        if not m:
            raise SystemExit(f"bad memory operand: {tok}")
        return self.val(m.group(1) or "0"), self.reg(m.group(2))

    # ---- emission ----------------------------------------------------
    def emit(self, word):
        self.out += struct.pack(">I", word & 0xFFFFFFFF)
        self.pc += 4

    def emit_bytes(self, b):
        self.out += b
        self.pc += len(b)

    def align(self, n):
        while self.pc % n:
            self.emit_bytes(b"\0")

    # ---- encoders ----------------------------------------------------
    @staticmethod
    def d_form(op, rt, ra, d):
        return (op << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFF)

    @staticmethod
    def ds_form(op, rt, ra, d, xo):
        if d & 3:
            raise SystemExit("DS-form displacement must be a multiple of 4")
        return (op << 26) | (rt << 21) | (ra << 16) | (d & 0xFFFC) | xo

    @staticmethod
    def x_form(rt, ra, rb, xo, rc=0):
        return (31 << 26) | (rt << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc

    @staticmethod
    def md_form(rs, ra, sh, mbe, xo, rc=0):
        return ((30 << 26) | (rs << 21) | (ra << 16) | ((sh & 31) << 11) | ((mbe & 31) << 6)
                | ((mbe >> 5) << 5) | (xo << 2) | ((sh >> 5) << 1) | rc)

    def branch(self, target, link, cond=None):
        off = target - self.pc if self.pass_no else 0
        if cond is None:
            if not -0x2000000 <= off < 0x2000000:
                raise SystemExit("branch out of range")
            return (18 << 26) | (off & 0x3FFFFFC) | link
        bo, bi = cond
        if not -0x8000 <= off < 0x8000:
            raise SystemExit("conditional branch out of range")
        return (16 << 26) | (bo << 21) | (bi << 16) | (off & 0xFFFC) | link

    # ---- instructions ------------------------------------------------
    def instr(self, mn, args):
        a = [x.strip() for x in args.split(",")] if args.strip() else []
        R, V = self.reg, self.val
        conds = {"beq": (12, 2), "bne": (4, 2), "blt": (12, 0), "bge": (4, 0),
                 "bgt": (12, 1), "ble": (4, 1)}
        if mn == "li":
            self.emit(self.d_form(14, R(a[0]), 0, V(a[1])))
        elif mn == "lis":
            self.emit(self.d_form(15, R(a[0]), 0, V(a[1])))
        elif mn == "addi":
            self.emit(self.d_form(14, R(a[0]), R(a[1]), V(a[2])))
        elif mn == "addis":
            self.emit(self.d_form(15, R(a[0]), R(a[1]), V(a[2])))
        elif mn == "ori":
            self.emit(self.d_form(24, R(a[1]), R(a[0]), V(a[2])))
        elif mn == "oris":
            self.emit(self.d_form(25, R(a[1]), R(a[0]), V(a[2])))
        elif mn == "andi.":
            self.emit(self.d_form(28, R(a[1]), R(a[0]), V(a[2])))
        elif mn == "nop":
            self.emit(0x60000000)
        elif mn == "la":
            addr = V(a[1])
            self.emit(self.d_form(15, R(a[0]), 0, (addr >> 16) & 0xFFFF))
            self.emit(self.d_form(24, R(a[0]), R(a[0]), addr & 0xFFFF))
        elif mn == "li64":
            v = V(a[1]) & 0xFFFFFFFFFFFFFFFF
            r = R(a[0])
            self.emit(self.d_form(15, r, 0, (v >> 48) & 0xFFFF))
            self.emit(self.d_form(24, r, r, (v >> 32) & 0xFFFF))
            self.emit(self.md_form(r, r, 32, 31, 1))  # sldi r, r, 32
            self.emit(self.d_form(25, r, r, (v >> 16) & 0xFFFF))
            self.emit(self.d_form(24, r, r, v & 0xFFFF))
        elif mn in ("lwz", "stw", "lbz", "stb", "lhz", "sth"):
            op = {"lwz": 32, "stw": 36, "lbz": 34, "stb": 38, "lhz": 40, "sth": 44}[mn]
            d, ra = self.mem(a[1])
            self.emit(self.d_form(op, R(a[0]), ra, d))
        elif mn == "ld":
            d, ra = self.mem(a[1])
            self.emit(self.ds_form(58, R(a[0]), ra, d, 0))
        elif mn == "std":
            d, ra = self.mem(a[1])
            self.emit(self.ds_form(62, R(a[0]), ra, d, 0))
        elif mn == "lbzx":
            self.emit(self.x_form(R(a[0]), R(a[1]), R(a[2]), 87))
        elif mn == "stbx":
            self.emit(self.x_form(R(a[0]), R(a[1]), R(a[2]), 215))
        elif mn == "add":
            self.emit(self.x_form(R(a[0]), R(a[1]), R(a[2]), 266))
        elif mn == "subf":
            self.emit(self.x_form(R(a[0]), R(a[1]), R(a[2]), 40))
        elif mn == "mulld":
            self.emit(self.x_form(R(a[0]), R(a[1]), R(a[2]), 233))
        elif mn == "mullw":
            self.emit(self.x_form(R(a[0]), R(a[1]), R(a[2]), 235))
        elif mn in ("xor", "or", "and"):
            xo = {"xor": 316, "or": 444, "and": 28}[mn]
            self.emit(self.x_form(R(a[1]), R(a[0]), R(a[2]), xo))
        elif mn == "mr":
            self.emit(self.x_form(R(a[1]), R(a[0]), R(a[1]), 444))
        elif mn == "sldi":
            n = V(a[2])
            self.emit(self.md_form(R(a[1]), R(a[0]), n, 63 - n, 1))
        elif mn == "srdi":
            n = V(a[2])
            self.emit(self.md_form(R(a[1]), R(a[0]), 64 - n, n, 0))
        elif mn == "rotldi":
            n = V(a[2])
            self.emit(self.md_form(R(a[1]), R(a[0]), n, 0, 0))
        elif mn == "lwzx":
            self.emit(self.x_form(R(a[0]), R(a[1]), R(a[2]), 23))
        elif mn == "clrldi":
            n = V(a[2])
            self.emit(self.md_form(R(a[1]), R(a[0]), 0, n, 0))
        elif mn == "cmpwi":
            self.emit((11 << 26) | (R(a[0]) << 16) | (V(a[1]) & 0xFFFF))
        elif mn == "cmpdi":
            self.emit((11 << 26) | (1 << 21) | (R(a[0]) << 16) | (V(a[1]) & 0xFFFF))
        elif mn == "cmpd":
            self.emit((31 << 26) | (1 << 21) | (R(a[0]) << 16) | (R(a[1]) << 11) | (0 << 1))
        elif mn == "cmpw":
            self.emit((31 << 26) | (R(a[0]) << 16) | (R(a[1]) << 11))
        elif mn == "b":
            self.emit(self.branch(V(a[0]), 0))
        elif mn == "bl":
            self.emit(self.branch(V(a[0]), 1))
        elif mn in conds:
            self.emit(self.branch(V(a[0]), 0, conds[mn]))
        elif mn == "blr":
            self.emit(0x4E800020)
        elif mn == "bctr":
            self.emit(0x4E800420)
        elif mn == "bctrl":
            self.emit(0x4E800421)
        elif mn == "mflr":
            self.emit(self.x_form(R(a[0]), 8, 0, 339))
        elif mn == "mtlr":
            self.emit(self.x_form(R(a[0]), 8, 0, 467))
        elif mn == "mtctr":
            self.emit(self.x_form(R(a[0]), 9, 0, 467))
        elif mn == "sc":
            self.emit(0x44000002)
        elif mn == "sync":
            self.emit(0x7C0004AC)
        else:
            raise SystemExit(f"unknown mnemonic: {mn}")

    def directive(self, d, args):
        parts = [x.strip() for x in re.split(r",(?=(?:[^\"]*\"[^\"]*\")*[^\"]*$)", args)] if args.strip() else []
        if d == ".quad":
            for p in parts:
                self.emit_bytes(struct.pack(">Q", self.val(p) & 0xFFFFFFFFFFFFFFFF))
        elif d == ".long":
            for p in parts:
                self.emit_bytes(struct.pack(">I", self.val(p) & 0xFFFFFFFF))
        elif d == ".byte":
            for p in parts:
                self.emit_bytes(bytes([self.val(p) & 0xFF]))
        elif d == ".ascii":
            s = args.strip()
            if not (s.startswith('"') and s.endswith('"')):
                raise SystemExit(".ascii wants a quoted string")
            self.emit_bytes(s[1:-1].encode().decode("unicode_escape").encode("latin1"))
        elif d == ".align":
            self.align(self.val(parts[0]))
        elif d == ".space":
            self.emit_bytes(b"\0" * self.val(parts[0]))
        elif d == ".opd":
            # a PS3 function descriptor: 32-bit address, 32-bit TOC
            self.align(8)
            self.labels[parts[0]] = self.pc
            self.emit_bytes(struct.pack(">II", self.val(parts[1]), 0))
        else:
            raise SystemExit(f"unknown directive: {d}")

    def run(self, text):
        for self.pass_no in (0, 1):
            self.out = bytearray()
            self.pc = self.base
            for lineno, raw in enumerate(text.splitlines(), 1):
                line = raw.split(";", 1)[0].rstrip()
                if "#" in line and '"' not in line:
                    line = line.split("#", 1)[0].rstrip()
                if not line.strip():
                    continue
                m = re.match(r"\s*(\w+):\s*(.*)$", line)
                if m:
                    if self.pass_no == 0 and m.group(1) in self.labels:
                        raise SystemExit(f"line {lineno}: duplicate label {m.group(1)}")
                    self.labels[m.group(1)] = self.pc
                    line = m.group(2)
                    if not line.strip():
                        continue
                m = re.match(r"\s*([.\w]+)\s*(.*)$", line)
                mn, args = m.group(1), m.group(2)
                try:
                    if mn.startswith("."):
                        self.directive(mn, args)
                    else:
                        self.instr(mn, args)
                except SystemExit as e:
                    raise SystemExit(f"line {lineno}: {e}")
        return bytes(self.out)


def write_elf(path, base, image, entry):
    # ELF64 big-endian PPC64 executable: one RWX PT_LOAD, no sections.
    ehdr_size, phdr_size = 64, 56
    offset = ehdr_size + phdr_size
    # the segment's file offset must be congruent to its vaddr modulo the
    # page size for real loaders; rpcs3 copies bytes, so match anyway
    pad = (base - offset) % 0x10000
    offset += pad
    e_ident = b"\x7fELF" + bytes([2, 2, 1, 0]) + b"\0" * 8
    ehdr = e_ident + struct.pack(">HHIQQQIHHHHHH", 2, 0x15, 1, entry, ehdr_size, 0, 0,
                                 ehdr_size, phdr_size, 1, 0, 0, 0)
    phdr = struct.pack(">IIQQQQQQ", 1, 7, offset, base, base, len(image), len(image), 0x10000)
    with open(path, "wb") as f:
        f.write(ehdr + phdr + b"\0" * pad + image)


def main():
    argv = sys.argv[1:]
    base = 0x10000
    entry = "_start"
    while argv and argv[0].startswith("--"):
        if argv[0] == "--base":
            base = int(argv[1], 0)
        elif argv[0] == "--entry":
            entry = argv[1]
        argv = argv[2:]
    src, dst = argv
    asm = Asm(base)
    image = asm.run(open(src).read())
    if entry not in asm.labels:
        raise SystemExit(f"no entry symbol {entry}")
    write_elf(dst, base, image, asm.labels[entry])
    print(f"{dst}: {len(image)} bytes at {base:#x}, entry {asm.labels[entry]:#x}")


if __name__ == "__main__":
    main()
