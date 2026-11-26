#!/usr/bin/env bash
# tests/recettear.sh R — the Recettear crossover mod end to end (api 1.11) on a running `sml` run R started with
# `--mods recettear` (its first-time setup done: `--cfg setup_trust=recettear`, OSS_STEAM=<a Steam library with
# Recettear>) and continued into a save.
#   setup      the setup ran, its data is there, the mod installed (maps edited / defined)
#   door       the storeroom (210141): the new door (zone 6) by keys → the branch (210146), its map from the overlay
#   intro      the first-visit scene runs once (story flag); Tear's starter kit lands in the bag (api 1.12)
#   cast       Recette + Tear wear converted sprite banks, stand on the floor line; a portrait bank exists; the shop
#              music plays and the engine's own BGM pick is silenced
#   tear       order 5 of the first food item: stock +5, gold down by the wholesale price
#   counter    open the shop → a customer walks in on the floor line → asks → 110 % → sold: stock −1, gold up, xp up
#   out        back through the door to the storeroom; the music stops
# PASS / FAIL on stdout, exit 1 on a FAIL.  Looks: SHOTS=DIR writes a PNG per stage there.
set -u
R=$1; SML="$(dirname "$0")/../tools/sml"
L() { "$SML" lua "$R" "$1"; }
P() { "$SML" play "$R" "$1" --wait >/dev/null; }
W() { "$SML" wait "$R" "$1" --timeout "${2:-300}" >/dev/null; }
shot() { [ -n "${SHOTS:-}" ] && "$SML" shot "$R" "$SHOTS/$1" >/dev/null; return 0; }
fail() { echo "FAIL  recettear.sh  $*"; shot "fail.png"; exit 1; }
MODE='local w = mod.mem.read_u32(mod.sym.va("g_world")) local ev = mod.mem.read_u32(w + 0x1054) return mod.game.map.event() and mod.mem.read_u32(ev + 0x20) or -1'
mode() { L "$MODE"; }
gold() { L 'return (mod.game.items.gold())'; }
ended() { [ "$(mode)" = -1 ] && { P ".*40"; [ "$(mode)" = -1 ]; }; }
to_choice() { for _ in $(seq 40); do [ "$(mode)" = 5 ] && return 0; ended && return 1; P "ent*6 .*40"; done; return 1; }
pick() { P ".*20"; for _ in $(seq $(($1 - 1))); do P "D*6 .*12"; done; P "ent*6 .*60"; }
to_end() { for _ in $(seq 60); do ended && return 0; P "ent*6 .*40"; done; return 1; }
talk() {
  local x; x=$(L "for _, a in ipairs(mod.game.actors.list()) do if a.handle == $1 then return a.x end end")
  [ -n "$x" ] && [ "$x" != null ] || fail "no actor with handle $1"
  L "mod.game.coordinates.set(nil, $x + 4400, 52000) return true" >/dev/null
  P ".*20 L*20 .*20 V*10 .*80"
  W 'return mod.game.map.event()' 60 || fail "talk $1: no event"
}
area() { L 'return (mod.game.map.get() or {}).area'; }

[ "$(L 'return recettear ~= nil and mod.setup.done ~= nil and mod.game.maps ~= nil')" = true ] || fail "no recettear mod / api 1.11 (setup not done?)"
[ "$(L 'return mod.fs.exists(recettear.A.source) and recettear.A.chars.recette.n > 0 and (recettear.A.faces.recette or 0) > 0')" = true ] || fail "setup data incomplete"
W 'return not mod.game.map.event()' || fail "an event never ended"
# room in the bag for the kit (the Steam save carries SE's 80): 20 unworn records into storage, as the box does
L 'local w = mod.mem.read_u32(mod.sym.va("g_world")) local room = mod.mem.read_u32(w + 0x3788)
   local k = 0 for i = mod.mem.read_u16(room + 0x4010) - 1, 0, -1 do local r = room + 0x2010 + 16 * i
   if k < 20 and mod.mem.read_u32(r + 4) == 0 and mod.mem.read_u8(r + 0xa) == 0 then mod.mem.write_u8(r + 0xa, 1) k = k + 1 end end return k' >/dev/null
if [ "$(area)" != 210141 ]; then
  L 'return mod.game.map.warp(210141, 2, true)' >/dev/null
  W 'return (mod.game.map.get() or {}).area == 210141 and mod.game.map.warp_status() == "idle" and not mod.game.map.event()' || fail "storeroom never loaded"
fi
[ "$(L 'return mod.game.maps.hits(1026) >= 1 and mod.game.maps.edits(1026) >= 1')" = true ] || fail "storeroom: the edited DATA 1026 was not loaded"
# the new door: its zone device is at x 192 (column 5 + 32 px) — stand in it, press Up
L 'mod.game.coordinates.set(nil, 20800, 52000) return true' >/dev/null
P ".*40"; shot storeroom.png
P "U*20 .*120"
W 'return (mod.game.map.get() or {}).area == 210146' 80 || fail "door: zone 6 did not lead to the branch"
[ "$(L 'return mod.game.maps.hits(4010) >= 1')" = true ] || fail "branch: DATA 4010 not served"
echo "    door: storeroom zone 6 -> 210146"

# the intro (first visit): runs by itself, once
W 'return mod.game.map.event() or recettear.fget(recettear.F.INTRO) == 1' 120 || fail "intro: never started"
P ".*200"; shot intro.png
to_end || fail "intro: never ended"
[ "$(L 'return recettear.fget(recettear.F.INTRO)')" = 1 ] || fail "intro: flag not set"
[ "$(L 'return mod.game.script.errors()')" = 0 ] || fail "intro: script errors"
echo "    intro: ran, flag set"
# api 1.12: Tear's starter kit — Recettear's own items as new items of this game (needs room in the bag: SE carries
# 80 normal items; a full bag gets retail's "bag is full" instead)
if [ "$(L 'return mod.game.items.defined ~= nil and mod.game.items.defined(recettear.IT.longsword) == true')" = true ]; then
  KIT=$(L 'local n = {} for _, it in ipairs(mod.game.items.list()) do if it.id >= 70400000 then n[it.name] = (n[it.name] or 0) + math.max(1, it.count) end end
           return recettear.fget(recettear.F.KIT) == 1 and n["Longsword"] and n["Windbreaker"] and n["Ham Sandwich"] == 3 and n["Strong Concoction"] and n["Ferromin"] and true or n')
  [ "$KIT" = true ] || fail "kit: $KIT"
  echo "    kit: Longsword, Windbreaker, 3 Ham Sandwiches, Strong Concoction, Ferromin (new item rows, Recettear's icons)"
fi

# cast: on the leader's floor line, in our banks; a portrait; the music
[ "$(L 'local f, bad, n = nil, nil, 0 for _, a in ipairs(mod.game.actors.list()) do if a.leader then f = a.y + a.h end end
        for _, a in ipairs(mod.game.actors.list()) do if a.handle == recettear.H.RECETTE or a.handle == recettear.H.TEAR then n = n + 1
          local b = mod.game.sprites.bank(mod.mem.read_u16(a.ptr + 0x48))
          if a.y + a.h ~= f then bad = (bad or "") .. a.handle .. ":" .. (a.y + a.h - f) .. " " end
          if not (b and b.ours) then bad = (bad or "") .. a.handle .. ":bank " end end end
        return n == 2 and (bad or "ok") or "cast " .. n')" = '"ok"' ] || fail "cast: not on the floor / not in converted banks"
[ "$(L 'local b = recettear.face_bank("recette") local i = b and mod.game.sprites.bank(b) return i ~= nil and i.cell_w == 160 and i.cell_h == 176')" = true ] || fail "portrait bank"
[ "$(L 'local s = mod.game.sound.music_status() return s ~= nil and s.playing and s.file:find("bgm_shop") ~= nil')" = true ] || fail "music: the shop theme is not playing"
echo "    cast: Recette + Tear on the floor in converted banks; portrait bank; shop music playing"

# Tear: 5 of the first food item
L 'mod.game.items.set_gold(5000) return true' >/dev/null
talk "$(L 'return recettear.H.TEAR')"
to_choice || fail "tear: no menu"; shot tear_menu.png
pick 1; to_choice || fail "tear: no list"; shot tear_list.png
pick 1; to_choice || fail "tear: no quantity"
pick 2
W "return #recettear.in_stock() == 1" 80 || fail "tear: nothing ordered"
[ "$(L 'local e = recettear.in_stock()[1] return e.n == 5 and (mod.game.items.gold()) == 5000 - 5 * math.max(1, math.floor(e.g.price * (72 - 2 * recettear.level()) / 100 + 0.5))')" = true ] || fail "tear: stock / price wrong"
to_choice || fail "tear: no list after the order"
P "esc*6 .*60"; to_choice || fail "tear: no menu after the list"
pick 6; to_end || fail "tear: talk never ended"
echo "    tear: 5 ordered at wholesale"

# the counter: one customer, the middle of her range, 110 %
L 'local r = recettear r.fset(r.F.TRAFFIC, 9) r.shop.next_kind = "woman" r.shop.max_customers = 1 r.shop.rng = function(a, b) return math.floor((a + b) / 2) end return true' >/dev/null
g0=$(gold); xp0=$(L 'return recettear.fget(recettear.F.XP)')
talk "$(L 'return recettear.H.RECETTE')"
to_choice || fail "recette: no menu"; shot recette_menu.png
pick 1; to_end || fail "recette: opening never ended"
[ "$(L 'return recettear.shop.open')" = true ] || fail "counter: the shop did not open"
W 'return recettear.shop.cust ~= nil and recettear.shop.cust.state == "in"' 120 || fail "counter: no customer came"
P ".*60"; shot customer_walk.png
[ "$(L 'local c, f = recettear.shop.cust for _, a in ipairs(mod.game.actors.list()) do if a.handle == recettear.H.RECETTE then f = a.y + a.h end end
        for _, a in ipairs(mod.game.actors.list()) do if a.slot == c.slot then return a.y + a.h - f end end return "gone"')" = 0 ] || fail "counter: the customer is not on the floor line"
W 'return mod.game.map.event()' 200 || fail "counter: the haggle never started"
to_choice || fail "counter: no price list"; shot price_list.png
P "ent*6 .*60"                                   # the cursor starts on 110 %
to_end || fail "counter: the haggle never ended"; shot sold.png
[ "$(L "local r = recettear return r.shop.sold == 1 and r.in_stock()[1].n == 4 and (mod.game.items.gold()) > $g0 and r.fget(r.F.XP) > $xp0")" = true ] || fail "counter: no sale recorded"
W 'return recettear.shop.cust == nil' 200 || fail "counter: the customer never left"
W 'return mod.game.map.event()' 200 || fail "counter: no closing scene"
to_end || fail "counter: closing never ended"
[ "$(L 'return recettear.shop.open == false and mod.game.script.errors() == 0')" = true ] || fail "counter: not closed / script errors"
echo "    counter: a customer walked in, bought at 110 %, left; closed"

# out through the door
L 'mod.game.coordinates.set(nil, 76800, 52000) return true' >/dev/null
P ".*30 U*20 .*120"
W 'return (mod.game.map.get() or {}).area == 210141' 80 || fail "out: the door did not lead back"
W 'return mod.game.sound.music_status() == nil' 40 || fail "out: the music kept playing"
echo "PASS  recettear.sh"
