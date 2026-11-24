#!/usr/bin/env python3
"""sml_mcp — an MCP server (stdio, stdlib only) for agents driving the REAL game through the loader.

A typed skin over `tools/sml` (runs under wine, own prefix / Xvfb per run) and each run's dev server
(Lua on the engine thread).  The actions are the loader's mod.game ops — the game's own mechanisms
(the warp item's request, room_item_add, ...), every edition (enold / ense-en / ense-jp).

  runs      sml (passthrough: up / down / continue / play / wait / log / ls / test ...)
  observe   state · rooms · catalog · flags · shot · lua
  act       warp · teleport · give · heal · set_stats · gold · set_flag · keys · skip_dialogue

Register (.mcp.json):  "sml": {"type": "stdio", "command": "python3",
                               "args": ["/opt/src/sotes-mod-loader/tools/sml_mcp.py"]}
CLI (no MCP client):   tools/sml_mcp.py state run=R      tools/sml_mcp.py warp run=R key=210110
                       tools/sml_mcp.py --selftest       (the tool list)
`lua` / `sml` reach anything the typed tools don't, so new loader ops need no edit here.
"""
from __future__ import annotations

import base64
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sml  # noqa: E402  (meta / lua: the dev-server client)

SML = os.path.join(HERE, "sml")


class ToolError(RuntimeError):
    pass


def L(run, code, timeout=30):
    """Lua on the engine thread of `run`; returns the decoded JSON result."""
    ok, txt = sml.lua(sml.meta(run), code, timeout=timeout)
    if not ok:
        raise ToolError(txt)
    try:
        return json.loads(txt)
    except ValueError:
        return txt


def sml_cli(args, timeout=600):
    p = subprocess.run([SML] + args, capture_output=True, text=True, timeout=timeout)
    out = (p.stdout + p.stderr).strip()
    if p.returncode != 0:
        raise ToolError(out or f"sml {' '.join(args)}: exit {p.returncode}")
    return out


def lstr(v):
    """a Lua literal for a Python value"""
    if v is None:
        return "nil"
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, (int, float)):
        return repr(v)
    return "[==[" + str(v) + "]==]"


# ── tools ─────────────────────────────────────────────────────────────────────
STATE_LUA = r"""
local m = game.map.get()
local s = { edition = mod.edition, frame = mod.frame() }
if not m then s.scene = false return s end
s.scene, s.room, s.map, s.event = true, m.area, m.map, game.map.event()
s.warp = { game.map.warp_status() }
local cp, cap = game.items.gold(); s.gold, s.gold_cap = cp, cap
s.party = {}
for _, p in ipairs(game.roster.members()) do
  s.party[#s.party + 1] = { id = p.id, code = p.code, lv = p.level, hp = p.hp, hp_max = p.hp_max, mp = p.mp,
                            mp_max = p.mp_max, x = p.x, y = p.y, active = p.active }
end
local here = game.map.room(m.area); s.exits = here and here.exits
return s
"""


def t_sml(a):
    return sml_cli(a["args"].split(), timeout=a.get("timeout", 600))


def t_lua(a):
    return L(a["run"], a["code"], timeout=a.get("timeout", 30))


def t_state(a):
    return L(a["run"], STATE_LUA)


def t_rooms(a):
    flt = "" if a.get("map") is None else f"if r.map ~= {int(a['map'])} then goto c end"
    return L(a["run"], f"""local o = {{}}
for _, r in ipairs(game.map.rooms() or {{}}) do {flt}
  local ex = {{}}; for _, x in ipairs(r.exits) do ex[#ex + 1] = x.to end
  o[#o + 1] = {{ key = r.key, map = r.map, loadable = r.loadable, exits = ex }}
  ::c::
end
return o""")


def t_catalog(a):
    q = (a.get("query") or "").lower()
    cat = L(a["run"], "return game.items.catalog()") or []
    return [c for c in cat if not q or q in c["name"].lower() or q == str(c["id"])][: a.get("limit", 60)]


def t_warp(a):
    run, key = a["run"], int(a["key"])
    r = L(run, f"return {{game.map.warp({key}, {int(a.get('gate', 0))}, {lstr(a.get('force', True))})}}")
    if not (isinstance(r, list) and r and r[0] is True):
        raise ToolError(f"warp refused: {r}")
    if not a.get("wait", True):
        return "armed"
    t0 = time.time()
    while time.time() - t0 < a.get("timeout", 60):
        s = L(run, "local m = game.map.get(); return m and {m.area, #game.roster.members(), (game.map.warp_status())}")
        if isinstance(s, list) and s[0] == key and s[1] > 0 and s[2] == "idle":
            return L(run, STATE_LUA)
        time.sleep(0.25)
    raise ToolError(f"no arrival in {key} after {a.get('timeout', 60)} s (state: {L(run, STATE_LUA)})")


def t_teleport(a):
    r = L(a["run"], f"return {{game.coordinates.set({lstr(a.get('code'))}, {int(a['x'])}, {int(a['y'])})}}")
    if not (isinstance(r, list) and r and r[0] is True):
        raise ToolError(f"teleport refused: {r}")
    return L(a["run"], "return game.coordinates.target()")


def t_give(a):
    r = L(a["run"], f"return {{game.items.give({int(a['id'])}, {int(a.get('count', 0))})}}")
    if not (isinstance(r, list) and r and isinstance(r[0], int)):
        raise ToolError(f"give refused: {r}")
    return {"index": r[0]}


def t_heal(a):
    return L(a["run"], """for _, m in ipairs(game.roster.members()) do
  game.roster.set(m.code, { hp = m.hp_max, mp = m.mp_max }) end return true""")


def t_set_stats(a):
    fields = ", ".join(f"{k} = {int(a[k])}" for k in ("hp", "mp") if a.get(k) is not None)
    return L(a["run"], f"return {{game.roster.set({lstr(a.get('code'))}, {{ {fields} }})}}")


def t_gold(a):
    return L(a["run"], f"return game.items.set_gold({int(a['cp'])})")


def t_flags(a):
    if a.get("id") is not None:
        return L(a["run"], f"return {{game.flags.story.get({int(a['id'])})}}")
    return L(a["run"], "return game.flags.story.list()")


def t_set_flag(a):
    return L(a["run"], f"return game.flags.story.set({int(a['id'])}, {int(a['v'])}, {int(a.get('v2', 0))})")


def t_skip_dialogue(a):
    return L(a["run"], f"return {{game.dialogue.skip({'true' if a.get('on', True) else 'false'})}}")


def t_keys(a):
    return sml_cli(["play", a["run"], a["movie"], "--wait"])


def t_shot(a):
    out = sml_cli(["shot", a["run"]] + (["--x"] if a.get("x") else [])).splitlines()[-1].strip()
    return {"_image": out}


S = lambda **p: {"type": "object", "properties": p, "required": [k for k, v in p.items() if v.pop("req", False)]}
RUN = lambda: {"type": "string", "description": "the sml run name (tools/sml ls)", "req": True}
INT = lambda d, req=False: {"type": "integer", "description": d, "req": req}

TOOLS = {
    "sml": (t_sml, "Run tools/sml with these args (up R --edition E --saves steam|DIR [--mods trainer] / continue R "
                   "--slot N / down R / play R MOVIE --wait / wait R EXPR / log R / ls / test R tests/X.lua).",
            S(args={"type": "string", "description": "the sml command line after `sml`", "req": True},
              timeout=INT("seconds (default 600)"))),
    "lua": (t_lua, "Evaluate Lua on the game's engine thread (globals: mod, game = mod.game, mem, sym, trainer "
                   "when the trainer mod is loaded). Returns the JSON-encoded result.",
            S(run=RUN(), code={"type": "string", "description": "a Lua chunk; `return` a value", "req": True})),
    "state": (t_state, "Edition, frame, current room/map, event running, warp status, gold, party (hp/mp/pos), exits.",
              S(run=RUN())),
    "rooms": (t_rooms, "The room table (warp destinations): key, map, loadable, exits. Optional map filter.",
              S(run=RUN(), map=INT("only this map id"))),
    "catalog": (t_catalog, "Item definitions {id, name}, filtered by a name substring or exact id.",
                S(run=RUN(), query={"type": "string"}, limit=INT("max results (60)"))),
    "warp": (t_warp, "Warp the party to room `key` (gate 0 = its first gate). force (default true) = anywhere, "
                     "instant; false = the warp item's rules + dissolve. Waits for the arrival; returns the state.",
             S(run=RUN(), key=INT("room key (rooms)", True), gate=INT("arrival gate"),
               force={"type": "boolean"}, wait={"type": "boolean"}, timeout=INT("seconds (60)"))),
    "teleport": (t_teleport, "Move a member's body box to world x, y (centi-px; the controlled member by default).",
                 S(run=RUN(), x=INT("world x (centi-px)", True), y=INT("world y (centi-px)", True),
                   code=INT("member code (50010 Arche, 50011 Sana, 50012 Stella, 50013 Chiffon)"))),
    "give": (t_give, "Give an item through the game's own room_item_add (count 0 = the item's default).",
             S(run=RUN(), id=INT("item id (catalog)", True), count=INT("count"))),
    "heal": (t_heal, "Every member's HP / MP to max.", S(run=RUN())),
    "set_stats": (t_set_stats, "Set a member's current HP / MP (the controlled member by default).",
                  S(run=RUN(), code=INT("member code"), hp=INT("hp"), mp=INT("mp"))),
    "gold": (t_gold, "Set the money (CP, clamped to the cap).", S(run=RUN(), cp=INT("CP", True))),
    "flags": (t_flags, "Story flags: one {v, v2} by id, or the whole list.", S(run=RUN(), id=INT("flag id"))),
    "set_flag": (t_set_flag, "Set story flag id to (v, v2); 0, 0 clears it.",
                 S(run=RUN(), id=INT("flag id", True), v=INT("value", True), v2=INT("second value"))),
    "skip_dialogue": (t_skip_dialogue, "Skip dialogue the game's own way (held TAB while an event runs) until "
                                       "turned off. on (default true).", S(run=RUN(), on={"type": "boolean"})),
    "keys": (t_keys, "Play a keyboard movie (rd syntax: 'R*30 .*10 ent X*2'; U D L R X C Z V F ent esc f1..f6).",
             S(run=RUN(), movie={"type": "string", "req": True})),
    "shot": (t_shot, "The game's exact back buffer (640x480) as an image; x=true for the whole X screen (UI too).",
             S(run=RUN(), x={"type": "boolean"})),
}


def call(name, args):
    if name not in TOOLS:
        raise ToolError(f"unknown tool {name}")
    return TOOLS[name][0](args)


def content(r):
    if isinstance(r, dict) and "_image" in r:
        data = base64.b64encode(open(r["_image"], "rb").read()).decode()
        return [{"type": "image", "data": data, "mimeType": "image/png"}, {"type": "text", "text": r["_image"]}]
    return [{"type": "text", "text": r if isinstance(r, str) else json.dumps(r, ensure_ascii=False)}]


def serve():
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except ValueError:
            continue
        mid, meth, p = req.get("id"), req.get("method"), req.get("params") or {}
        if mid is None:          # a notification
            continue
        try:
            if meth == "initialize":
                res = {"protocolVersion": p.get("protocolVersion", "2024-11-05"), "capabilities": {"tools": {}},
                       "serverInfo": {"name": "sml", "version": "0.1.0"}}
            elif meth == "tools/list":
                res = {"tools": [{"name": n, "description": d, "inputSchema": s} for n, (_, d, s) in TOOLS.items()]}
            elif meth == "tools/call":
                try:
                    res = {"content": content(call(p["name"], p.get("arguments") or {}))}
                except (ToolError, OSError, KeyError, subprocess.TimeoutExpired, SystemExit) as e:
                    res = {"content": [{"type": "text", "text": f"error: {e}"}], "isError": True}
            elif meth == "ping":
                res = {}
            else:
                raise ToolError(f"method not found: {meth}")
            out = {"jsonrpc": "2.0", "id": mid, "result": res}
        except ToolError as e:
            out = {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": str(e)}}
        sys.stdout.write(json.dumps(out) + "\n")
        sys.stdout.flush()


def main():
    if len(sys.argv) == 1:
        return serve()
    if sys.argv[1] == "--selftest":
        for n, (_, d, _) in TOOLS.items():
            print(f"{n:10} {d}")
        return
    args = {}
    for kv in sys.argv[2:]:
        k, _, v = kv.partition("=")
        args[k] = int(v) if v.lstrip("-").isdigit() else {"true": True, "false": False}.get(v, v)
    try:
        r = call(sys.argv[1], args)
    except ToolError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)
    print(r if isinstance(r, str) else json.dumps(r, ensure_ascii=False, indent=1))


if __name__ == "__main__":
    main()
