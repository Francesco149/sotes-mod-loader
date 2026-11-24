-- tests/roster.lua — mod.game.roster / coordinates / layout on a loaded save (any edition).
-- Run in-scene:  tools/sml test R tests/roster.lua      Returns {pass, fails, info}.
local fails, info = {}, {}
local function check(c, msg) if not c then fails[#fails + 1] = msg end end

local w = mod.game.roster.world()
check(w and w.world ~= 0 and w.room ~= 0 and w.view ~= 0, "world/room/view resolved")
local ms = mod.game.roster.members()
check(#ms >= 1, "at least one party member")
info[#info + 1] = ("camera view %d,%d"):format(mod.mem.read_i32((w and w.view or 0) + 0x60) or 0, mod.mem.read_i32((w and w.view or 0) + 0x5c) or 0)
local leaders = 0
local view = w and w.view or 0
local vl, vt = mod.mem.read_i32(view + 0x60) or 0, mod.mem.read_i32(view + 0x5c) or 0
local vw, vh = mod.mem.read_i32(view + 0x64) or 0, mod.mem.read_i32(view + 0x68) or 0
check(vw == 64000 and vh == 48000, ("camera viewport 64000x48000 (got %dx%d)"):format(vw, vh))
for _, m in ipairs(ms) do
  local tag = (m.id or "?") .. "/" .. m.code
  if m.active then leaders = leaders + 1 end
  check(m.hp_max and m.hp_max > 0 and m.hp >= 0 and m.hp <= m.hp_max, tag .. ": 0 <= hp <= hp_max > 0")
  check(m.mp_max and m.mp >= 0 and m.mp <= m.mp_max, tag .. ": 0 <= mp <= mp_max")
  check(m.combat_level and m.combat_level >= 0 and m.combat_level <= m.combat_level_max and m.combat_level_max <= 10,
        tag .. ": combat level M <= N <= 10")
  check(m.adventurer_level and m.adventurer_level >= 1 and m.adventurer_level <= 99, tag .. ": adventurer level 1..99")
  check(m.level == m.combat_level + m.adventurer_level, tag .. ": level = combat + adventurer")
  -- position vs the camera's map size (camera_view +0x00 / +0x04): inside the room
  local mw, mh = mod.mem.read_i32(view + 0x0) or 0, mod.mem.read_i32(view + 0x4) or 0
  check(m.x >= 0 and m.x <= mw and m.y >= 0 and m.y <= mh,
        ("%s: inside the map (x %d y %d, map %dx%d)"):format(tag, m.x, m.y, mw, mh))
  info[#info + 1] = ("%s Lv%d (%d+%d, max %d) hp %d/%d mp %d/%d exp %d/%d @%d,%d%s"):format(tag, m.level,
    m.combat_level, m.adventurer_level, m.combat_level_max, m.hp, m.hp_max, m.mp, m.mp_max, m.exp, m.exp_max,
    m.x, m.y, m.active and " *" or "")
end
check(leaders == 1, "exactly one controlled member (got " .. leaders .. ")")
local lead = mod.game.coordinates.target()
check(lead ~= nil, "coordinates.target()")
local arche = mod.game.coordinates.player()
check(arche == nil or arche.code == 50010, "coordinates.player() is Arche")
return { pass = #fails == 0, fails = fails, info = info, edition = mod.edition }
