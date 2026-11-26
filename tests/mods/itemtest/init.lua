-- tests/mods/itemtest — the items.sh gate's mod (api 1.12): new item rows, appended icons, a character type's drops.
-- Every pixel comes from the user's own icon sheet (retail cells with their colours rotated).
local I, A, SPR = mod.game.items, mod.game.actors, mod.game.sprites
local ID = { blade = 7000101, mail = 7000102, bun = 7000103, drop = 7000104 }
local SLIME = 51995

-- a retail icon cell with r -> g -> b -> r rotated: an RGBA 32 x 32 image
local sheet
local function icon(cell)
  sheet = sheet or assert(SPR.sheet(I.icons().bank))
  local cols = math.floor(sheet.w / 32)
  local bx, by = (cell % cols) * 32, math.floor(cell / cols) * 32
  local img = mod.image.new(32, 32)
  for y = 0, 31 do
    for x = 0, 31 do
      local k = sheet:get(bx + x, by + y)
      if k ~= 0 then local c = sheet.pal[k] img:set(x, y, c[3], c[1], c[2], 255) end
    end
  end
  return img
end

local function def(t) local ok, err = I.define(t) if not ok then mod.log('define ' .. t.id .. ': ' .. tostring(err)) end return ok end
def { id = ID.blade, like = 110010, name = 'Test Blade', desc = 'A loader test: a sword past the retail table.', attack = 21, price = 999, icon = icon(128) }
def { id = ID.mail, like = 120010, name = 'Test Mail', desc = 'A loader test: an armour past the retail table.', defense = 17, price = 888, icon = icon(160) }
def { id = ID.bun, like = 201010, name = 'Test Bun', desc = 'A loader test: heals 150 HP.', use = { a = 150, b = 0 }, price = 77, icon = icon(44) }
def { id = ID.drop, like = 202010, name = 'Test Drop', desc = 'A loader test: Attack +5 for good.', use = { a = 1, b = 5, c = 0 }, price = 66, icon = icon(8) }
assert(A.define { code = SLIME, base = 51010, name = 'Test Slime', hp = 1, coins = 3,
                  drops = { { id = ID.bun, chance = 1000 }, { id = ID.blade, chance = 1000 } } })
_G.itemtest = { ID = ID, SLIME = SLIME }
mod.log('itemtest: 4 items, 1 type')
