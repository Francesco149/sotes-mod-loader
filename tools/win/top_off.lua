TOP = false
local ffi = require "ffi"
pcall(ffi.cdef, [[ void *FindWindowA(const char *c, const char *t); int SetWindowPos(void *h, void *a, int x, int y, int cx, int cy, unsigned f); ]])
local h = ffi.C.FindWindowA("CLASS_LIZSOFT_SOTES", nil)
ffi.C.SetWindowPos(h, ffi.cast("void *", TOP and -1 or -2), 0, 0, 0, 0, 0x13)   -- HWND_TOPMOST / NOTOPMOST, NOSIZE|NOMOVE|NOACTIVATE
return "ok"
