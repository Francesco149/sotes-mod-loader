#!/usr/bin/env python3
"""charcodes.py — the character codes each edition's char_init accepts (the codes mod.game.actors.spawn may use).

char_init (enold 0x41f200) dispatches on the object code through a compare tree + jump tables; a code no case
takes falls to the default arm: "Unknown Object Type Character." + exit.  This walks that dispatch statically
(capstone over the user's own unpacked exes): interval arithmetic on the code register through cmp / jcc, lea /
add / sub, byte index tables and dword jump tables, up to the first instruction that is not dispatch (a case
body).  A body that pushes the "Unknown Object Type" string is the default.  Output: per edition, the merged
intervals of codes reaching a real case → core/gen/sotes_charcodes.c (numbers only; no game bytes).

  python3 tools/charcodes.py            # write core/gen/sotes_charcodes.c
  python3 tools/charcodes.py --print    # per-edition summary + intervals, no write
"""
import argparse
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symgen import EXES, PE  # noqa: E402

import capstone  # noqa: E402
from capstone import x86  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "core", "gen", "sotes_charcodes.c")
EDS = ("enold", "ense-en", "ense-jp")
M32 = 0xFFFFFFFF
MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = True
REG32 = {"al": "eax", "bl": "ebx", "cl": "ecx", "dl": "edx"}


def sym_vas(name):
    src = open(os.path.join(ROOT, "core", "gen", "sotes_syms.c")).read()
    m = re.search(r'\{ "%s", SYM_FUNC, \{ (0x[0-9a-f]+)u, (0x[0-9a-f]+)u, (0x[0-9a-f]+)u \}' % name, src)
    return dict(zip(EDS, (int(v, 16) for v in m.groups())))


def ins_at(pe, va):
    return next(MD.disasm(pe.read(va, 16), va))


def find_va(pe, needle):
    for name, sva, vsz, rptr, rsz in pe.secs:
        i = pe.d.find(needle, rptr, rptr + rsz)
        if i >= 0:
            return sva + (i - rptr)
    return None


def pieces(lo, hi, off):
    """split code interval [lo, hi] into pieces where v = code + off is monotone as unsigned AND signed"""
    out, c = [], lo
    while c <= hi:
        v = (c + off) & M32
        # next boundary of v: 2^31 or 2^32
        nb = 0x80000000 if v < 0x80000000 else 0x100000000
        e = min(hi, c + (nb - v) - 1)
        out.append((c, e, v))
        c = e + 1
    return out


def cond_split(lo, hi, off, op, imm, signed):
    """-> (taken intervals, fallthrough intervals) for `cmp v, imm; j<op>` with v = code + off"""
    t, f = [], []
    for c0, c1, v0 in pieces(lo, hi, off):
        def val(c):
            v = (c - c0 + v0) & M32
            return v - (1 << 32) if signed and v >= 0x80000000 else v
        k = imm - (1 << 32) if signed and imm >= 0x80000000 else imm
        # codes where val(c) <, ==, > k: val is monotone (+1 per code) on the piece
        a, b = val(c0), val(c1)
        lt = (c0, min(c1, c0 + (k - a) - 1)) if a < k else None
        eq = (c0 + (k - a),) * 2 if a <= k <= b else None
        gt = (max(c0, c0 + (k - a) + 1), c1) if b > k else None
        parts = {"lt": lt, "eq": eq, "gt": gt}
        want = {"a": ("gt",), "ae": ("gt", "eq"), "b": ("lt",), "be": ("lt", "eq"), "e": ("eq",), "ne": ("lt", "gt"),
                "g": ("gt",), "ge": ("gt", "eq"), "l": ("lt",), "le": ("lt", "eq")}[op]
        for n, iv in parts.items():
            if iv and iv[0] <= iv[1]:
                (t if n in want else f).append(iv)
    return t, f


JCC = {"ja": ("a", 0), "jae": ("ae", 0), "jb": ("b", 0), "jbe": ("be", 0), "je": ("e", 0), "jne": ("ne", 0),
       "jg": ("g", 1), "jge": ("ge", 1), "jl": ("l", 1), "jle": ("le", 1)}


def walk(pe, start, reg):
    """-> {target_va: [(lo, hi), ...]} — every code interval and the case body it reaches"""
    targets = {}
    work = [(start, 0, M32, {reg: ("code", 0)}, None)]
    seen = set()
    while work:
        va, lo, hi, regs, last_cmp = work.pop()
        key = (va, lo, hi, tuple(sorted(regs.items())), last_cmp)
        if key in seen:
            continue
        seen.add(key)
        while True:
            i = ins_at(pe, va)
            m, ops = i.mnemonic, i.operands
            nxt = va + i.size
            R = lambda o: i.reg_name(o.reg) if o.type == x86.X86_OP_REG else None  # noqa: E731
            if m == "cmp" and R(ops[0]) in regs and regs[R(ops[0])][0] == "code" and ops[1].type == x86.X86_OP_IMM:
                last_cmp = (regs[R(ops[0])][1], ops[1].imm & M32, "all")
                va = nxt
                continue
            if m in JCC and last_cmp:
                op, signed = JCC[m]
                if last_cmp[2] == "z" and op not in ("e", "ne"):
                    raise SystemExit("%s after inc/dec/add at %#x: flags not modelled" % (m, va))
                t, f = cond_split(lo, hi, last_cmp[0], op, last_cmp[1], signed)
                for a, b in t:
                    work.append((ops[0].imm, a, b, dict(regs), None))
                for a, b in f:
                    work.append((nxt, a, b, dict(regs), last_cmp))
                break
            if m == "jmp" and ops[0].type == x86.X86_OP_IMM:
                va = ops[0].imm
                continue
            if m == "lea" and ops[1].mem.base and i.reg_name(ops[1].mem.base) in regs and not ops[1].mem.index:
                b = regs[i.reg_name(ops[1].mem.base)]
                if b[0] == "code":
                    regs = dict(regs); regs[R(ops[0])] = ("code", b[1] + ops[1].mem.disp); va = nxt; continue
            # arithmetic on the code register sets the flags a following jcc reads: `sub v, k` == `cmp v, k` for
            # every condition; dec / inc / add only for je / jne (result == 0 <=> old v == 1 / -1 / -k)
            arith = None
            if m in ("add", "sub") and R(ops[0]) in regs and ops[1].type == x86.X86_OP_IMM and regs[R(ops[0])][0] == "code":
                arith = ops[1].imm if m == "add" else -ops[1].imm
                zcmp = (regs[R(ops[0])][1], ops[1].imm & M32, "all") if m == "sub" else \
                    (regs[R(ops[0])][1], (-ops[1].imm) & M32, "z")
            elif m in ("dec", "inc") and R(ops[0]) in regs and regs[R(ops[0])][0] == "code":
                arith = -1 if m == "dec" else 1
                zcmp = (regs[R(ops[0])][1], 1 if m == "dec" else M32, "z")
            if arith is not None:
                regs = dict(regs); regs[R(ops[0])] = ("code", regs[R(ops[0])][1] + arith)
                last_cmp = zcmp
                va = nxt
                continue
            if m == "mov" and R(ops[0]) and R(ops[1]) in regs:
                regs = dict(regs); regs[R(ops[0])] = regs[R(ops[1])]; va = nxt; continue
            if m == "xor" and R(ops[0]) and R(ops[0]) == R(ops[1]):
                va = nxt
                continue
            if m in ("mov", "movzx") and R(ops[0]) and ops[1].type == x86.X86_OP_MEM and ops[1].size == 1:
                mm = ops[1].mem
                base = i.reg_name(mm.base) if mm.base else None
                idx = i.reg_name(mm.index) if mm.index else None
                src = idx if idx in regs else base if base in regs else None
                if src and regs[src][0] == "code":
                    d = REG32.get(R(ops[0]), R(ops[0]))
                    regs = dict(regs); regs[d] = ("byte", regs[src][1], mm.disp & M32); va = nxt; continue
            if m == "jmp" and ops[0].type == x86.X86_OP_MEM and ops[0].mem.index:
                mm = ops[0].mem
                r = regs.get(i.reg_name(mm.index))
                if r is None or hi - lo > 0x10000:
                    raise SystemExit("unhandled jump table at %#x" % va)
                for c in range(lo, hi + 1):
                    v = (c + r[1]) & M32
                    ix = pe.read(r[2] + v, 1)[0] if r[0] == "byte" else v
                    tgt = struct.unpack("<I", pe.read((mm.disp & M32) + ix * mm.scale, 4))[0]
                    targets.setdefault(tgt, []).append((c, c))
                break
            targets.setdefault(va, []).append((lo, hi))
            break
    return targets


def is_default(pe, va, msg_va, n=16):
    for i in MD.disasm(pe.read(va, 96), va):
        if i.mnemonic == "push" and i.operands[0].type == x86.X86_OP_IMM and (i.operands[0].imm & M32) == msg_va:
            return True
        n -= 1
        if n == 0 or i.mnemonic in ("ret", "jmp"):
            break
    return False


def merge(ivs):
    out = []
    for a, b in sorted(ivs):
        if out and a <= out[-1][1] + 1:
            out[-1][1] = max(out[-1][1], b)
        else:
            out.append([a, b])
    return out


def codes_for(ed, ci_va):
    pe = PE(EXES[ed])
    msg = find_va(pe, b"Unknown Object Type Character")
    if msg is None:
        raise SystemExit("%s: default-arm string not found" % ed)
    # the dispatch head: char_init's first `cmp reg32, imm` with a character-code immediate
    for i in MD.disasm(pe.read(ci_va, 0x2000), ci_va):
        o = i.operands
        if i.mnemonic == "cmp" and len(o) == 2 and o[0].type == x86.X86_OP_REG and o[1].type == x86.X86_OP_IMM \
                and 50000 <= o[1].imm < 200000:
            start, reg = i.address, i.reg_name(o[0].reg)
            break
    else:
        raise SystemExit("%s: dispatch head not found" % ed)
    tg = walk(pe, start, reg)
    good, bad, ncase = [], 0, 0
    for t, ivs in tg.items():
        if is_default(pe, t, msg):
            bad += 1
        else:
            ncase += 1
            good += ivs
    if bad == 0:
        raise SystemExit("%s: no default arm identified — walker out of sync" % ed)
    return start, merge(good), ncase


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--print", action="store_true")
    a = ap.parse_args()
    ci = sym_vas("char_init")
    res = {}
    for ed in EDS:
        start, ivs, ncase = codes_for(ed, ci[ed])
        n = sum(b - a + 1 for a, b in ivs)
        res[ed] = ivs
        print("%-8s char_init %#x dispatch %#x: %d case bodies, %d codes in %d intervals" % (ed, ci[ed], start, ncase, n,
                                                                                        len(ivs)))
        if a.print:
            print("  " + " ".join("%d" % x if x == y else "%d-%d" % (x, y) for x, y in ivs))
    if a.print:
        return
    with open(OUT, "w") as f:
        f.write("// GENERATED by tools/charcodes.py — the character codes each edition's char_init dispatch accepts\n")
        f.write("// (anything else = retail's \"Unknown Object Type Character.\" + exit).  Numbers only.\n")
        f.write("#include \"../syms.h\"\n#include \"../sotes_charcodes.h\"\n\n")
        for ed in EDS:
            f.write("static const charcode_iv CC_%s[] = {\n" % ed.replace("-", "_").upper())
            for x, y in res[ed]:
                f.write("    { %d, %d },\n" % (x, y))
            f.write("};\n")
        f.write("\nconst charcode_iv *charcodes_get(int edition, int *n) {\n    switch (edition) {\n")
        for ed, tag in zip(EDS, ("SYM_ENOLD", "SYM_ENSE_EN", "SYM_ENSE_JP")):
            v = "CC_" + ed.replace("-", "_").upper()
            f.write("    case %s: *n = (int)(sizeof %s / sizeof *%s); return %s;\n" % (tag, v, v, v))
        f.write("    default: *n = 0; return 0;\n    }\n}\n")
    print("wrote", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
