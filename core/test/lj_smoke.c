// core/test/lj_smoke.c — proves the cross-built LuaJIT (JIT off, FFI on) actually
// RUNS on the i686 Windows target, independent of game injection.  Built as a tiny
// console exe and run via WSLInterop.  Exercises: interpreter, jit.status()==off,
// and a real FFI call (GetTickCount from kernel32) so we know FFI marshaling works.
#include <stdio.h>
#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "luajit.h"

static const char *SCRIPT =
    "io.write('lua: hello from LuaJIT ', jit.version, '\\n')\n"
    "io.write('lua: jit.status = ', tostring(jit.status()), '  (want false)\\n')\n"
    "local ffi = require('ffi')\n"
    "ffi.cdef[[unsigned long __stdcall GetTickCount(void);]]\n"
    "local t = ffi.C.GetTickCount()\n"
    "io.write('lua: ffi GetTickCount() = ', tostring(tonumber(t)), '  (nonzero => FFI ok)\\n')\n"
    "io.write('SMOKE_OK\\n')\n";

int main(void) {
    lua_State *L = luaL_newstate();
    if (!L) { printf("newstate FAILED\n"); return 2; }
    luaL_openlibs(L);
    luaJIT_setmode(L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    if (luaL_dostring(L, SCRIPT) != 0) {
        printf("SCRIPT ERROR: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return 1;
    }
    lua_close(L);
    return 0;
}
