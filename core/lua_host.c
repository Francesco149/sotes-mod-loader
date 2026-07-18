// core/lua_host.c — LuaJIT runtime backing the mod.* API (P0 surface).
//
// One shared lua_State, JIT forced off (interpreter + FFI only — the stable mode
// for an injected DLL: no JIT code pages in the game's address space; FFI stays on
// for later native calls / struct cdefs).  Each Lua mod runs inside its own
// environment table + a protected call, so a faulting mod is caught + logged and
// never takes down the game or the other mods (loader invariant #7).
//
// P0 `mod` table:  mod.name  mod.dir  mod.log(...)  mod.loader  mod.loader_version
// Later phases add mod.mem / mod.hook / mod.main / mod.on_frame / mod.ui onto the
// same table (docs/DESIGN.md).  P0 runs init.lua on the loader thread; the
// main-thread executor (P2) will marshal engine-touching work onto the game thread.

#include "loader_internal.h"
#include "lua_host.h"

#include <string.h>
#include <stdio.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "luajit.h"

static lua_State *g_L;   // shared state; P0 touches it only from the loader thread

// mod.log(...) — like print(): tostring() each arg, space-join, route to the loader
// log with the mod's name (carried as an upvalue so every mod's log is attributed).
static int l_mod_log(lua_State *L) {
    const char *name = lua_tostring(L, lua_upvalueindex(1));
    int n = lua_gettop(L);
    char buf[1024];
    size_t off = 0;
    for (int i = 1; i <= n; i++) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char *s = lua_tostring(L, -1);
        if (!s) s = "?";
        off += (size_t)_snprintf(buf + off, sizeof buf - off, "%s%s", (i > 1 ? " " : ""), s);
        lua_pop(L, 1);
        if (off >= sizeof buf - 1) break;
    }
    buf[sizeof buf - 1] = 0;
    ml_log("[mod] %s: %s", name ? name : "?", buf);
    return 0;
}

// Build + push a fresh per-mod `mod` table (its own name/dir/log closure).
static void push_mod_table(lua_State *L, const char *name, const char *dir) {
    lua_newtable(L);

    lua_pushstring(L, name);                 lua_setfield(L, -2, "name");
    lua_pushstring(L, dir);                  lua_setfield(L, -2, "dir");
    lua_pushstring(L, OSS_ML_NAME);          lua_setfield(L, -2, "loader");
    lua_pushstring(L, OSS_ML_VERSION);       lua_setfield(L, -2, "loader_version");

    lua_pushstring(L, name);                 // upvalue for the log closure
    lua_pushcclosure(L, l_mod_log, 1);       lua_setfield(L, -2, "log");
}

int lh_init(void) {
    g_L = luaL_newstate();
    if (!g_L) { ml_log("[lua] luaL_newstate FAILED"); return 1; }
    luaL_openlibs(g_L);
    // Belt-and-suspenders: the lib is already built with LUAJIT_DISABLE_JIT, but
    // force the engine off at runtime too so no path ever JIT-compiles.
    luaJIT_setmode(g_L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);
    ml_log("[lua] LuaJIT up (%s, JIT off, FFI on)", LUAJIT_VERSION);
    return 0;
}

void lh_run_mod(const char *name, const char *dir, const char *init_lua_path) {
    if (!g_L) { ml_log("[lua] no state — skip mod %s", name); return; }

    // Load the chunk; a syntax error is caught here (not propagated).
    if (luaL_loadfile(g_L, init_lua_path) != 0) {
        ml_log("[lua] LOAD FAILED %s: %s", name, lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return;
    }

    // Fresh sandbox environment: the mod's writes land here (mods don't clobber
    // each other's globals); reads fall through to _G via the metatable, so the
    // Lua stdlib (string/table/math/print/...) is available.  Lua 5.1 setfenv.
    lua_newtable(g_L);                                    // env
    push_mod_table(g_L, name, dir);                      // env, mod
    lua_setfield(g_L, -2, "mod");                        // env.mod = <table>
    lua_newtable(g_L);                                   // env, meta
    lua_getglobal(g_L, "_G");                            // env, meta, _G
    lua_setfield(g_L, -2, "__index");                    // meta.__index = _G
    lua_setmetatable(g_L, -2);                           // setmetatable(env, meta)
    lua_setfenv(g_L, -2);                                // setfenv(chunk, env)

    // Protected call — a runtime error is caught, the mod flagged, loader lives.
    if (lua_pcall(g_L, 0, 0, 0) != 0) {
        ml_log("[lua] RUNTIME ERROR %s: %s", name, lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return;
    }
    ml_log("[lua] mod ok: %s", name);
}

void lh_shutdown(void) {
    if (g_L) { lua_close(g_L); g_L = NULL; ml_log("[lua] state closed"); }
}
