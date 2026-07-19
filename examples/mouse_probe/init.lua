-- examples/mouse_probe/init.lua — validate mod.game.mouse (screen + world mapping).
-- Logs the cursor in game 640x480 screen-space + world centi-px every ~1s, plus the controlled
-- member's world pos as a cross-check (put the cursor on a character; world_* should ~match).
mod.log("=== mouse_probe init ===")
local f = 0
mod.on_frame(function()
  f = f + 1
  if f % 120 ~= 0 then return end
  local mo = mod.game.mouse.get()
  if mo then
    mod.log(string.format("mouse screen=(%.1f,%.1f) over=%s world=(%s,%s)",
      mo.screen_x, mo.screen_y, tostring(mo.over), tostring(mo.world_x), tostring(mo.world_y)))
  end
  local t = mod.game.coordinates.target()
  if t then mod.log(string.format("   [ref] controlled member world=(%d,%d)", t.x, t.y)) end
end)
mod.log("=== mouse_probe armed ===")
