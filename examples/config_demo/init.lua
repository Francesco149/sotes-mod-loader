-- examples/config_demo/init.lua — mod.config demo.
--
-- Settings are declared in mod.toml [config]; the loader exposes them as mod.config:
--   mod.config.get(key)        -> the stored value (coerced to the declared type) or the default
--   mod.config.set(key, value) -> clamps to min/max, writes oss_mods.cfg (namespaced), persists
--   mod.config.schema()        -> (schema, order) for a generic editor (this panel; later the launcher)
-- No per-setting Lua plumbing: the panel below renders straight from the schema.

local cfg = mod.config

mod.log(string.format("config_demo: enabled=%s speed=%s ratio=%s",
  tostring(cfg.get("enabled")), tostring(cfg.get("speed")), tostring(cfg.get("ratio"))))

-- A GENERIC config table, built from the schema — the exact shape the launcher will mirror.
mod.ui.panel("Config Demo", function()
  local ui = mod.ui
  local schema, order = cfg.schema()
  ui.text_disabled("edited live; persists to oss_mods.cfg")
  ui.separator()
  for _, key in ipairs(order) do
    local spec = schema[key]
    local label = spec.label or key
    if spec.type == "bool" then
      local v, changed = ui.checkbox(label, cfg.get(key))
      if changed then cfg.set(key, v) end
    elseif spec.type == "int" then
      local v, changed = ui.slider_int(label, cfg.get(key), spec.min or 0, spec.max or 100)
      if changed then cfg.set(key, v) end
    elseif spec.type == "float" then
      local v, changed = ui.slider_float(label, cfg.get(key), spec.min or 0.0, spec.max or 1.0)
      if changed then cfg.set(key, v) end
    else
      ui.text(string.format("%s = %s", label, tostring(cfg.get(key))))   -- string/enum: no widget yet
    end
    if spec.description then ui.text_disabled("   " .. spec.description) end
  end
end)

mod.log("config_demo: armed — edit the settings in the Config Demo panel (F10)")
