// core/game_bindings.h — the togglable game-knowledge registry (mod.game.*).
//
// As we RE the engine, the known structs + pointer chains (roster, coordinates,
// camera, map, scene, ...) are centralized HERE in the loader core and exposed to
// every mod as `mod.game.<id>`, so no mod re-RE's them.  Each binding is an
// independently ENABLE/DISABLE-able module (a stability valve: if a binding proves
// unstable it's turned off without touching the rest), live-toggled — a disabled
// binding hides its `mod.game.<id>` table AND its accessors go inert even through a
// captured reference.
//
// The registry is game-agnostic; the bindings themselves are game-specific and
// register from a profile TU (e.g. sotes_bindings.c) gated on the host exe.  Reads
// inside a binding always go through the guarded mem service (mem.h), so an accessor
// returns nil on bad data rather than faulting.
#ifndef OSS_GAME_BINDINGS_H
#define OSS_GAME_BINDINGS_H

struct lua_State;

// A binding's install fn pushes its API table (a table of C closures) onto L.  Called
// lazily the first time an ENABLED binding is accessed; the result is cached.
typedef void (*gb_install_fn)(struct lua_State *L);

typedef struct {
    const char   *id;              // mod.game.<id>
    const char   *desc;            // human description (for the toggle UI / list())
    gb_install_fn install;         // builds + pushes the module's Lua API table
    int           default_enabled; // initial enabled state
} gb_def;

// Register a binding (call from a profile TU's init, before gb_finalize_lua).
void gb_register(const gb_def *def);

// Build the shared `mod.game` table (control fns + a dynamic-resolve metatable) and
// keep it alive; then gb_push_lua pushes it for each mod's `mod.game`.
void gb_finalize_lua(struct lua_State *L);
void gb_push_lua(struct lua_State *L);

// Toggle / query (usable from C accessors for the inert-when-disabled self-check).
int  gb_set_enabled(const char *id, int on);   // 1 if the id exists
int  gb_enabled(const char *id);               // 1 = on, 0 = off/unknown
int  gb_count(void);

#endif
