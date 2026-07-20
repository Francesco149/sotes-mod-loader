-- examples/roster_probe/init.lua — log the live party roster every ~2s (validation harness).
-- Reads mod.game.roster (now backed by the direct party-array chain).  Time-throttled because the
-- SotES safepoint fires ~1000x/sec in gameplay, so a frame counter would spam.

mod.log("=== roster_probe: logging the party every ~2s ===")
local last = 0
mod.on_frame(function()
  local now = os.time()
  if now - last < 2 then return end
  last = now
  local ok, r = pcall(function() return mod.game.roster.members() end)
  if not ok or not r then mod.log("roster: <error>"); return end
  mod.log(string.format("roster: %d member(s)", #r))
  for _, m in ipairs(r) do
    -- char_level = combat_level + adventurer_level (the status screen's big "Level"); attack/defense/
    -- spirit/resist are the raw base stats (see docs/findings/save15-live-stats.md in ../OpenSummoners).
    mod.log(string.format("  %-7s Lv%d  hp %d/%d  mp %d/%d  atk %d def %d spi %d res %d  exp %d/%d  (%d,%d)%s",
      m.name, m.char_level, m.hp, m.hp_max, m.mp, m.mp_max,
      m.attack, m.defense, m.spirit, m.resist, m.exp, m.exp_max, m.x, m.y,
      m.active and "  <-active" or ""))
  end
end)
