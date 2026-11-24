#!/usr/bin/env python3
"""fieldxw.py — struct-offset crosswalk between editions: where did enold field +DISP move in the SE build?

The port's struct layouts (OpenSummoners src/*.h) are enold's.  The SE builds grew some structs (e.g. the
world's stack-ref field 0x27ac → 0x37b0).  This aligns every crosswalked function pair (instruction shapes:
mnemonic + registers, immediates / displacements abstracted) and, at each aligned instruction that uses
[reg + DISP] in the source edition, records the displacement the target edition uses there.  Votes per
query; a field whose votes split is reported as such — verify live (tools/sml) before a binding relies on it.

  python3 tools/fieldxw.py 0x2784 0x4030 0x9f4            # enold -> ense-en
  python3 tools/fieldxw.py --to ense-jp 0x2784            # enold -> ense-en -> ense-jp (chained)
  python3 tools/fieldxw.py --fn 0x41dc90 0x4050           # only sites inside these enold functions
  python3 tools/fieldxw.py --sites 0x2784                 # list the aligned sites

No game bytes are written anywhere; output is offsets only.
"""
import argparse
import collections
import difflib
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symgen import EXES, PE, load_funcs, load_xw  # noqa: E402

import capstone  # noqa: E402
from capstone import x86  # noqa: E402

MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = True


def disasm(pe, va, size):
    code = pe.read(va, size)
    return list(MD.disasm(code, va))


def shape(ins):
    """mnemonic + operand shape: registers kept, imm / disp abstracted (so a moved offset still aligns)"""
    ops = []
    for o in ins.operands:
        if o.type == x86.X86_OP_REG:
            ops.append(ins.reg_name(o.reg))
        elif o.type == x86.X86_OP_IMM:
            ops.append("i")
        elif o.type == x86.X86_OP_MEM:
            m = o.mem
            ops.append("[%s+%s*%d%s]" % (ins.reg_name(m.base) if m.base else "", ins.reg_name(m.index) if m.index else "",
                                         m.scale, "+d" if m.disp else ""))
    return ins.mnemonic + " " + ",".join(ops)


def mem_disps(ins):
    return [o.mem.disp & 0xFFFFFFFF for o in ins.operands if o.type == x86.X86_OP_MEM and o.mem.base]


def align_pair(A, B):
    """yield (a_ins, b_ins) aligned instruction pairs"""
    sa, sb = [shape(i) for i in A], [shape(i) for i in B]
    sm = difflib.SequenceMatcher(None, sa, sb, autojunk=len(sa) > 4000)
    for blk in sm.get_matching_blocks():
        for k in range(blk.size):
            yield A[blk.a + k], B[blk.b + k]


def walk(src, dst, queries, only_fns=None):
    """{disp: Counter(dst_disp)}, {disp: [(a_va, b_va, b_disp)]}"""
    pe = {e: PE(EXES[e]) for e in (src, dst)}
    fs, fd = load_funcs(src), load_funcs(dst)
    fm, _ = load_xw(src, dst)
    votes = collections.defaultdict(collections.Counter)
    sites = collections.defaultdict(list)
    q = set(queries)
    for a, (b, _same) in fm.items():
        if only_fns and a not in only_fns:
            continue
        if a not in fs or b not in fd:
            continue
        A = disasm(pe[src], a, fs[a][0])
        if not any(x in q for i in A for x in mem_disps(i)):
            continue
        B = disasm(pe[dst], b, fd[b][0])
        for ia, ib in align_pair(A, B):
            da, db = mem_disps(ia), mem_disps(ib)
            if len(da) != len(db):
                continue
            for x, y in zip(da, db):
                if x in q:
                    votes[x][y] += 1
                    sites[x].append((ia.address, ib.address, y))
    return votes, sites


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("disp", nargs="+", type=lambda s: int(s, 0))
    ap.add_argument("--src", default="enold")
    ap.add_argument("--to", default="ense-en", choices=["ense-en", "ense-jp"])
    ap.add_argument("--fn", action="append", type=lambda s: int(s, 0), help="restrict to these src functions")
    ap.add_argument("--sites", action="store_true")
    a = ap.parse_args()
    votes, sites = walk(a.src, "ense-en", a.disp, set(a.fn) if a.fn else None)
    if a.to == "ense-jp":
        # chain: the en winners → jp
        en = {d: votes[d].most_common(1)[0][0] for d in a.disp if votes[d]}
        v2, s2 = walk("ense-en", "ense-jp", sorted(set(en.values())))
        for d in a.disp:
            if d not in en:
                print(f"0x{d:x}: no aligned sites")
                continue
            e = en[d]
            print(f"0x{d:x} -> ense-en 0x{e:x} -> ense-jp " +
                  ", ".join(f"0x{y:x}×{n}" for y, n in v2[e].most_common(4)))
        return
    for d in a.disp:
        c = votes[d]
        tot = sum(c.values())
        if not tot:
            print(f"0x{d:x}: no aligned sites")
            continue
        top = c.most_common(5)
        print(f"0x{d:x} -> " + ", ".join(f"0x{y:x}×{n}" for y, n in top) + f"  ({tot} sites)")
        if a.sites:
            for sa, sb, y in sites[d][:40]:
                print(f"    {sa:08x} -> {sb:08x}  0x{y:x}")


if __name__ == "__main__":
    main()
