// examples/early_probe/early_probe.c — a NATIVE mod that uses the EARLY-BOOT ABI (OssModEarlyInit).
//
// The counterpart of examples/native_hello (which uses OssModInit at the safepoint): this one runs
// in the loader's EARLY-BOOT phase — on the loader thread, before the game's own boot code — the
// slot for patches that must land before the engine initializes (the built-in Japanese voice restore
// is the real consumer; see core/voice.c).  This example is OBSERVE-ONLY: it proves the whole early
// path without altering the game.
//
//   Build:   nix develop --command make -C examples/early_probe   (-> early_probe.dll)
//   Install: drop early_probe.dll into the game's mods\ folder, next to version.dll.
//
// What it does, and what each step proves:
//   OssModEarlyInit (loader thread, pre-boot): logs, resolves the SotES per-frame safepoint
//     (0x437c70, ImageBase-relative -> mem_reloc), and registers a `when_text_ready` callback keyed
//     on that function's real prologue.  It does NOT touch engine code yet — on the packed retail
//     build .text is still encrypted here (this proves you set up early + defer the real work).
//   when_text_ready callback (shared poll thread, the instant .text is decrypted): reads the prologue
//     back to confirm the target is present (proves the barrier fired at the right time + guarded
//     mem), then does a NO-OP self-patch of the first byte (writes it back unchanged) to exercise the
//     `patch` RWX path on a real .text page WITHOUT changing behavior (proves code-patching works).
//
// The 8-byte prologue signature IS the safety gate: on a wrong edition / build the bytes won't match,
// so the callback simply never fires — no patch, never a corrupt game (same discipline as voice.c).

#include "oss_mod_api.h"
#include <windows.h>
#include <stdio.h>

// SotES EN-SE per-frame safepoint (0x437c70) prologue: push ebx; mov ebx,[esp+0xc]; push ebp; mov ebp,...
#define EARLY_SAFEPOINT_VA  0x437c70u
static const unsigned char SAFEPOINT_SIG[8] = { 0x53, 0x8B, 0x5C, 0x24, 0x0C, 0x55, 0x8B, 0x6C };

static const OssEarlyApi *g_api;
static void              *g_target;   // live safepoint address (resolved in OssModEarlyInit)

// Runs on the loader's shared decrypt/boot poll thread, the moment [g_target] == SAFEPOINT_SIG.
static void on_text_ready(void *user) {
    (void)user;
    unsigned char now[8] = { 0 };
    int ok = g_api->mem_read(g_target, now, sizeof now);
    char b[192];
    _snprintf(b, sizeof b,
              "early_probe: .text ready — safepoint @%p prologue = %02x %02x %02x %02x %02x %02x %02x %02x (read=%d)",
              g_target, now[0], now[1], now[2], now[3], now[4], now[5], now[6], now[7], ok);
    g_api->log(b);

    // Exercise the code-patch RWX path on a REAL .text page, harmlessly: write byte[0] back unchanged.
    int patched = g_api->patch(g_target, now, 1);
    _snprintf(b, sizeof b, "early_probe: no-op self-patch of the prologue byte -> %s (RWX code-patch path OK)",
              patched ? "ok" : "FAILED");
    g_api->log(b);
}

// The early entry the loader resolves + calls ONCE on the loader thread in the early-boot phase.
__declspec(dllexport) int OssModEarlyInit(const OssEarlyApi *api) {
    g_api = api;
    if (api->abi_version != OSS_ABI_VERSION) { api->log("early_probe: ABI version mismatch — not loading"); return 1; }
    api->log("early_probe: OssModEarlyInit (early-boot phase, loader thread — before the game boots)");

    g_target = (void *)api->mem_reloc(EARLY_SAFEPOINT_VA);
    int reg = api->when_text_ready(g_target, SAFEPOINT_SIG, sizeof SAFEPOINT_SIG, on_text_ready, NULL);

    char b[128];
    _snprintf(b, sizeof b, "early_probe: registered when_text_ready on the safepoint @%p -> %d", g_target, reg);
    api->log(b);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) { (void)h; (void)reason; (void)r; return TRUE; }
