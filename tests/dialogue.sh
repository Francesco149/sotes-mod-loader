#!/usr/bin/env bash
# tests/dialogue.sh R — mod.game.dialogue.skip on a running `sml` run R started with `--saves none` (title).
# 1. new game by Enter (the default cursor is the new game) until the intro event owns the game; enold's new-game
#    config opens on "Game Difficulty", so after 80 tries (well past the title) it's Up (wraps to "Start Game") + Enter
# 2. control: left alone, the event's dialogue waits (still running 600 frames later)
# 3. dialogue.skip(true): the game's own TAB skip ends the event; free roam then holds nothing
# SKIP_ON='trainer.skip_dialogue(true)' (a run with --mods trainer) drives the trainer mod's option instead.
# PASS / FAIL on stdout, exit 1 on a FAIL.
set -u
R=$1; SML="$(dirname "$0")/../tools/sml"
L() { "$SML" lua "$R" "$1"; }
fail() { L 'mod.game.dialogue.skip(false)' >/dev/null 2>&1; echo "FAIL  dialogue.sh  $*"; exit 1; }
[ "$(L 'return mod.game.dialogue.skip(false)')" = false ] || fail "no mod.game.dialogue"
for i in $(seq 600); do                      # logos → title → new game → narration → the first event
  [ "$(L 'return mod.game.map.event() == true')" = true ] && break
  [ "$i" = 600 ] && fail "no event after the new game"
  if [ "$i" -le 80 ]; then mv="ent*2 .*38"; else mv="U*2 .*20 ent*2 .*38"; fi
  "$SML" play "$R" "$mv" --wait >/dev/null
done
area=$(L 'return mod.game.map.get().area')
"$SML" play "$R" ".*600" --wait >/dev/null
[ "$(L 'return mod.game.map.event()')" = true ] || fail "control: the event in $area ended with no input"
echo "    control: event in $area still waiting after 600 idle frames"
f0=$(L "${SKIP_ON:-mod.game.dialogue.skip(true)}; return mod.frame()")
"$SML" wait "$R" 'mod.game.map.event() == false' --timeout 600 >/dev/null || fail "skip: the event never ended"
f1=$(L 'return mod.frame()')
"$SML" play "$R" ".*30" --wait >/dev/null
r=$(L 'local on, h = mod.game.dialogue.get(); return tostring(on) .. "," .. tostring(h) .. "," .. tostring(mod.game.map.event())')
case "$r" in
  '"true,false,false"') ;;
  *'true,true,true'*) ;;                     # the next room's own event already started: skip is doing its job
  *) fail "after the event: get/event = $r" ;;
esac
L "${SKIP_OFF:-mod.game.dialogue.skip(false)}" >/dev/null
echo "    skip: the event ended after $((f1 - f0)) frames (now $(L 'return mod.game.map.get().area'))"
echo "PASS  dialogue.sh"
