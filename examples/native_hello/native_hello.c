// examples/native_hello/native_hello.c — a NATIVE mod (compiles to native_hello.dll).
//
// The native counterpart of examples/hook_typed: it uses the OssModInit C ABI (oss_mod_api.h)
// to log, read memory, register a Tier-1 hook on the engine keyboard poll, and run per-frame —
// interleaving in the SAME registries as Lua mods.  Observe-only, so the game is unaffected.
//
//   Build:   nix develop --command make -C examples/native_hello   (-> native_hello.dll)
//   Install: drop native_hello.dll into the game's mods\ folder, next to version.dll.
//
// (An out-of-tree mod copies oss_mod_api.h beside its source and drops the -I../../core.)

#include "oss_mod_api.h"
#include <windows.h>
#include <stdio.h>

static const OssApi *g_api;
static uint32_t g_expect_this;
static int      g_hits, g_frames, g_hook, g_shown, g_removed;

// Tier-1 observer on kb_poll (thiscall; ecx = the keyboard device *(0x92d5bc)).
static void kb_hook(const OssHookCtx *ctx, void *user) {
    (void)user;
    g_hits++;
    if (!g_shown) {
        g_shown = 1;
        char b[160];
        _snprintf(b, sizeof b, "native_hello: first kb_poll hit  ecx=0x%08x  this_match=%d",
                  ctx->ecx, ctx->ecx == g_expect_this);
        g_api->log(b);
    }
}

static void per_frame(void *user) {
    (void)user;
    if (++g_frames % 120 != 0) return;
    char b[128];
    _snprintf(b, sizeof b, "native_hello: [frame %d] kb_poll hits=%d", g_frames, g_hits);
    g_api->log(b);
    if (!g_removed && g_frames >= 600) {
        g_api->hook_remove(g_hook); g_removed = 1;
        g_api->log("native_hello: removed the hook -> hits should stop climbing");
    }
}

// The entry the loader resolves + calls ONCE on the engine thread at the first safepoint.
__declspec(dllexport) int OssModInit(const OssApi *api) {
    g_api = api;
    if (api->abi_version != OSS_ABI_VERSION) { api->log("native_hello: ABI version mismatch — not loading"); return 1; }
    api->log("native_hello: init (a native C mod on the OssModInit ABI)");

    uintptr_t kb_poll   = api->mem_reloc(0x5e2a10);
    uintptr_t kb_global = api->mem_reloc(0x92d5bc);
    api->mem_read((const void *)kb_global, &g_expect_this, 4);   // -> the keyboard device (expected `this`)

    g_hook = api->hook_entry(kb_poll, kb_hook, NULL);
    api->on_frame(per_frame, NULL);

    char b[96];
    _snprintf(b, sizeof b, "native_hello: armed (kb_poll @ 0x%08x, handle=%d)", (unsigned)kb_poll, g_hook);
    api->log(b);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)h; (void)reason; (void)r; return TRUE; }
