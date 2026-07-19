// core/test/native_mod_test.c — drives the P4 native-mod C ABI WITHOUT the game.
//
// Acts as a native mod: a local OssModInit uses the OssApi vtable (as a real mods\<name>.dll
// would) to read/write/guard/scan memory, register a Tier-1 hook_entry observer on a local
// target, an on_frame callback, and a main_enqueue job.  The harness drives it exactly as the
// loader does — exec_defer_native(OssModInit) then exec_on_safepoint() — and verifies each
// service fired.  Links every core TU except loader.o and supplies ml_log.  Run via WSLInterop.

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <windows.h>

#include "oss_mod_api.h"
#include "native_bridge.h"
#include "executor.h"
#include "profile.h"
#include "lua_host.h"

void ml_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}

// ── data the "mod" reads through the guarded ABI ──────────────────────────────
static volatile uint32_t g_known   = 0x00C0FFEE;                 // mem_read target
static volatile uint32_t g_scratch = 0;                          // mem_write target
static const uint8_t     g_needle[] = { 0xDE,0xAD,0xBE,0xEF,0xCA,0xFE };  // mem_scan target (in .rdata)

// ── the hook target (a real call through a volatile fn ptr; -O0 build => relocatable) ──
static volatile int g_tnat_calls = 0;
static int __attribute__((cdecl, noinline)) tnat(int a, int b) { g_tnat_calls++; return a + b; }
typedef int (*cd_fp)(int, int);

// ── what the "mod" records (the harness asserts on these) ─────────────────────
static int      g_mod_rc      = -1;
static int      g_mem_ok      = 0;
static int      g_scan_ok     = 0;
static int      g_hook_hits   = 0;
static uint32_t g_hook_arg0   = 0;
static int      g_onframe_n   = 0;
static int      g_main_ran    = 0;
static int      g_hook_handle = 0;

static void nat_hook_cb(const OssHookCtx *ctx, void *user) {
    const OssApi *api = (const OssApi *)user;
    g_hook_hits++;
    api->mem_read((const void *)(uintptr_t)(ctx->esp + 4), &g_hook_arg0, 4);   // cdecl arg0 = [esp+4]
}
static void nat_onframe_cb(void *user) { (void)user; g_onframe_n++; }
static void nat_main_cb(void *user)    { (void)user; g_main_ran = 1; }

// The mod's entry — the loader resolves + defers this (here we pass it to exec_defer_native).
static int TestModInit(const OssApi *api) {
    if (!api || api->abi_version != OSS_ABI_VERSION) return 1;
    api->log("native test mod init");

    uint32_t v = 0, w = 0x1234;
    g_mem_ok  = api->mem_read((const void *)&g_known, &v, 4) && v == 0x00C0FFEE;
    g_mem_ok &= api->mem_write((void *)&g_scratch, &w, 4) && g_scratch == 0x1234;
    g_mem_ok &= (api->mem_readable((const void *)&g_known, 4) == 1);
    g_mem_ok &= (api->mem_readable((const void *)0, 4) == 0);     // a bad address is guarded, not fatal
    g_mem_ok &= (api->mem_base() != 0);

    g_scan_ok = (api->mem_scan("DE AD BE EF CA FE") == (uintptr_t)g_needle);

    g_hook_handle = api->hook_entry((uintptr_t)&tnat, nat_hook_cb, (void *)api);
    api->on_frame(nat_onframe_cb, NULL);
    api->main_enqueue(nat_main_cb, NULL);
    return 0;
}

// ── assertions ────────────────────────────────────────────────────────────────
static int g_fails = 0;
static void check(const char *what, long got, long want) {
    if (got == want) printf("  PASS  %-30s = %ld\n", what, got);
    else { printf("  FAIL  %-30s = %ld (want %ld)\n", what, got, want); g_fails++; }
}

// remove the native hook via the ABI and confirm it stops firing
static void api_remove_check(const OssApi *api) {
    api->hook_remove(g_hook_handle);
    int before = g_hook_hits;
    volatile cd_fp fp = tnat; volatile int a = 1, b = 2; fp(a, b);
    check("hook_remove stops firing", g_hook_hits, before);
}

int main(void) {
    profile_select(1);
    if (lh_init() != 0) { printf("lh_init FAILED\n"); return 1; }

    printf(">> ABI vtable: abi_version=%u struct_size=%u (sizeof=%u)\n",
           oss_api()->abi_version, oss_api()->struct_size, (unsigned)sizeof(OssApi));
    check("abi_version", oss_api()->abi_version, OSS_ABI_VERSION);
    check("struct_size", oss_api()->struct_size, (long)sizeof(OssApi));

    exec_defer_native("nativetest", TestModInit);
    exec_on_safepoint((void *)0x1);   // captures the main tid, runs OssModInit (registers), drains, on_frame tick 1
    g_mod_rc = 0;                      // (OssModInit returned 0; logged by the executor)

    printf(">> after first safepoint (OssModInit ran on the engine thread):\n");
    check("hook installed (handle!=0)", g_hook_handle != 0, 1);
    check("mem read/write/guard/base", g_mem_ok, 1);
    check("mem_scan found the needle", g_scan_ok, 1);
    check("main_enqueue job ran", g_main_ran, 1);

    { volatile cd_fp fp = tnat; volatile int a = 7, b = 8; check("hooked tnat still returns", fp(a, b), 15); }
    check("native hook_entry fired", g_hook_hits, 1);
    check("native hook read arg0", (long)g_hook_arg0, 7);

    exec_on_safepoint((void *)0x1);   // on_frame tick 2
    exec_on_safepoint((void *)0x1);   // on_frame tick 3
    check("on_frame ticked per frame", g_onframe_n, 3);

    api_remove_check(oss_api());      // remove the hook, confirm it stops firing

    printf(">> %s (fails=%d)\n", g_fails ? "NATIVE_ABI_FAIL" : "NATIVE_ABI_OK", g_fails);
    return g_fails ? 1 : 0;
}
