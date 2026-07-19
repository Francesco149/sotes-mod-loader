-- examples/ui_demo/init.lua — P5 UI demo.
--
-- Registers a panel + a floating window through mod.ui.  The loader draws them in its companion
-- window.  Exercises the widget set + live mod.game reads.  The callbacks run on the ENGINE thread
-- (the loader records their output into a lock-free snapshot the UI thread replays) — so they may
-- read game state directly; push any heavier/engine-mutating work through mod.main.

mod.log("=== ui_demo: registering UI ===")

local ui = mod.ui
local frames, show_details, speed = 0, true, 1.0

mod.on_frame(function() frames = frames + 1 end)   -- proves the panel sees live per-frame state

-- A panel — a collapsing section inside the loader's "SotES Mod Loader" host window.
ui.panel("UI Demo", function()
  ui.text(string.format("frames observed: %d", frames))
  ui.text_disabled("drawn on the UI thread — reads are guarded, writes go through mod.main")
  ui.separator()

  if ui.button("log a line") then mod.log("ui_demo: button pressed at frame " .. frames) end
  ui.same_line()
  show_details = ui.checkbox("roster details", show_details)   -- 1st return = new state
  speed = ui.slider_float("example speed", speed, 0.0, 4.0)     -- 1st return = new value

  ui.separator()
  local ok, roster = pcall(function() return mod.game.roster.members() end)
  if ok and roster and #roster > 0 then
    ui.text(string.format("party: %d member(s)", #roster))
    if show_details then
      for _, m in ipairs(roster) do
        ui.bullet(string.format("%-7s Lv%d  hp %d/%d  (%d,%d)%s",
          m.name, m.level, m.hp, m.hp_max, m.x, m.y, m.active and "  <- active" or ""))
      end
    end
  else
    ui.text_disabled("no party loaded (load a save to see the live roster)")
  end
end)

-- A standalone floating window (a second surface, movable).
ui.window("UI Demo — About", function()
  ui.text_wrapped("Provided by examples/ui_demo.  mod.ui panels + floating windows are recorded on " ..
    "the engine thread into a lock-free snapshot and replayed by the loader's companion window.")
  ui.separator()
  ui.text("loader: " .. mod.loader .. " " .. mod.loader_version)
end)

mod.log("=== ui_demo armed — open the loader window (F8 toggles it) ===")
