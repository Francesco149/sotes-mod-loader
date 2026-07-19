-- examples/ui_demo/init.lua — P5 UI demo.
--
-- Registers a panel + a floating window through mod.ui.  The loader draws them into BOTH the
-- standalone loader window AND the in-game overlay (a mirror), from the SAME Lua callback — so
-- pressing the overlay hotkey (INSERT by default) in-game shows the identical panels on top of
-- the game.  Exercises the widget set + live mod.game reads.  The draw callbacks run on the UI
-- thread under the loader's Lua lock: they READ game memory (guarded) and enqueue any engine work
-- through mod.main — they never call the engine directly.

mod.log("=== ui_demo: registering UI (loader window + overlay mirror) ===")

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

-- A standalone floating window (a second surface, movable; also mirrored into the overlay).
ui.window("UI Demo — About", function()
  ui.text_wrapped("Provided by examples/ui_demo.  mod.ui panels + floating windows are rendered " ..
    "by the loader into both the standalone window and the in-game overlay from one Lua callback.")
  ui.separator()
  ui.text("loader: " .. mod.loader .. " " .. mod.loader_version)
  if ui.button("toggle overlay") then ui.overlay_toggle() end
  ui.same_line()
  ui.text_disabled(ui.overlay_visible() and "(overlay shown)" or "(overlay hidden)")
end)

mod.log("=== ui_demo armed — open the loader window; press INSERT in-game for the overlay ===")
