// core/sotes_autoload.c — the SotES save auto-load harness (dev).  See sotes_autoload.h.
//
// Menu-drive (the trainer's verified path).  At the title the game polls input via 0x437c70 (our
// safepoint); we mash CONFIRM there until the save picker opens.  The picker polls via 0x4378d0; we
// hook it (a Tier-1 native observer) and mash CONFIRM there until a gameplay scene loads.  With no
// slot targeted the picker's default (newest) save loads.  The attract/demo trigger is patched so
// the title doesn't cut to the demo mid-drive.  Everything runs on the engine thread.
//
// Input-record contract (from the trainer / engine-quirks): a record is {id@0, now@4, state=1@8};
// its pointer is published into the manager's 64-slot ring at mgr+0x0c, slot 63 (first polled); the
// poll matches id + state==1 + (poll_now - record_now) <= 0x64.  `now` = the poll's own timestamp
// (0x437c70 arg1 = exec_sp_now(); 0x4378d0 arg2 = [esp+8]).

#include "sotes_autoload.h"
#include "mem.h"
#include "executor.h"
#include "hooks.h"
#include "oss_mod_api.h"        // OssHookCtx
#include "loader_internal.h"

#include <windows.h>
#include <string.h>

// EN-SE VAs (ImageBase 0x400000) — trainer / SE_CODE_MAP.md.
#define VA_PICKER_POLL  0x4378d0    // save-slot picker input poll (thiscall; ecx = picker ctrl, arg2 = now)
#define VA_DEMO_JL      0x583866    // attract/demo trigger (jl 0x5832e1) — patched to always skip the demo
#define VA_RENDER_ROOT  0x92dd38    // *(this) != 0  <=>  a gameplay scene is loaded
#define BTN_CONFIRM     0x25        // title/picker confirm button id
#define RING_OFF        0x0c        // input mgr -> 64 record-pointer ring
#define RING_SLOT       63          // ordinal 0 = the first-polled ring slot

static int      g_target = -1;      // savedataNN file index; < 0 = newest/default (no selection)
static int      g_state;            // 0 idle, 1 title-confirm, 2 picker-confirm, 3 done
static uint32_t g_pk_mgr;           // captured picker controller (ecx of the picker poll)
static int      g_patched, g_pk_logged, g_done_logged;
static uint8_t  g_rec[8][16];       // rotating record buffers (static -> always game-readable)
static int      g_recn;

static int scene_loaded(void) {
    uint32_t root = 0;
    return mem_rd32((void *)mem_reloc(VA_RENDER_ROOT), &root) && root != 0;
}
static void mark_done(void) {
    g_state = 3;
    if (!g_done_logged) { g_done_logged = 1; ml_log("[autoload] scene loaded — gameplay reached, drive stopped"); }
}

// Publish a synthetic {id, now, 1} record into ring slot 63 of a manager.
static void inject(uint32_t mgr, uint32_t id, uint32_t now) {
    if (!mgr) return;
    uintptr_t slot = mgr + RING_OFF + RING_SLOT * 4;
    if (!mem_writable((void *)slot, 4)) return;
    uint8_t *rec = g_rec[g_recn++ & 7];
    *(uint32_t *)(rec + 0) = id;
    *(uint32_t *)(rec + 4) = now;
    *(uint32_t *)(rec + 8) = 1;
    *(uint32_t *)slot = (uint32_t)(uintptr_t)rec;
}

// Freeze the attract/demo trigger so the title stays up during the drive (0f 8c .. -> e9 .. 90).
static void patch_demo(void) {
    if (g_patched) return;
    g_patched = 1;
    void *va = (void *)mem_reloc(VA_DEMO_JL);
    static const uint8_t want[2] = { 0x0f, 0x8c };
    if (!mem_readable(va, 6) || memcmp(va, want, 2) != 0) { ml_log("[autoload] demo trigger bytes unexpected — not patched"); return; }
    static const uint8_t patch[6] = { 0xe9, 0x76, 0xfa, 0xff, 0xff, 0x90 };   // jmp 0x5832e1 + nop
    DWORD old;
    if (VirtualProtect(va, 6, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(va, patch, 6);
        VirtualProtect(va, 6, old, &old);
        FlushInstructionCache(GetCurrentProcess(), va, 6);
        ml_log("[autoload] demo trigger frozen @ %p", va);
    }
}

// The save-picker poll observer (Tier-1, engine thread): capture the picker ctrl + mash confirm.
static void picker_cb(const OssHookCtx *ctx, void *user) {
    (void)user;
    if (!g_pk_logged) { g_pk_logged = 1; ml_log("[autoload] save picker open (ctrl=0x%08x) — confirming", (unsigned)ctx->ecx); }
    g_pk_mgr = ctx->ecx;
    if (g_state == 1) g_state = 2;                    // title -> picker
    if (g_state != 2) return;
    if (scene_loaded()) { mark_done(); return; }
    uint32_t now = 0;
    mem_rd32((void *)(uintptr_t)(ctx->esp + 8), &now); // 0x4378d0 arg2 = the poll's `now`
    inject(ctx->ecx, BTN_CONFIRM, now);
}

// The title/gameplay input poll runs every safepoint (0x437c70) — drives the title-confirm step.
static void title_cb(void *user) {
    (void)user;
    patch_demo();
    if (g_state == 3) return;
    if (scene_loaded()) { mark_done(); return; }
    if (g_state == 1 && !g_pk_mgr) inject(exec_ti_mgr(), BTN_CONFIRM, exec_sp_now());
}

// Runs on the engine thread at the first safepoint (via exec_defer_fn) — safe to install hooks +
// register on_frame here.
static void arm(void) {
    uintptr_t pk = mem_reloc(VA_PICKER_POLL);
    int h = hooks_entry_c(pk, picker_cb, NULL);       // observe the picker poll
    exec_on_frame_c(title_cb, NULL);                  // drive the title poll every safepoint
    g_state = 1;
    ml_log("[autoload] armed — picker hook @ 0x%08x (handle %d), target slot %d (<0 = newest)", (unsigned)pk, h, g_target);
}

void sotes_autoload_enable(int slot) {
    g_target = slot;
    exec_defer_fn(arm);   // arm on the engine thread at the first safepoint
}
