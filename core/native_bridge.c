// core/native_bridge.c — the native-mod C ABI bridge (P4).  See native_bridge.h.
//
// Wires the OssApi function pointers to the loader's internal services.  The two wrappers
// below adapt the guarded mem primitives into the ABI's copy-in / copy-out shape; everything
// else is a direct pointer to an existing internal function (same signature).  Native mod
// callbacks (on_frame / hook observers) run on the engine thread via those registries; they
// are NOT yet wrapped in an SEH boundary, so a faulting native mod can still fault the game
// (native is the powerful/you-are-responsible tier — Lua mods get pcall isolation).  Adding
// SEH is a later refinement; the ABI itself doesn't change.

#include "native_bridge.h"
#include "mem.h"
#include "executor.h"
#include "hooks.h"
#include "config.h"
#include "loader_internal.h"

#include <windows.h>
#include <string.h>

static void api_log(const char *msg) { if (msg) ml_log("[nmod] %s", msg); }

static int api_mem_read(const void *src, void *dst, size_t n) {
    if (!dst || !mem_readable(src, n)) return 0;
    memcpy(dst, src, n);
    return 1;
}
static int api_mem_write(void *dst, const void *src, size_t n) {
    if (!src || !mem_writable(dst, n)) return 0;
    memcpy(dst, src, n);
    return 1;
}

static const OssApi g_api = {
    .abi_version  = OSS_ABI_VERSION,
    .struct_size  = sizeof(OssApi),
    .log          = api_log,
    .mem_readable = mem_readable,
    .mem_writable = mem_writable,
    .mem_read     = api_mem_read,
    .mem_write    = api_mem_write,
    .mem_base     = mem_main_base,
    .mem_reloc    = mem_reloc,
    .mem_scan     = mem_scan_aob,
    .main_enqueue = exec_enqueue_c,
    .on_frame     = exec_on_frame_c,
    .hook_entry   = hooks_entry_c,
    .hook_remove  = hooks_remove,
};

const OssApi *oss_api(void) { return &g_api; }

// ─────────────────────────────────────────────────────────────────────────────
// EARLY-BOOT ABI (OssModEarlyInit) — see oss_mod_api.h + native_bridge.h.
//
// A smaller vtable for the loader's early-boot phase (loader thread, before the game boots): log,
// guarded mem, a code-patch primitive, config, and a decrypt/boot barrier (when_text_ready).  The
// barrier is the only stateful part: registrations are appended on the LOADER thread during the
// early pass; the shared poll thread is started (oss_early_waiter_start) only AFTER that pass, so
// registration and polling never overlap — no lock needed (the `done` flag is the waiter's alone).

// patch CODE (or any page) even when read-only/executable — the native form of Lua's mod.mem.patch
// (mem.c l_patch).  mem_write refuses a read-only code page; this flips protection to do it.
static int api_early_patch(void *va, const void *bytes, size_t n) {
    if (!va || !bytes || n == 0 || !mem_readable(va, n)) return 0;
    DWORD old;
    if (!VirtualProtect(va, n, PAGE_EXECUTE_READWRITE, &old)) return 0;
    memcpy(va, bytes, n);
    VirtualProtect(va, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), va, n);
    return 1;
}

#define OSS_EARLY_MAX_WAIT   16
#define OSS_EARLY_SIG_MAX    16
#define OSS_EARLY_POLL_MS    1
#define OSS_EARLY_TIMEOUT_MS 120000   // ~120 s then give up (never blocks the game: own thread)

typedef struct {
    const void     *probe;
    unsigned char   sig[OSS_EARLY_SIG_MAX];
    size_t          n;
    OssEarlyReadyFn fn;
    void           *user;
    int             done;
} early_wait;

static early_wait g_ew[OSS_EARLY_MAX_WAIT];
static int        g_ew_n;
static HANDLE     g_ew_thread;

static DWORD WINAPI early_waiter(void *unused) {
    (void)unused;
    int ms = 0;
    for (;;) {
        int pending = 0;
        for (int i = 0; i < g_ew_n; i++) {
            if (g_ew[i].done) continue;
            if (mem_readable(g_ew[i].probe, g_ew[i].n) &&
                memcmp(g_ew[i].probe, g_ew[i].sig, g_ew[i].n) == 0) {
                ml_log("[early] .text ready (probe %p matched) — firing early callback", g_ew[i].probe);
                g_ew[i].fn(g_ew[i].user);   // applies the patch, here on the waiter thread
                g_ew[i].done = 1;
            } else pending = 1;
        }
        if (!pending) return 0;
        if ((ms += OSS_EARLY_POLL_MS) > OSS_EARLY_TIMEOUT_MS) {
            for (int i = 0; i < g_ew_n; i++)
                if (!g_ew[i].done)
                    ml_log("[early] TIMEOUT: probe %p never matched — callback skipped (fail safe)", g_ew[i].probe);
            return 0;
        }
        Sleep(OSS_EARLY_POLL_MS);
    }
}

static int api_when_text_ready(const void *probe, const void *sig, size_t n,
                               OssEarlyReadyFn fn, void *user) {
    if (!probe || !sig || !fn || n == 0 || n > OSS_EARLY_SIG_MAX) return 0;
    if (g_ew_n >= OSS_EARLY_MAX_WAIT) { ml_log("[early] when_text_ready: table full — dropping"); return 0; }
    early_wait *w = &g_ew[g_ew_n];
    w->probe = probe; w->n = n; w->fn = fn; w->user = user; w->done = 0;
    memcpy(w->sig, sig, n);
    g_ew_n++;
    return 1;
}

static const OssEarlyApi g_early_api = {
    .abi_version     = OSS_ABI_VERSION,
    .struct_size     = sizeof(OssEarlyApi),
    .log             = api_log,
    .mem_readable    = mem_readable,
    .mem_writable    = mem_writable,
    .mem_read        = api_mem_read,
    .mem_write       = api_mem_write,
    .mem_base        = mem_main_base,
    .mem_reloc       = mem_reloc,
    .mem_scan        = mem_scan_aob,
    .patch           = api_early_patch,
    .config_get_int  = config_get_int,
    .config_get_str  = config_get_str,
    .when_text_ready = api_when_text_ready,
};

const OssEarlyApi *oss_early_api(void) { return &g_early_api; }

void oss_early_waiter_start(void) {
    if (g_ew_n > 0 && !g_ew_thread) {
        g_ew_thread = CreateThread(NULL, 0, early_waiter, NULL, 0, NULL);
        ml_log("[early] decrypt/boot waiter started — %d callback(s) pending", g_ew_n);
    }
}
