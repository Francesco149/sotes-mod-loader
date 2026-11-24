-- tests/voice.lua — the early-boot monster-SFX fix (core/voice.c), any edition, after boot (title is enough).
-- SE + voice on: the registrar's deluxe-skip je is redirected and EVERY sound-def row registered: no-variant rows
-- (monsters) with their normal id, deluxe rows with their deluxe id from the voice bank (⇒ the bank was loaded
-- before the registrar ran).  SE + voice=0: the shipped registrar, which drops the no-variant rows.  enold: off.
local fails, info = {}, {}
local function check(c, msg) if not c then fails[#fails + 1] = msg end end
local v = mod.game.voice.status()
info[#info + 1] = ("state %s%s wait %d ms; registrar %08x je %08x bank %08x; rows %s (+%s shadowed): deluxe0 %s/%s, deluxe %s/%s, off %s")
  :format(v.state, v.why and (" (" .. v.why .. ")") or "", v.wait_ms, v.registrar, v.je, v.bank or 0, tostring(v.rows), tostring(v.shadowed),
    tostring(v.deluxe0_ok), tostring(v.deluxe0), tostring(v.deluxe_ok), tostring(v.deluxe), tostring(v.deluxe_off))
if mod.edition == "enold" then
  check(v.state == "off", "enold: off")
elseif v.state == "patched" then
  check(v.bank and v.bank ~= 0, "voice bank loaded")
  check(v.deluxe0 > 0 and v.deluxe0_ok == v.deluxe0, "every no-variant row registered its normal id")
  check(v.deluxe > 0 and v.deluxe_ok == v.deluxe, "every deluxe row registered from the bank")
elseif v.state == "unpatched" then
  check(v.deluxe0 > 0 and v.deluxe0_ok == 0, "shipped registrar drops the no-variant rows")
  check(v.deluxe > 0 and v.deluxe_ok == v.deluxe, "every deluxe row registered from the bank")
else
  check(false, "unexpected state " .. v.state)
end
return { pass = #fails == 0, fails = fails, info = info, edition = mod.edition }
