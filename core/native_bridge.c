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
#include "loader_internal.h"

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
