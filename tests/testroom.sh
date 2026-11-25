#!/usr/bin/env bash
# tests/testroom.sh R — custom content end to end (api 1.7: mod.res + map.add_room / set_exit) on a running `sml` run R
# started with `--mods testroom` (../sotes-mods/mods/testroom) and continued into a save (`sml continue R`).
# 1. house: warp to Arche's house (210140); the game loads the mod's DATA 1025 (overlay hit) with the new door (zone 4)
# 2. door: stand at it (x 64 px, ground floor), hold Up → the scene loop loads the custom room 210142 from DATA 4001
# 3. talk: face the custom NPC (handle 990001), V → the mod's script runs through the retail primitives: 3 message
#    boxes, Enter through them, the event ends (script_end) and the game keeps running
# 4. again: a second talk = the second-visit line; then back through the room's door → the house (zone 4 arrival)
# Keys are held ≥ 10 flips: under wine the flip rate (~200 fps) is well above the game's input poll, short taps drop.
# PASS / FAIL on stdout, exit 1 on a FAIL.  Looks: SHOTS=DIR writes house.png / room.png / talk.png there.
set -u
R=$1; SML="$(dirname "$0")/../tools/sml"
L() { "$SML" lua "$R" "$1"; }
P() { "$SML" play "$R" "$1" --wait >/dev/null; }
W() { "$SML" wait "$R" "$1" --timeout "${2:-900}" >/dev/null; }
shot() { [ -n "${SHOTS:-}" ] && "$SML" shot "$R" "$SHOTS/$1" >/dev/null; return 0; }
fail() { echo "FAIL  testroom.sh  $*"; exit 1; }
[ "$(L 'return testroom ~= nil and mod.res ~= nil')" = true ] || fail "no testroom mod / mod.res (api 1.7)"
W 'return not mod.game.map.event()' || fail "an event never ended"

# 1. house
[ "$(L 'return mod.game.map.warp(210140, 1, true)')" = true ] || fail "warp to the house refused"
W 'return mod.game.map.get().area == 210140 and not mod.game.map.event()' || fail "house never loaded"
[ "$(L 'return mod.res.hits("DATA", 1025) >= 1')" = true ] || fail "house: the game did not load the overlay's DATA 1025"
[ "$(L 'for _, e in ipairs(mod.game.map.room(210140).exits) do if e.gate == 4 and e.to == 210142 then return true end end return false')" = true ] \
  || fail "house: no zone-4 exit"
echo "    house: DATA 1025 from the overlay, zone 4 → 210142"

# 2. the new door
L 'mod.game.coordinates.set(nil, 6400, 52000) return true' >/dev/null
P ".*40"; shot house.png
P "U*20 .*30"
W 'return mod.game.map.get().area == 210142 and not mod.game.map.event()' || fail "door: room 210142 never loaded (area $(L 'return mod.game.map.get().area'))"
[ "$(L 'return mod.res.hits("DATA", 4001) >= 1')" = true ] || fail "room: DATA 4001 not loaded from the overlay"
P ".*40"; shot room.png
echo "    door: Up at the new door → 210142 (DATA 4001)"

# 3. talk to the NPC (x 224 px, facing right): stand right of her, turn left, V
talk() {
  L 'mod.game.coordinates.set(nil, 31000, 52000) return true' >/dev/null
  P ".*20 L*30 .*40 V*10 .*60"
  [ "$(L 'return mod.game.map.event()')" = true ] || fail "talk $1: no event (talks $(L 'return testroom.talks()'))"
  [ "$(L 'return testroom.talks()')" = "$1" ] || fail "talk $1: the mod's script did not run"
  P ".*300"; [ "$1" = 1 ] && shot talk.png
  for i in $(seq 8); do
    [ "$(L 'return mod.game.map.event()')" = false ] && break
    P "ent*10 .*200"
  done
  [ "$(L 'return mod.game.map.event()')" = false ] || fail "talk $1: the event never ended"
  P ".*60"
  [ "$(L 'return mod.game.map.get().area')" = 210142 ] || fail "talk $1: left the room?"
}
talk 1; echo "    talk 1: the 3-line script ran and ended"
talk 2; echo "    talk 2: the second-visit line ran and ended"

# 4. back through the room's ground-floor door (zone 2, x 448 px) → the house's new door
L 'mod.game.coordinates.set(nil, 45600, 52000) return true' >/dev/null
P ".*30 U*20 .*30"
W 'return mod.game.map.get().area == 210140 and not mod.game.map.event()' || fail "back: the house never loaded"
x=$(L 'return mod.game.coordinates.get().x')
[ "$x" -lt 16000 ] || fail "back: arrived at x $x, not the new door"
echo "    back: 210142 zone 2 → the house's new door (x $x)"
echo "PASS  testroom.sh"
