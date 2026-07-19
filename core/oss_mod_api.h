// core/oss_mod_api.h — the STABLE C ABI a native mod (mods\<name>.dll) links against.
//
// A native mod is a plain DLL that exports:
//   __declspec(dllexport) int OssModInit(const OssApi *api);
// The loader calls it ONCE on the engine (main) thread at the first safepoint — after the
// DLL's DllMain, exactly like a Lua mod's init.lua — handing it this vtable of loader
// services.  The mod registers hooks / per-frame / one-shot callbacks and reads/writes
// memory through `api`; native and Lua mods share the SAME hook + executor registries, so
// they interleave safely.  Return 0 on success (non-zero => the loader logs the failure).
//
// STABILITY: `abi_version` + `struct_size` make this forward-compatible — new fields are
// only ever APPENDED.  A mod built against an older header sees a struct at least as large
// as it expects; a mod using a newer field guards on `struct_size`.  This one header is the
// whole contract (copy it beside a native mod's source; no other loader header is needed).
#ifndef OSS_MOD_API_H
#define OSS_MOD_API_H

#include <stdint.h>
#include <stddef.h>

#define OSS_ABI_VERSION   1
#define OSS_MOD_INIT_NAME "OssModInit"

// Registers captured at a Tier-1 hook target's ENTRY (see hook_entry).  Read stack args via
// mem_read of (esp + 4 + 4*i); ecx is the __thiscall `this`.  Observe only — a Tier-1 hook
// cannot modify args or block (that is a Lua Tier-2 mod.hook.typed; native typed is a later
// ABI addition).
typedef struct OssHookCtx {
    uint32_t ecx, edx, eax;   // GP registers at entry
    uint32_t esp;             // caller's stack pointer (return addr at [esp], args above)
    uint32_t ret;             // return address (= *(uint32_t*)esp)
    uint32_t va;              // the hooked target VA
} OssHookCtx;

typedef void (*OssJobFn)(void *user);                              // main_enqueue / on_frame
typedef void (*OssHookEntryFn)(const OssHookCtx *ctx, void *user); // hook_entry observer

typedef struct OssApi {
    uint32_t abi_version;   // == OSS_ABI_VERSION at load (a mod may refuse a mismatch)
    uint32_t struct_size;   // == sizeof(OssApi); fields are append-only — check before a newer field

    // ── logging (-> oss_modloader.log) ────────────────────────────────────────
    void (*log)(const char *msg);

    // ── guarded memory (VirtualQuery-checked; never faults) ───────────────────
    int  (*mem_readable)(const void *p, size_t n);            // 1 iff [p,p+n) readable
    int  (*mem_writable)(const void *p, size_t n);            // 1 iff [p,p+n) writable
    int  (*mem_read)(const void *src, void *dst, size_t n);   // guarded copy in;  0 = bad addr
    int  (*mem_write)(void *dst, const void *src, size_t n);  // guarded copy out; 0 = bad addr
    uintptr_t (*mem_base)(void);                              // running exe base
    uintptr_t (*mem_reloc)(uintptr_t va);                     // ImageBase-relative VA -> live addr
    uintptr_t (*mem_scan)(const char *pattern);               // AOB over the exe image; 0 = miss

    // ── main-thread execution (engine calls inside are safe by construction) ───
    void (*main_enqueue)(OssJobFn fn, void *user);            // run fn(user) once at the next safepoint
    void (*on_frame)(OssJobFn fn, void *user);                // run fn(user) every frame

    // ── Tier-1 hooks (entry observers; multi-mod chain; shared with Lua mods) ──
    int  (*hook_entry)(uintptr_t va, OssHookEntryFn fn, void *user);  // -> handle (0 = fail)
    void (*hook_remove)(int handle);
} OssApi;

// The entry a native mod exports (resolved by name OSS_MOD_INIT_NAME).
typedef int (*OssModInitFn)(const OssApi *api);

#endif
