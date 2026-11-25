#!/usr/bin/env python3
"""symgen.py — the loader's per-edition symbol table (names → VAs + code hashes), generated from the
OpenSummoners RE data + the user's own exes.  Output carries NO game bytes: names, VAs, sizes, and a
32-bit FNV-1a hash of each function's first bytes (the runtime verifies the code at a VA is what we
think before hooking / calling it).

  python3 tools/symgen.py [--oss ../OpenSummoners] [--out core/gen/sotes_syms.c]

Inputs (all local, none committed here):
  names     OpenSummoners tools/rhost/rh_hooks.c  ("name", 0xVA)  — the port's names, enold VAs
            OpenSummoners tools/ida/retail_fields.json             — curated engine names (enold)
            OpenSummoners tools/ida/gen_knowledge.py GLOBALS       — engine singletons (enold)
            OpenSummoners tools/rhost/rhost.c EDITIONS             — per-edition host VAs (enold, ense-en)
            IDA functions_ida.tsv non-sub_ names                   — knowledge-pack names per edition
            tools/symgen_manual.tsv                                — loader-RE'd names (any edition)
  mapping   tools/ida/crosswalk.py run into the cache (functions + globals, enold↔ense-en↔ense-jp)
  bytes     the unpacked exes (EXES below) — hashed, never copied
"""
import argparse
import collections
import os
import re
import subprocess
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
EDS = ["enold", "ense-en", "ense-jp"]
IDA = os.environ.get("OSS_IDA_LOCAL", "/mnt/ephemeral/opensummoners/decompiled-ida")
CACHE = os.environ.get("SML_CACHE", "/mnt/ephemeral/sotes-mod-loader/cache")
EXES = {
    "enold": "/mnt/ephemeral/opensummoners/game/enold/sotes.unpacked.exe",
    "ense-en": "/mnt/ephemeral/opensummoners/game/ense/sotes_en.unpacked.exe",
    "ense-jp": "/mnt/ephemeral/opensummoners/game/ense/sotes.unpacked.exe",
}
HASH_LEN = 16


def fnv1a(b):
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


class PE:
    def __init__(self, path):
        self.d = open(path, "rb").read()
        d = self.d
        pe = struct.unpack_from("<I", d, 0x3C)[0]
        self.timestamp = struct.unpack_from("<I", d, pe + 8)[0]
        nsec = struct.unpack_from("<H", d, pe + 6)[0]
        opt = pe + 24
        self.entry = struct.unpack_from("<I", d, opt + 16)[0] + 0x400000
        self.size_of_image = struct.unpack_from("<I", d, opt + 56)[0]
        osz = struct.unpack_from("<H", d, pe + 20)[0]
        self.secs = []
        for i in range(nsec):
            s = opt + osz + 40 * i
            name = d[s:s + 8].rstrip(b"\0").decode()
            vsz, va, rsz, rptr = struct.unpack_from("<IIII", d, s + 8)
            self.secs.append((name, 0x400000 + va, vsz, rptr, rsz))
        self.text = next(s for s in self.secs if s[0] == ".text")

    def read(self, va, n):
        for name, sva, vsz, rptr, rsz in self.secs:
            if sva <= va < sva + vsz:
                o = va - sva
                return self.d[rptr + o: rptr + min(o + n, rsz)]
        return b""


def load_funcs(ed):
    """va -> (size, ida_name)"""
    out = {}
    with open(os.path.join(IDA, ed, "functions_ida.tsv")) as f:
        next(f)
        for l in f:
            c = l.rstrip("\n").split("\t")
            out[int(c[0], 16)] = (int(c[1]), c[2])
    return out


def run_crosswalk(oss):
    os.makedirs(CACHE, exist_ok=True)
    for a, b in (("enold", "ense-en"), ("ense-en", "ense-jp")):
        if not os.path.exists(os.path.join(CACHE, f"crosswalk_data_{a}_{b}.tsv")):
            subprocess.run([sys.executable, os.path.join(oss, "tools/ida/crosswalk.py"), a, b, "--out", CACHE],
                           check=True, stdout=subprocess.DEVNULL)


def load_xw(a, b):
    """functions: a_va -> (b_va, same); data: a_va -> b_va"""
    fm, dm = {}, {}
    with open(os.path.join(CACHE, f"crosswalk_{a}_{b}.tsv")) as f:
        next(f)
        for l in f:
            c = l.split("\t")
            fm[int(c[0], 16)] = (int(c[1], 16), c[3] == "1")
    with open(os.path.join(CACHE, f"crosswalk_data_{a}_{b}.tsv")) as f:
        next(f)
        for l in f:
            c = l.split("\t")
            dm[int(c[0], 16)] = int(c[1], 16)
    return fm, dm


def names_rh_hooks(oss):
    src = open(os.path.join(oss, "tools/rhost/rh_hooks.c")).read()
    out = {}
    for name, va in re.findall(r'"([a-z_][a-z0-9_]*)",\s*0x([0-9a-f]+)u?\s*[),]', src):
        out.setdefault(int(va, 16), name)
    return out


def names_retail_fields(oss):
    import json
    j = json.load(open(os.path.join(oss, "tools/ida/retail_fields.json")))["fields"]
    return {int(k, 16): v["name"] for k, v in j.items() if v.get("name")}


def globals_gen_knowledge(oss):
    src = open(os.path.join(oss, "tools/ida/gen_knowledge.py")).read()
    return {int(va, 16): name for va, name in re.findall(r'0x([0-9a-f]+):\s*\("(\w+)"', src)}


RHOST_FIELDS = ["flip", "easer", "srand", "rand", "rng_seed", "state_ptr", "active", "dlgproc", "launch",
                "cfg_screen", "cfg_sound", "zdd", "wndproc", "hwnd_slot", "pump_wait_off", "world_ptr"]
RHOST_KIND = {"rng_seed": "data", "state_ptr": "data", "active": "data", "launch": "data", "cfg_screen": "data",
              "cfg_sound": "data", "zdd": "data", "hwnd_slot": "data", "world_ptr": "data"}
RHOST_NAME = {"flip": "flip", "easer": "camera_follow_step", "srand": "crt_srand", "rand": "rand_draw",
              "rng_seed": "g_rand_seed", "state_ptr": "g_app_state", "active": "g_app_active",
              "dlgproc": "launcher_dlgproc", "launch": "g_launcher_ok", "cfg_screen": "g_cfg_screen",
              "cfg_sound": "g_cfg_sound", "zdd": "g_zdd", "wndproc": "game_wndproc", "hwnd_slot": "g_hwnd",
              "world_ptr": "g_world"}


def rhost_editions(oss):
    src = open(os.path.join(oss, "tools/rhost/rhost.c")).read()
    body = src[src.index("EDITIONS[] = {"):]
    body = body[:body.index("};")]
    out = {}
    for tag, rest in re.findall(r'\{\s*"([\w-]+)",(.*?)\{\s*0x', body, re.S):
        rest = re.sub(r"/\*.*?\*/", "", rest, flags=re.S)
        vals = [int(x, 16) for x in re.findall(r"0x([0-9a-f]+)u", rest)]
        # vals: image_size, entry, then RHOST_FIELDS..., pump_wait_off, world_ptr
        out[tag] = dict(zip(RHOST_FIELDS, vals[2:2 + len(RHOST_FIELDS)]))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--oss", default=os.path.join(os.path.dirname(ROOT), "OpenSummoners"))
    ap.add_argument("--out", default=os.path.join(ROOT, "core/gen/sotes_syms.c"))
    a = ap.parse_args()

    run_crosswalk(a.oss)
    pes = {e: PE(EXES[e]) for e in EDS}
    funcs = {e: load_funcs(e) for e in EDS}
    xa_f, xa_d = load_xw("enold", "ense-en")
    xb_f, xb_d = load_xw("ense-en", "ense-jp")
    rev_a_f = {v[0]: (k, v[1]) for k, v in xa_f.items()}
    rev_a_d = {v: k for k, v in xa_d.items()}

    # sym: name -> {"kind": func|data, "va": {ed: (va, conf)}}; conf 3 = explicit for that edition
    # (loader / rhost / curated / IDA name), 2 = crosswalk bytes|mnem match, 1 = crosswalk graph|order
    syms = {}
    conflicts = []

    def put(name, kind, ed, va, conf=3):
        s = syms.setdefault(name, {"kind": kind, "va": {}})
        cur = s["va"].get(ed)
        if cur and cur[0] != va:
            if cur[1] >= conf:
                if conf == 3:
                    conflicts.append((name, ed, cur[0], va))
                return
        if not cur or cur[1] < conf:
            s["va"][ed] = (va, conf)

    xw_f = {("enold", "ense-en"): xa_f, ("ense-en", "enold"): {v[0]: (k, v[1]) for k, v in xa_f.items()},
            ("ense-en", "ense-jp"): xb_f, ("ense-jp", "ense-en"): {v[0]: (k, v[1]) for k, v in xb_f.items()}}
    xw_d = {("enold", "ense-en"): xa_d, ("ense-en", "enold"): {v: k for k, v in xa_d.items()},
            ("ense-en", "ense-jp"): xb_d, ("ense-jp", "ense-en"): {v: k for k, v in xb_d.items()}}
    meth = {}
    for (x, y), path in ((("enold", "ense-en"), "crosswalk_enold_ense-en.tsv"), (("ense-en", "ense-jp"), "crosswalk_ense-en_ense-jp.tsv")):
        for l in list(open(os.path.join(CACHE, path)))[1:]:
            c = l.split("\t")
            conf = 2 if c[2] in ("bytes", "mnem") else 1
            meth[(x, y, int(c[0], 16))] = conf
            meth[(y, x, int(c[1], 16))] = conf

    def spread(name):
        s = syms[name]
        for _ in range(3):
            for (x, y) in xw_f:
                if x not in s["va"]:
                    continue
                va, cx = s["va"][x]
                if s["kind"] == "func":
                    m = xw_f[(x, y)].get(va)
                    v = m[0] if m else None
                    c = meth.get((x, y, va), 1)
                else:
                    v = xw_d[(x, y)].get(va)
                    c = 2
                if v:
                    put(name, s["kind"], y, v, min(c, cx))

    # 1. loader-RE'd (highest priority)
    man = os.path.join(HERE, "symgen_manual.tsv")
    if os.path.exists(man):
        for l in open(man):
            l = l.split("#")[0].strip()
            if not l:
                continue
            name, kind, ed, va = l.split()[:4]
            put(name, kind, ed, int(va, 16))
    # 2. rhost EDITIONS (both editions given explicitly)
    for ed, row in rhost_editions(a.oss).items():
        for f, va in row.items():
            if f not in RHOST_NAME:
                continue          # pump_wait_off: an offset, not a VA
            put(RHOST_NAME[f], RHOST_KIND.get(f, "func"), ed, va)
    # 3. engine singletons + curated names (enold)
    for va, n in globals_gen_knowledge(a.oss).items():
        put(n, "data", "enold", va)
    for va, n in names_retail_fields(a.oss).items():
        put(n, "func" if va in funcs["enold"] else "data", "enold", va)
    # 4. the port's names (enold)
    for va, n in names_rh_hooks(a.oss).items():
        if va in funcs["enold"]:
            put(n, "func", "enold", va)
    # 5. IDA knowledge names per edition (strip the _VA suffix)
    for ed in EDS:
        for va, (sz, n) in funcs[ed].items():
            if n.startswith(("sub_", "j_", "nullsub", "unknown_libname", "?")) or "@" in n:
                continue
            base = re.sub(r"_[0-9A-F]{6,8}$", "", n)
            if base != n:
                put(base, "func", ed, va)
    for n in list(syms):
        spread(n)
    for c in conflicts:
        print("  conflict (kept first): %s %s %08x vs %08x" % c)

    # hashes
    rows = []
    for n in sorted(syms):
        s = syms[n]
        r = {"name": n, "kind": s["kind"], "va": [0] * 3, "hash": [0] * 3, "size": [0] * 3, "conf": [0] * 3}
        for i, ed in enumerate(EDS):
            if ed not in s["va"]:
                continue
            va, r["conf"][i] = s["va"][ed]
            r["va"][i] = va
            if s["kind"] == "func":
                sz = funcs[ed].get(va, (0,))[0]
                if not sz:
                    # not an IDA function start in this edition → drop (never hook garbage)
                    r["va"][i] = r["conf"][i] = 0
                    continue
                r["size"][i] = min(sz, 0xFFFF)
                r["hash"][i] = fnv1a(pes[ed].read(va, min(sz, HASH_LEN)))
        if any(r["va"]):
            rows.append(r)

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "w") as f:
        f.write("// GENERATED by tools/symgen.py — do not edit.  Names + per-edition VAs + FNV-1a hashes of\n"
                f"// each function's first {HASH_LEN} bytes (verified at runtime).  No game bytes.\n"
                "#include \"../syms.h\"\n\n")
        f.write("const sym_edition_t SYM_EDITIONS[SYM_NED] = {\n")
        for ed in EDS:
            p = pes[ed]
            f.write(f'    {{ "{ed}", 0x{p.timestamp:08x}u, 0x{p.text[2]:08x}u, 0x{p.entry:08x}u, 0x{p.size_of_image:08x}u }},\n')
        f.write("};\n\nconst sym_row SYM_ROWS[] = {\n")
        for r in rows:
            va = ", ".join(f"0x{v:08x}u" for v in r["va"])
            h = ", ".join(f"0x{v:08x}u" for v in r["hash"])
            sz = ", ".join(str(v) for v in r["size"])
            cf = ", ".join(str(v) for v in r["conf"])
            k = "SYM_FUNC" if r["kind"] == "func" else "SYM_DATA"
            f.write(f'    {{ "{r["name"]}", {k}, {{ {va} }}, {{ {h} }}, {{ {sz} }}, {{ {cf} }} }},\n')
        f.write("};\nconst int SYM_NROWS = sizeof SYM_ROWS / sizeof *SYM_ROWS;\n")
    c = collections.Counter()
    for r in rows:
        for i, ed in enumerate(EDS):
            if r["va"][i]:
                c[(ed, r["kind"])] += 1
    print(f"symgen: {len(rows)} symbols → {a.out}")
    for ed in EDS:
        print(f"  {ed}: {c[(ed, 'func')]} funcs, {c[(ed, 'data')]} data")


if __name__ == "__main__":
    main()
