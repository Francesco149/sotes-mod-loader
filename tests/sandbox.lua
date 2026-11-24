-- tests/sandbox.lua — the api-1.2 sandbox ops on a loaded save (any edition), the synchronous half:
-- map.rooms / room (+ loadable), items.catalog / give, roster.set, coordinates.set, map.warp's refusals.
-- The warp itself spans frames: tools/sml test R tests/sandbox.lua, then tests/warp.sh R (sml lua + wait).
local fails, info = {}, {}
local function check(c, msg) if not c then fails[#fails + 1] = msg end end

-- rooms: the area table; the current room is in it and loadable; exits point at real rooms
local rooms = mod.game.map.rooms()
check(rooms and #rooms > 100, "map.rooms() lists the area table")
local byk, nload = {}, 0
for _, r in ipairs(rooms or {}) do byk[r.key] = r; if r.loadable then nload = nload + 1 end end
local here = mod.game.map.get().area
check(byk[here] and byk[here].loadable, "current room listed + loadable")
local bad = 0
for _, r in ipairs(rooms or {}) do
  for _, x in ipairs(r.exits) do if x.to ~= 0 and not byk[x.to] then bad = bad + 1 end end
end
info[#info + 1] = ("%d rooms, %d loadable, %d dangling exits; here %d (%d exits)"):format(
  #(rooms or {}), nload, bad, here, byk[here] and #byk[here].exits or -1)
check(nload < #rooms, "some rooms have no map data (999999 at least)")
local r1 = mod.game.map.room(here)
check(r1 and r1.key == here and #r1.exits == #byk[here].exits, "map.room(key) == rooms() entry")
check(mod.game.map.room(123) == nil, "map.room(unknown) = nil")

-- warp refusals (no state change): unknown room, map-less room
local ok, why = mod.game.map.warp(123)
check(ok == nil and why and why:find("no room"), "warp(unknown) refused")
for _, r in ipairs(rooms or {}) do
  if not r.loadable then
    ok, why = mod.game.map.warp(r.key, 0, true)
    check(ok == nil and why and why:find("no map data"), "warp(map-less " .. r.key .. ") refused")
    break
  end
end
check(mod.game.map.warp_status() == "idle", "no warp pending after refusals")

check(type(mod.game.map.event()) == "boolean", "map.event() is a boolean in a scene")

-- items: the catalog names the inventory; give adds one record through the engine
local cat = mod.game.items.catalog()
check(cat and #cat > 100, "items.catalog() lists definitions")
local cname = {}
for _, c in ipairs(cat or {}) do cname[c.id] = c.name end
local inv = mod.game.items.list()
for _, it in ipairs(inv) do check(cname[it.id] == it.name, "catalog name == inventory name for " .. it.id) end
local pick = cat[1]
local idx, err = mod.game.items.give(pick.id, 0)
local inv2 = mod.game.items.list()
check(idx ~= nil, "items.give: " .. tostring(err))
check(#inv2 == #inv + 1, "inventory +1 after give")
local last = inv2[#inv2]
check(last and last.id == pick.id and last.index == idx and last.owner == 0, "the new record = the given id, unworn")
info[#info + 1] = ("catalog %d; gave %d %s -> index %s (count %s)"):format(#cat, pick.id, pick.name, tostring(idx),
  last and tostring(last.count) or "?")
local okb, errb = mod.game.items.give(424242, 1)
check(okb == nil and errb, "items.give(unknown id) refused by the game")
check(#mod.game.items.list() == #inv2, "refused give adds nothing")

-- roster.set: current hp / mp of the controlled member (round trip)
local m
for _, x in ipairs(mod.game.roster.members()) do if x.active then m = x end end
check(m ~= nil, "a controlled member")
if m then
  check(mod.game.roster.set(nil, { hp = m.hp - 1, mp = m.mp }) == true, "roster.set")
  local m2
  for _, x in ipairs(mod.game.roster.members()) do if x.code == m.code then m2 = x end end
  check(m2.hp == m.hp - 1 and m2.mp == m.mp, "hp set")
  mod.game.roster.set(m.code, { hp = m.hp })
  for _, x in ipairs(mod.game.roster.members()) do if x.code == m.code then m2 = x end end
  check(m2.hp == m.hp, "hp restored by code")
end

-- coordinates.set: move the controlled member 1 px right (the body box), then back
local t = mod.game.coordinates.target()
check(mod.game.coordinates.set(nil, t.x + 100, t.y) == true, "coordinates.set")
local t2 = mod.game.coordinates.target()
check(t2.x == t.x + 100 and t2.y == t.y, "body x moved")
mod.game.coordinates.set(t.code, t.x, t.y)
check(mod.game.coordinates.target().x == t.x, "body x restored")

return { pass = #fails == 0, fails = fails, info = info, edition = mod.edition }
