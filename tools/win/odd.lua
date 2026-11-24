local ffi = require "ffi"
pcall(ffi.cdef, [[ typedef struct { long l, t, r, b; } RECT_; void *FindWindowA(const char *c, const char *t);
int GetWindowRect(void *h, RECT_ *r); int SetWindowPos(void *h, void *a, int x, int y, int cx, int cy, unsigned f); ]])
local h = ffi.C.FindWindowA("CLASS_LIZSOFT_SOTES", nil)
local r = ffi.new("RECT_"); ffi.C.GetWindowRect(h, r)
ffi.C.SetWindowPos(h, nil, r.l, r.t, 1100, 760, 0x14)   -- not a multiple: expect 1x image centred (integer scaling)
return "ok"
