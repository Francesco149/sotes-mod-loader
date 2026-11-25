-- core/lua/sprites.lua — mod.game.sprites (api 1.9, embedded).
-- Receives (mem, sym, log, game, res, edition_fn); fills game.sprites.
--
-- CUSTOM SPRITES ON RETAIL'S OWN SPRITE BANKS.
--   A SHEET is a DATA resource: a 0x458-byte header whose self-rebasing pointer table holds the width / height / bit
--   depth (bs_parse_compressed_header 0x5b7c10: base = dword +0x448; signature dword +0x438 - base == 0x2711; width =
--   dword (4 + 4 (dword +0x440 - base)) - base; height = dword (0x420 + 4 (dword +0x18 - base)) - base; bpp = dword
--   +0x430 - base; pixels at 0x458 + (dword +0x450 - base)), a 256-entry BGRA palette at +0x20, then the pixels as a
--   bottom-up DIB.  The character sheets are 8bpp; index 0 (black) is the colour key.
--   A BANK (sys + 0xaac / SE 0xb08 / JP 0xb04, 925 u16-indexed ar_sprite_slot*) is a sheet cut into a grid of cells
--   (slot +0x20 / +0x24 cell w / h, +0x3c the module, u16 +0x40 the DATA id); its frames are decoded lazily on first
--   draw (0x4184a0).  The bank's palette descriptor (sys + 0x18e0 / SE 0x197c / JP 0x1988, same index) starts with
--   the MIRROR frame offset: a character facing left draws frame + that (the folk sheets: 16 = two rows down).
--   A CHARACTER draws its track 0 (char +0x48 u16 = the bank) through its body's clip (body +0x6c): frame =
--   base + delta[f]; the char's state clips: stand +0x15914, air +0x15918 / +0x1591c, walk +0x15980, run +0x15994
--   (SE / JP +0x20 each).
--   slot_clone (0x4179b0 / SE 0x411b10, thiscall sys, dst, src) makes bank dst a frameless copy of src's descriptor
--   (cell size, key, module, DATA id, aux records, mirror offset) — retail's own way to make a variant bank.  The
--   last 25 banks are retail's dynamic tint clones (the allocator marks desc +0x10); the rest with no DATA id are
--   unused: 2 to 4 per edition.
--
-- BANKS PAST THE END (api 1.10).  The two tables are fixed arrays inside the static system object — 909 entries on
--   enold, 925 on SE EN, 929 on SE JP (slots end where the descriptors start), allocated once at boot (build_title_
--   drive: a 0x44-byte slot + a 0x14-byte descriptor per entry) and never moved.  Bank numbers are u16 everywhere.
--   So the loader RELOCATES the tables: two arrays of CAP entries of its own, the retail pointers mirrored in front,
--   and every instruction that reads `table[register]` (97 / 113 / 115 sites, found by tools/banksites.py, each
--   verified before anything is written) gets its disp32 rebased onto them.  Reads with a constant bank and the
--   whole-table loops (unload by group, shutdown) stay on the old arrays, which stay right for every retail bank.
--   One loop is rebased too: the display restore (enold 0x5b14c0 — after a lost-surface restore it re-decodes every
--   decoded frame of every bank); it dereferences each entry, so unused entries point at one empty slot.
--   new_bank then takes the first unused index past the retail count: effectively unlimited (CAP - count).
--
--   sprites.decode(blob) -> img           an 8bpp sheet → {w, h, pal[0..255] = {r, g, b}, px (top-down bytes)}
--   sprites.encode(img, template) -> blob  back to a sheet (template = any stock sheet blob: its header is reused)
--   sprites.bmp(bytes) -> img              an 8-bit indexed, uncompressed BMP (index 0 = transparent)
--   sprites.image(w, h[, pal]) -> img      a blank image (all index 0)
--   img:get(x, y) / img:set(x, y, i) / img:blit(src, sx, sy, w, h, dx, dy[, map]) / img:fill(x, y, w, h, i)
--   sprites.bank(i) -> {index, res, module, cell_w, cell_h, mirror, frames} | nil
--   sprites.sheet(i) -> img, blob          bank i's sheet (the user's own data, read from the game's module)
--   sprites.new_bank{like = i, image = img | sheet = blob[, cell_w=, cell_h=, mirror=, retail=]} -> index
--                                          (at most as many cells as `like` has frames: its trim records are reused;
--                                          retail = true: one of the game's own unused banks instead of a new one)
--   sprites.capacity() -> {retail, total, used, free, extended, restores[, error]}   bank counts (extended = tables
--                                          relocated; restores = display restores seen since — each is logged)
--   sprites.clip{frames = {..}[, base=0, dur=14, loop=true, loop_to=0, dx={..}, dy={..}]} -> clip (keep it alive)
--   sprites.set_look(actor, bank)          actor = a char pointer or an actors.list() entry
--   sprites.set_clips(actor, {stand=, walk=, run=, air=})   clip objects or raw pointers; nil = unchanged
--   sprites.get_clips(actor) -> {stand=, walk=, run=, air0=, air1=} (raw pointers)
--   sprites.play(actor, clip)              put the clip on the body now (the state machine replaces it on the
--                                          next state change; for a lasting pose use set_clips{stand=})
--
-- Also on mod.game.actors (characters made custom need these):
--   actors.name(actor) / actors.set_name(actor, s)   the display name (char +0x274, 20 bytes: the target window's
--                                                     label; char_stats_init copies the stat table's name there)
--   actors.hp(actor) -> cur, max / actors.set_hp(actor, cur[, max])   the live stat block (*(char +0x750 / SE +0x760)
--                                                     +0x54 current, +0x58 base max)
--
-- NEW CHARACTER TYPES (api 1.10).  The character codes are cases of char_init's compiled switch (an unknown code =
--   "Unknown Object Type Character." + exit) and every later dispatch (AI, skills, stats) keys on the code the
--   character carries — there is no table to add a row to.  So a new type = a code of its own that char_init never
--   sees: a typed hook on char_init (enold 0x41f200; thiscall char + 14 args, the code = the 6th) swaps a defined
--   code for its BASE (a retail code: its body, AI, stats, sounds) on the way in, and on the way out puts the type's
--   look / clips / name / HP on the new character and tracks it for the type's Lua callbacks.  Map records and
--   actors.spawn both go through char_init, so both take the new code.  A map object's code picks its loader by
--   range: characters are 50001..59999 — a new code must be in there (and not one of the game's).
--   actors.define{code=, base=[, name=, hp=, bank= index | function(ptr) -> index, clips={stand=, walk=, ...},
--                 on_spawn=function(a), on_tick=function(a), on_death=function(a)]} -> true | nil, why
--        on_spawn / on_tick run on free-roam frames (not during events) with the character's actors.list() entry;
--        on_death once, with its last entry, when its HP reaches 0 or it disappears while its room is still loaded.
--   actors.defined(code) -> the definition | nil;  actors.list() entries of such characters carry type = the code
local mem, sym, log, game, res, edition_fn, banksites_fn, hook = ...
local ffi = require('ffi')

local ED = {
  enold       = { banks = 0xaac, descs = 0x18e0, clip = 0x15914, stats = 0x750 },
  ['ense-en'] = { banks = 0xb08, descs = 0x197c, clip = 0x15934, stats = 0x760 },
  ['ense-jp'] = { banks = 0xb04, descs = 0x1988, clip = 0x15934, stats = 0x760 },
}
local CAP = 8192                                         -- entries in the relocated tables
local L
local function layout()
  if not L then
    L = ED[edition_fn()]
    if L then L.count = (L.descs - L.banks) / 4 end
  end
  return L
end

local sprites = {}
local st = { next_res = 0xE100, mine = {}, clips = {}, header = nil }
local ext = { on = false, next = nil, keep = {} }       -- the relocated tables (extend())

-- ── bytes ────────────────────────────────────────────────────────────────────────────────────────────────────────
local function u32s(s, o) local a, b, c, d = s:byte(o + 1, o + 4) return a + b * 256 + c * 65536 + d * 16777216 end
local function le32(v) v = v % 4294967296 return string.char(v % 256, math.floor(v / 256) % 256, math.floor(v / 65536) % 256, math.floor(v / 16777216)) end

-- ── images ───────────────────────────────────────────────────────────────────────────────────────────────────────
local Img = {}
Img.__index = Img
local function new_img(w, h, pal)
  local p = {}
  for i = 0, 255 do p[i] = pal and pal[i] and { pal[i][1], pal[i][2], pal[i][3] } or { 0, 0, 0 } end
  local px = ffi.new('uint8_t[?]', w * h)
  return setmetatable({ w = w, h = h, pal = p, px = px }, Img)
end
function Img:get(x, y)
  if x < 0 or y < 0 or x >= self.w or y >= self.h then return 0 end
  return self.px[y * self.w + x]
end
function Img:set(x, y, i)
  if x < 0 or y < 0 or x >= self.w or y >= self.h then return end
  self.px[y * self.w + x] = i
end
function Img:fill(x, y, w, h, i) for yy = y, y + h - 1 do for xx = x, x + w - 1 do self:set(xx, yy, i) end end end
-- copy a w×h block of src at (sx, sy) to (dx, dy); index 0 is transparent; map (optional) remaps src indices
function Img:blit(src, sx, sy, w, h, dx, dy, map)
  for y = 0, h - 1 do
    for x = 0, w - 1 do
      local i = src:get(sx + x, sy + y)
      if i ~= 0 then self:set(dx + x, dy + y, map and map[i] or i) end
    end
  end
end
function Img:clone()
  local o = new_img(self.w, self.h, self.pal)
  ffi.copy(o.px, self.px, self.w * self.h)
  return o
end
sprites.image = new_img

local function header_of(blob)
  if type(blob) ~= 'string' or #blob < 0x460 then return nil, 'not a sheet' end
  local base = u32s(blob, 0x448)
  if u32s(blob, 0x438) - base ~= 0x2711 then return nil, 'bad sheet signature' end
  local wo = 4 + 4 * (u32s(blob, 0x440) - base)
  local ho = 0x420 + 4 * (u32s(blob, 0x18) - base)
  local h = { base = base, wo = wo, ho = ho, w = u32s(blob, wo) - base, h = u32s(blob, ho) - base,
              bpp = u32s(blob, 0x430) - base, pix = 0x458 + (u32s(blob, 0x450) - base) }
  return h
end

function sprites.decode(blob)
  local h, err = header_of(blob)
  if not h then return nil, err end
  if h.bpp ~= 8 then return nil, ('%d-bit sheet: only 8-bit sheets are supported'):format(h.bpp) end
  local img = new_img(h.w, h.h)
  for i = 0, 255 do
    local b, g, r = blob:byte(0x21 + 4 * i, 0x23 + 4 * i)
    img.pal[i] = { r, g, b }
  end
  local stride = h.w + (4 - h.w % 4) % 4
  local src = ffi.cast('const uint8_t *', blob) + h.pix
  if #blob < h.pix + stride * h.h then return nil, 'truncated sheet' end
  for y = 0, h.h - 1 do ffi.copy(img.px + y * h.w, src + (h.h - 1 - y) * stride, h.w) end
  st.header = st.header or blob:sub(1, h.pix)
  return img
end

function sprites.encode(img, template)
  local t = template or st.header
  assert(t, 'sprites.encode: needs a template sheet (decode any stock sheet first, or pass one)')
  local h = assert(header_of(t))
  assert(h.bpp == 8, 'sprites.encode: the template must be an 8-bit sheet')
  local head = t:sub(1, h.pix)
  local pal = {}
  for i = 0, 255 do local c = img.pal[i] or { 0, 0, 0 } pal[#pal + 1] = string.char(c[3], c[2], c[1], 0) end
  -- patch: palette, width, height (the pointer table's rebased dwords)
  local function put(s, off, bytes) return s:sub(1, off) .. bytes .. s:sub(off + #bytes + 1) end
  head = put(head, 0x20, table.concat(pal))
  head = put(head, h.wo, le32(h.base + img.w))
  head = put(head, h.ho, le32(h.base + img.h))
  local stride = img.w + (4 - img.w % 4) % 4
  local buf = ffi.new('uint8_t[?]', stride * img.h)
  for y = 0, img.h - 1 do ffi.copy(buf + (img.h - 1 - y) * stride, img.px + y * img.w, img.w) end
  return head .. ffi.string(buf, stride * img.h)
end

function sprites.bmp(bytes)
  if type(bytes) ~= 'string' or bytes:sub(1, 2) ~= 'BM' then return nil, 'not a BMP' end
  local off = u32s(bytes, 10)
  local hsz = u32s(bytes, 14)
  local w = u32s(bytes, 18)
  local hraw = u32s(bytes, 22)
  local h = hraw >= 2147483648 and hraw - 4294967296 or hraw
  local bpp = bytes:byte(29) + bytes:byte(30) * 256
  local comp = u32s(bytes, 30)
  if bpp ~= 8 or comp ~= 0 then return nil, ('BMP: %d-bit, compression %d — save it as 8-bit indexed, uncompressed'):format(bpp, comp) end
  local ncol = u32s(bytes, 46)
  if ncol == 0 then ncol = 256 end
  local img = new_img(w, math.abs(h))
  for i = 0, ncol - 1 do
    local p = 14 + hsz + 4 * i
    local b, g, r = bytes:byte(p + 1, p + 3)
    img.pal[i] = { r, g, b }
  end
  local stride = w + (4 - w % 4) % 4
  local ah = math.abs(h)
  if #bytes < off + stride * ah then return nil, 'BMP: truncated' end
  local src = ffi.cast('const uint8_t *', bytes) + off
  for y = 0, ah - 1 do
    local sy = h > 0 and (ah - 1 - y) or y
    ffi.copy(img.px + y * w, src + sy * stride, w)
  end
  return img
end

-- ── banks ────────────────────────────────────────────────────────────────────────────────────────────────────────
local function sys() return sym.va('g_sys') end
local function slot(i)
  if ext.on then return ext.slots[i] end
  local l = layout() return l and mem.read_u32(sys() + l.banks + 4 * i) or 0
end
local function desc(i)
  if ext.on then return ext.descs[i] end
  local l = layout() return l and mem.read_u32(sys() + l.descs + 4 * i) or 0
end
local function nbanks() local l = layout() return ext.on and CAP or (l and l.count or 0) end
local function addr(cd) return tonumber(ffi.cast('uintptr_t', cd)) end

-- Relocate the bank tables (see the header).  Everything is checked before the first byte changes; a refusal leaves
-- the game untouched and is remembered (new_bank then falls back to the game's own unused banks).
local function extend()
  if ext.on then return true end
  if ext.err then return nil, ext.err end
  local l, b = layout(), banksites_fn and banksites_fn()
  if not l or not b then ext.err = 'unknown edition' return nil, ext.err end
  local olds, oldd = b.sys + b.slots_off, b.sys + b.descs_off
  if b.sys ~= sys() or b.slots_off ~= l.banks or b.descs_off ~= l.descs or b.count ~= l.count then
    ext.err = 'bank site table does not match this edition' return nil, ext.err
  end
  for i = 0, b.count - 1 do                              -- booted: every retail entry allocated (not latched)
    if mem.read_u32(olds + 4 * i) == 0 or mem.read_u32(oldd + 4 * i) == 0 then return nil, 'the sprite system is not up yet' end
  end
  local function fail(what) ext.err = what log('[sprites] banks not extended: ' .. what) return nil, what end
  for _, s in ipairs(b.sites) do
    local want = s[2]
    if mem.read_u32(s[1]) ~= want then return fail(('site %08x holds %08x, not %08x'):format(s[1], mem.read_u32(s[1]) or 0, want)) end
    local t = s[3] == 0 and b.slots_off or b.descs_off
    if want ~= t and want ~= b.sys + t then return fail(('site %08x: unexpected operand'):format(s[1])) end
  end
  if mem.read_u32(b.restore_base) ~= olds or mem.read_u32(b.restore_count) ~= b.count then return fail('the display-restore loop differs') end
  ext.slots, ext.descs = ffi.new('uint32_t[?]', CAP), ffi.new('uint32_t[?]', CAP)
  ext.null_slot, ext.null_desc = ffi.new('uint8_t[0x44]'), ffi.new('uint8_t[0x14]')   -- no entries: every loop skips it
  for i = 0, CAP - 1 do
    if i < b.count then
      ext.slots[i], ext.descs[i] = mem.read_u32(olds + 4 * i), mem.read_u32(oldd + 4 * i)
    else
      ext.slots[i], ext.descs[i] = addr(ext.null_slot), addr(ext.null_desc)
    end
  end
  local news, newd = addr(ext.slots), addr(ext.descs)
  local done = {}
  local function put(va, v)
    local old = mem.read_u32(va)
    if not mem.patch(va, le32(v)) then return false end
    done[#done + 1] = { va, old }
    return true
  end
  local ok = true
  for _, s in ipairs(b.sites) do
    local new = s[3] == 0 and news or newd
    if s[2] < 0x10000 then new = (new - b.sys) % 4294967296 end   -- [sys register + index * 4 + offset]
    if not put(s[1], new) then ok = false break end
  end
  ok = ok and put(b.restore_base, news) and put(b.restore_count, CAP)
  if not ok then
    for i = #done, 1, -1 do mem.patch(done[i][1], le32(done[i][2])) end
    return fail('a code patch was refused')
  end
  ext.on, ext.next, ext.retail = true, b.count, b.count
  -- Instrumentation: the display restore.  The loop is entered right after the engine's "restore every surface" call
  -- (enold 0x5b9240, reached only when DirectDraw reported a lost surface): an observer on that call's target logs
  -- each restore with how many of the new banks hold decoded frames the loop is about to re-decode.
  local loop = b.restore_base - 1
  if hook and hook.entry and mem.read_u8(loop - 5) == 0xE8 then
    local target = (loop + mem.read_i32(loop - 4)) % 4294967296
    local okh, errh = pcall(hook.entry, target, function()
      ext.restores = (ext.restores or 0) + 1
      local banks, frames = 0, 0
      for i = ext.retail, ext.next - 1 do
        local sl = ext.slots[i]
        local e, n, k = mem.read_u32(sl), mem.read_u16(sl + 4), 0
        if e ~= 0 then for f = 0, n - 1 do if mem.read_u32(e + 8 * f) ~= 0 then k = k + 1 end end end
        if k > 0 then banks, frames = banks + 1, frames + k end
      end
      log(('[sprites] DISPLAY RESTORE #%d: the engine restores its surfaces and re-decodes every bank; %d new banks hold %d decoded frames (table of %d entries walked)')
          :format(ext.restores, banks, frames, mem.read_u32(b.restore_count) or -1))
    end)
    log(okh and ('[sprites] display-restore observer on 0x%x'):format(target) or ('[sprites] no display-restore observer: ' .. tostring(errh)))
  end
  log(('[sprites] bank tables relocated: %d retail banks + %d new (%d sites)'):format(b.count, CAP - b.count, #b.sites))
  return true
end

function sprites.bank(i)
  if type(i) ~= 'number' or i < 0 or i >= nbanks() or i % 1 ~= 0 then return nil end
  local s = slot(i)
  if s == 0 or (ext.on and s == addr(ext.null_slot)) then return nil end
  local d = desc(i)
  local e = mem.read_u32(s)
  return { index = i, res = mem.read_u16(s + 0x40), module = mem.read_u32(s + 0x3c), cell_w = mem.read_u32(s + 0x20),
           cell_h = mem.read_u32(s + 0x24), mirror = d ~= 0 and mem.read_u16(d) or 0,
           frames = mem.read_u16(s + 0x38), decoded = e ~= 0 and mem.read_u32(e) ~= 0, ours = st.mine[i] ~= nil }
end

function sprites.sheet(i)
  local b = sprites.bank(i)
  if not b or b.res == 0 then return nil, 'no such bank' end
  local blob = res.read('DATA', b.res, b.module ~= 0 and b.module or nil)
  if not blob then return nil, 'sheet DATA ' .. b.res .. ' not found' end
  local img, err = sprites.decode(blob)
  return img, blob or err
end

-- the game's own unused banks (no DATA id, nothing decoded), short of its dynamic tint clones (the last 25)
local function retail_free(i)
  local s = slot(i)
  return s ~= 0 and mem.read_u16(s + 0x40) == 0 and mem.read_u32(s) == 0 and not st.mine[i]
end
local function free_retail_bank()
  local l = layout()
  for i = 1, (l and l.count or 0) - 26 do if retail_free(i) then return i end end
end
local function count_retail_free()
  local l, n = layout(), 0
  for i = 1, (l and l.count or 0) - 26 do if retail_free(i) then n = n + 1 end end
  return n
end
-- a new bank past the retail tables: its own (zeroed, as at boot) slot + descriptor
local function new_index()
  if not extend() then return nil end
  local i = ext.next
  if i >= CAP then return nil end
  local sl, de = ffi.new('uint8_t[0x44]'), ffi.new('uint8_t[0x14]')
  ext.keep[#ext.keep + 1] = { sl, de }
  ext.slots[i], ext.descs[i] = addr(sl), addr(de)
  ext.next = i + 1
  return i
end
function sprites.free_banks()
  if extend() then return CAP - ext.next end
  return count_retail_free()
end
function sprites.capacity()
  local l = layout()
  local ok, err = extend()
  local used = 0
  for _ in pairs(st.mine) do used = used + 1 end
  return { retail = l and l.count or 0, total = ok and CAP or (l and l.count or 0), used = used,
           free = ok and CAP - ext.next or count_retail_free(), extended = ok and true or false, error = err,
           restores = ext.restores or 0 }
end

local clone_fn
function sprites.new_bank(o)
  assert(type(o) == 'table' and type(o.like) == 'number', 'sprites.new_bank{like = bank, image = img | sheet = blob}')
  if not layout() then return nil, 'unknown edition' end
  local src = sprites.bank(o.like)
  if not src or src.res == 0 then return nil, 'like: bank ' .. tostring(o.like) .. ' is empty' end
  local blob = o.sheet
  if not blob then
    assert(o.image, 'sprites.new_bank: image or sheet')
    local tmpl = res.read('DATA', src.res, src.module ~= 0 and src.module or nil)
    blob = sprites.encode(o.image, tmpl)
  end
  local hd, err = header_of(blob)
  if not hd then return nil, err end
  local i
  if not o.retail then i = new_index() end
  i = i or free_retail_bank()
  if not i then return nil, 'no free sprite bank' end
  if not clone_fn then
    local va = sym.va('slot_clone')
    if not va then return nil, 'slot_clone unavailable' end
    clone_fn = ffi.cast('void *(__thiscall *)(void *, uint16_t, uint16_t)', va)
  end
  local id = st.next_res
  while res.read('DATA', id) or (src.module ~= 0 and res.read('DATA', id, src.module)) do id = id + 1 end   -- never shadow a real one
  st.next_res = id + 1
  if not res.set('DATA', id, blob) then return nil, 'resource overlay refused' end
  clone_fn(ffi.cast('void *', sys()), i, o.like)
  local s = slot(i)
  mem.write_u16(s + 0x40, id)
  if o.cell_w then mem.write_u32(s + 0x20, o.cell_w) end
  if o.cell_h then mem.write_u32(s + 0x24, o.cell_h) end
  if o.mirror then local d = desc(i) if d ~= 0 then mem.write_u16(d, o.mirror) end end
  -- the per-frame trim records (slot +0x34, 24 bytes per cell: x left / right, y top / bottom, has opaque pixels, has
  -- key pixels — bs_trim_opaque_rect's result, which the slicer uses instead of scanning) came with the clone from
  -- `like`: recompute them for the new pixels, or a cell that is empty in `like`'s sheet stays invisible here.
  local aux, naux = mem.read_u32(s + 0x34), 0
  local srcs = slot(o.like)
  naux = mem.read_u32(srcs + 0x38)
  if naux == 0 then                                      -- `like` not decoded yet: its sheet's cell count
    local sh = header_of(res.read('DATA', src.res, src.module ~= 0 and src.module or nil) or '')
    if sh then naux = math.floor(sh.w / src.cell_w) * math.floor(sh.h / src.cell_h) end
  end
  if aux ~= 0 then
    local img = o.image or sprites.decode(blob)
    local cw, ch = mem.read_u32(s + 0x20), mem.read_u32(s + 0x24)
    local cols, rows = math.floor(hd.w / cw), math.floor(hd.h / ch)
    if cols * rows > naux then
      return nil, ('the sheet has %d cells, `like` only %d trim records'):format(cols * rows, naux)
    end
    local r = ffi.cast('int32_t *', aux)
    for c = 0, naux - 1 do
      local x0, x1, y0, y1, op, key = cw, 0, ch, 0, 0, 0
      if c < cols * rows and img then
        local bx, by = (c % cols) * cw, math.floor(c / cols) * ch
        for y = 0, ch - 1 do
          local row = img.px + (by + y) * img.w + bx
          for x = 0, cw - 1 do
            if row[x] ~= 0 then
              op = 1
              if x < x0 then x0 = x end
              if x > x1 then x1 = x end
              if y < y0 then y0 = y end
              if y > y1 then y1 = y end
            else
              key = 1
            end
          end
        end
      else
        key = 1
      end
      r[6 * c], r[6 * c + 1], r[6 * c + 2], r[6 * c + 3], r[6 * c + 4], r[6 * c + 5] = x0, x1, y0, y1, op, key
    end
  end
  st.mine[i] = { res = id }
  log(('[sprites] bank %d = DATA %d (%dx%d, like %d)'):format(i, id, hd.w, hd.h, o.like))
  return i
end

-- ── clips + characters ───────────────────────────────────────────────────────────────────────────────────────────
local Clip = {}
Clip.__index = Clip
function sprites.clip(o)
  local fr = assert(o.frames, 'sprites.clip{frames = {...}}')
  assert(#fr >= 1 and #fr <= 32, 'sprites.clip: 1..32 frames')
  local buf = ffi.new('uint8_t[0x154]')
  local p = ffi.cast('uint8_t *', buf)
  ffi.cast('int16_t *', p)[0] = o.base or 0
  for f = 1, #fr do ffi.cast('int16_t *', p + 2)[f - 1] = fr[f] end
  ffi.cast('uint16_t *', p + 0x42)[0] = #fr
  ffi.cast('uint16_t *', p + 0x44)[0] = o.dur or 14
  ffi.cast('int32_t *', p + 0x48)[0] = (o.loop == false) and 1 or 0
  for f = 1, #fr do
    ffi.cast('int32_t *', p + 0x50)[f - 1] = o.dx and o.dx[f] or 0
    ffi.cast('int32_t *', p + 0xd0)[f - 1] = o.dy and o.dy[f] or 0
  end
  ffi.cast('uint16_t *', p + 0x152)[0] = o.loop_to or 0
  local c = setmetatable({ buf = buf, ptr = tonumber(ffi.cast('uintptr_t', buf)) }, Clip)
  st.clips[#st.clips + 1] = c                          -- clips live as long as the session: the game keeps pointers
  return c
end

local function ptr_of(a)
  if type(a) == 'table' then return a.ptr end
  return a
end
local function clip_ptr(c) if type(c) == 'table' then return c.ptr end return c end

function sprites.set_look(a, bank)
  local p = ptr_of(a)
  if not p or p == 0 then return nil, 'no actor' end
  mem.write_u16(p + 0x48, bank)
  return true
end
local CLIPS = { stand = 0, air0 = 4, air1 = 8, walk = 0x6c, run = 0x80 }
function sprites.set_clips(a, t)
  local p = ptr_of(a)
  if not p or p == 0 or not layout() then return nil, 'no actor' end
  for k, v in pairs(t) do
    local keys = k == 'air' and { 'air0', 'air1' } or { k }
    for _, kk in ipairs(keys) do
      local off = CLIPS[kk]
      if not off then error('sprites.set_clips: unknown clip ' .. tostring(k), 2) end
      mem.write_u32(p + L.clip + off, clip_ptr(v) or 0)
    end
  end
  return true
end
function sprites.get_clips(a)
  local p = ptr_of(a)
  if not p or p == 0 or not layout() then return nil end
  local o = {}
  for k, off in pairs(CLIPS) do o[k] = mem.read_u32(p + L.clip + off) end
  return o
end
function sprites.play(a, c)
  local p = ptr_of(a)
  if not p or p == 0 then return nil, 'no actor' end
  local body = mem.read_u32(p + 0x40)
  mem.write_u32(body + 0x6c, clip_ptr(c))
  mem.write_u16(body + 0x70, 0); mem.write_u16(body + 0x72, 0); mem.write_u8(body + 0x74, 0)
  return true
end

local actors_ext = {}
function actors_ext.name(a)
  local p = ptr_of(a)
  if not p or p == 0 then return nil end
  return ffi.string(ffi.cast('const char *', p + 0x274))
end
function actors_ext.set_name(a, s)
  local p = ptr_of(a)
  if not p or p == 0 then return nil, 'no actor' end
  s = tostring(s):sub(1, 19)
  for i = 1, #s do mem.write_u8(p + 0x274 + i - 1, s:byte(i)) end
  mem.write_u8(p + 0x274 + #s, 0)
  return true
end
local function stats(a)
  local p = ptr_of(a)
  if not p or p == 0 or not layout() then return nil end
  local sb = mem.read_u32(p + L.stats)
  return sb ~= 0 and sb or nil
end
function actors_ext.hp(a)
  local sb = stats(a)
  if not sb then return nil end
  return mem.read_i32(sb + 0x54), mem.read_i32(sb + 0x58)
end
function actors_ext.set_hp(a, cur, max)
  local sb = stats(a)
  if not sb then return nil, 'no stat block' end
  if max then mem.write_u32(sb + 0x58, max) end
  mem.write_u32(sb + 0x54, cur)
  return true
end
-- fill the bindings' own (cached) tables in place: assigning game.X would shadow the registry's toggle
local A, SPR = game.actors, game.sprites
if A then for k, v in pairs(actors_ext) do A[k] = v end end
if SPR then for k, v in pairs(sprites) do SPR[k] = v end end

-- ── new character types ──────────────────────────────────────────────────────────────────────────────────────────
if A then
  local defs, live, cur = {}, {}, {}                     -- by code; by char pointer; char_init calls in flight
  local pend                                             -- the type the next char_init makes (actors.spawn)
  local armed
  local list0, spawn0 = A.list, A.spawn
  local function area() local m = game.map and game.map.get() return m and m.area end
  local function apply(p, d)
    if d.bank then
      local b = d.bank
      if type(b) == 'function' then
        local ok, r = pcall(b, p)
        if not ok then log('[actors] type ' .. d.code .. ' bank: ' .. tostring(r)) r = nil end
        b = r
      end
      if b then mem.write_u16(p + 0x48, b) end
    end
    if d.clips then
      sprites.set_clips(p, d.clips)
      if d.clips.stand then sprites.play(p, d.clips.stand) end
    end
    if d.name then actors_ext.set_name(p, d.name) end
    if d.hp then actors_ext.set_hp(p, d.hp, d.hp) end
    live[p] = { def = d, area = area(), new = true }
  end
  local function ci_pre(ctx)
    local p = addr(ctx.args[1])
    live[p] = nil                                        -- the slot is being made into something else
    local d = pend or defs[ctx.args[7]]
    pend = nil
    if d then ctx.args[7] = d.base; cur[#cur + 1] = { p, d } end
  end
  local function ci_post(ctx)
    local c = cur[#cur]
    if c and c[1] == addr(ctx.args[1]) then cur[#cur] = nil; apply(c[1], c[2]) end
  end
  local function call(d, name, a)
    local f = d[name]
    if not f then return end
    local ok, err = pcall(f, a)
    if not ok then log(('[actors] type %d %s: %s'):format(d.code, name, tostring(err))) end
  end
  local function tick()
    if not next(live) then return end
    local here, by = area(), {}
    for _, a in ipairs(list0() or {}) do by[a.ptr] = a end
    for p, t in pairs(live) do
      local a, d = by[p], t.def
      if t.area ~= here then
        live[p] = nil                                    -- its room is gone: not a death
      elseif not a then
        live[p] = nil
        if t.last then call(d, 'on_death', t.last) end
      else
        a.type = d.code
        local hp, max = actors_ext.hp(a)
        if t.new then t.new = nil; call(d, 'on_spawn', a) end
        if hp and max and max > 0 and hp <= 0 then
          live[p] = nil
          call(d, 'on_death', a)
        else
          t.last = a
          call(d, 'on_tick', a)
        end
      end
    end
  end
  local function arm()
    if armed then return true end
    if not hook then return nil, 'no hook layer' end
    local ci, gp = sym.va('char_init'), sym.va('gimmick_pass')
    if not (ci and gp) then return nil, 'char_init / gimmick_pass missing on this build' end
    hook.typed(ci, 'void __thiscall(void *, int, int, int, int, int, int, int16_t, int, int, int, int, int, int, int)',
               { pre = ci_pre, post = ci_post })
    hook.typed(gp, 'int __thiscall(void *, void *)', { pre = function() tick() end })
    armed = true
    log(('[actors] character types armed (char_init 0x%x)'):format(ci))
    return true
  end
  function A.define(d)
    assert(type(d) == 'table' and type(d.code) == 'number' and type(d.base) == 'number', 'actors.define{code=, base=, ...}')
    if d.code <= 50000 or d.code >= 60000 or d.code % 1 ~= 0 then return nil, 'code: a character code is 50001..59999' end
    if A.valid(d.code) then return nil, ('code %d is one of the game\'s own'):format(d.code) end
    if not A.valid(d.base) then return nil, ('base %d is not a character of this edition'):format(d.base) end
    local ok, err = arm()
    if not ok then return nil, err end
    local t = {}
    for k, v in pairs(d) do t[k] = v end
    defs[d.code] = t
    return true
  end
  function A.defined(code) return defs[code] end
  function A.spawn(code, o)
    local d = defs[code]
    if not d then return spawn0(code, o) end
    pend = d
    local slot, err = spawn0(d.base, o)
    if pend then                                         -- char_init ran unhooked (a spawn from inside another one)
      pend = nil
      if slot then for _, a in ipairs(list0() or {}) do if a.slot == slot then apply(a.ptr, d) end end end
    end
    return slot, err
  end
  function A.list()
    local t = list0()
    if t and next(live) then
      for _, a in ipairs(t) do local lv = live[a.ptr] if lv then a.type = lv.def.code end end
    end
    return t
  end
end
