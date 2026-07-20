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

// ─────────────────────────────────────────────────────────────────────────────
// EARLY-BOOT ABI (OssModEarlyInit) — patch the game DURING ITS OWN BOOT
//
// OssModInit (above) runs at the first-frame safepoint, on the engine thread — the right place for
// hooks / per-frame / roster reads.  But some patches must land BEFORE the game's boot code runs
// (e.g. seeding a global the boot registrar reads, fixing a boot-time table) — the safepoint is far
// too late.  A native mod that also exports:
//   __declspec(dllexport) int OssModEarlyInit(const OssEarlyApi *api);
// gets called ONCE on the LOADER thread in the loader's early-boot phase — after config is loaded,
// before the game boots — with the smaller vtable below.  (A mod may export either entry or both;
// the two are independent.  Early runs first, on the loader thread; Init later, on the engine thread.)
//
// TIMING / WHAT'S SAFE HERE.  The engine does not exist yet: there is NO safepoint, NO engine
// thread, NO hook/executor registry (main_enqueue / on_frame / hook_entry are NOT in this vtable —
// use OssModInit for those).  On the retail SteamStub build the game's .text is still ENCRYPTED at
// this point, so DO NOT read / scan / patch engine code in OssModEarlyInit itself — instead register
// a `when_text_ready` callback and do the patching there, the instant .text is decrypted (the loader
// runs one shared poll thread; on an already-unpacked exe it fires almost immediately).  What IS safe
// now: log, read config, compute addresses (mem_base / mem_reloc), and register when_text_ready.
// Return 0 on success (non-zero => the loader logs it; the mod is not unloaded — early is fire-once).
//
// This is the general form of the loader's built-in Japanese-voice restore (which seeds a bank global
// + fixes a boot table right after .text decrypt).  struct_size / abi_version version it like OssApi.

#define OSS_MOD_EARLY_INIT_NAME "OssModEarlyInit"

typedef void (*OssEarlyReadyFn)(void *user);   // when_text_ready callback (runs on the shared poll thread)

typedef struct OssEarlyApi {
    uint32_t abi_version;   // == OSS_ABI_VERSION at load (a mod may refuse a mismatch)
    uint32_t struct_size;   // == sizeof(OssEarlyApi); fields are append-only

    // ── logging (-> oss_modloader.log) ────────────────────────────────────────
    void (*log)(const char *msg);

    // ── guarded memory (VirtualQuery-checked; never faults) ───────────────────
    // Usable NOW for anything already mapped (PE headers, your own DLL).  NB: on the packed retail
    // build the engine's .text reads as CIPHERTEXT until decrypt — gate real engine access on
    // when_text_ready, don't trust a scan/read of engine code before it fires.
    int  (*mem_readable)(const void *p, size_t n);
    int  (*mem_writable)(const void *p, size_t n);
    int  (*mem_read)(const void *src, void *dst, size_t n);
    int  (*mem_write)(void *dst, const void *src, size_t n);
    uintptr_t (*mem_base)(void);                 // running exe base
    uintptr_t (*mem_reloc)(uintptr_t va);        // ImageBase-relative VA -> live addr (ASLR-safe)
    uintptr_t (*mem_scan)(const char *pattern);  // AOB over the exe image; 0 = miss (see .text caveat)

    // ── patch CODE (or any page), even read-only / executable ─────────────────
    // VirtualProtect RWX -> memcpy -> restore protection -> flush icache.  Use this for .text
    // patches: mem_write REFUSES a read-only code page, this does not.  Returns 1 on success, 0 if
    // the range is unreadable or the protect failed.  (The native form of Lua's mod.mem.patch.)
    int  (*patch)(void *va, const void *bytes, size_t n);

    // ── loader config (oss_loader.cfg) — the same reader the core uses ─────────
    int         (*config_get_int)(const char *key, int def);
    const char *(*config_get_str)(const char *key, const char *def);

    // ── decrypt/boot barrier ──────────────────────────────────────────────────
    // Register fn(user) to fire the instant [probe, probe+n) reads back exactly as `sig` — i.e. once
    // the packed exe's .text is decrypted (SteamStub decrypts .text wholesale at OEP) and the code you
    // mean to patch is present.  The loader runs ONE shared thread that polls ~every 1 ms and calls
    // fn on that thread the moment it matches (times out ~120 s -> skipped, fail-safe; on an already-
    // unpacked exe it fires on the first poll).  Pick `probe`/`sig` = the first bytes of the very
    // function you will patch, so the barrier also VALIDATES the target (a build shift => no match =>
    // no patch, never a corrupt game).  n <= 16.  Returns 1 if registered, 0 if bad args / table full.
    int  (*when_text_ready)(const void *probe, const void *sig, size_t n, OssEarlyReadyFn fn, void *user);
} OssEarlyApi;

// The early entry a native mod exports (resolved by name OSS_MOD_EARLY_INIT_NAME).
typedef int (*OssModEarlyInitFn)(const OssEarlyApi *api);

#endif
