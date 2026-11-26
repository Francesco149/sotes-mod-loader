-- core/lua/items.lua — new item definitions, their icons, items on the floor, monster drops (api 1.12, embedded).
-- Receives (mem, sym, log, game, res, edition_fn, itemsites_fn, hook, on_frame); fills game.items in place.
--
-- ITEMS PAST THE RETAIL TABLE.  Every item definition is a row of ONE static array in the exe (item_table: enold
--   215 rows of 736 B, SE 306 rows of 940 B), ended by a row whose id is 0.  Every lookup — item_def, the shop, the
--   lists, the save loader, the drops, the pickups — scans it inline from the start to that 0 row (SE then falls back
--   to its generated-gear table, sys + 0x308, ids 910000 + slot); nothing holds a count or an end pointer.  So the
--   loader RELOCATES the table, as it does the sprite banks: a CAP-row copy, the retail rows in front, the mods' rows
--   after them, zeroes after that (the end row); and every reference gets the copy's address.  tools/itemsites.py
--   proved the references uniform: 77 / 106 / 106 dwords, each the table base (`[T]`, `[reg*4 + T]`, imm32 T), and
--   the address occurs nowhere else in the file.  Each is verified before anything is written; a mismatch = nothing
--   written.  Inventory records keep a row POINTER (+0xc): rows never move once added, and the retail rows of the
--   old table stay as they were (a record made before the relocation stays right).
--   A row is a copy of a retail (or defined) row, `like`, with fields replaced — so a new sword, armour, food or
--   stat-up item behaves as the game's own of that kind (equip slot, class fit, list kind, use effect, caps).
--
--   items.define{id=, like=[, name=, desc=, price=, icon=, attack=, defense=, spirit=, resist=, max_hp=, max_mp=,
--                use = {effect=, a=, b=, c=}, set = {[offset] = int32}]} -> id | nil, why
--        id: a number the game never uses: not a retail id, not SE's generated gear (910000..910511), not 0;
--            it is what the save stores — keep it stable across versions of the mod.
--        icon: a cell of the game's icon sheet (number), or an image (mod.image RGBA or an indexed sprites image,
--            32 x 32) — appended to the sheet (items.icon).
--        use: the consumable's effect (+0x58) and its numbers (+0x68 / +0x6c / +0x70): SE effect 8 = HP (a + b *
--            rand), 17 = MP, 18 = a permanent stat (a: 1 attack, 2 defense, 3 spirit, 4 resist, 5 max HP, 6 max
--            MP; b + c * rand points).  Best left to `like`.
--   items.def(id) -> {id, ptr, name, desc, price, icon, kind, slot, fit, attack, defense, spirit, resist, max_hp,
--                     max_mp, effect, a, b, c, ours} | nil
--   items.defined(id) -> true | nil;  items.capacity() -> {retail, defined, total, extended, error}
--   items.icon(img) -> cell                 a new cell of the icon sheet (see ICONS)
--   items.drop(id, where) -> true | nil, why   an item lying on the floor (pickup_spawn, as a defeated foe drops
--                                         one): where = {x=, y=} (world centi-px) or an actors entry / char pointer
--   actors.define{..., drops = {{id=, chance=}, ...}, coins = n}   a character type's drops (chance per mille):
--        the first one replaces the base character's own item in retail's drop roll (foe_drops); the rest are
--        rolled after it, at the same spot.  coins replaces its money.
--   items.catalog() also lists the defined items (ours = true).
--
-- ICONS.  The item icons are one 8-bit sheet (sprite bank 50 enold / 56 SE EN / 60 SE JP: 256 x 1024, 8 cells a row;
--   a row's u16 +0x54 = the cell), decoded lazily on first draw.  New icons go in rows APPENDED to the sheet (the
--   retail cells keep their numbers and pixels) and take the palette entries no retail icon uses (128 of 256 on SE):
--   a median cut over every added icon picks those colours; each pixel then takes the nearest of the whole palette.
--   The sheet is rebuilt once per frame at most (queued), under a new DATA id of the resource overlay; if the bank
--   was already decoded, retail's own sprite_set_free drops its frames and the next draw decodes the new sheet.
local mem, sym, log, game, res, edition_fn, itemsites_fn, hook, on_frame = ...
local ffi = require('ffi')
local bit = require('bit')

local CAP = 4096                                        -- rows in the relocated table
local GEN0, GEN1 = 910000, 910511                       -- SE's generated gear: 910000 + its table slot
local items = {}
local T = { on = false }                                -- the relocated table
local defs, order = {}, {}                              -- by id: {ptr, row}; ids in definition order

local function addr(cd) return tonumber(ffi.cast('uintptr_t', cd)) end
local function le32(v) v = v % 4294967296 return string.char(v % 256, math.floor(v / 256) % 256, math.floor(v / 65536) % 256, math.floor(v / 16777216)) end
local function sites()
  if not T.sites then T.sites = itemsites_fn and itemsites_fn() or false end
  return T.sites or nil
end
local function table_va() return T.on and addr(T.buf) or (sites() and sites().table) end
local function row_size() local s = sites() return s and s.row_size end

-- ── the table ────────────────────────────────────────────────────────────────────────────────────────────────────
local function extend()
  if T.on then return true end
  if T.err then return nil, T.err end
  local s = sites()
  if not s then T.err = 'unknown edition' return nil, T.err end
  local rs, t = s.row_size, s.table
  local n = 0
  while mem.read_u32(t + rs * n) ~= 0 do n = n + 1 if n > CAP then break end end
  local function fail(what) T.err = what log('[items] table not extended: ' .. what) return nil, what end
  if n ~= s.rows then return fail(('the table has %d rows, not %d'):format(n, s.rows)) end
  for _, va in ipairs(s.sites) do
    if mem.read_u32(va) ~= t then return fail(('site %08x holds %08x, not the table'):format(va, mem.read_u32(va) or 0)) end
  end
  T.buf = ffi.new('uint8_t[?]', CAP * rs)             -- zeroed: every row past the last defined one is the end row
  ffi.copy(T.buf, ffi.cast('const uint8_t *', t), n * rs)
  local new, done = addr(T.buf), {}
  for _, va in ipairs(s.sites) do
    if not mem.patch(va, le32(new)) then
      for i = #done, 1, -1 do mem.patch(done[i], le32(t)) end
      return fail(('the patch at %08x was refused'):format(va))
    end
    done[#done + 1] = va
  end
  T.on, T.rows, T.retail = true, n, n
  log(('[items] item table relocated: %d retail rows + room for %d (%d sites)'):format(n, CAP - 1 - n, #s.sites))
  return true
end

local function find(id)                                 -- the row the game would find (static table first)
  local t, rs = table_va(), row_size()
  if not t or not id or id == 0 then return nil end
  local i = 0
  while true do
    local v = mem.read_u32(t + rs * i)
    if v == 0 then break end
    if v == id then return t + rs * i end
    i = i + 1
  end
  local ext = sym.va('item_table_ext')
  local e = ext and mem.read_u32(ext) or 0
  if e ~= 0 then
    for k = 0, 511 do if mem.read_u32(e + rs * k) == id then return e + rs * k end end
  end
end

local function put_str(p, s, cap)
  s = tostring(s)
  if #s > cap - 1 then s = s:sub(1, cap - 1) end
  ffi.fill(ffi.cast('uint8_t *', p), cap, 0)
  ffi.copy(ffi.cast('uint8_t *', p), s, #s)
end
local STAT = { attack = 0x80, defense = 0x84, spirit = 0x88, resist = 0x8c, max_hp = 0x90, max_mp = 0x94 }

function items.define(d)
  assert(type(d) == 'table' and type(d.id) == 'number' and type(d.like) == 'number', 'items.define{id=, like=, ...}')
  local id = d.id
  if id <= 0 or id % 1 ~= 0 or id > 2147483647 then return nil, 'id: a positive whole number' end
  if id >= GEN0 and id <= GEN1 then return nil, ('id %d: SE\'s generated gear uses 910000..910511'):format(id) end
  if defs[id] then return nil, ('id %d is already defined'):format(id) end
  local ok, err = extend()
  if not ok then return nil, err end
  if find(id) then return nil, ('id %d is one of the game\'s own'):format(id) end
  local src = find(d.like)
  if not src then return nil, ('like: no item %d'):format(d.like) end
  if T.rows >= CAP - 1 then return nil, 'the item table is full' end
  local rs = row_size()
  local p = addr(T.buf) + rs * T.rows
  ffi.copy(ffi.cast('uint8_t *', p), ffi.cast('const uint8_t *', src), rs)
  mem.write_u32(p, id)
  if d.name then put_str(p + 4, d.name, 0x50) end
  if d.desc then put_str(p + 0xac, d.desc, 0x200) end
  if d.price then mem.write_u32(p + 0x60, d.price) end
  if d.icon ~= nil then
    local cell = d.icon
    if type(cell) ~= 'number' then
      local c, e = items.icon(cell)
      if not c then return nil, 'icon: ' .. tostring(e) end
      cell = c
    end
    mem.write_u16(p + 0x54, cell)
  end
  for k, off in pairs(STAT) do if d[k] then mem.write_i32(p + off, d[k]) end end
  if d.use then
    local u = d.use
    if u.effect then mem.write_i32(p + 0x58, u.effect) end
    if u.a then mem.write_i32(p + 0x68, u.a) end
    if u.b then mem.write_i32(p + 0x6c, u.b) end
    if u.c then mem.write_i32(p + 0x70, u.c) end
  end
  for off, v in pairs(d.set or {}) do
    assert(type(off) == 'number' and off >= 4 and off + 4 <= rs, 'set: offsets inside the row, past the id')
    mem.write_i32(p + off, v)
  end
  T.rows = T.rows + 1
  defs[id] = { ptr = p, row = T.rows - 1 }
  order[#order + 1] = id
  return id
end

function items.defined(id) return defs[id] ~= nil or nil end

local function cstr(p, cap)
  local s = ffi.string(ffi.cast('const char *', p), cap)
  return (s:match('^[^%z]*'))
end
function items.def(id)
  local p = find(id)
  if not p then return nil end
  local function i32(o) return mem.read_i32(p + o) end
  local t = { id = id, ptr = p, name = cstr(p + 4, 0x50), desc = cstr(p + 0xac, 0x200), price = i32(0x60),
              icon = mem.read_u16(p + 0x54), kind = mem.read_u16(p + 0x66), slot = i32(0x78), fit = i32(0x7c),
              effect = i32(0x58), a = i32(0x68), b = i32(0x6c), c = i32(0x70), ours = defs[id] ~= nil }
  for k, off in pairs(STAT) do t[k] = i32(off) end
  return t
end

function items.capacity()
  local ok, err = extend()
  local s = sites()
  return { retail = s and s.rows or 0, defined = #order, total = CAP - 1, extended = ok and true or false, error = err }
end

-- ── icons ────────────────────────────────────────────────────────────────────────────────────────────────────────
local IC = { queue = {}, cells = 0 }                    -- added icons (RGBA cells) in cell order; built lazily
local function icon_bank() local s = sites() return s and s.icon_bank end
local function slot_of(b)
  local spr = game.sprites
  local info = spr and spr.bank(b)
  if not info then return nil end
  local l = ({ enold = 0xaac, ['ense-en'] = 0xb08, ['ense-jp'] = 0xb04 })[edition_fn()]
  return mem.read_u32(sym.va('g_sys') + l + 4 * b), info
end
local function retail_sheet()
  if IC.base then return IC.base end
  local spr = game.sprites
  local b = icon_bank()
  if not (spr and b) then return nil, 'no sprite layer' end
  local s, info = slot_of(b)
  if not s then return nil, 'no icon bank' end
  IC.res0, IC.module = info.res, info.module ~= 0 and info.module or nil
  local blob = res.read('DATA', info.res, IC.module)
  if not blob then return nil, 'icon sheet DATA ' .. info.res .. ' not found' end
  local img, e = spr.decode(blob)
  if not img then return nil, e end
  local used = {}
  for i = 0, img.w * img.h - 1 do used[img.px[i]] = true end
  IC.free, IC.used = {}, {}
  for i = 1, 255 do if used[i] then IC.used[#IC.used + 1] = i else IC.free[#IC.free + 1] = i end end
  IC.base, IC.template, IC.cols = img, blob, math.floor(img.w / 32)
  IC.banks = {}
  local spr_n = ({ enold = 909, ['ense-en'] = 925, ['ense-jp'] = 929 })[edition_fn()] or 0
  for i = 0, spr_n - 1 do
    local bi = spr.bank(i)
    if bi and bi.res == info.res and bi.module == info.module then IC.banks[#IC.banks + 1] = i end
  end
  IC.first = IC.cols * math.floor(img.h / 32)           -- the first appended cell
  return img
end

local function to_rgba(img)                             -- an indexed sprites image → RGBA bytes (index 0 transparent)
  if not img.pal then return img end
  local o = { w = img.w, h = img.h, px = ffi.new('uint8_t[?]', img.w * img.h * 4) }
  for i = 0, img.w * img.h - 1 do
    local k = img.px[i]
    if k ~= 0 then local c = img.pal[k] o.px[4 * i], o.px[4 * i + 1], o.px[4 * i + 2], o.px[4 * i + 3] = c[1], c[2], c[3], 255 end
  end
  return o
end

-- median cut over the opaque pixels of every added icon → at most n colours
local function median_cut(px_list, n)
  local boxes = { px_list }
  local function range(b)
    local lo, hi = { 255, 255, 255 }, { 0, 0, 0 }
    for _, c in ipairs(b) do for k = 1, 3 do if c[k] < lo[k] then lo[k] = c[k] end if c[k] > hi[k] then hi[k] = c[k] end end end
    local best, w = 1, -1
    for k = 1, 3 do if hi[k] - lo[k] > w then best, w = k, hi[k] - lo[k] end end
    return best, w
  end
  while #boxes < n do
    local bi, bk, bw = nil, nil, 0
    for i, b in ipairs(boxes) do
      if #b > 1 then local k, w = range(b) if w > bw then bi, bk, bw = i, k, w end end
    end
    if not bi then break end
    local b = boxes[bi]
    table.sort(b, function(x, y) return x[bk] < y[bk] end)
    local m = math.floor(#b / 2)
    local l, r = {}, {}
    for i = 1, #b do if i <= m then l[#l + 1] = b[i] else r[#r + 1] = b[i] end end
    boxes[bi] = l
    boxes[#boxes + 1] = r
  end
  local out = {}
  for _, b in ipairs(boxes) do
    if #b > 0 then
      local s = { 0, 0, 0 }
      for _, c in ipairs(b) do for k = 1, 3 do s[k] = s[k] + c[k] end end
      out[#out + 1] = { math.floor(s[1] / #b + 0.5), math.floor(s[2] / #b + 0.5), math.floor(s[3] / #b + 0.5) }
    end
  end
  return out
end

local function rebuild()
  IC.dirty = false
  local base, e = retail_sheet()
  if not base then log('[items] icons: ' .. tostring(e)) return end
  local spr = game.sprites
  local n = #IC.queue
  local rows = math.ceil(n / IC.cols)
  local img = spr.image(base.w, base.h + 32 * rows, base.pal)
  ffi.copy(img.px, base.px, base.w * base.h)
  -- palette: the retail entries stay; the free ones get the added icons' colours
  local seen, list = {}, {}
  for _, ic in ipairs(IC.queue) do
    for i = 0, 32 * 32 - 1 do
      if ic.px[4 * i + 3] >= 128 then
        local r, g, b = ic.px[4 * i], ic.px[4 * i + 1], ic.px[4 * i + 2]
        local key = r * 65536 + g * 256 + b
        if not seen[key] then seen[key] = true list[#list + 1] = { r, g, b } end
      end
    end
  end
  local cols = median_cut(list, #IC.free)
  for i, c in ipairs(cols) do img.pal[IC.free[i]] = c end
  local cand = {}
  for _, k in ipairs(IC.used) do cand[#cand + 1] = k end
  for i = 1, #cols do cand[#cand + 1] = IC.free[i] end
  local memo = {}
  local function nearest(r, g, b)
    local key = r * 65536 + g * 256 + b
    local m = memo[key]
    if m then return m end
    local best, bd = 1, 1e9
    for _, k in ipairs(cand) do
      local c = img.pal[k]
      local dr, dg, db = c[1] - r, c[2] - g, c[3] - b
      local d = 3 * dr * dr + 4 * dg * dg + 2 * db * db
      if d < bd then best, bd = k, d end
    end
    memo[key] = best
    return best
  end
  for j, ic in ipairs(IC.queue) do
    local cell = IC.first + j - 1
    local bx, by = (cell % IC.cols) * 32, math.floor(cell / IC.cols) * 32
    for y = 0, 31 do
      for x = 0, 31 do
        local i = y * 32 + x
        local v = 0
        if ic.px[4 * i + 3] >= 128 then v = nearest(ic.px[4 * i], ic.px[4 * i + 1], ic.px[4 * i + 2]) end
        img.px[(by + y) * img.w + bx + x] = v
      end
    end
  end
  local blob = spr.encode(img, IC.template)
  local id = IC.res or 0xE800
  if not IC.res then
    while res.read('DATA', id) or (IC.module and res.read('DATA', id, IC.module)) do id = id + 1 end
    IC.res = id
  end
  if not res.set('DATA', id, blob) then log('[items] icons: the resource overlay refused the sheet') return end
  -- every bank cut from the icon sheet (same DATA id, same module) takes the new sheet: the menus' icon bank AND the
  -- world's (SE 446: the pickup sparkle, particle 100143, shows the picked item's icon from it — a bank left on the
  -- retail sheet would index past its frames)
  local free = sym.va('sprite_set_free')
  for _, b in ipairs(IC.banks) do
    local s = slot_of(b)
    if mem.read_u16(s + 0x40) ~= id then mem.write_u16(s + 0x40, id) end
    local e0 = mem.read_u32(s)
    if e0 ~= 0 and mem.read_u32(e0) ~= 0 and free then     -- decoded already: retail's own free; the next draw decodes ours
      ffi.cast('void (__thiscall *)(void *, uint16_t)', free)(ffi.cast('void *', sym.va('g_sys')), b)
    end
    -- the per-cell trim records (+0x34, 24 B a cell): the slicer (SE 0x412a10) makes them on the FIRST decode, sized for
    -- that sheet's cells, and reuses them on every later one — a taller sheet would read past them (a bogus cell rect →
    -- "Failed Surface Load").  Cleared, the slicer makes a set for this sheet.  (Retail's old buffer stays allocated:
    -- 6 KB, once per rebuild.)
    if mem.read_u32(s + 0x34) ~= 0 then mem.write_u32(s + 0x34, 0) end
  end
  IC.built = n
  log(('[items] icon sheet: %d added icons (cells %d..%d, %d new colours), DATA %d, banks %s'):format(n, IC.first, IC.first + n - 1, #cols, id, table.concat(IC.banks, ' ')))
end

function items.icon(img)
  assert(type(img) == 'table' and img.w and img.px, 'items.icon(img): a mod.image or a sprites image')
  local base, e = retail_sheet()
  if not base then return nil, e end
  local rgba = to_rgba(img)
  if rgba.w ~= 32 or rgba.h ~= 32 then
    if rgba.crop then
      local c = rgba:crop(0, 0, 32, 32)
      rgba = c
    else
      return nil, 'an icon is 32 x 32'
    end
  end
  local own = { px = ffi.new('uint8_t[4096]') }
  ffi.copy(own.px, rgba.px, 4096)
  IC.queue[#IC.queue + 1] = own
  IC.dirty = true
  if not on_frame then rebuild()
  elseif not IC.ticker then                             -- one permanent per-frame check (on_frame has no removal)
    IC.ticker = true
    on_frame(function() if IC.dirty then rebuild() end end)
  end
  return IC.first + #IC.queue - 1
end
function items.icons() return { first = IC.first, added = #IC.queue, built = IC.built or 0, bank = icon_bank(), banks = IC.banks, res = IC.res } end

-- ── equip / use (retail's own steps on an inventory record) ──────────────────────────────────────────────────────
-- the member id (100000101 Arche, ...) in the party slot: SE +0xa10 (found by value); enold +0x9f0 (SE = enold + 0x20 here)
local LAY = { enold = { room = 0x2784, id = 0x9f0 }, ['ense-en'] = { room = 0x3788, id = 0xa10 }, ['ense-jp'] = { room = 0x3788, id = 0xa10 } }
local function room_ptr()
  local l = LAY[edition_fn()]
  local w = l and mem.read_u32(sym.va('g_world'))
  return w and w ~= 0 and mem.read_u32(w + l.room) or nil
end
local function member(m)                                -- a roster member (or its slot pointer) → slot, member id
  local sl = type(m) == 'table' and m.slot or m
  if type(sl) ~= 'number' then
    for _, r in ipairs(game.roster and game.roster.members() or {}) do if r.active then sl = r.slot break end end
  end
  if not sl then return nil end
  return sl, mem.read_u32(sl + LAY[edition_fn()].id)
end
-- items.equip(index, member) -> true | nil, why   room_item_equip: the record goes on the member (its equip slot taken
--   off the member's other items), stats recomputed.  member = a roster.members() entry (nil = the first active one);
--   the class fit is the caller's business (retail's menus check it first).
function items.equip(index, m)
  local f, room = sym.va('room_item_equip'), room_ptr()
  if not (f and room) then return nil, 'room_item_equip unavailable' end
  local sl, mid = member(m)
  if not mid or mid == 0 then return nil, 'no such member' end
  ffi.cast('int (__thiscall *)(void *, uint16_t, int, int)', f)(ffi.cast('void *', room), index, mid, 1)
  return true
end
-- items.use(index, member) -> used, message   SE only: the field use of a record on a member (heal / stat / ...) —
--   retail's message ("You used ...") back as text; the record's count goes down as in the menu.
function items.use(index, m)
  local f, room = sym.va('item_use'), room_ptr()
  if not (f and room) then return nil, 'item_use unavailable on this edition' end
  local sl = member(m)
  if not sl then return nil, 'no such member' end
  local buf, lines = ffi.new('char[2048]'), ffi.new('uint16_t[2]')
  local r = ffi.cast('int (__thiscall *)(void *, void *, uint16_t, char *, uint16_t *, int, int)', f)(
    ffi.cast('void *', room), ffi.cast('void *', sl), index, buf, lines, 0, 0)
  return r ~= 0, ffi.string(buf)
end

-- items.take(id[, n]) -> taken | nil, why   [api 1.13] n units (default 1) of `id` off the bag, the story scripts'
--   way: retail's item_take (enold 0x406500 / SE 0x4039e0 / JP 0x403a30) removes ONE record — the unstored one with
--   the smallest count — moves the rest down and recomputes the party's stats.  A record holding more units than are
--   still to take only has its count lowered (what using one does).  Stored records (the box) are never touched.
--   Returns the units taken (fewer than n when the bag runs out).
local ROOM_ITEMS, ROOM_NITEMS = 0x2010, 0x4010            -- every edition (fieldxw)
function items.take(id, n)
  n = math.floor(tonumber(n) or 1)
  local f, room = sym.va('item_take'), room_ptr()
  if not (f and room) then return nil, 'item_take unavailable' end
  if type(id) ~= 'number' or id <= 3 then return nil, 'take(id): an item id (coins: items.set_gold)' end
  local o = ffi.new('uint32_t[4]')                        -- the runner retail passes: only +8 (the room) is read
  o[2] = room
  local take = ffi.cast('int (__thiscall *)(void *, int, int)', f)
  local taken = 0
  while taken < n do
    local best, bc
    for i = 0, (mem.read_u16(room + ROOM_NITEMS) or 0) - 1 do
      local r = room + ROOM_ITEMS + 16 * i
      if mem.read_i32(r) == id and mem.read_u8(r + 0xa) == 0 then
        local c = mem.read_u16(r + 8)
        if not best or c < bc then best, bc = r, c end
      end
    end
    if not best then break end
    local units, want = math.max(1, bc), n - taken
    if units > want then
      if not mem.write_u16(best + 8, bc - want) then return nil, 'refused (not the engine thread?)' end
      taken = n
    else
      take(o, id, 1)
      taken = taken + units
    end
  end
  return taken
end

-- ── on the floor ─────────────────────────────────────────────────────────────────────────────────────────────────
local function body_of(where)                         -- an actors entry (.ptr), a roster member (.actor), a char pointer, or {x=, y=}
  local p = where
  if type(where) == 'table' then
    p = where.ptr or where.actor
    if not p then return nil, where.x, where.y end
  end
  if type(p) ~= 'number' then return nil end
  local b = mem.read_u32(p + 0x40)
  if not b or b == 0 then return nil end
  return b
end
function items.drop(id, where)
  if not find(id) then return nil, ('no item %d'):format(id) end
  local s = sites()
  local f = sym.va('pickup_spawn')
  if not (s and f) then return nil, 'pickup_spawn unavailable' end
  if game.map and game.map.event and game.map.event() then return nil, 'an event is running' end
  local body, x, y = body_of(where)
  local fake
  if not body then
    if not x then return nil, 'where: {x=, y=} or an actor' end
    fake = ffi.new('int32_t[64]')                       -- a body: +4 x, +8 y, +0xc w, +0x10 h (the spawn takes its centre)
    fake[1], fake[2] = x, y
    body = addr(fake)
  end
  local p = ffi.cast('int (__stdcall *)(int, void *, int)', f)(s.pickup_kind, ffi.cast('void *', body), id)
  if p == 0 then return nil, 'no free pickup slot' end
  return true
end

-- ── a character type's drops (actors.define{drops=, coins=}) ─────────────────────────────────────────────────────
local drops_armed
local function arm_drops()
  if drops_armed then return true end
  local f = sym.va('foe_drops')
  if not (f and hook and hook.typed) then return nil, 'foe_drops unavailable' end
  local extra = nil
  hook.typed(f, 'int __thiscall(void *, int, int, int)', {
    pre = function(ctx)
      local A = game.actors
      local p = addr(ctx.args[1])
      local d
      for _, a in ipairs(A.list() or {}) do if a.ptr == p and a.type then d = A.defined(a.type) break end end
      if not d or not (d.drops or d.coins) then return end
      if d.coins then ctx.args[2] = d.coins end
      local first = d.drops and d.drops[1]
      if first then ctx.args[3], ctx.args[4] = first.id, first.chance or 1000 end
      if d.drops and #d.drops > 1 then extra = { p = p, list = d.drops } end
    end,
    post = function()
      local e = extra
      extra = nil
      if not e then return end
      for i = 2, #e.list do
        local r = e.list[i]
        if math.random(0, 999) < (r.chance or 1000) then
          local okd, why = items.drop(r.id, e.p)
          if not okd then log(('[items] drop %d: %s'):format(r.id, tostring(why))) end
        end
      end
    end })
  drops_armed = true
  log(('[items] foe_drops hooked (0x%x): character types drop their own items'):format(f))
  return true
end

-- fill the bindings' own (cached) tables in place
local I, A = game.items, game.actors
if I then
  local catalog0 = I.catalog
  for k, v in pairs(items) do I[k] = v end
  I.catalog = function()
    local t = catalog0 and catalog0() or {}
    for _, id in ipairs(order) do
      local d = items.def(id)
      if d then t[#t + 1] = { id = id, name = d.name, price = d.price, icon = d.icon, ours = true } end
    end
    return t
  end
end
if A and A.define then
  local define0 = A.define
  A.define = function(d)
    local ok, err = define0(d)
    if ok and type(d) == 'table' and (d.drops or d.coins) then
      local okd, e2 = arm_drops()
      if not okd then log('[items] drops: ' .. tostring(e2)) end
    end
    return ok, err
  end
end
