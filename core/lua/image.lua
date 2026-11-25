-- core/lua/image.lua — mod.image: true-colour images for mods.   [api 1.11]
--
-- The game's sprites are 8-bit sheets with one palette each (mod.game.sprites); art from anywhere else is usually
-- 24 / 32-bit.  This module is the bridge: decode, cut, scale, mirror, and reduce to a palette.
--
--   mod.image.new(w, h)                       -> img          RGBA, all transparent
--   mod.image.decode(bytes)                   -> img | nil, err
--        BMP (8-bit indexed, 24, 32-bit; uncompressed) and TGA (24 / 32-bit, raw or RLE; 8-bit grey).
--   mod.image.quantize(img[, {colors = 255, alpha = 128, dither = false}]) -> indexed
--        median cut → an indexed image as mod.game.sprites uses them ({w, h, pal[0..255] = {r, g, b}, px}):
--        index 0 = transparent (alpha < `alpha`), 1..colors = the palette.  Pass it to sprites.new_bank{image=}.
--   img.w, img.h, img.px (uint8_t[w * h * 4], RGBA rows top-down)
--   img:get(x, y) -> r, g, b, a      img:set(x, y, r, g, b[, a])
--   img:crop(x, y, w, h) -> img      (outside the source = transparent)
--   img:blit(src, sx, sy, w, h, dx, dy[, over])     copy a block; over = true: only where src is opaque (alpha ≥ 128)
--   img:flip_h() -> img              img:scale(w, h) -> img   (area average when shrinking, nearest when growing)
--   img:key(r, g, b[, tol])          make that colour transparent (green-screen sheets); returns img
--   img:bounds([alpha]) -> x0, y0, x1, y1 | nil     the opaque pixels' box
--   img:bmp([key]) -> bytes          a 24-bit BMP (transparent pixels in `key` = {r, g, b}, default magenta)
local log = ...
local ffi = require('ffi')
local floor, min, max = math.floor, math.min, math.max

local I = {}
I.__index = I
local M = {}

local function new(w, h)
  assert(w > 0 and h > 0 and w <= 16384 and h <= 16384, 'image: bad size')
  return setmetatable({ w = w, h = h, px = ffi.new('uint8_t[?]', w * h * 4) }, I)
end
M.new = new

function I:get(x, y)
  if x < 0 or y < 0 or x >= self.w or y >= self.h then return 0, 0, 0, 0 end
  local p = self.px + (y * self.w + x) * 4
  return p[0], p[1], p[2], p[3]
end
function I:set(x, y, r, g, b, a)
  if x < 0 or y < 0 or x >= self.w or y >= self.h then return end
  local p = self.px + (y * self.w + x) * 4
  p[0], p[1], p[2], p[3] = r, g, b, a or 255
end
function I:blit(src, sx, sy, w, h, dx, dy, over)
  -- clip to both images
  if sx < 0 then w, dx, sx = w + sx, dx - sx, 0 end
  if sy < 0 then h, dy, sy = h + sy, dy - sy, 0 end
  if dx < 0 then w, sx, dx = w + dx, sx - dx, 0 end
  if dy < 0 then h, sy, dy = h + dy, sy - dy, 0 end
  w = min(w, src.w - sx, self.w - dx)
  h = min(h, src.h - sy, self.h - dy)
  if w <= 0 or h <= 0 then return self end
  for y = 0, h - 1 do
    local s = src.px + ((sy + y) * src.w + sx) * 4
    local d = self.px + ((dy + y) * self.w + dx) * 4
    if over then
      for x = 0, w - 1 do
        if s[x * 4 + 3] >= 128 then ffi.copy(d + x * 4, s + x * 4, 4) end
      end
    else
      ffi.copy(d, s, w * 4)
    end
  end
  return self
end
function I:crop(x, y, w, h)
  local o = new(w, h)
  o:blit(self, x, y, w, h, 0, 0)
  return o
end
function I:flip_h()
  local o = new(self.w, self.h)
  local w = self.w
  local s, d = ffi.cast('uint32_t *', self.px), ffi.cast('uint32_t *', o.px)
  for y = 0, self.h - 1 do
    local r = y * w
    for x = 0, w - 1 do d[r + w - 1 - x] = s[r + x] end
  end
  return o
end
function I:key(r, g, b, tol)
  tol = tol or 0
  local p = self.px
  for i = 0, self.w * self.h - 1 do
    local q = p + i * 4
    if math.abs(q[0] - r) <= tol and math.abs(q[1] - g) <= tol and math.abs(q[2] - b) <= tol then q[3] = 0 end
  end
  return self
end
function I:bounds(alpha)
  alpha = alpha or 128
  local x0, y0, x1, y1 = self.w, self.h, -1, -1
  local p, w = self.px, self.w
  for y = 0, self.h - 1 do
    local row = p + y * w * 4
    for x = 0, w - 1 do
      if row[x * 4 + 3] >= alpha then
        if x < x0 then x0 = x end
        if x > x1 then x1 = x end
        if y < y0 then y0 = y end
        y1 = y
      end
    end
  end
  if x1 < 0 then return nil end
  return x0, y0, x1, y1
end
-- area average (alpha-weighted, so transparent pixels don't darken the edges) when shrinking; nearest when growing
function I:scale(w, h)
  local o = new(w, h)
  local sw, sh, sp, dp = self.w, self.h, self.px, o.px
  local fx, fy = sw / w, sh / h
  if fx <= 1 and fy <= 1 then
    for y = 0, h - 1 do
      local sy = min(sh - 1, floor((y + 0.5) * fy))
      for x = 0, w - 1 do
        local sx = min(sw - 1, floor((x + 0.5) * fx))
        ffi.copy(dp + (y * w + x) * 4, sp + (sy * sw + sx) * 4, 4)
      end
    end
    return o
  end
  for y = 0, h - 1 do
    local ya, yb = floor(y * fy), max(floor(y * fy) + 1, min(sh, floor((y + 1) * fy + 0.9999)))
    for x = 0, w - 1 do
      local xa, xb = floor(x * fx), max(floor(x * fx) + 1, min(sw, floor((x + 1) * fx + 0.9999)))
      local r, g, b, a, n = 0, 0, 0, 0, 0
      for yy = ya, yb - 1 do
        local row = sp + yy * sw * 4
        for xx = xa, xb - 1 do
          local q = row + xx * 4
          local al = q[3]
          r, g, b, a, n = r + q[0] * al, g + q[1] * al, b + q[2] * al, a + al, n + 1
        end
      end
      local d = dp + (y * w + x) * 4
      if a > 0 then d[0], d[1], d[2] = floor(r / a + 0.5), floor(g / a + 0.5), floor(b / a + 0.5) end
      d[3] = floor(a / n + 0.5)
    end
  end
  return o
end
function I:bmp(key)
  key = key or { 255, 0, 255 }
  local w, h = self.w, self.h
  local stride = floor((w * 3 + 3) / 4) * 4
  local buf = ffi.new('uint8_t[?]', stride * h)
  for y = 0, h - 1 do
    local s, d = self.px + y * w * 4, buf + (h - 1 - y) * stride
    for x = 0, w - 1 do
      local q, t = s + x * 4, d + x * 3
      if q[3] >= 128 then t[0], t[1], t[2] = q[2], q[1], q[0] else t[0], t[1], t[2] = key[3], key[2], key[1] end
    end
  end
  local function le32(v) return string.char(v % 256, floor(v / 256) % 256, floor(v / 65536) % 256, floor(v / 16777216) % 256) end
  local head = 'BM' .. le32(54 + stride * h) .. le32(0) .. le32(54) .. le32(40) .. le32(w) .. le32(h) ..
    string.char(1, 0, 24, 0) .. le32(0) .. le32(stride * h) .. le32(2835) .. le32(2835) .. le32(0) .. le32(0)
  return head .. ffi.string(buf, stride * h)
end

-- ── decoders ─────────────────────────────────────────────────────────────────────────────────────────────────────
local function u16(s, o) local a, b = s:byte(o + 1, o + 2) return a + b * 256 end
local function u32(s, o) local a, b, c, d = s:byte(o + 1, o + 4) return a + b * 256 + c * 65536 + d * 16777216 end
local function decode_bmp(s)
  if #s < 54 then return nil, 'BMP: truncated' end
  local off, hsz, w, hraw = u32(s, 10), u32(s, 14), u32(s, 18), u32(s, 22)
  local h = hraw >= 2147483648 and hraw - 4294967296 or hraw
  local bpp, comp = u16(s, 28), u32(s, 30)
  if comp ~= 0 and not (comp == 3 and bpp == 32) then return nil, 'BMP: compressed (' .. comp .. ')' end
  if bpp ~= 8 and bpp ~= 24 and bpp ~= 32 then return nil, 'BMP: ' .. bpp .. '-bit' end
  local ah = math.abs(h)
  local stride = floor((w * bpp / 8 + 3) / 4) * 4
  if #s < off + stride * ah then return nil, 'BMP: truncated' end
  local img = new(w, ah)
  local src = ffi.cast('const uint8_t *', s) + off
  local pal = ffi.cast('const uint8_t *', s) + 14 + hsz
  for y = 0, ah - 1 do
    local r = src + (h > 0 and (ah - 1 - y) or y) * stride
    local d = img.px + y * w * 4
    if bpp == 24 then
      for x = 0, w - 1 do local q, t = r + x * 3, d + x * 4 t[0], t[1], t[2], t[3] = q[2], q[1], q[0], 255 end
    elseif bpp == 32 then
      for x = 0, w - 1 do local q, t = r + x * 4, d + x * 4 t[0], t[1], t[2], t[3] = q[2], q[1], q[0], 255 end
    else
      for x = 0, w - 1 do local q, t = pal + r[x] * 4, d + x * 4 t[0], t[1], t[2], t[3] = q[2], q[1], q[0], 255 end
    end
  end
  return img
end
local function decode_tga(s)
  if #s < 18 then return nil, 'TGA: truncated' end
  local idlen, cmap, kind = s:byte(1), s:byte(2), s:byte(3)
  local w, h, bpp, desc = u16(s, 12), u16(s, 14), s:byte(17), s:byte(18)
  if cmap ~= 0 or not (kind == 2 or kind == 10 or kind == 3 or kind == 11) then return nil, 'TGA: type ' .. kind end
  local bytes = bpp / 8
  if not (bytes == 3 or bytes == 4 or bytes == 1) or w == 0 or h == 0 then return nil, 'TGA: ' .. bpp .. '-bit' end
  local img = new(w, h)
  local src = ffi.cast('const uint8_t *', s)
  local pos, n = 18 + idlen, #s
  local top = bit.band(desc, 0x20) ~= 0
  local total, i = w * h, 0
  local rle = kind >= 10
  local function put(q)
    local y = floor(i / w)
    local d = img.px + ((top and y or (h - 1 - y)) * w + i % w) * 4
    if bytes == 1 then d[0], d[1], d[2], d[3] = q[0], q[0], q[0], 255
    else d[0], d[1], d[2], d[3] = q[2], q[1], q[0], bytes == 4 and q[3] or 255 end
    i = i + 1
  end
  if not rle then
    if n < pos + total * bytes then return nil, 'TGA: truncated' end
    if bytes == 4 and top then                              -- the common case, fast: swap B and R a row at a time
      local d = img.px
      for k = 0, total - 1 do
        local q, t = src + pos + k * 4, d + k * 4
        t[0], t[1], t[2], t[3] = q[2], q[1], q[0], q[3]
      end
    else
      for k = 0, total - 1 do put(src + pos + k * bytes) end
    end
  else
    while i < total do
      if pos >= n then return nil, 'TGA: truncated' end
      local c = src[pos]; pos = pos + 1
      local cnt = bit.band(c, 0x7f) + 1
      if c >= 128 then
        for _ = 1, cnt do if i < total then put(src + pos) end end
        pos = pos + bytes
      else
        for _ = 1, cnt do if i < total then put(src + pos) end pos = pos + bytes end
      end
    end
  end
  return img
end
function M.decode(s)
  if type(s) ~= 'string' then return nil, 'image.decode: bytes expected' end
  if s:sub(1, 2) == 'BM' then return decode_bmp(s) end
  return decode_tga(s)
end

-- ── palette reduction: median cut over a 5-6-5 histogram ─────────────────────────────────────────────────────────
function M.quantize(img, o)
  o = o or {}
  local ncol, athr = min(255, o.colors or 255), o.alpha or 128
  local w, h, p = img.w, img.h, img.px
  local hist = ffi.new('uint32_t[65536]')
  for i = 0, w * h - 1 do
    local q = p + i * 4
    if q[3] >= athr then
      local k = bit.bor(bit.lshift(bit.rshift(q[0], 3), 11), bit.lshift(bit.rshift(q[1], 2), 5), bit.rshift(q[2], 3))
      hist[k] = hist[k] + 1
    end
  end
  local bins = {}
  for k = 0, 65535 do
    local c = hist[k]
    if c > 0 then
      local r, g, b = bit.rshift(k, 11), bit.band(bit.rshift(k, 5), 63), bit.band(k, 31)
      bins[#bins + 1] = { r * 8 + 4, g * 4 + 2, b * 8 + 4, c, k }
    end
  end
  local boxes = { bins }
  local function extent(box)
    local lo, hi = { 255, 255, 255 }, { 0, 0, 0 }
    for _, e in ipairs(box) do
      for ch = 1, 3 do
        if e[ch] < lo[ch] then lo[ch] = e[ch] end
        if e[ch] > hi[ch] then hi[ch] = e[ch] end
      end
    end
    local ch, span = 1, -1
    for c = 1, 3 do
      local s = (hi[c] - lo[c]) * (c == 2 and 1.2 or 1)     -- green weighs a little more (the eye does)
      if s > span then ch, span = c, s end
    end
    return ch, span
  end
  while #boxes < ncol do
    local bi, bspan, bch = nil, 0, 1
    for i, box in ipairs(boxes) do
      if #box > 1 then
        local ch, span = extent(box)
        local score = span * math.sqrt(#box)
        if score > bspan then bi, bspan, bch = i, score, ch end
      end
    end
    if not bi then break end
    local box = boxes[bi]
    table.sort(box, function(a, b) return a[bch] < b[bch] end)
    local total = 0
    for _, e in ipairs(box) do total = total + e[4] end
    local acc, cut = 0, 1
    for i, e in ipairs(box) do
      acc = acc + e[4]
      if acc * 2 >= total then cut = i break end
    end
    cut = max(1, min(#box - 1, cut))
    local a, b = {}, {}
    for i = 1, cut do a[i] = box[i] end
    for i = cut + 1, #box do b[#b + 1] = box[i] end
    boxes[bi] = a
    boxes[#boxes + 1] = b
  end
  local out = { w = w, h = h, pal = {}, px = ffi.new('uint8_t[?]', w * h) }
  for i = 0, 255 do out.pal[i] = { 0, 0, 0 } end
  local map = ffi.new('uint8_t[65536]')
  for i, box in ipairs(boxes) do
    local r, g, b, n = 0, 0, 0, 0
    for _, e in ipairs(box) do
      r, g, b, n = r + e[1] * e[4], g + e[2] * e[4], b + e[3] * e[4], n + e[4]
      map[e[5]] = i
    end
    if n > 0 then out.pal[i] = { min(255, floor(r / n + 0.5)), min(255, floor(g / n + 0.5)), min(255, floor(b / n + 0.5)) } end
  end
  local d = out.px
  for i = 0, w * h - 1 do
    local q = p + i * 4
    if q[3] >= athr then
      d[i] = map[bit.bor(bit.lshift(bit.rshift(q[0], 3), 11), bit.lshift(bit.rshift(q[1], 2), 5), bit.rshift(q[2], 3))]
    end
  end
  out.colors = #boxes
  -- the indexed image carries the sprite module's methods when that module is up (get / set / blit / clone)
  if M._indexed_meta then setmetatable(out, M._indexed_meta) end
  return out
end

-- indexed image <-> bytes, for a mod's cache: "OSI1" w h ncolors, the palette (3 bytes each from index 0), the pixels
function M.pack_indexed(ix)
  local function le32(v) return string.char(v % 256, floor(v / 256) % 256, floor(v / 65536) % 256, floor(v / 16777216) % 256) end
  local pal = {}
  for i = 0, 255 do local c = ix.pal[i] or { 0, 0, 0 } pal[#pal + 1] = string.char(c[1], c[2], c[3]) end
  return 'OSI1' .. le32(ix.w) .. le32(ix.h) .. table.concat(pal) .. ffi.string(ix.px, ix.w * ix.h)
end
function M.unpack_indexed(s)
  if type(s) ~= 'string' or s:sub(1, 4) ~= 'OSI1' then return nil, 'not an indexed image' end
  local w, h = u32(s, 4), u32(s, 8)
  if #s < 12 + 768 + w * h then return nil, 'truncated' end
  local out = { w = w, h = h, pal = {}, px = ffi.new('uint8_t[?]', w * h) }
  for i = 0, 255 do local r, g, b = s:byte(13 + i * 3, 15 + i * 3) out.pal[i] = { r, g, b } end
  ffi.copy(out.px, ffi.cast('const uint8_t *', s) + 12 + 768, w * h)
  if M._indexed_meta then setmetatable(out, M._indexed_meta) end
  return out
end

return { image = M }
