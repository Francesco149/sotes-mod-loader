// core/hooks.h — the chained hook registry (P3, Tier-1 entry observers).
//
// The multi-mod keystone: ONE MinHook trampoline per target VA -> a per-VA generated
// thunk that captures registers -> a central dispatcher that walks an ORDERED CHAIN of
// registered observer callbacks -> the original.  Adding/removing a hook edits the chain,
// never re-patching the bytes, so two mods hooking the same VA never fight over the 5 bytes.
//
// Tier-1 (this): register-capture OBSERVERS — a cb receives a context {ecx,edx,eax,esp,ret,va}
// and reads args via mod.mem; it cannot modify args or block (ultra-stable, no signature).
// Callbacks run on the engine thread only (a hook that fires off-thread skips the Lua cb),
// inside a pcall, with a per-VA reentrancy guard.  (Tier-2 typed pre/post/replace via FFI
// closures is the next increment.)
//
// Lua surface (added to every mod's `mod` table):
//   local h = mod.hook.entry(addr, function(ctx) ... end)   -- addr ABSOLUTE (use mod.mem.reloc)
//   mod.hook.remove(h)
//   mod.hook.count()   -- installed hook count (diagnostics)
#ifndef OSS_HOOKS_H
#define OSS_HOOKS_H

struct lua_State;

void hooks_init(struct lua_State *L);         // bind the Lua state (call in lh_init)
void hooks_push_table(struct lua_State *L);    // push the shared `hook` table (for push_mod_table)

#endif
