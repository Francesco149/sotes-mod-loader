-- examples/probe_live/init.lua — an in-game sanity probe for P0-P2.
--
-- Runs on the engine thread. On init it checks mod.mem against the real exe; then every
-- ~2s (via on_frame, proving the safepoint hook fires) it logs the live party roster +
-- the controlled member's coordinates.  At the title the roster is empty (no fault); load
-- a save and the members appear -> proof the RE'd bindings read the real game.

mod.log("=== probe_live init (engine thread) ===")
local m = mod.mem
mod.log(string.format("  exe base = 0x%08x  MZ = 0x%04x  reloc(0x437c70) = 0x%08x",
  m.base(), m.read_u16(m.base()) or 0, m.reloc(0x437c70)))
mod.log(string.format("  scan('4d 5a') = %s", (function(h) return h and string.format("0x%08x", h) or "nil" end)(m.scan("4d 5a"))))

local f = 0
mod.on_frame(function()
  f = f + 1
  if f % 120 ~= 0 then return end                 -- ~ every 2s (SotES ~60fps)
  local r = mod.game.roster.members()
  mod.log(string.format("[frame %d] roster = %d member(s)", f, #r))
  for _, mem in ipairs(r) do
    mod.log(string.format("   %-7s Lv%d  pos=(%d,%d)  hp=%d/%d  active=%s",
      mem.name, mem.level, mem.x, mem.y, mem.hp, mem.hp_max, tostring(mem.active)))
  end
  local t = mod.game.coordinates.target()
  if t then mod.log(string.format("   controlled member @ (%d,%d)", t.x, t.y)) end
end)

mod.log("=== probe_live armed (logging roster every ~2s) ===")
