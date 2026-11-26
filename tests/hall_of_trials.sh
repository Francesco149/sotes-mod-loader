#!/usr/bin/env bash
# tests/hall_of_trials.sh R — the recettear mod's Hall of Trials (loader api 1.13: maps.set, items.take) end to end on a
# running `sml` run R started with `--mods recettear` (setup 3 done: `--cfg setup_trust=recettear`, OSS_STEAM=<a Steam
# library with Recettear>) and continued into a save.
#   enter     Recette's menu "The Hall of Trials" -> Yes -> floor 1 (room 240901, its map re-served: DATA 4021)
#   floor 1   the banner scene; Recettear's monsters (map records of the mod's types) on the floor; the chest records
#             of this floor freed in the object perpetuity table; a chest opened by the talk key (retail's chest)
#   circle    walking onto the magic circle asks "Go to the next floor?" -> Yes -> floor 2, a different layout
#   drops     floor 2's drop table is Recettear's (Slime Fluid common, Worn Sword rare for green slimes)
#   den       floor 5: its circle leads home ("cleared"); the welcome-back scene shelves the finds (items.take +
#             shop stock)
#   fallen    a defeat during a run (the counter room +0x40b0 the game's Game Over raises): all finds but one are taken,
#             Tear's scene, back to the branch
#   again     a second run gets another seed (another layout)
# PASS / FAIL on stdout, exit 1 on a FAIL.  Looks: SHOTS=DIR writes a PNG per stage there.
set -u
R=$1; SML="$(dirname "$0")/../tools/sml"
L() { "$SML" lua "$R" "$1"; }
P() { "$SML" play "$R" "$1" --wait >/dev/null; }
W() { "$SML" wait "$R" "$1" --timeout "${2:-300}" >/dev/null; }
shot() { [ -n "${SHOTS:-}" ] && "$SML" shot "$R" "$SHOTS/$1" >/dev/null; return 0; }
fail() { echo "FAIL  hall_of_trials.sh  $*"; shot "fail.png"; exit 1; }
MODE='local w = mod.mem.read_u32(mod.sym.va("g_world")) local ev = mod.mem.read_u32(w + 0x1054) return mod.game.map.event() and mod.mem.read_u32(ev + 0x20) or -1'
mode() { L "$MODE"; }
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
at() { W "return (mod.game.map.get() or {}).area == $1 and mod.game.map.warp_status() == 'idle'" "${2:-300}"; }
H='recettear.hall()'
# the leader onto the circle: from 60 px left of it, walking right until the prompt
onto_circle() {
  local x; x=$(L "return $H.run.info.exit.x * 100 - 6000")
  L "mod.game.coordinates.set(nil, $x, $H.run.info.exit.feet * 100 - 9000) return true" >/dev/null
  P ".*60"
  for _ in $(seq 10); do P "R*20 .*10"; [ "$(L 'return mod.game.map.event()')" = true ] && return 0; done
  return 1
}

[ "$(L "return recettear ~= nil and recettear.hall ~= nil and $H ~= nil and mod.game.maps.set ~= nil and mod.game.items.take ~= nil")" = true ] \
  || fail "no Hall of Trials (setup 3 / loader api 1.13?)"
[ "$(L 'return #recettear.A.hall.pool > 30 and #recettear.A.hall.floors >= 4 and recettear.A.chars.circle ~= nil')" = true ] || fail "setup data: hall tables / circle"
to_end || fail "an event never ended"
# room in the bag (the Steam save carries SE's 80): 30 unworn records into storage, as the box does
L 'local w = mod.mem.read_u32(mod.sym.va("g_world")) local room = mod.mem.read_u32(w + 0x3788)
   local k = 0 for i = mod.mem.read_u16(room + 0x4010) - 1, 0, -1 do local r = room + 0x2010 + 16 * i
   if k < 30 and mod.mem.read_u32(r + 4) == 0 and mod.mem.read_u8(r + 0xa) == 0 then mod.mem.write_u8(r + 0xa, 1) k = k + 1 end end return k' >/dev/null
if [ "$(L 'return (mod.game.map.get() or {}).area')" != 210146 ]; then
  L 'return mod.game.map.warp(210146, 2, true)' >/dev/null
  at 210146 || fail "the branch never loaded"
fi
P ".*80"; to_end

# enter: through Recette's menu
RUNS0=$(L "return recettear.fget($H.F.RUNS)")
talk 991001
to_choice || fail "enter: no menu"
pick 4
to_choice || fail "enter: no Yes / No"
shot enter.png
pick 1
to_end
at 240901 || fail "enter: floor 1 never loaded"
[ "$(L 'return mod.game.maps.hits(4021) >= 1')" = true ] || fail "floor 1: DATA 4021 not served"
SEED1=$(L "return $H.run.seed")
[ "$(L "return recettear.fget($H.F.RUNS)")" = $((RUNS0 + 1)) ] || fail "enter: run counter"
echo "    enter: Recette's menu -> floor 1 (seed $SEED1)"

# floor 1: banner, monsters, chest
W "return $H.run.banner == 1" 200 || fail "floor 1: no banner scene"
shot floor1_banner.png
to_end || fail "floor 1: banner never ended"
N=$(L 'local n = 0 for _, a in ipairs(mod.game.actors.list()) do if a.type and a.type >= 51972 and a.type <= 51975 then n = n + 1 end end return n')
[ "$N" -ge 3 ] || fail "floor 1: $N monsters"
[ "$(L 'for _, a in ipairs(mod.game.actors.list()) do if a.type == 50989 then return true end end return false')" = true ] || fail "floor 1: no circle"
echo "    floor 1: banner, $N Recettear monsters, the circle"
CH=$(L 'local m = mod.game.maps.parse(mod.game.maps.bytes(4021)) for _, o in ipairs(m.objs) do if o.rec[5] == 70220 then return o.rec[2] .. " " .. o.rec[3] .. " " .. o.A[7] .. " " .. o.A[8] end end')
set -- $(echo "$CH" | tr -d '"')
CX=$1; CY=$2; CID=$3; CN=$4
L "for _, a in ipairs(mod.game.actors.list()) do if a.type and a.type ~= 50989 then mod.game.actors.remove(a.slot) end end return true" >/dev/null   # (no foe near: touch_pass refuses then)
L "_G.hall_g0 = mod.game.items.gold() _G.hall_n0 = #mod.game.items.list() return true" >/dev/null
# the chest stands at one end of a stretch of floor: come at it along that floor, from whichever side is floor
for side in "+100 L" "-80 R"; do
  set -- $side
  L "mod.game.coordinates.set(nil, ($CX $1) * 100, ($CY - 100) * 100) return true" >/dev/null
  P ".*80"
  for _ in $(seq 8); do P "$2*12 .*10 V*10 .*60"; [ "$(L 'return mod.game.map.event()')" = true ] && break 2; done
done
P ".*80"; shot chest.png
to_end || fail "chest: the scene never ended"
GOT=$(L "return {mod.game.items.gold() - hall_g0, #mod.game.items.list() - hall_n0}")
[ "$GOT" != "[0,0]" ] || fail "chest ($CID x $CN) gave nothing"
echo "    chest: item $CID x $CN -> gold / records $GOT (retail's chest)"

# circle: floor 1 -> 2
onto_circle || fail "circle: no prompt on floor 1"
to_choice || fail "circle: no Yes / No"
shot circle.png
pick 1
to_end
at 240902 || fail "circle: floor 2 never loaded"
[ "$(L 'return mod.game.maps.hits(4022) >= 1')" = true ] || fail "floor 2: DATA 4022 not served"
DROPS=$(L 'local d = mod.game.actors.defined(51975) return d and d.drops and #d.drops or 0')
[ "$DROPS" -ge 1 ] || fail "floor 2: green slimes have no drop table"
echo "    circle: Yes -> floor 2 ($DROPS drops for green slimes)"
to_end

# den: floor 5 (jumped to), its circle leads home
L "return {$H.go(5)}" >/dev/null
at 240905 || fail "den never loaded"
P ".*120"; to_end
UNITS='local n = 0 for _, it in ipairs(mod.game.items.list()) do if (it.id == 70400002 or it.id == 70403105) and not it.stored and it.owner == 0 then n = n + math.max(1, it.count) end end return n'
U0=$(L "$UNITS")
L 'mod.game.items.give(70400002, 1) mod.game.items.give(70403105, 2) return true' >/dev/null    # finds to bring home
F0=$(L "local n = 0 for _, f in ipairs($H.finds()) do n = n + f.n end return n")
onto_circle || fail "den: no prompt"
to_choice || fail "den: no Yes / No"
shot den.png
pick 1
at 210146 || fail "den: not back at the branch"
P ".*120"
to_choice || fail "home: no shelve question"
shot home.png
pick 1
to_end
[ "$(L 'return recettear.stock(70400002) >= 1 and recettear.stock(70403105) >= 2')" = true ] || fail "home: finds not on the shelves"
U1=$(L "$UNITS")
[ "$U1" -le "$U0" ] || fail "home: finds still in the bag ($U0 before, $U1 after)"
echo "    den: circle -> the branch; $F0 finds onto the shelves (clears $(L "return recettear.fget($H.F.CLEARS)"))"

# fallen: a run, finds, then a defeat (the counter the Game Over raises), Recettear's rule
L "return {$H.start()}" >/dev/null
at 240901 || fail "fallen: no floor 1"
P ".*120"; to_end
SEED2=$(L "return $H.run.seed")
[ "$SEED2" != "$SEED1" ] || fail "again: the same seed twice"
L 'mod.game.items.give(70400002, 1) mod.game.items.give(70403105, 1) mod.game.items.give(70404014, 1) return true' >/dev/null
L 'local w = mod.mem.read_u32(mod.sym.va("g_world")) local room = mod.mem.read_u32(w + 0x3788) mod.mem.write_i32(room + 0x40b0, mod.mem.read_i32(room + 0x40b0) + 1) return true' >/dev/null
W 'return mod.game.map.event()' 120 || fail "fallen: no scene"
P ".*120"; shot fallen.png
[ "$(mode)" = 1 ] || fail "fallen: Tear's line is not waiting on the screen (mode $(mode))"
[ "$(L 'return (mod.game.map.get() or {}).area')" != 210146 ] || fail "fallen: the scene did not play where the party fell"
to_end
FALL=$(L "local f = $H.run.last_fall return f and (f.found .. ' ' .. f.lost .. ' ' .. tostring(f.kept ~= nil)) or 'none'")
[ "$FALL" = '"3 2 true"' ] || fail "fallen: $FALL (3 found, 2 lost, 1 kept expected)"
at 210146 || fail "fallen: not back at the branch"
P ".*120"; to_end
echo "    fallen: 3 finds -> 2 lost, 1 kept, back at the branch; again: seed $SEED2 != $SEED1"
[ "$(L 'return mod.game.script.errors()')" = 0 ] || fail "script errors"
echo "PASS  hall_of_trials.sh"
