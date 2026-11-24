-- tests/actors.lua — mod.game.actors (api 1.4) on a loaded save, any edition: the pool listing, the
-- char_init code set, spawn (by the leader / absolute) through retail's summon_spawn, remove via char_unload.
local fails, info = {}, {}
local function check(c, msg) if not c then fails[#fails + 1] = msg end end
local A = mod.game.actors

local function by_slot(s) for _, a in ipairs(A.list()) do if a.slot == s then return a end end end
local pool = A.list()
check(pool and #pool >= 1, "actors.list() lists the room's characters")
local lead, nparty = nil, 0
for _, a in ipairs(pool or {}) do
  if a.leader then lead = a end
  if a.party then nparty = nparty + 1 end
end
check(lead and lead.party, "exactly the controlled member is the leader")
check(nparty == #mod.game.roster.members(), "party flags == roster")

-- codes: char_init's own dispatch (122 on enold, 134 on SE); unknown / zero refused before the game sees them
local codes = A.codes()
check(#codes == (mod.edition == "enold" and 122 or 134), "codes() size for " .. mod.edition .. ": " .. #codes)
check(A.valid(50300) and A.valid(51110) and not A.valid(0) and not A.valid(424242), "valid()")
for _, a in ipairs(pool) do check(A.valid(a.code), "live code " .. a.code .. " is in codes()") end
local s, why = A.spawn(424242)
check(s == nil and why and why:find("not a character"), "spawn(unknown) refused")
check(#A.list() == #pool, "refused spawn adds nothing")

-- spawn by the leader: feet = leader's feet + 60 px ahead
local s1, e1 = A.spawn(51110)
check(s1, "spawn(51110): " .. tostring(e1))
local a1 = s1 and by_slot(s1)
check(a1 and a1.code == 51110 and a1.spawned and a1.origin == 4 and not a1.party, "spawned record")
if a1 then
  local dir = lead.facing == 3 and -1 or 1
  local cx, lcx = a1.x + math.floor(a1.w / 2), lead.x + math.floor(lead.w / 2)
  check(math.abs(cx - (lcx + dir * 6000)) <= 100, ("centre x %d vs leader %d + 6000"):format(cx, lcx))
  check(a1.facing == lead.facing, "facing follows the leader")
  info[#info + 1] = ("by leader: slot %d at (%d,%d) %dx%d; leader (%d,%d) %dx%d"):format(s1, a1.x, a1.y, a1.w, a1.h,
    lead.x, lead.y, lead.w, lead.h)
end
check(#A.list() == #pool + 1, "pool +1")

-- absolute: feet at (x, y)
local tx, ty = lead.x + math.floor(lead.w / 2) - 9000, lead.y + lead.h
local s2, e2 = A.spawn(50300, { x = tx, y = ty, facing = 3 })
check(s2, "spawn(50300, {x,y}): " .. tostring(e2))
local a2 = s2 and by_slot(s2)
check(a2 and a2.x + math.floor(a2.w / 2) == tx and a2.facing == 3, "absolute: centre x + facing")
if a2 then info[#info + 1] = ("absolute: slot %d at (%d,%d) %dx%d, feet asked (%d,%d)"):format(s2, a2.x, a2.y, a2.w,
  a2.h, tx, ty) end

-- remove: the spawned ones go, the party stays
check(A.remove(s1) == true and A.remove(s2) == true, "remove(spawned)")
check(by_slot(s1) == nil and by_slot(s2) == nil, "removed slots are dead")
check(#A.list() == #pool, "pool back to its size")
local okr, er = A.remove(lead.slot)
check(okr == nil and er and er:find("party"), "remove(leader) refused")
local okx = A.remove(s1)
check(okx == nil, "remove(empty slot) refused")

return { pass = #fails == 0, fails = fails, info = info, edition = mod.edition }
