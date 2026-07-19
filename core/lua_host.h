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

#ifdef __cplusplus
extern "C" {          // ui.cpp (the C++ UI host) locks the shared state through lh_lock/unlock
#endif

// Create the shared Lua state and install the base `mod` table.  Returns 0 on
// success, non-zero on failure (loader logs + skips Lua mods).
int  lh_init(void);

// ── the "Lua Big Lock" (LBL) ─────────────────────────────────────────────────
// LuaJIT is single-threaded: only ONE thread may touch the shared lua_State at a
// time.  Before P5 that was always the engine thread; now the UI thread (ui.cpp)
// also runs Lua (mod.ui draw callbacks + the mem/game reads inside them).  This
// RECURSIVE lock serializes the two: the UI thread holds it around each frame's
// draw callbacks, and EVERY engine-thread Lua entry point (the safepoint drain and
// both hook dispatchers) holds it around its Lua work.  Lazily initialized, so it
// is safe to call before lh_init (host test harnesses that skip it).  Recursive, so
// nesting (e.g. a hook that fires during the safepoint drain) never self-deadlocks.
void lh_lock(void);
void lh_unlock(void);

// Run one Lua mod: load <init_lua_path> into a fresh sandbox env carrying a
// per-mod `mod` table (mod.name, mod.dir, mod.log, ...), then pcall it.  Any
// load/run error is caught + logged; the mod is flagged, the loader continues.
void lh_run_mod(const char *name, const char *dir, const char *init_lua_path);

// The shared Lua state (NULL before lh_init).  A test/introspection seam — the loader
// itself always reaches Lua through the typed helpers above.
struct lua_State *lh_state(void);

// Tear down the Lua state (on process detach / clean unload).
void lh_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
