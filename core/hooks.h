// core/hooks.h — the chained hook registry (P3, Tier-1 entry observers).
//
// The multi-mod keystone: ONE MinHook trampoline per target VA -> a per-VA generated
// thunk that captures registers -> a central dispatcher that walks an ORDERED CHAIN of
// registered observer callbacks -> the original.  Adding/removing a hook edits the chain,
// never re-patching the bytes, so two mods hooking the same VA never fight over the 5 bytes.
//
// Tier-1: register-capture OBSERVERS — a cb receives a context {ecx,edx,eax,esp,ret,va}
// and reads args via mod.mem; it cannot modify args or block (ultra-stable, no signature).
//
// Tier-2: TYPED hooks — the mod declares the C signature and a LuaJIT FFI closure IS the
// detour, so a `pre` cb gets typed args (mutate ctx.args / set ctx.blocked+ctx.ret to
// short-circuit) and a `post` cb can rewrite ctx.ret.  A tiny C main-thread GATE fronts the
// closure so an off-engine-thread call to the target never reenters the shared lua_State
// (it tail-jumps the original instead).  A VA is Tier-1 xor Tier-2 (one MinHook per target);
// within a tier, multiple mods chain safely.  Both tiers run cbs on the engine thread only,
// inside a pcall (a faulting cb is disabled + logged), with a per-VA reentrancy guard.
//
// Lua surface (added to every mod's `mod` table):
//   local h = mod.hook.entry(addr, function(ctx) ... end)          -- Tier-1 observe; addr ABSOLUTE
//   local h = mod.hook.typed(addr, "int __thiscall(void*, int)",   -- Tier-2 typed
//                            { pre = function(ctx) ... end, post = function(ctx) ... end })
//   mod.hook.remove(h)     -- routes to the right tier by handle
//   mod.hook.count()       -- Tier-1 installed VA count;  mod.hook.typed_count() -- Tier-2 VA count
#ifndef OSS_HOOKS_H
#define OSS_HOOKS_H

#include "oss_mod_api.h"   // OssHookEntryFn (native entry observers)
struct lua_State;

void hooks_init(struct lua_State *L);         // bind the Lua state (call in lh_init)
void hooks_push_table(struct lua_State *L);    // push the shared `hook` table (for push_mod_table)

// Native ABI: a C entry observer joins the same Tier-1 chain as Lua mod.hook.entry.
int  hooks_entry_c(uintptr_t va, OssHookEntryFn fn, void *user);   // -> handle (0 = fail)
void hooks_remove(int handle);                                     // remove an entry hook (Lua or native)

#endif
