#!/usr/bin/env python3
"""sml — drive the REAL retail exe with the mod loader in it, under wine, for agents and tests.

  tools/sml up R [--edition ense-en] [--mods autoload,trainer] [--saves steam|DIR|none] [--cfg k=v ...]
  tools/sml ping R                       # frame / armed / edition (no Lua)
  tools/sml lua R 'return mod.edition'   # Lua on the engine thread, JSON result; -f FILE for a file
  tools/sml shot R [OUT.png]             # the X display (what a player sees)
  tools/sml log R [-n 40]                # tail of oss_modloader.log
  tools/sml down R | down --all
  tools/sml ls

Isolation (a port session runs in parallel on this box): everything lives under $SML_HOME
(/mnt/ephemeral/sotes-mod-loader): its OWN wine prefix (never OpenSummoners' prefixes, never
`wineserver -k` while another run is up), one Xvfb per run, run dirs of symlinked game assets +
a hard-linked exe + copied saves.  Processes are stopped by exact pid only.

Editions: enold (2012 EN), ense-en (SE EN), ense-jp (SE JP) — the user's unpacked exes.
"""
import argparse
import binascii
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import time

HOME = os.environ.get("SML_HOME", "/mnt/ephemeral/sotes-mod-loader")
PREFIX = os.path.join(HOME, "prefix")
RUNS = os.path.join(HOME, "runs")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MODS_REPO = os.environ.get("SML_MODS", os.path.join(os.path.dirname(ROOT), "sotes-mods"))
GAME = "/mnt/ephemeral/opensummoners/game"
STEAM_USER = "/mnt/ephemeral/Steam/steamapps/common/sotes/user"
EDITIONS = {
    "enold": {"dir": f"{GAME}/enold", "exe": "sotes.unpacked.exe", "as": "sotes.exe"},
    "ense-en": {"dir": f"{GAME}/ense", "exe": "sotes_en.unpacked.exe", "as": "sotes_en.exe"},
    "ense-jp": {"dir": f"{GAME}/ense", "exe": "sotes.unpacked.exe", "as": "sotes.exe"},
}
ASSETS = ["lizsoft.spl", "_sotes2.ini", "sotesd.dll", "sotesd_en.dll", "sotesp.dll", "sotesw.dll",
          "sotesx_d2.dll", "sotesx_d3.dll", "sotesx_s.dll"]


def die(msg, code=1):
    print(msg, file=sys.stderr)
    sys.exit(code)


def run_dir(r):
    return os.path.join(RUNS, r)


def meta(r):
    p = os.path.join(run_dir(r), "run.json")
    if not os.path.exists(p):
        die(f"no run {r} (sml up {r})")
    return json.load(open(p))


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def slot_prefix(slot):
    return PREFIX if slot == 0 else f"{PREFIX}-{slot}"


def free_slot():
    """one run per wine prefix: the game holds a single-instance mutex, and a prefix = one wineserver
    = one named-object namespace.  Slot 0 = the base prefix; slots 1.. are copies made on first use."""
    used = set()
    for r in os.listdir(RUNS) if os.path.isdir(RUNS) else []:
        p = os.path.join(RUNS, r, "run.json")
        if os.path.exists(p):
            m = json.load(open(p))
            if alive(m["game_pid"]):
                used.add(m.get("slot", 0))
    slot = next(i for i in range(64) if i not in used)
    if slot and not os.path.exists(os.path.join(slot_prefix(slot), "system.reg")):
        ensure_prefix()
        subprocess.run(["cp", "-a", PREFIX, slot_prefix(slot)], check=True)
    return slot


def wine_env(display, port, slot=0):
    e = dict(os.environ)
    e.update(WINEPREFIX=slot_prefix(slot), WINEDEBUG="-all", WINEDLLOVERRIDES="version=n,b;mscoree,mshtml=",
             DISPLAY=display, OSS_DEVSERVER=str(port))
    return e


def free_display():
    for n in range(300, 400):
        if not os.path.exists(f"/tmp/.X11-unix/X{n}") and not os.path.exists(f"/tmp/.X{n}-lock"):
            return n
    die("no free X display")


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def ensure_prefix():
    if not os.path.exists(os.path.join(PREFIX, "system.reg")):
        os.makedirs(PREFIX, exist_ok=True)
        subprocess.run(["wineboot", "-i"], env=dict(os.environ, WINEPREFIX=PREFIX, WINEDEBUG="-all"),
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


# ── the dev-server client ─────────────────────────────────────────────────────
def request(m, line, timeout=30):
    s = socket.create_connection(("127.0.0.1", m["port"]), timeout=timeout)
    s.sendall(line.encode() + b"\n")
    buf = b""
    while not buf.endswith(b"\n"):
        k = s.recv(65536)
        if not k:
            break
        buf += k
    s.close()
    txt = buf.decode(errors="replace").rstrip("\n")
    st, _, payload = txt.partition(" ")
    out, i = [], 0
    while i < len(payload):
        c = payload[i]
        if c == "\\" and i + 1 < len(payload):
            out.append({"n": "\n", "r": "\r", "\\": "\\"}.get(payload[i + 1], payload[i + 1]))
            i += 2
        else:
            out.append(c)
            i += 1
    return st == "ok", "".join(out)


def lua(m, code, timeout=30):
    return request(m, "lua " + binascii.hexlify(code.encode()).decode(), timeout)


# ── commands ──────────────────────────────────────────────────────────────────
def cmd_up(a):
    ed = EDITIONS.get(a.edition) or die(f"edition: {' | '.join(EDITIONS)}")
    d = run_dir(a.run)
    if os.path.exists(os.path.join(d, "run.json")):
        m = json.load(open(os.path.join(d, "run.json")))
        if alive(m["game_pid"]):
            die(f"run {a.run} is up (sml down {a.run})")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "mods"))
    for f in ASSETS:
        src = os.path.join(ed["dir"], f)
        if os.path.exists(src):
            os.symlink(os.path.realpath(src), os.path.join(d, f))
    exe = os.path.join(d, ed["as"])
    os.link(os.path.join(ed["dir"], ed["exe"]), exe)   # hard link: the app dir stays the run dir
    shutil.copy(os.path.join(ROOT, "build", "version.dll"), d)
    ensure_prefix()
    shutil.copy(os.path.join(PREFIX, "drive_c/windows/syswow64/version.dll"), os.path.join(d, "realver.dll"))
    if a.saves != "none":
        src = STEAM_USER if a.saves == "steam" else a.saves
        os.makedirs(os.path.join(d, "user"))
        for f in os.listdir(src):
            if f.endswith((".sdt", ".dat")):
                shutil.copy(os.path.join(src, f), os.path.join(d, "user", f))
    for mname in filter(None, (a.mods or "").split(",")):
        for base in (os.path.join(MODS_REPO, "mods"), os.path.join(ROOT, "examples"), os.path.join(ROOT, "tests", "mods")):
            if os.path.isdir(os.path.join(base, mname)):
                shutil.copytree(os.path.join(base, mname), os.path.join(d, "mods", mname))
                break
        else:
            die(f"mod {mname}: not in {MODS_REPO}/mods, examples/ or tests/mods/")
    cfg = {"ui": "0", "keepactive": "1", "skip_launcher": "1"}
    for kv in a.cfg or []:
        k, _, v = kv.partition("=")
        cfg[k] = v
    with open(os.path.join(d, "oss_loader.cfg"), "w") as f:
        for k, v in cfg.items():
            f.write(f"{k}={v}\n")

    import fcntl
    lk = open(os.path.join(HOME, "display.lock"), "w")
    fcntl.flock(lk, fcntl.LOCK_EX)        # parallel `up`s: one display allocation at a time
    disp = free_display()
    port = free_port()
    slot = free_slot()
    log = open(os.path.join(d, "x.log"), "w")
    xv = subprocess.Popen(["Xvfb", f":{disp}", "-screen", "0", "1024x768x24", "-nolisten", "tcp"],
                          stdout=log, stderr=log, start_new_session=True)
    for _ in range(100):
        if os.path.exists(f"/tmp/.X11-unix/X{disp}"):
            break
        time.sleep(0.05)
    fcntl.flock(lk, fcntl.LOCK_UN)
    wl = open(os.path.join(d, "wine.log"), "w")
    g = subprocess.Popen(["wine", ed["as"]], cwd=d, env=wine_env(f":{disp}", port, slot), stdout=wl, stderr=wl,
                         start_new_session=True)
    m = {"run": a.run, "edition": a.edition, "display": f":{disp}", "port": port, "xvfb_pid": xv.pid,
         "game_pid": g.pid, "slot": slot, "dir": d, "started": time.time()}
    json.dump(m, open(os.path.join(d, "run.json"), "w"), indent=1)
    t0 = time.time()
    while time.time() - t0 < a.timeout:
        if not alive(g.pid):
            die(f"game exited during boot — sml log {a.run}", 2)
        try:
            ok, txt = request(m, "ping", timeout=2)
            if ok and "armed=1" in txt:
                print(f"{a.run}: up ({txt}) display {m['display']} port {port} dir {d}")
                return
        except OSError:
            pass
        time.sleep(0.5)
    die(f"{a.run}: not armed after {a.timeout}s — sml log {a.run}", 3)


def cmd_play(a):
    m = meta(a.run)
    ok, txt = lua(m, f"return mod.input.play([[{a.movie}]])")
    if not ok:
        die(txt)
    if a.wait:
        t0 = time.time()
        while time.time() - t0 < a.timeout:
            ok, left = lua(m, "return mod.input.pending()")
            if ok and left == "0":
                break
            time.sleep(0.1)
    ok, txt = lua(m, "return {frame=mod.frame(), pending=mod.input.pending()}")
    print(txt)


def cmd_wait(a):
    """poll a Lua expression until truthy (prints its last value); exit 4 on timeout"""
    m = meta(a.run)
    t0 = time.time()
    txt = ""
    while time.time() - t0 < a.timeout:
        ok, txt = lua(m, a.expr)
        if not ok:
            die(txt)
        if txt not in ("null", "false", "0", '""'):
            print(txt)
            return
        time.sleep(a.every)
    print(f"timeout; last = {txt}")
    sys.exit(4)


def cmd_ping(a):
    ok, txt = request(meta(a.run), "ping", timeout=5)
    print(txt)
    sys.exit(0 if ok else 1)


def cmd_lua(a):
    code = open(a.file).read() if a.file else " ".join(a.code)
    ok, txt = lua(meta(a.run), code, timeout=a.timeout)
    print(txt)
    sys.exit(0 if ok else 1)


def winpath(p):
    return "Z:" + os.path.realpath(p).replace("/", "\\")


def cmd_shot(a):
    """the game's back buffer (exact 640x480 frame) via the dev server; --x = the whole X screen"""
    m = meta(a.run)
    out = a.out or os.path.join(m["dir"], f"shot-{int(time.time() * 1000)}.png")
    if not a.x:
        bmp = os.path.join(m["dir"], "_shot.bmp")
        ok, txt = lua(m, f"return mod.screen.shot([[{winpath(bmp)}]])")
        if ok and txt == "true":
            subprocess.run(["magick", bmp, out], check=True)
            os.unlink(bmp)
            print(out)
            return
        print(f"back-buffer shot failed ({txt}); falling back to the X screen", file=sys.stderr)
    xwd = subprocess.run(["xwd", "-root", "-display", m["display"], "-silent"], capture_output=True, check=True).stdout
    subprocess.run(["magick", "xwd:-", out], input=xwd, check=True)
    print(out)


def cmd_log(a):
    p = os.path.join(meta(a.run)["dir"], "oss_modloader.log")
    lines = open(p, errors="replace").read().splitlines() if os.path.exists(p) else []
    print("\n".join(lines[-a.n:]))


def stop(m):
    for k in ("game_pid", "xvfb_pid"):
        pid = m.get(k)
        if pid and alive(pid):
            try:
                os.killpg(pid, signal.SIGTERM)
            except OSError:
                pass
    for _ in range(50):
        if not any(m.get(k) and alive(m[k]) for k in ("game_pid", "xvfb_pid")):
            return
        time.sleep(0.2)
    for k in ("game_pid", "xvfb_pid"):
        pid = m.get(k)
        if pid and alive(pid):
            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass


def others_up(exclude):
    n = 0
    for r in os.listdir(RUNS) if os.path.isdir(RUNS) else []:
        p = os.path.join(RUNS, r, "run.json")
        if r != exclude and os.path.exists(p) and alive(json.load(open(p))["game_pid"]):
            n += 1
    return n


def cmd_down(a):
    runs = [r for r in os.listdir(RUNS)] if a.all else [a.run]
    for r in runs:
        p = os.path.join(RUNS, r, "run.json")
        if os.path.exists(p):
            m = json.load(open(p))
            was_up = alive(m["game_pid"])
            stop(m)
            if was_up:   # that run's own prefix (one run per prefix) — never anyone else's wineserver
                subprocess.run(["wineserver", "-k"], env=dict(os.environ, WINEPREFIX=slot_prefix(m.get("slot", 0))),
                               stderr=subprocess.DEVNULL)
            print(f"{r}: down")


def cmd_ls(a):
    for r in sorted(os.listdir(RUNS)) if os.path.isdir(RUNS) else []:
        p = os.path.join(RUNS, r, "run.json")
        if os.path.exists(p):
            m = json.load(open(p))
            print(f"{r:16} {m['edition']:8} {'UP' if alive(m['game_pid']) else 'down':5} {m['display']} port {m['port']}")


def main():
    ap = argparse.ArgumentParser(prog="sml", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    p = sp.add_parser("up"); p.add_argument("run"); p.add_argument("--edition", default="ense-en")
    p.add_argument("--mods"); p.add_argument("--saves", default="steam"); p.add_argument("--cfg", nargs="*")
    p.add_argument("--timeout", type=float, default=90); p.set_defaults(f=cmd_up)
    p = sp.add_parser("ping"); p.add_argument("run"); p.set_defaults(f=cmd_ping)
    p = sp.add_parser("lua"); p.add_argument("run"); p.add_argument("code", nargs="*"); p.add_argument("-f", "--file")
    p.add_argument("--timeout", type=float, default=30); p.set_defaults(f=cmd_lua)
    p = sp.add_parser("play"); p.add_argument("run"); p.add_argument("movie"); p.add_argument("--wait", action="store_true")
    p.add_argument("--timeout", type=float, default=120); p.set_defaults(f=cmd_play)
    p = sp.add_parser("wait"); p.add_argument("run"); p.add_argument("expr"); p.add_argument("--timeout", type=float, default=60)
    p.add_argument("--every", type=float, default=0.25); p.set_defaults(f=cmd_wait)
    p = sp.add_parser("shot"); p.add_argument("run"); p.add_argument("out", nargs="?")
    p.add_argument("--x", action="store_true", help="the X screen instead of the back buffer"); p.set_defaults(f=cmd_shot)
    p = sp.add_parser("log"); p.add_argument("run"); p.add_argument("-n", type=int, default=40); p.set_defaults(f=cmd_log)
    p = sp.add_parser("down"); p.add_argument("run", nargs="?"); p.add_argument("--all", action="store_true"); p.set_defaults(f=cmd_down)
    p = sp.add_parser("ls"); p.set_defaults(f=cmd_ls)
    a = ap.parse_args()
    a.f(a)


if __name__ == "__main__":
    main()
