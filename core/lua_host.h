// core/lua_host.h — the LuaJIT runtime that backs the mod.* API.
//
// P0 surface: bring up one LuaJIT state (JIT compiled OUT — interpreter + FFI
// only, the most stable mode inside an injected DLL), and run each Lua mod's
// init.lua inside an isolation boundary (its own environment + a protected call,
// so a mod error is caught + logged, never propagated — loader invariant #7).
// Later phases hang mod.mem / mod.hook / mod.main / mod.ui off the same `mod`
// table (see docs/DESIGN.md).
#ifndef OSS_LUA_HOST_H
#define OSS_LUA_HOST_H

// Create the shared Lua state and install the base `mod` table.  Returns 0 on
// success, non-zero on failure (loader logs + skips Lua mods).
int  lh_init(void);

// Run one Lua mod: load <init_lua_path> into a fresh sandbox env carrying a
// per-mod `mod` table (mod.name, mod.dir, mod.log, ...), then pcall it.  Any
// load/run error is caught + logged; the mod is flagged, the loader continues.
void lh_run_mod(const char *name, const char *dir, const char *init_lua_path);

// The shared Lua state (NULL before lh_init).  A test/introspection seam — the loader
// itself always reaches Lua through the typed helpers above.
struct lua_State *lh_state(void);

// Tear down the Lua state (on process detach / clean unload).
void lh_shutdown(void);

#endif
