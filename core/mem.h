// core/mem.h — the guarded memory service (P1).
//
// The low-level, never-fault memory primitives every mod + every game binding builds
// on: VirtualQuery-guarded reads/writes so a bad pointer returns "no" instead of
// crashing the game (loader invariant #4).  The pure guards (mem_readable/writable +
// rd32) carry no Lua dependency so the native game bindings (sotes_bindings.c) can use
// them directly; mem_install_lua() hangs the typed `mod.mem` table off the Lua side.
#ifndef OSS_MEM_H
#define OSS_MEM_H

#include <windows.h>
#include <stdint.h>
#include <stddef.h>

struct lua_State;

// ── never-fault guards (VirtualQuery) ────────────────────────────────────────
int mem_readable(const void *p, size_t n);   // 1 iff [p, p+n) is committed + readable
int mem_writable(const void *p, size_t n);   // 1 iff [p, p+n) is committed + writable
int mem_rd32(const void *p, uint32_t *out);   // guarded 4-byte read (0 = unreadable)

// ── module base / ASLR reloc ─────────────────────────────────────────────────
void      mem_init(void);            // compute the host module's base + PE ImageBase + delta
uintptr_t mem_main_base(void);       // GetModuleHandle(NULL) (the running exe base)
uintptr_t mem_reloc(uintptr_t va);   // an ImageBase-relative VA -> live address (va + ASLR delta)

// ── Lua surface ──────────────────────────────────────────────────────────────
void mem_install_lua(struct lua_State *L);   // build the shared `mem` table; store a registry ref
void mem_push_lua(struct lua_State *L);       // push that shared table (for per-mod mod.mem)

#endif
