-- core/lua/maps.lua — mod.game.maps: the game's room maps as editable objects, shared between mods.   [api 1.11]
--
-- A room's map is a DATA resource (map_res_load 0x587970; the port's tools/extract/map_data.py):
--   0x00 dword 0x30 + 0x30-byte editor block | 0x34 map header: name[0x20], cols, rows, planes, objects, tail
--   0x68 cells: planes*rows*cols × 28 B {co, piece, bank, frame, shape, -, -}   (index (rows*z+y)*cols+x, 32 px)
--   then objects: a 60-byte record {id, x, y, depth, code, -, u16 sub, -, nA, nB, nC, nD, variant, tint, -}
--                 + A[nA] dwords, B[nB] × 12 B, C[nC] × 256 B, D[nD] × 8 B.
--   Zone devices: code 70101 walk-in exit, 70102 press-Up door; A[1] (Lua index) = the zone id the room's exit
--   table is keyed by (map.set_exit).  Characters (50001..59999): A[2] = talk handle, A[5] facing (0 right, else
--   left), A[7] presence (9 = always); a character record's x / y = the top-left of its sprite cell (feet = y +
--   the sheet's cell height).
--
--   maps.read(id)            -> Map | nil, err     the game's REAL map (never another mod's version of it)
--   maps.parse(bytes)        -> Map                m:serialize() -> bytes
--   maps.edit(id, fn[, who]) -> true | nil, err    change a map the game ships: fn(m) edits the Map in place.
--        EDITS COMPOSE: every mod's edit of the same map is applied, in registration order, to one copy of the
--        real data, and the result is what the game loads — two mods can each add a door to the same house.
--        (An edit that throws is dropped and logged; the others still apply.)
--   maps.define(id, src[, who]) -> true | nil, err a NEW map under DATA id `id`: src = a Map, bytes, or
--        function() -> Map | bytes.  Refused when the game or another mod already has that id.
--   maps.bytes(id)           -> bytes | nil        what the game will load for `id` right now
--   maps.owner(id)           -> who | nil          who defined a new map
--   maps.hits(id)            -> n                  how many times the game loaded the served version
--   maps.clone(obj)          -> a deep copy of a map object
--   Map: m.cols, m.rows, m.planes, m.cells, m.objs (objects: {rec = {15 ints}, A = {...}, B = {...}, C, D})
--     m:cell(z, y, x) / m:set_cell(z, y, x, {7 ints})
--     m:find(pred) / m:find_all(pred) / m:filter(pred) / m:next_id()
--     m:widen(c0, w, copies)      repeat columns [c0, c0 + w) `copies` more times (objects to the right move)
--     m:add(obj[, first])         append an object (first = true: at the head of the list — zone devices are
--                                 matched in list order, and a door must come before overlapping script zones)
--     m:add_door{like = zone | obj, zone =, x =[, y =, first = true]} -> obj     a copy of an existing door device
--     m:add_character{like = code | obj, code =, x =, floor =, cell_h =, handle =[, facing = 'left' | 'right',
--                     always = true]} -> obj     a copy of an existing character record, standing on `floor`
local log, mem, sym, game, res = ...

local function u32(s, o) local a, b, c, d = s:byte(o + 1, o + 4) return a + b * 256 + c * 65536 + d * 16777216 end
local function i32(s, o) local v = u32(s, o) return v >= 2147483648 and v - 4294967296 or v end
local function p32(v)
  v = v % 4294967296
  return string.char(v % 256, math.floor(v / 256) % 256, math.floor(v / 65536) % 256, math.floor(v / 16777216) % 256)
end
local function ints(s, o, n) local t = {} for i = 1, n do t[i] = i32(s, o + 4 * (i - 1)) end return t end
local function pack(t) local r = {} for i = 1, #t do r[i] = p32(t[i]) end return table.concat(r) end

local Map = {}
Map.__index = Map
local maps = { Map = Map }

function maps.parse(blob)
  assert(type(blob) == 'string' and #blob >= 0x68, 'maps.parse: not a map')
  local m = setmetatable({}, Map)
  m.head = blob:sub(1, 0x68)
  m.cols, m.rows, m.planes = u32(blob, 0x54), u32(blob, 0x58), u32(blob, 0x5c)
  local n = u32(blob, 0x60)
  m.cells = {}
  local pos = 0x68
  for i = 1, m.cols * m.rows * m.planes do m.cells[i] = ints(blob, pos, 7); pos = pos + 28 end
  m.objs = {}
  for _ = 1, n do
    local r = ints(blob, pos, 15)
    local na, nb, nc, nd = r[8], r[9], r[10], r[11]
    local o = { rec = r, A = ints(blob, pos + 0x3c, na), B = ints(blob, pos + 0x3c + na * 4, nb * 3) }
    local q = pos + 0x3c + na * 4 + nb * 12
    o.C = blob:sub(q + 1, q + nc * 256); o.D = blob:sub(q + nc * 256 + 1, q + nc * 256 + nd * 8)
    m.objs[#m.objs + 1] = o
    pos = q + nc * 256 + nd * 8
  end
  assert(pos == #blob, 'maps.parse: trailing bytes')
  return m
end
function Map:cell(z, y, x) return self.cells[(self.rows * z + y) * self.cols + x + 1] end
function Map:set_cell(z, y, x, v) self.cells[(self.rows * z + y) * self.cols + x + 1] = v end
function Map:serialize()
  local out = { self.head:sub(1, 0x54), p32(self.cols), self.head:sub(0x59, 0x60), p32(#self.objs), self.head:sub(0x65, 0x68) }
  for i = 1, #self.cells do out[#out + 1] = pack(self.cells[i]) end
  for _, o in ipairs(self.objs) do
    local r = o.rec
    r[8], r[9], r[10], r[11] = #o.A, #o.B / 3, #o.C / 256, #o.D / 8
    out[#out + 1] = pack(r); out[#out + 1] = pack(o.A); out[#out + 1] = pack(o.B)
    out[#out + 1] = o.C; out[#out + 1] = o.D
  end
  return table.concat(out)
end
function Map:find(pred) for _, o in ipairs(self.objs) do if pred(o) then return o end end end
function Map:find_all(pred) local k = {} for _, o in ipairs(self.objs) do if pred(o) then k[#k + 1] = o end end return k end
function Map:filter(pred) local k = {} for _, o in ipairs(self.objs) do if pred(o) then k[#k + 1] = o end end self.objs = k end
function Map:next_id() local n = 0 for _, o in ipairs(self.objs) do if o.rec[1] > n then n = o.rec[1] end end return n + 1 end
function Map:widen(c0, w, copies)
  local cols, add = self.cols, w * copies
  local nc = {}
  for z = 0, self.planes - 1 do
    for y = 0, self.rows - 1 do
      for x = 0, cols + add - 1 do
        local sx
        if x < c0 + w then sx = x
        elseif x < c0 + w + add then sx = c0 + (x - c0 - w) % w
        else sx = x - add end
        nc[(self.rows * z + y) * (cols + add) + x + 1] = { unpack(self:cell(z, y, sx)) }
      end
    end
  end
  self.cells, self.cols = nc, cols + add
  for _, o in ipairs(self.objs) do
    if o.rec[2] >= (c0 + w) * 32 then o.rec[2] = o.rec[2] + add * 32 end
  end
end
function maps.clone(o) return { rec = { unpack(o.rec) }, A = { unpack(o.A) }, B = { unpack(o.B) }, C = o.C, D = o.D } end
function Map:add(o, first)
  o.rec[1] = self:next_id()
  if first then table.insert(self.objs, 1, o) else self.objs[#self.objs + 1] = o end
  return o
end
function Map:add_door(t)
  local src = t.like
  if type(src) == 'number' then
    src = self:find(function(o) return (o.rec[5] == 70102 or o.rec[5] == 70101) and o.A[1] == t.like end)
  end
  assert(type(src) == 'table', 'add_door: like = an existing door\'s zone id (or its object)')
  assert(t.zone and t.x, 'add_door{like =, zone =, x =}')
  local d = maps.clone(src)
  d.rec[2] = t.x
  if t.y then d.rec[3] = t.y end
  d.A[1] = t.zone
  return self:add(d, t.first ~= false)
end
function Map:add_character(t)
  local src = t.like
  if type(src) == 'number' then src = self:find(function(o) return o.rec[5] == t.like end) end
  assert(type(src) == 'table', 'add_character: like = a character code present in this map (or an object)')
  assert(t.x and t.floor and t.cell_h, 'add_character{x =, floor = (feet y), cell_h = (the sheet\'s cell height)}')
  local c = maps.clone(src)
  c.rec[5] = t.code or c.rec[5]
  c.rec[2], c.rec[3] = t.x, t.floor - t.cell_h
  while #c.A < 7 do c.A[#c.A + 1] = 0 end
  if t.handle then c.A[2] = t.handle end
  if t.facing then c.A[5] = (t.facing == 'left' or t.facing == 1) and 1 or 0 end
  if t.always ~= false then c.A[7] = 9 end
  return self:add(c)
end

-- ── the shared registry ──────────────────────────────────────────────────────────────────────────────────────────
local edits, defined = {}, {}                -- id → { {fn, who}, ... } / id → { who, bytes }
-- The module the game reads maps from (0 = the exe).  Overlay entries are made for THAT module only: resource ids
-- collide between modules (DATA 1026 = the storeroom's map in the exe and a sprite sheet in sotesd.dll — an entry
-- for "any module" made the sheet's load fail: "Failed Surface Load'1026").
local function map_hmod()
  local mm = sym.va('map_module')
  local h = mm and mem.read_u32(mm) or 0
  return h ~= 0 and h or mem.base()
end
local function real_bytes(id) return res.read('DATA', id, map_hmod()) end
function maps.read(id)
  local b = real_bytes(id)
  if not b then return nil, 'DATA ' .. tostring(id) .. ' not found' end
  local ok, m = pcall(maps.parse, b)
  if not ok then return nil, m end
  return m
end
local function rebuild(id)
  local m, err = maps.read(id)
  if not m then return nil, err end
  local applied = 0
  for _, e in ipairs(edits[id]) do
    if not e.dead then
      local ok, why = pcall(e.fn, m)
      if ok then applied = applied + 1
      else
        e.dead = true
        log(('[maps] edit of DATA %d by %s dropped: %s'):format(id, tostring(e.who), tostring(why)))
        m = nil
        break
      end
    end
  end
  if not m then return rebuild(id) end       -- a failed edit may have left the Map half-changed: start over without it
  local bytes = m:serialize()
  res.set('DATA', id, bytes, map_hmod())
  return bytes, applied
end
function maps.edit(id, fn, who)
  assert(type(id) == 'number' and type(fn) == 'function', 'maps.edit(id, fn[, who])')
  if defined[id] then return nil, ('DATA %d is a map defined by %s — edit your own maps before defining them'):format(id, tostring(defined[id].who)) end
  if not real_bytes(id) then return nil, 'DATA ' .. id .. ' not found' end
  edits[id] = edits[id] or {}
  local e = { fn = fn, who = who or '?' }
  table.insert(edits[id], e)
  local bytes, n = rebuild(id)
  if not bytes then return nil, n end
  if e.dead then return nil, 'the edit failed (see the log)' end
  log(('[maps] DATA %d: %d edit(s) applied (%s added one), %d B'):format(id, n, tostring(e.who), #bytes))
  return true
end
function maps.define(id, src, who)
  assert(type(id) == 'number', 'maps.define(id, map | bytes | fn[, who])')
  if defined[id] then return nil, ('DATA %d is already defined by %s'):format(id, tostring(defined[id].who)) end
  if real_bytes(id) or edits[id] then return nil, 'DATA ' .. id .. ' exists in the game' end
  if res.read('DATA', id) then return nil, 'DATA ' .. id .. ' exists in the game' end
  if type(src) == 'function' then src = src() end
  if type(src) == 'table' then src = src:serialize() end
  if type(src) ~= 'string' then return nil, 'maps.define: nothing to define' end
  if not res.set('DATA', id, src, map_hmod()) then return nil, 'the resource overlay refused' end
  defined[id] = { who = who or '?', bytes = src }
  log(('[maps] DATA %d defined by %s, %d B'):format(id, tostring(who or '?'), #src))
  return true
end
function maps.bytes(id)
  if defined[id] then return defined[id].bytes end
  if edits[id] then return (rebuild(id)) end
  return real_bytes(id)
end
function maps.hits(id) return res.hits('DATA', id, map_hmod()) end   -- times the game loaded our version
function maps.owner(id) return defined[id] and defined[id].who end
function maps.edits(id) local n = 0 for _, e in ipairs(edits[id] or {}) do if not e.dead then n = n + 1 end end return n end

rawset(game, 'maps', maps)
return {}
