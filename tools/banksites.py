#!/usr/bin/env python3
"""banksites.py — every instruction that indexes the sprite-bank table or its descriptor table with a RUNTIME index.

The sprite banks are two parallel pointer arrays inside the static system object (g_sys): slots (sys + 0xaac / SE
0xb08 / JP 0xb04) and descriptors (sys + 0x18e0 / SE 0x197c / JP 0x1988), 909 / 925 / 929 entries, nearly all taken.
To give mods banks past the end, the loader relocates both arrays into larger ones of its own: every instruction
that reads `table[reg]` gets its disp32 rebased (core/lua/sprites.lua, mod.game.sprites.extend).  Reads with a
constant index, and the whole-table loops (`mov reg, offset table` + a count), stay on the old arrays — which stay
valid for every retail index, the old pointers being mirrored in the new arrays.

This walks every IDA function of the user's own unpacked exes with capstone and collects the sites:
  absolute      [table_va + reg(*4)]            (any index register, no base)
  sys-relative  [reg + reg*4 + table_off]       (thiscall methods of the system object)
plus the whole-table loops (an immediate == table_va; listed by --print).  One of those is rebased too — the
display-restore loop (enold 0x5b14c0: after a lost-surface restore it re-decodes every decoded frame of every bank;
`mov ebx, table / mov ebp, count / mov edi, [ebx] / xor esi, esi / cmp [edi+4], si`): it must cover the new banks,
or their frames stay lost.  It dereferences every entry, so the loader fills unused entries with an empty slot.
Output: core/gen/sotes_banksites.c (VAs of the disp32 / imm32 fields + the value expected there; numbers only, no
game bytes).

  python3 tools/banksites.py            # write core/gen/sotes_banksites.c
  python3 tools/banksites.py --print    # per-edition report, no write
"""
import argparse
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from symgen import EXES, PE, load_funcs  # noqa: E402

import capstone  # noqa: E402
from capstone import x86  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "core", "gen", "sotes_banksites.c")
EDS = ("enold", "ense-en", "ense-jp")
LAYOUT = {   # g_sys, slots offset, descriptors offset
    "enold": (0x8a6b60, 0xaac, 0x18e0),
    "ense-en": (0x92af08, 0xb08, 0x197c),
    "ense-jp": (0x926078, 0xb04, 0x1988),
}
MD = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
MD.detail = True


def scan(ed):
    pe = PE(EXES[ed])
    sysva, so, do = LAYOUT[ed]
    n = (do - so) // 4
    tabs = {"slot": (sysva + so, so), "desc": (sysva + do, do)}
    sites, loops, odd = [], [], []
    for va, size, name in funcs(ed):
        code = pe.read(va, size)
        if not code:
            continue
        for ins in MD.disasm(code, va):
            if ins.disp_size != 4 and not ins.imm_size:
                continue
            for op in ins.operands:
                if op.type == x86.X86_OP_IMM:
                    for k, (tva, _) in tabs.items():
                        if op.imm & 0xFFFFFFFF == tva:
                            loops.append((ins.address, k, name, f"{ins.mnemonic} {ins.op_str}"))
                if op.type != x86.X86_OP_MEM:
                    continue
                m = op.mem
                d = m.disp & 0xFFFFFFFF
                for k, (tva, toff) in tabs.items():
                    hit = None
                    if d == tva and (m.base or m.index):
                        # [abs + reg*4] or [abs + reg] (the index already scaled)
                        if m.base and m.index:
                            odd.append((ins.address, k, name, f"{ins.mnemonic} {ins.op_str}"))
                        else:
                            hit = "abs"
                    elif d == toff and m.index and m.scale == 4 and m.base not in (0, x86.X86_REG_ESP):
                        hit = "rel"
                    if hit:
                        dva = ins.address + ins.disp_offset
                        assert struct.unpack("<I", pe.read(dva, 4))[0] == d
                        sites.append((dva, k, hit, d, ins.address, name, f"{ins.mnemonic} {ins.op_str}"))
    # the display-restore loop: BB <slots> BD <n> 8B 3B 33 F6 66 39 77 04
    pat = b"\xbb" + struct.pack("<I", sysva + so) + b"\xbd" + struct.pack("<I", n) + bytes.fromhex("8b3b33f666397704")
    restore = [l[0] for l in loops if pe.read(l[0], len(pat)) == pat]
    assert len(restore) == 1, (ed, restore)
    return n, sites, loops, odd, restore[0]


def funcs(ed):
    import csv
    from symgen import IDA
    with open(os.path.join(IDA, ed, "functions_ida.tsv")) as f:
        r = csv.reader(f, delimiter="\t")
        next(r)
        for row in r:
            yield int(row[0], 16), int(row[1]), row[2]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--print", action="store_true")
    a = ap.parse_args()
    res = {ed: scan(ed) for ed in EDS}
    for ed in EDS:
        n, sites, loops, odd, restore = res[ed]
        ns = sum(1 for s in sites if s[1] == "slot")
        rel = sorted({s[5] for s in sites if s[2] == "rel"})
        print(f"{ed}: {n} banks; {len(sites)} sites ({ns} slot, {len(sites) - ns} desc; "
              f"{sum(1 for s in sites if s[2] == 'rel')} sys-relative in {len(rel)} functions); "
              f"{len(loops)} whole-table loops (restore @ {restore:08x}); {len(odd)} odd")
        if a.print:
            for s in sites:
                if s[2] == "rel":
                    print(f"   rel  {s[4]:08x} {s[1]} {s[5]:40s} {s[6]}")
            for l in loops:
                print(f"   loop {l[0]:08x} {l[1]} {l[2]:40s} {l[3]}")
            for l in odd:
                print(f"   ODD  {l[0]:08x} {l[1]} {l[2]:40s} {l[3]}")
    if a.print:
        return
    out = ["// GENERATED by tools/banksites.py — do not edit.  The disp32 fields that index the sprite-bank tables with a",
           "// runtime index (numbers only; no game bytes).  kind: 0 = slots, 1 = descriptors.",
           '#include "../sotes_banksites.h"', ""]
    for ed in EDS:
        n, sites, _, _, _ = res[ed]
        cn = ed.replace("-", "_")
        out.append(f"static const BankSite sites_{cn}[] = {{")
        row = []
        for s in sorted(sites):
            row.append(f"{{0x{s[0]:08x}u, 0x{s[3]:08x}u, {0 if s[1] == 'slot' else 1}}}")
            if len(row) == 4:
                out.append("    " + ", ".join(row) + ",")
                row = []
        if row:
            out.append("    " + ", ".join(row) + ",")
        out.append("};")
    out.append("")
    out.append("const BankSites g_banksites[3] = {")
    for ed in EDS:
        n, sites, _, _, restore = res[ed]
        sysva, so, do = LAYOUT[ed]
        cn = ed.replace("-", "_")
        out.append(f'    {{ "{ed}", 0x{sysva:08x}u, 0x{so:x}u, 0x{do:x}u, {n}, 0x{restore + 1:08x}u, 0x{restore + 6:08x}u, '
                   f'{len(sites)}, sites_{cn} }},')
    out.append("};")
    with open(OUT, "w") as f:
        f.write("\n".join(out) + "\n")
    print("wrote", OUT)


if __name__ == "__main__":
    main()
