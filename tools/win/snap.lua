local ffi = require "ffi"
pcall(ffi.cdef, [[
typedef struct { long l, t, r, b; } RECT_;
void *FindWindowA(const char *c, const char *t);
long SendMessageA(void *h, unsigned m, unsigned w, RECT_ *l);
int GetWindowRect(void *h, RECT_ *r);
int GetClientRect(void *h, RECT_ *r);
int SetWindowPos(void *h, void *a, int x, int y, int cx, int cy, unsigned f);
]])
local U = ffi.C
local h = U.FindWindowA("CLASS_LIZSOFT_SOTES", nil)
local out = {}
local function try(edge, dw, dh, name)
  local r = ffi.new("RECT_"); U.GetWindowRect(h, r)
  r.r = r.r + dw; r.b = r.b + dh
  U.SendMessageA(h, 0x214, edge, r)                       -- WM_SIZING
  out[#out + 1] = ("%s +%d,+%d -> %dx%d"):format(name, dw, dh, r.r - r.l, r.b - r.t)
  return r
end
try(8, 300, 250, "bottomright")   -- 1x + ~half -> nearest
try(8, 700, 540, "bottomright")
try(6, 0, 400, "bottom")
local r = try(8, 660, 500, "bottomright")
U.SetWindowPos(h, nil, r.l, r.t, r.r - r.l, r.b - r.t, 0x14)  -- apply (NOZORDER|NOACTIVATE)
local c = ffi.new("RECT_"); U.GetClientRect(h, c)
U.SendMessageA(h, 0x232, 0, nil)   -- WM_EXITSIZEMOVE (what a real drag ends with)
U.GetClientRect(h, c)
out[#out + 1] = ("applied client %dx%d"):format(c.r, c.b)
return out
