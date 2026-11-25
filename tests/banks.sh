#!/usr/bin/env bash
# tests/banks.sh R — sprite banks past the retail tables (api 1.10: mod.game.sprites relocates the bank tables) on a
# running `sml` run R continued into a save (no mod needed).
#   extend     capacity(): the tables are relocated (every site verified), 7000+ banks free
#   40 banks   40 recolours of the leader's own sheet → 40 new banks, all past the retail count
#   drawn      the leader wears each in turn for a few frames: every one decodes a frame (the engine drew from it),
#              through the mirror offset too (facing left)
#   retail     new_bank{retail = true} still hands out one of the game's own unused banks
#   stock      back in the stock look, the stock bank still draws; a room change and back keeps everything alive
# PASS / FAIL on stdout, exit 1 on a FAIL.  Looks: SHOTS=DIR writes PNGs there.
set -u
R=$1; SML="$(dirname "$0")/../tools/sml"
L() { "$SML" lua "$R" "$1"; }
P() { "$SML" play "$R" "$1" --wait >/dev/null; }
W() { "$SML" wait "$R" "$1" --timeout "${2:-300}" >/dev/null; }
shot() { [ -n "${SHOTS:-}" ] && "$SML" shot "$R" "$SHOTS/$1" >/dev/null; return 0; }
fail() { echo "FAIL  banks.sh  $*"; shot "banks_fail.png"; exit 1; }
N=40

W 'return not mod.game.map.event()' || fail "an event never ended"
cap=$(L 'local c = mod.game.sprites.capacity() return ("%s %d %d %s"):format(tostring(c.extended), c.retail, c.free, tostring(c.error))')
case "$cap" in '"true '*) ;; *) fail "not extended: $cap";; esac
echo "    extend: $cap (extended, retail banks, free)"

r=$(L "local SPR, A = mod.game.sprites, mod.game.actors
  local lead for _, a in ipairs(A.list()) do if a.leader then lead = a end end
  local stock = mod.mem.read_u16(lead.ptr + 0x48)
  local img = assert(SPR.sheet(stock))
  _G.BT = { stock = stock, banks = {}, ptr = lead.ptr }
  for k = 1, $N do
    local im = img:clone()
    for i = 1, 255 do                                  -- rotate the hue a step per bank
      local c = img.pal[i] local t = k / $N
      im.pal[i] = { math.floor(c[1] * (1 - t) + c[3] * t), math.floor(c[2] * (1 - t) + c[1] * t), math.floor(c[3] * (1 - t) + c[2] * t) }
    end
    local b, err = SPR.new_bank { like = stock, image = im }
    if not b then return 'new_bank ' .. k .. ': ' .. tostring(err) end
    BT.banks[k] = b
  end
  local lo, hi = BT.banks[1], BT.banks[$N]
  return (lo >= SPR.capacity().retail and hi == lo + $N - 1) and ('ok ' .. lo .. '..' .. hi) or ('bad range ' .. lo .. '..' .. hi)")
case "$r" in '"ok '*) ;; *) fail "$r";; esac
echo "    $N banks: $r"

# the leader wears each one for a few frames, turning around half way (the mirror frames)
L 'BT.k, BT.seen, BT.h = 0, 0, mod.on_frame(function()
     if not BT.k or BT.k > #BT.banks * 6 then return end
     local i = math.floor(BT.k / 6) + 1
     if BT.k > 0 and BT.k % 6 == 0 and mod.game.sprites.bank(BT.banks[i - 1]).decoded then BT.seen = BT.seen + 1 end
     if i <= #BT.banks then mod.game.sprites.set_look(BT.ptr, BT.banks[i]) end
     BT.k = BT.k + 1
   end) return true' >/dev/null
P ".*60 L*20 .*80"; shot banks_mid.png
P "R*20 .*200"
W "return BT.k > $N * 6" 120 || fail "the look cycle never finished"
seen=$(L 'return BT.seen')
[ "$seen" = "$N" ] || fail "only $seen of $N banks drew a frame"
shot banks_last.png
echo "    drawn: $seen / $N banks decoded a frame on the leader"

r=$(L 'local SPR = mod.game.sprites local b, err = SPR.new_bank { like = BT.stock, image = assert(SPR.sheet(BT.stock)), retail = true }
       BT.retail = b return b and b < SPR.capacity().retail and b or ("bad: " .. tostring(b) .. " " .. tostring(err))')
case "$r" in [0-9]*) ;; *) fail "retail bank: $r";; esac
L 'mod.game.sprites.set_look(BT.ptr, BT.retail) return true' >/dev/null; P ".*30"
[ "$(L 'return mod.game.sprites.bank(BT.retail).decoded')" = true ] || fail "the retail bank $r never drew"
echo "    retail: bank $r (one of the game's own unused ones) draws too"

L 'mod.game.sprites.set_look(BT.ptr, BT.stock) return true' >/dev/null; P ".*30"; shot banks_stock.png
# a room change and back (frames are unloaded by group on the way; ours stay, the stock ones re-decode)
here=$(L 'return mod.game.map.get().area')
dest=$(L 'local m = mod.game.map for _, r in ipairs(m.rooms()) do if r.loadable and r.key ~= m.get().area and not r.town then return r.key end end')
L "return mod.game.map.warp($dest, 0, true)" >/dev/null
W "return mod.game.map.get().area == $dest and not mod.game.map.event()" 400 || fail "warp to $dest never arrived"
P ".*60"
L "return mod.game.map.warp($here, 0, true)" >/dev/null
W "return mod.game.map.get().area == $here and not mod.game.map.event()" 400 || fail "warp back to $here never arrived"
P ".*60"
L 'for _, a in ipairs(mod.game.actors.list()) do if a.leader then BT.ptr = a.ptr end end mod.game.sprites.set_look(BT.ptr, BT.banks[7]) return true' >/dev/null
P ".*40"; shot banks_after_warp.png
[ "$(L 'return mod.game.sprites.bank(BT.banks[7]).decoded')" = true ] || fail "bank 7 not drawn after the room change"
L 'mod.game.sprites.set_look(BT.ptr, BT.stock) return true' >/dev/null; P ".*20"
echo "    stock look restored; room change $here -> $dest -> $here survived"
echo "PASS  banks.sh"
