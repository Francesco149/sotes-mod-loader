-- tests/world.lua — mod.game.camera / map / flags / items on a loaded save (any edition).
-- Run in-scene:  tools/sml test R tests/world.lua      Returns {pass, fails, info}.
local fails, info = {}, {}
local function check(c, msg) if not c then fails[#fails + 1] = msg end end

local cam = mod.game.camera.get()
check(cam and cam.width == 64000 and cam.height == 48000, "camera viewport 64000x48000")
if cam then
  check(cam.map_w >= cam.width and cam.map_h >= cam.height, "map >= viewport")
  check(cam.left >= 0 and cam.top >= 0 and cam.left + cam.width <= cam.map_w and cam.top + cam.height <= cam.map_h,
        ("view inside the map (%d,%d in %dx%d)"):format(cam.left, cam.top, cam.map_w, cam.map_h))
  info[#info + 1] = ("camera %d,%d map %dx%d"):format(cam.left, cam.top, cam.map_w, cam.map_h)
end

local m = mod.game.map.get()
check(m and m.area and m.area > 0, "map.area > 0")
check(m and m.map ~= nil, "area found in the area table (map id)")
check(m and m.nareas and m.nareas > 0 and m.nareas < 1024, "area table count sane")
if m then info[#info + 1] = ("area %d map %s (%d areas)"):format(m.area, tostring(m.map), m.nareas or -1) end

-- story flags: the fill table; a loaded mid-game save has some
local sl = mod.game.flags.story.list()
check(sl and #sl > 0, "story flags present")
local seen = {}
for _, f in ipairs(sl or {}) do
  check(not seen[f[1]], "story flag ids unique (" .. f[1] .. ")"); seen[f[1]] = true
  local v, v2 = mod.game.flags.story.get(f[1])
  check(v == f[2] and v2 == f[3], "story.get == list for " .. f[1])
end
info[#info + 1] = ("%d story flags"):format(sl and #sl or -1)
-- round trip on an id no script uses, then clear it (engine semantics: 0,0 frees the slot)
local T = 987654321
check(mod.game.flags.story.get(T) == nil, "test flag absent before")
check(mod.game.flags.story.set(T, 7, 3) == true, "story.set")
local a, b = mod.game.flags.story.get(T)
check(a == 7 and b == 3, "story.get after set")
mod.game.flags.story.set(T, 0, 0)
check(mod.game.flags.story.get(T) == nil, "story.set 0,0 clears")
check(#mod.game.flags.story.list() == #sl, "story list size restored")

local al = mod.game.flags.area.list()
check(al ~= nil, "area flags readable")
info[#info + 1] = ("%d area flags"):format(al and #al or -1)

local items = mod.game.items.list()
check(items and #items > 0, "inventory non-empty")
local named = 0
for _, it in ipairs(items or {}) do
  check(it.id > 0 and it.count >= 0, "item " .. it.index .. " sane")
  if it.name and #it.name > 0 then named = named + 1 end
end
check(items and named == #items, ("every item named (%d/%d)"):format(named, items and #items or 0))
for i = 1, math.min(5, items and #items or 0) do
  local it = items[i]
  info[#info + 1] = ("item %d: %s x%d owner %d%s"):format(it.id, it.name or "?", it.count, it.owner, it.stored and " (stored)" or "")
end
local cp, cap = mod.game.items.gold()
check(cp and cap and cp >= 0 and cp <= cap, "0 <= gold <= cap")
info[#info + 1] = ("gold %d CP (cap %d)"):format(cp or -1, cap or -1)
check(mod.game.items.set_gold(cp + 1) == math.min(cp + 1, cap), "set_gold")
mod.game.items.set_gold(cp)
check(mod.game.items.gold() == cp, "gold restored")
return { pass = #fails == 0, fails = fails, info = info, edition = mod.edition }
