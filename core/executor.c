// core/executor.c — the main-thread executor (P2).  See executor.h.

#include "executor.h"
#include "profile.h"
#include "mem.h"
#include "lua_host.h"
#include "config.h"
#include "native_bridge.h"
#include "ui.h"
#include "prof.h"
#include "ddraw_present.h"
#include "loader_internal.h"

#include <windows.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "MinHook.h"

static lua_State *g_L;
static int g_armed;
static uint32_t g_main_tid;   // engine thread id (captured at the first safepoint)

uint32_t exec_main_tid(void) { return g_main_tid; }
uint32_t *exec_main_tid_ptr(void) { return &g_main_tid; }   // hooks.c bakes this addr into the typed-hook gate

// ── the register-capture observer thunk (Tier-1: convention-agnostic, can't modify) ──
// MinHook patches the safepoint's first 5 bytes to jmp here.  We capture ecx (= the
// input manager `this`), run the C drain, then TAIL-JUMP to the trampoline (the original)
// with the caller's stack untouched — so the observed function runs exactly as before.
// asm labels give the referenced C symbols fixed, prefix-free names for the thunk.
void      safepoint_c(void)        asm("oss_sp_c");
void     *g_sp_orig                asm("oss_sp_orig");   // MinHook trampoline (original)
volatile uint32_t g_ti_mgr         asm("oss_sp_mgr");    // captured input manager (ecx)
volatile uint32_t g_sp_now         asm("oss_sp_now");    // captured poll timestamp (arg1 at [esp+4])

__asm__(
    ".text\n"
    ".globl oss_sp_detour\n"
    "oss_sp_detour:\n"
    "  movl %ecx, oss_sp_mgr\n"   // capture this (= input manager) before anything clobbers ecx
    "  pushal\n"                   // preserve all GP regs across the observer
    "  movl 0x24(%esp), %eax\n"   // capture arg1 = the poll's `now` timestamp — AFTER pushal (the
    "  movl %eax, oss_sp_now\n"   // original eax is saved; popal restores it).  arg1 = entry [esp+4]
    "  call oss_sp_c\n"            // run the drain (cdecl, no args; reads oss_sp_mgr)
    "  popal\n"                    // restores the original eax (+ all regs) for the trampoline
    "  jmp *oss_sp_orig\n"         // continue into the original (stack intact) — never returns here
);
extern void oss_sp_detour(void) asm("oss_sp_detour");   // defined by the asm block above (no _ prefix)

// ── the ddraw present observer thunk (zdd_present 0x5b8fc0; thiscall, ECX = the ZDD screen ctx) ──
// Same shape as the safepoint thunk: capture ECX (+ arg1 HWND) then TAIL-JUMP to the trampoline so
// the game's OWN present still runs (Phase A: capture only, no window takeover).  ddp_on_present
// reads the finished back-buffer off the ctx and copies it into the lock-free capture buffer.
void      present_c(void)          asm("oss_present_c");
void     *g_present_orig           asm("oss_present_orig");   // MinHook trampoline (original present)
volatile uint32_t g_present_ctx    asm("oss_present_ctx");    // captured screen ctx (ecx)
volatile uint32_t g_present_hwnd   asm("oss_present_hwnd");   // captured HWND (arg1)
volatile uint32_t g_present_skip   asm("oss_present_skip");   // 1 = SKIP the game's own present (takeover)

__asm__(
    ".text\n"
    ".globl oss_present_detour\n"
    "oss_present_detour:\n"
    "  movl %ecx, oss_present_ctx\n"   // capture this (= the ZDD screen ctx) before ecx is clobbered
    "  pushal\n"
    "  movl 0x24(%esp), %eax\n"        // arg1 (HWND) at entry [esp+4]; after pushal it's at [esp+0x24]
    "  movl %eax, oss_present_hwnd\n"
    "  call oss_present_c\n"            // capture (+ takeover-present); sets oss_present_skip
    "  popal\n"                          // restores all GP regs (incl. the original ecx) for the tail
    "  cmpl $0, oss_present_skip\n"
    "  jne 1f\n"
    "  jmp *oss_present_orig\n"         // NOT takeover: run the game's own present (stack intact)
    "1:\n"
    "  ret $4\n"                         // TAKEOVER: skip it — zdd_present is thiscall(HWND), so ret 4
);
extern void oss_present_detour(void) asm("oss_present_detour");

void present_c(void) {
    ddp_on_present((void *)(uintptr_t)g_present_ctx, (void *)(uintptr_t)g_present_hwnd);
    g_present_skip = (uint32_t)ddp_takeover_active();   // 1 -> the thunk ret 4's, skipping the game's BitBlt
    ui_wake();   // render the companion mirror at the present rate (harmless while takeover owns the window)
}

uint32_t exec_ti_mgr(void) { return g_ti_mgr; }
uint32_t exec_sp_now(void) { return g_sp_now; }   // the safepoint poll's timestamp (for input injection)
int      exec_armed(void)  { return g_armed; }

// ── deferred mod inits (run on the main thread at the first safepoint) ───────
#define MAX_DEFER 64
static struct { char name[64], dir[MAX_PATH], path[MAX_PATH]; } g_defer[MAX_DEFER];
static int g_ndefer, g_inited;
// native mods: OssModInit is deferred here too, so it runs on the engine thread (like Lua init).
#define MAX_NDEFER 64
static struct { char name[64]; OssModInitFn init; } g_ndef[MAX_NDEFER];
static int g_nndef;

void exec_defer_mod(const char *name, const char *dir, const char *init_path) {
    if (g_ndefer >= MAX_DEFER) { ml_log("[exec] defer list full — dropping %s", name); return; }
    _snprintf(g_defer[g_ndefer].name, 64, "%s", name);
    _snprintf(g_defer[g_ndefer].dir, MAX_PATH, "%s", dir);
    _snprintf(g_defer[g_ndefer].path, MAX_PATH, "%s", init_path);
    g_ndefer++;
}
void exec_defer_native(const char *name, OssModInitFn init) {
    if (g_nndef >= MAX_NDEFER) { ml_log("[exec] native defer full — dropping %s", name); return; }
    _snprintf(g_ndef[g_nndef].name, 64, "%s", name);
    g_ndef[g_nndef].init = init;
    g_nndef++;
}
// One-shot C callbacks run on the ENGINE thread at the first safepoint (like a native mod init but
// no ABI) — for game-specific setup that must install hooks / register on_frame on the engine
// thread without racing the loader thread (e.g. the SotES save auto-load harness).
#define MAX_DEFERFN 8
static void (*g_deferfn[MAX_DEFERFN])(void);
static int g_ndeferfn;
void exec_defer_fn(void (*fn)(void)) {
    if (fn && g_ndeferfn < MAX_DEFERFN) g_deferfn[g_ndeferfn++] = fn;
    else if (fn) ml_log("[exec] defer-fn list full");
}
static void run_deferred(void) {
    // Native mods first (typically infrastructure), then Lua mods — both on the engine thread.
    for (int i = 0; i < g_nndef; i++) {
        int rc = g_ndef[i].init(oss_api());
        ml_log("[exec] native mod init: %s -> %d", g_ndef[i].name, rc);
    }
    for (int i = 0; i < g_ndefer; i++) lh_run_mod(g_defer[i].name, g_defer[i].dir, g_defer[i].path);
    for (int i = 0; i < g_ndeferfn; i++) g_deferfn[i]();
}
void exec_run_deferred_inline(void) { run_deferred(); }   // fallback (loader thread)

// ── job queue (mod.main) + on_frame list — each entry is a Lua ref OR a native C cb ──
typedef struct { int is_c; int ref; OssJobFn cfn; void *user; } exec_cb;
#define MAX_JOBS 256
static exec_cb g_job[MAX_JOBS]; static int g_job_head, g_job_tail;
static CRITICAL_SECTION g_lock;
static int g_lock_init;

#define MAX_ONFRAME 128
static exec_cb g_onframe[MAX_ONFRAME]; static int g_nonframe;

static void run_ref_once(int ref) {   // rawgeti -> pcall -> unref (main thread)
    lua_rawgeti(g_L, LUA_REGISTRYINDEX, ref);
    if (lua_pcall(g_L, 0, 0, 0) != 0) { ml_log("[exec] main job error: %s", lua_tostring(g_L, -1)); lua_pop(g_L, 1); }
    luaL_unref(g_L, LUA_REGISTRYINDEX, ref);
}
static void job_push(exec_cb j) {
    EnterCriticalSection(&g_lock);
    g_job[g_job_tail] = j;
    g_job_tail = (g_job_tail + 1) % MAX_JOBS;
    if (g_job_tail == g_job_head) g_job_head = (g_job_head + 1) % MAX_JOBS;   // full: drop oldest
    LeaveCriticalSection(&g_lock);
}
static void drain_jobs(void) {
    for (;;) {
        exec_cb j; int have = 0;
        EnterCriticalSection(&g_lock);
        if (g_job_head != g_job_tail) { j = g_job[g_job_head]; g_job_head = (g_job_head + 1) % MAX_JOBS; have = 1; }
        LeaveCriticalSection(&g_lock);
        if (!have) break;
        if (j.is_c) { if (j.cfn) j.cfn(j.user); } else run_ref_once(j.ref);   // outside the lock: may enqueue more
    }
}
static void run_onframe(void) {
    for (int i = 0; i < g_nonframe; ) {
        exec_cb *c = &g_onframe[i];
        if (c->is_c) { if (c->cfn) c->cfn(c->user); i++; continue; }   // native cb (not pcall-isolated — see note)
        lua_rawgeti(g_L, LUA_REGISTRYINDEX, c->ref);
        if (lua_pcall(g_L, 0, 0, 0) != 0) {   // a faulting Lua callback is DISABLED, not propagated (invariant #7)
            ml_log("[exec] on_frame error (disabling cb): %s", lua_tostring(g_L, -1));
            lua_pop(g_L, 1);
            luaL_unref(g_L, LUA_REGISTRYINDEX, c->ref);
            g_onframe[i] = g_onframe[--g_nonframe];
        } else i++;
    }
}

// MAIN THREAD, every frame (via the safepoint hook, or a test harness).
void exec_on_safepoint(void *ti_mgr) {
    static volatile long in_sp;
    if (InterlockedExchange(&in_sp, 1)) return;   // reentrancy guard
    if (!g_main_tid) g_main_tid = GetCurrentThreadId();   // this IS the engine thread
    g_ti_mgr = (uint32_t)(uintptr_t)ti_mgr;
    uint64_t t0 = prof_now();                      // profile the loader's per-frame cost (profile=1)
    if (!g_inited) { g_inited = 1; run_deferred(); }   // mods init here — on the main thread
    drain_jobs();
    run_onframe();
    uint64_t tb = prof_now(); ui_build(); prof_add(PROF_UI_BUILD, tb);   // UI snapshot build (self-throttled; P5)
    config_mod_flush(0);   // debounced: persist mod-config changes once a burst (e.g. a slider drag) settles
    prof_add(PROF_SAFEPOINT, t0);
    prof_frame();
    InterlockedExchange(&in_sp, 0);
}
void safepoint_c(void) { exec_on_safepoint((void *)(uintptr_t)g_ti_mgr); }

// ── Lua: mod.main / mod.on_frame ─────────────────────────────────────────────
static int l_main(lua_State *L) {
    if (!lua_isfunction(L, 1)) return 0;
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (!g_armed) { run_ref_once(ref); return 0; }   // no executor: run now (already a safe thread)
    exec_cb j = { 0, ref, NULL, NULL };
    job_push(j);
    return 0;
}
static int l_on_frame(lua_State *L) {
    if (!lua_isfunction(L, 1)) return 0;
    lua_pushvalue(L, 1);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    if (g_nonframe < MAX_ONFRAME) { exec_cb c = { 0, ref, NULL, NULL }; g_onframe[g_nonframe++] = c; }
    else { luaL_unref(L, LUA_REGISTRYINDEX, ref); ml_log("[exec] on_frame list full"); }
    return 0;
}

// ── native-mod C callbacks (the ABI bridge's main_enqueue / on_frame) ─────────
void exec_enqueue_c(OssJobFn fn, void *user) {
    if (!fn) return;
    if (!g_armed) { fn(user); return; }               // no executor yet: run now (caller is a safe thread)
    exec_cb j = { 1, 0, fn, user };
    job_push(j);                                       // thread-safe (locked): a worker may enqueue engine work
}
void exec_on_frame_c(OssJobFn fn, void *user) {
    if (!fn) return;                                   // register on the main thread (the list is unlocked, like Lua's)
    if (g_nonframe < MAX_ONFRAME) { exec_cb c = { 1, 0, fn, user }; g_onframe[g_nonframe++] = c; }
    else ml_log("[exec] on_frame list full (native)");
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

void *exec_game_hwnd(void) { return (void *)g_hwnd; }   // reserved: the future in-game overlay backend hooks/tracks this

// ── keepactive: keep the game running while its window is UNFOCUSED ────────────
// SotES (like many games) pauses/idles its main loop when the window isn't the foreground app —
// so its per-frame input poll (our safepoint 0x437c70) stops firing.  For headless testing + the
// save auto-load drive we must keep it ticking regardless of desktop focus.  Re-post
// WM_ACTIVATEAPP(TRUE) to the game window periodically (the shipped trainer's proven trick), from a
// dedicated thread since the safepoint itself is what stalls when unfocused.  DEFAULT-ON: the
// companion UI window steals foreground when clicked, which would otherwise idle the game loop.
static DWORD WINAPI keepactive_thread(void *unused) {
    (void)unused;
    for (;;) {
        HWND h = g_hwnd;
        // Only when the game ISN'T the foreground window: re-post WM_ACTIVATEAPP(TRUE) so its loop
        // keeps ticking (the engine idles the loop on foreground loss).  When the game IS foreground
        // we post nothing — normal play is completely untouched (no 5 Hz activation churn/hitch).
        // WM_ACTIVATEAPP(TRUE) ONLY — never WM_ACTIVATE / SetForegroundWindow: stealing focus back
        // would fight the companion window for the keyboard/mouse (the trainer's no-focus-steal trick).
        if (h && GetForegroundWindow() != h) PostMessageA(h, WM_ACTIVATEAPP, TRUE, 0);
        Sleep(200);
    }
    return 0;
}
void exec_keepactive(void) {
    HANDLE t = CreateThread(NULL, 0, keepactive_thread, NULL, 0, NULL);
    if (t) { CloseHandle(t); ml_log("[exec] keepactive ON — re-posting WM_ACTIVATEAPP so the game ticks while unfocused"); }
}

static void install_present_hook(void);   // ddraw capture backend (defined below); armed after the safepoint

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
    install_present_hook();   // ddraw capture / re-present backend (same engine thread, MH already up)
}

// Install the ddraw present hook (zdd_present) right after the safepoint, on the MAIN thread — the
// capture/re-present backend (ddraw_present.cpp).  MH is already initialized by the safepoint install.
// Opt-out with ddraw=0; non-fatal (a failure just leaves the game mirror/overlay off).
static void install_present_hook(void) {
    const oss_profile *p = profile_current();
    if (!p || !p->present_va) return;
    if (!config_get_int("ddraw", 1)) { ml_log("[exec] present hook off (ddraw=0)"); return; }
    ddp_set_takeover(config_get_int("ddraw_takeover", 0));   // own the game window (borderless) vs mirror-only
    void *va = (void *)mem_reloc(p->present_va);
    MH_STATUS s = MH_CreateHook(va, (LPVOID)&oss_present_detour, &g_present_orig);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { ml_log("[exec] present MH_CreateHook failed (%d)", s); return; }
    if (MH_EnableHook(va) != MH_OK) { ml_log("[exec] present MH_EnableHook failed"); return; }
    ml_log("[exec] present hook armed @ %p (zdd_present va 0x%p) — ddraw capture on", va, (void *)p->present_va);
}

static LRESULT CALLBACK boot_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_OSS_BOOT) { install_safepoint_hook(); return 0; }
    // In-game overlay (takeover): let the overlay's ImGui consume input (mouse/keyboard) so clicks on
    // the overlay don't fall through to the game.  No-op until the overlay is up / while hidden.  The
    // game window's WndProc runs on the engine thread — the same thread that owns the overlay's ImGui.
    if (ui_overlay_wndproc(h, m, w, l)) return 0;
    if (ddp_takeover_active()) {
        // We own the window's pixels (our D3D11 swapchain), so DON'T let the game GDI-paint it: during a
        // resize Windows floods WM_PAINT and the game's handler blits its 640x480 back-buffer 1:1 at the
        // top-left, on top of our scaled frame.  Validate + swallow (our next Present refills the window);
        // eat WM_ERASEBKGND too (no background flash).  Gated on takeover, so mirror/plain modes untouched.
        if (m == WM_ERASEBKGND) return 1;
        if (m == WM_PAINT) { ValidateRect(h, NULL); return 0; }
    }
    // Windowed takeover: track resizes so our swapchain follows the window (no-op in other modes).
    if (m == WM_SIZE && w != SIZE_MINIMIZED) ddp_on_resize((int)LOWORD(l), (int)HIWORD(l));
    return CallWindowProcA(g_orig_wndproc, h, m, w, l);
}
// Find the game's MAIN window.  Robustly: a top-level (no owner), visible window of OUR
// process whose class == the profile's window_class (the engine's own unique class, e.g.
// "CLASS_LIZSOFT_SOTES").  A positive class match never latches the launcher, a DirectShow
// "ActiveMovie" window, an IME window, or an early TRANSIENT window that later gets destroyed
// (which silently drops our subclass + the posted bootstrap — the bug this fixes).  If the
// profile has no window_class we fall back to the old heuristic (first non-launcher top-level).
static BOOL CALLBACK find_wnd(HWND h, LPARAM l) {
    (void)l;
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) != NULL) return TRUE;   // top-level + visible only
    char cls[64] = ""; GetClassNameA(h, cls, (int)sizeof cls);
    const oss_profile *p = profile_current();
    if (p && p->window_class) {
        if (strcmp(cls, p->window_class) != 0) return TRUE;                   // POSITIVE match the game's own class
    } else if (p && p->launcher_class && strcmp(cls, p->launcher_class) == 0) {
        return TRUE;                                                          // fallback: at least skip the launcher
    }
    g_hwnd = h; return FALSE;
}
// Auto-dismiss the game's pre-launch launcher (opt-in: skip_launcher=1).  In-process
// BM_CLICK of the profile's Launch button — works from this (loader) thread; it does NOT
// from an external process.  OFF by default: end users click Launch themselves.
static BOOL CALLBACK find_launcher(HWND h, LPARAM lp) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    const oss_profile *p = profile_current();
    char cls[64] = ""; GetClassNameA(h, cls, (int)sizeof cls);
    if (!p || !p->launcher_class || strcmp(cls, p->launcher_class) != 0) return TRUE;
    HWND btn = GetDlgItem(h, p->launch_ctrl_id);
    if (!btn) return TRUE;                                  // not the launcher we expect
    // Pick the display mode before launching (default WINDOWED; windowed=0 -> fullscreen).
    int win = config_get_int("windowed", 1);
    int radio = win ? p->windowed_ctrl_id : p->fullscreen_ctrl_id;
    if (radio) { HWND rb = GetDlgItem(h, radio); if (rb) SendMessageA(rb, BM_CLICK, 0, 0); }
    SendMessageA(btn, BM_CLICK, 0, 0);
    ml_log("[exec] launcher dismissed (%s mode)", win ? "windowed" : "fullscreen");
    *(int *)lp = 1; return FALSE;
}
static int dismiss_launcher(void) { int done = 0; EnumWindows(find_launcher, (LPARAM)&done); return done; }

int exec_bootstrap(void) {
    if (!profile_current()) { ml_log("[exec] no game profile — executor disabled (fallback)"); return 0; }
    const oss_profile *p = profile_current();
    int skip = config_get_int("skip_launcher", 1) && p->launcher_class;   // built-in default-on (hands-free boot)
    // Wait INDEFINITELY for the game window: it will appear as the game boots, and a bounded
    // timeout risks failing to arm on a slow boot (window-scanning is cheap; if the game
    // truly hangs the user kills it).  Optionally auto-dismiss the launcher while we wait.
    ml_log("[exec] waiting for the game window%s ...", skip ? " (skip_launcher: will dismiss the launcher)" : "");
    // Poll until the game's MAIN window exists: find_wnd positively matches the profile's
    // window_class, so it never latches the launcher or a transient window — it simply returns
    // nothing until the real game window is created (whether we auto-dismiss the launcher or the
    // user clicks Launch).  skip_launcher auto-clicks Launch (best-effort — BM_CLICK; the user
    // can always click manually, and the positive match still catches the window robustly).
    int launched = 0;
    while (!g_hwnd) {
        if (skip && !launched) launched = dismiss_launcher();
        EnumWindows(find_wnd, 0);
        if (!g_hwnd) Sleep(25);
    }
    g_orig_wndproc = (WNDPROC)SetWindowLongPtrA(g_hwnd, GWLP_WNDPROC, (LONG_PTR)boot_wndproc);
    if (!g_orig_wndproc) { ml_log("[exec] window subclass failed — fallback"); return 0; }
    ml_log("[exec] game window %p subclassed — posting bootstrap to the main thread", (void *)g_hwnd);
    PostMessageA(g_hwnd, WM_OSS_BOOT, 0, 0);
    return 1;
}
