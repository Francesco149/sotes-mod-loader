#!/usr/bin/env bash
# tests/warp.sh R — mod.game.map.warp across frames on a running `sml` run R (in a scene).
# 1. forced warp to a loadable neighbour of the current room → arrival (area, party, status idle)
# 2. back with retail's own warp (rules + dissolve) where retail allows it, else forced again
# PASS / FAIL on stdout, exit 1 on a FAIL.
set -u
R=$1; SML="$(dirname "$0")/../tools/sml"
L() { "$SML" lua "$R" "$1"; }
fail() { echo "FAIL  warp.sh  $*"; exit 1; }
here=$(L 'return mod.game.map.get().area') || fail "no scene"
# free roam first: a load-time cutscene owns the game (map.event()) and warp refuses then — Enter through it
for i in $(seq 300); do
  [ "$(L 'return mod.game.map.event()')" = false ] && break
  [ "$i" = 300 ] && fail "the event never ended"
  "$SML" play "$R" "ent .*8" --wait >/dev/null
done
"$SML" play "$R" ".*60" --wait >/dev/null
here=$(L 'return mod.game.map.get().area')
to=$(L "for _, x in ipairs(mod.game.map.room($here).exits) do local r = mod.game.map.room(x.to)
          if r and r.loadable and x.to ~= $here then return x.to end end return 0")
[ "$to" != 0 ] || fail "room $here has no loadable exit"
arrive() {  # $1 = room key.  The arrival may run the room's own event (Enter advances it, only while not arrived).
  local ok="local m = mod.game.map.get(); return m ~= nil and m.area == $1 and #mod.game.roster.members() > 0
            and mod.game.map.warp_status() == 'idle'"
  for _ in $(seq 40); do
    "$SML" wait "$R" "$ok" --timeout 3 >/dev/null 2>&1 && return 0
    "$SML" play "$R" "ent .*20" --wait >/dev/null
  done
  fail "no arrival in $1"
}
r=$(L "return mod.game.map.warp($to, 0, true)"); [ "$r" = true ] || fail "warp($to, force) = $r"
arrive "$to"; echo "    forced $here -> $to"
"$SML" play "$R" ".*60" --wait >/dev/null
kind=$(L "local w = mod.game.roster.world(); return mod.mem.read_u32(w.world + 0x1030) * 10 + mod.mem.read_u32(w.world + 0x1034)")
if [ "$kind" = 10 ]; then   # a field room, no block: retail's warp
  r=$(L "return mod.game.map.warp($here)"); [ "$r" = true ] || fail "warp($here) = $r"
  "$SML" play "$R" ".*2" --wait >/dev/null
  s=$(L "return (mod.game.map.warp_status())"); [ "$s" = '"warping"' ] || fail "retail warp not running ($s)"
  arrive "$here"; echo "    retail $to -> $here (dissolve)"
else
  r=$(L "return mod.game.map.warp($here, 0, true)"); [ "$r" = true ] || fail "warp($here, force) = $r"
  arrive "$here"; echo "    forced $to -> $here (retail forbids warping in $to: kind/block $kind)"
fi
echo "PASS  warp.sh  [$(L 'return mod.edition')]"
