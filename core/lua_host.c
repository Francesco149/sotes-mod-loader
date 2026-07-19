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
#include "mem.h"
#include "game_bindings.h"
#include "executor.h"
#include "hooks.h"
#include "ui.h"

#include <string.h>
#include <stdio.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "luajit.h"

static lua_State *g_L;   // shared state; P0 touches it only from the loader thread

// ── the "Lua Big Lock" (LBL) — serialize lua_State access across threads (P5) ──
// One RECURSIVE critical section owned here.  The UI thread (ui.cpp) holds it around
// each frame's draw callbacks; every engine-thread Lua entry point (executor safepoint
// + both hook dispatchers) holds it around its Lua work.  So the shared lua_State is
// only ever touched by one thread at a time even though two threads now run Lua.
// Lazily initialized behind a one-shot flag so it is valid even if a caller reaches it
// before lh_init (e.g. a host test that drives the executor/hooks without lh_init).
static CRITICAL_SECTION g_lua_cs;
static volatile LONG     g_lua_cs_state;   // 0 = uninit, 1 = ready, 2 = initializing
static void lh_lock_ensure(void) {
    if (g_lua_cs_state == 1) return;
    if (InterlockedCompareExchange(&g_lua_cs_state, 2, 0) == 0) {
        InitializeCriticalSection(&g_lua_cs);   // recursive by default on Win32
        InterlockedExchange(&g_lua_cs_state, 1);
    } else {
        while (g_lua_cs_state != 1) Sleep(0);    // another thread is initializing — spin briefly
    }
}
void lh_lock(void)   { lh_lock_ensure(); EnterCriticalSection(&g_lua_cs); }
void lh_unlock(void) { LeaveCriticalSection(&g_lua_cs); }

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

    mem_push_lua(L);                         lua_setfield(L, -2, "mem");   // shared mem service (P1)
    gb_push_lua(L);                          lua_setfield(L, -2, "game");  // shared game bindings
    exec_push_main(L);                       lua_setfield(L, -2, "main");     // run on the main thread (P2)
    exec_push_on_frame(L);                   lua_setfield(L, -2, "on_frame"); // per-frame callback (P2)
    hooks_push_table(L);                     lua_setfield(L, -2, "hook");     // chained hook registry (P3)
    ui_push_table(L);                        lua_setfield(L, -2, "ui");       // shared ImGui UI host (P5)
}

int lh_init(void) {
    lh_lock_ensure();           // stand up the LBL before any thread can touch the state
    g_L = luaL_newstate();
    if (!g_L) { ml_log("[lua] luaL_newstate FAILED"); return 1; }
    luaL_openlibs(g_L);
    // Belt-and-suspenders: the lib is already built with LUAJIT_DISABLE_JIT, but
    // force the engine off at runtime too so no path ever JIT-compiles.
    luaJIT_setmode(g_L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);

    mem_init();                 // host base / PE ImageBase / ASLR delta
    mem_install_lua(g_L);       // the shared mod.mem table
    gb_finalize_lua(g_L);       // the shared mod.game table (bindings registered before this)
    exec_init(g_L);             // the main-thread executor (mod.main / mod.on_frame)
    hooks_init(g_L);            // the chained hook registry (mod.hook.entry / remove)
    ui_init(g_L);               // the ImGui UI host — builds the shared mod.ui table (P5)

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

lua_State *lh_state(void) { return g_L; }

void lh_shutdown(void) {
    ui_shutdown();   // signal the UI thread to stop touching the state before we close it (best-effort)
    if (g_L) { lua_close(g_L); g_L = NULL; ml_log("[lua] state closed"); }
}
