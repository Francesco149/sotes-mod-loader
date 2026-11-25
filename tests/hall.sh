#!/usr/bin/env bash
# tests/hall.sh R — the testroom mod's Mod Hall end to end (api 1.8: mod.game.script / sound + custom rooms) on a running
# `sml` run R started with `--mods testroom` (../sotes-mods/mods/testroom) and continued into a save.
#   bookshelf  examine → the book (4 pages) → closes
#   guide      first-talk lines → the choice menu → the quiz (3 narrated questions, all right) → the 500 CP prize + the
#              story flag → Goodbye
#   merchant   the shop opens on the custom stock → backed out
#   bard       custom sound → its pool loads from the overlay and plays; a stock one
#   inn        a room → retail's own inn (price window, Stay) → gold − 2000 CP, rest, back from black
#   arena      Slimes → warped to 210143, 3 hostile slimes spawned → (killed by the test) → the reward cutscene
#              (+200 CP) → back through the door to the hall
#   warp mage  "Arche's house" → the house (zone 4 arrival)
# Drives the scene runner by its wait mode (*(world+0x1054) +0x20: 1 message, 5 choice): Enter on messages, the
# cursor + Enter on choices.  Keys held ≥ 6 flips (the wine flip rate is well above the game's input poll).
# PASS / FAIL on stdout, exit 1 on a FAIL.  Looks: SHOTS=DIR writes a PNG per stage there.
set -u
R=$1; SML="$(dirname "$0")/../tools/sml"
L() { "$SML" lua "$R" "$1"; }
P() { "$SML" play "$R" "$1" --wait >/dev/null; }
W() { "$SML" wait "$R" "$1" --timeout "${2:-300}" >/dev/null; }
shot() { [ -n "${SHOTS:-}" ] && "$SML" shot "$R" "$SHOTS/$1" >/dev/null; return 0; }
fail() { echo "FAIL  hall.sh  $*"; shot "fail.png"; exit 1; }
MODE='local w = mod.mem.read_u32(mod.sym.va("g_world")) local ev = mod.mem.read_u32(w + 0x1054) return mod.game.map.event() and mod.mem.read_u32(ev + 0x20) or -1'
mode() { L "$MODE"; }
gold() { L 'return (mod.game.items.gold())'; }
# advance messages until a choice (mode 5) is up; fail after N tries
to_choice() { for _ in $(seq 30); do [ "$(mode)" = 5 ] && return 0; [ "$(mode)" = -1 ] && return 1; P "ent*6 .*40"; done; return 1; }
# pick option k (1-based) of the open choice list
pick() { P ".*20"; for _ in $(seq $(($1 - 1))); do P "D*6 .*12"; done; P "ent*6 .*60"; }
# advance until the event ends
to_end() { for _ in $(seq 40); do [ "$(mode)" = -1 ] && return 0; P "ent*6 .*40"; done; return 1; }
# walk up to the NPC with handle H (stand right of it, face left) and talk
talk() {
  local x; x=$(L "for _, a in ipairs(mod.game.actors.list()) do if a.handle == $1 then return a.x end end")
  [ -n "$x" ] && [ "$x" != null ] || fail "no actor with handle $1"
  L "mod.game.coordinates.set(nil, $x + 4400, 52000) return true" >/dev/null
  P ".*20 L*20 .*20 ${2:-V}*10 .*80"
  W 'return mod.game.map.event()' 60 || fail "talk $1: no event"
}
H() { L "return testroom.H.$1"; }

[ "$(L 'return testroom ~= nil and mod.game.script ~= nil and testroom.H ~= nil')" = true ] || fail "no testroom 0.2 / api 1.8"
W 'return not mod.game.map.event()' || fail "an event never ended"
if [ "$(L 'return mod.game.map.get().area')" != 210142 ]; then
  L 'return mod.game.map.warp(210142, 2, true)' >/dev/null
  W 'return mod.game.map.get().area == 210142 and not mod.game.map.event()' || fail "hall never loaded"
fi
[ "$(L 'return mod.res.hits("DATA", 4001) >= 1')" = true ] || fail "hall: DATA 4001 not served by the overlay"
P ".*30"; shot hall.png

# bookshelf (x 32 px; examine = X with the sword sheathed)
L 'mod.game.coordinates.set(nil, 8000, 52000) return true' >/dev/null
P ".*30 L*6 .*20"
for _ in 1 2 3; do P "X*6 .*60"; [ "$(mode)" != -1 ] && break; done
W 'return mod.game.map.event()' 60 || fail "bookshelf: no event"
P ".*200"; shot book.png
to_end || fail "bookshelf: the book never closed"
[ "$(L 'return mod.game.script.errors()')" = 0 ] || fail "bookshelf: script errors"
echo "    bookshelf: the book opened and closed"

# guide: the quiz (answers 2, 2, 1), then Goodbye (4)
g0=$(gold)
talk "$(H GUIDE)"
to_choice || fail "guide: no menu"; shot guide_menu.png
pick 3
for a in 2 2 1; do to_choice || fail "guide: no quiz choice"; pick $a; done
to_choice || fail "guide: no menu after the quiz"
g1=$(gold)
[ "$g1" -ge $((g0 + 500)) ] || [ "$g0" -ge 14500 ] || fail "guide: no prize ($g0 -> $g1)"
[ "$(L 'return mod.game.flags.story.get(990001001) ~= nil')" = true ] || fail "guide: quiz flag not set"
pick 4; to_end || fail "guide: never ended"
echo "    guide: quiz 3/3, prize $g0 -> $g1 CP, story flag 990001001 set"

# merchant: the shop on the custom stock, then out (Esc until the event ends)
talk "$(H MERCHANT)"
for _ in $(seq 10); do P "ent*6 .*40"; [ "$(mode)" != 1 ] && break; done
P ".*80"; shot shop.png
for _ in $(seq 10); do [ "$(mode)" = -1 ] && break; P "esc*6 .*40"; [ "$(mode)" = 1 ] && P "ent*6 .*40"; done
[ "$(mode)" = -1 ] || fail "merchant: never left the shop"
echo "    merchant: the shop opened and closed"

# bard: custom chime (1) → its pool loads + plays; stock 5 (4); then That's enough (8)
talk "$(H BARD)"
to_choice || fail "bard: no menu"
pick 1
st=$(L 'local s = mod.game.sound.status(testroom.SFX.chime) return s and (s.loaded and 1 or 0) .. "/" .. s.playing or "nil"' | tr -d '"')
to_choice || fail "bard: no menu after the chime"
pick 4; to_choice || fail "bard: no menu after a stock sound"
pick 8; to_end || fail "bard: never ended"
[ "${st%%/*}" = 1 ] || fail "bard: the custom sound's pool did not load ($st)"
echo "    bard: custom chime loaded + playing $st (loaded/playing voices), stock sound 5"

# inn: a room (1) → retail's inn: its "Inn - N to stay" window (Stay = Enter on its first row) → paid, rest, healed
L 'mod.game.items.set_gold(5000) return true' >/dev/null   # the room costs 2000 CP
g0=$(gold)
talk "$(H INN)"
to_choice || fail "inn: no menu"; pick 1
P ".*200"; shot inn_offer.png
for _ in $(seq 6); do P "ent*6 .*60"; [ "$(gold)" != "$g0" ] && break; done
P ".*400"; shot inn_rest.png
to_end || fail "inn: never ended"
g1=$(gold)
[ "$g1" = $((g0 - 2000)) ] || fail "inn: paid $g0 -> $g1"
P ".*30"; shot inn_after.png
echo "    inn: retail's inn, $g0 -> $g1 CP"

# arena: Slimes (1) → 210143 → 3 slimes → removed by the test (a kill without a fight) → the reward → back
talk "$(H ARENA)"
to_choice || fail "arena: no menu"; pick 1
to_end || fail "arena master: never ended"
W 'return mod.game.map.get().area == 210143 and not mod.game.map.event()' || fail "arena: never arrived"
P ".*60"
n=$(L 'local n = 0 for _, a in ipairs(mod.game.actors.list()) do if a.code == 51010 or a.code == 51011 then n = n + 1 end end return n')
[ "$n" = 3 ] || fail "arena: $n slimes, want 3"
shot arena.png
g0=$(gold)
L 'for _, a in ipairs(mod.game.actors.list()) do if a.code == 51010 or a.code == 51011 then mod.game.actors.remove(a.slot) end end return true' >/dev/null
W 'return mod.game.map.event()' 120 || fail "arena: no reward event"
P ".*100"; shot arena_reward.png
to_end || fail "arena: the reward never ended"
g1=$(gold)
[ "$g1" -ge $((g0 + 200)) ] || [ "$g0" -ge 14800 ] || fail "arena: no reward gold ($g0 -> $g1)"
L 'mod.game.coordinates.set(nil, 45600, 52000) return true' >/dev/null
P ".*30 U*20 .*30"
W 'return mod.game.map.get().area == 210142 and not mod.game.map.event()' || fail "arena: the door did not lead back to the hall"
echo "    arena: 3 slimes, cleared → reward $g0 -> $g1 CP, door back to the hall"

# warp mage: Arche's house (1)
talk "$(H WARP)"
to_choice || fail "warp: no menu"; pick 1
to_end || fail "warp mage: never ended"
W 'return mod.game.map.get().area == 210140 and not mod.game.map.event()' || fail "warp: never reached the house"
echo "    warp mage: → Arche's house"
[ "$(L 'return mod.game.script.errors()')" = 0 ] || fail "script errors: $(L 'return mod.game.script.errors()')"
echo "PASS  hall.sh"
