// core/executor.c — the main-thread executor (P2).  See executor.h.

#include "executor.h"
#include "profile.h"
#include "mem.h"
#include "lua_host.h"
#include "loader_internal.h"

#include <windows.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "MinHook.h"

static lua_State *g_L;
static int g_armed;

// ── the register-capture observer thunk (Tier-1: convention-agnostic, can't modify) ──
// MinHook patches the safepoint's first 5 bytes to jmp here.  We capture ecx (= the
// input manager `this`), run the C drain, then TAIL-JUMP to the trampoline (the original)
// with the caller's stack untouched — so the observed function runs exactly as before.
// asm labels give the referenced C symbols fixed, prefix-free names for the thunk.
void      safepoint_c(void)        asm("oss_sp_c");
void     *g_sp_orig                asm("oss_sp_orig");   // MinHook trampoline (original)
volatile uint32_t g_ti_mgr         asm("oss_sp_mgr");    // captured input manager (ecx)

__asm__(
    ".text\n"
    ".globl oss_sp_detour\n"
    "oss_sp_detour:\n"
    "  movl %ecx, oss_sp_mgr\n"   // capture this (= input manager) before anything clobbers ecx
    "  pushal\n"                   // preserve all GP regs across the observer
    "  call oss_sp_c\n"            // run the drain (cdecl, no args; reads oss_sp_mgr)
    "  popal\n"
    "  jmp *oss_sp_orig\n"         // continue into the original (stack intact) — never returns here
);
extern void oss_sp_detour(void) asm("oss_sp_detour");   // defined by the asm block above (no _ prefix)

uint32_t exec_ti_mgr(void) { return g_ti_mgr; }
int      exec_armed(void)  { return g_armed; }

// ── deferred mod inits (run on the main thread at the first safepoint) ───────
#define MAX_DEFER 64
static struct { char name[64], dir[MAX_PATH], path[MAX_PATH]; } g_defer[MAX_DEFER];
static int g_ndefer, g_inited;

void exec_defer_mod(const char *name, const char *dir, const char *init_path) {
    if (g_ndefer >= MAX_DEFER) { ml_log("[exec] defer list full — dropping %s", name); return; }
    _snprintf(g_defer[g_ndefer].name, 64, "%s", name);
    _snprintf(g_defer[g_ndefer].dir, MAX_PATH, "%s", dir);
    _snprintf(g_defer[g_ndefer].path, MAX_PATH, "%s", init_path);
    g_ndefer++;
}
static void run_deferred(void) {
    for (int i = 0; i < g_ndefer; i++) lh_run_mod(g_defer[i].name, g_defer[i].dir, g_defer[i].path);
}
void exec_run_deferred_inline(void) { run_deferred(); }   // fallback (loader thread)

// ── job queue (mod.main) + on_frame list ─────────────────────────────────────
#define MAX_JOBS 256
static int g_job[MAX_JOBS], g_job_head, g_job_tail;
static CRITICAL_SECTION g_lock;
static int g_lock_init;

#define MAX_ONFRAME 128
static int g_onframe[MAX_ONFRAME], g_nonframe;

static void run_ref_once(int ref) {   // rawgeti -> pcall -> unref (main thread)
    lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(g_L, 0, 0, 0) != 0) { ml_log("[exec] main job error: %s", lua_tostring(g_L, -1)); lua_pop(g_L, 1); }
    luaL_unref(g_L, LUA_REGISTRYINDEX, ref);
}
static void drain_jobs(void) {
    for (;;) {
        int ref = -1;
        EnterCriticalSection(&g_lock);
        if (g_job_head != g_job_tail) { ref = g_job[g_job_head]; g_job_head = (g_job_head + 1) % MAX_JOBS; }
        LeaveCriticalSection(&g_lock);
        if (ref < 0) break;
        run_ref_once(ref);     // outside the lock: a job may enqueue more
    }
}
static void run_onframe(void) {
    for (int i = 0; i < g_nonframe; ) {
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, g_onframe[i]);
        if (lua_pcall(g_L, 0, 0, 0) != 0) {   // a faulting callback is DISABLED, not propagated (invariant #7)
            ml_log("[exec] on_frame error (disabling cb): %s", lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
            luaL_unref(g_L, LUA_REGISTRYINDEX, g_onframe[i]);
            g_onframe[i] = g_onframe[--g_nonframe];
        } else i++;
    }
}

// MAIN THREAD, every frame (via the safepoint hook, or a test harness).
void exec_on_safepoint(void *ti_mgr) {
    static volatile long in_sp;
    if (InterlockedExchange(&in_sp, 1)) return;   // reentrancy guard
    g_ti_mgr = (uint32_t)(uintptr_t)ti_mgr;
    if (!g_inited) { g_inited = 1; run_deferred(); }   // mods init here — on the main thread
    drain_jobs();
    run_onframe();
    InterlockedExchange(&in_sp, 0);
}
void safepoint_c(void) { exec_on_safepoint((void *)(uintptr_t)g_ti_mgr); }

// ── Lua: mod.main / mod.on_frame ─────────────────────────────────────────────
static int l_main(lua_State *L) {
    if (!lua_isfunction(L, 1)) return 0;
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (!g_armed) { run_ref_once(ref); return 0; }   // no executor: run now (already a safe thread)
    EnterCriticalSection(&g_lock);
    g_job[g_job_tail] = ref;
    g_job_tail = (g_job_tail + 1) % MAX_JOBS;
    if (g_job_tail == g_job_head) g_job_head = (g_job_head + 1) % MAX_JOBS;   // full: drop oldest
    LeaveCriticalSection(&g_lock);
    return 0;
}
static int l_on_frame(lua_State *L) {
    if (!lua_isfunction(L, 1)) return 0;
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (g_nonframe < MAX_ONFRAME) g_onframe[g_nonframe++] = ref;
    else { luaL_unref(L, LUA_REGISTRYINDEX, ref); ml_log("[exec] on_frame list full"); }
    return 0;
}

void exec_init(lua_State *L) {
    g_L = L;
    if (!g_lock_init) { InitializeCriticalSection(&g_lock); g_lock_init = 1; }
}
void exec_push_main(lua_State *L)     { lua_pushcfunction(L, l_main); }
void exec_push_on_frame(lua_State *L) { lua_pushcfunction(L, l_on_frame); }

// ── bootstrap: subclass the game window -> install the safepoint hook on the main thread ──
#define WM_OSS_BOOT (WM_APP + 0x5b01)
static WNDPROC g_orig_wndproc;
static HWND    g_hwnd;

static void install_safepoint_hook(void) {   // runs on the MAIN thread (in the WndProc)
    const oss_profile *p = profile_current();
    if (!p) return;
    void *va = (void *)mem_reloc(p->safepoint_va);
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { ml_log("[exec] MH_Initialize failed (%d)", s); return; }
    s = MH_CreateHook(va, (LPVOID)&oss_sp_detour, &g_sp_orig);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { ml_log("[exec] MH_CreateHook failed (%d)", s); return; }
    if (MH_EnableHook(va) != MH_OK) { ml_log("[exec] MH_EnableHook failed"); return; }
    g_armed = 1;
    ml_log("[exec] safepoint hook armed @ %p (profile %s va 0x%p)", va, p->id, (void *)p->safepoint_va);
}

static LRESULT CALLBACK boot_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_OSS_BOOT) { install_safepoint_hook(); return 0; }
    return CallWindowProcA(g_orig_wndproc, h, m, w, l);
}
static BOOL CALLBACK find_wnd(HWND h, LPARAM l) {
    (void)l;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != NULL) return TRUE;   // top-level, visible only
    char cls[64] = ""; GetClassNameA(h, cls, (int)sizeof cls);
    if (strcmp(cls, "#32770") == 0) return TRUE;                             // skip the launcher dialog
    g_hwnd = h; return FALSE;
}
int exec_bootstrap(void) {
    if (!profile_current()) { ml_log("[exec] no game profile — executor disabled (fallback)"); return 0; }
    for (int i = 0; i < 200 && !g_hwnd; i++) { EnumWindows(find_wnd, 0); if (!g_hwnd) Sleep(25); }
    if (!g_hwnd) { ml_log("[exec] game window not found — fallback (mods run on the loader thread)"); return 0; }
    g_orig_wndproc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)boot_wndproc);
    if (!g_orig_wndproc) { ml_log("[exec] window subclass failed — fallback"); return 0; }
    ml_log("[exec] game window %p subclassed — posting bootstrap to the main thread", (void *)g_hwnd);
    PostMessageA(g_hwnd, WM_OSS_BOOT, 0, 0);
    return 1;
}
