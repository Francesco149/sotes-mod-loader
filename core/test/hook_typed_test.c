// core/test/hook_typed_test.c — drives the P3 Tier-2 typed-hook path WITHOUT the game.
//
// Proves the full pipeline on the host: a mod's mod.hook.typed installs a LuaJIT FFI
// closure (fronted by the C main-thread gate) as a real MinHook detour on a local target,
// and the typed chain can MODIFY ARGS, MODIFY THE RETURN, and BLOCK the original — across
// cdecl / __stdcall / __thiscall (the callee-cleanup + ecx-capture paths, the crashy bits).
// Also checks the off-thread gate (a call off the engine thread skips the Lua chain) and the
// cross-tier exclusion (a VA is Tier-1 xor Tier-2).  Links every core TU except loader.o and
// supplies ml_log (normally loader.c's).  Run via WSLInterop.
//
// Targets are noinline + called through a `volatile` function pointer so the optimizer emits
// a real call to the (hooked) entry rather than inlining/folding it.  Built at -O0 anyway so
// MinHook always has a relocatable >=5-byte prologue.

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <windows.h>

#include "lua.h"
#include "lauxlib.h"
#include "lua_host.h"
#include "executor.h"
#include "profile.h"

void ml_log(const char *fmt, ...) {   // stand-in for loader.c's flush-to-file log
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}

// ── hook targets (one per convention) ────────────────────────────────────────
static volatile int g_block_calls = 0;   // t_block's body bumps this — must stay 0 when blocked

static int __attribute__((cdecl,    noinline)) t_pp(int a, int b)        { return a + b; }
static int __attribute__((cdecl,    noinline)) t_block(int a, int b)     { g_block_calls++; return a + b; }
static int __attribute__((stdcall,  noinline)) t_sc(int a, int b)        { return a - b; }
static int __attribute__((thiscall, noinline)) t_this(void *self, int x) { (void)self; return x * 2; }
static int __attribute__((cdecl,    noinline)) t_entry(int a, int b)     { return a * b; }

typedef int (*cd_fp)(int, int);
typedef int (__attribute__((stdcall))  *sc_fp)(int, int);
typedef int (__attribute__((thiscall)) *tc_fp)(void *, int);

// ── assertions ───────────────────────────────────────────────────────────────
static int g_fails = 0;
static void check(const char *what, long got, long want) {
    if (got == want) printf("  PASS  %-30s = %ld\n", what, got);
    else { printf("  FAIL  %-30s = %ld (want %ld)\n", what, got, want); g_fails++; }
}

static DWORD WINAPI off_thread(void *p) {   // call the hooked target off the engine thread
    volatile cd_fp fp = t_pp;
    *(int *)p = fp(2, 3);
    return 0;
}

// read a field the mod stashed in the shared RESULTS table
static double result_num(lua_State *L, const char *field) {
    lua_getglobal(L, "RESULTS"); lua_getfield(L, -1, field);
    double v = lua_tonumber(L, -1); lua_pop(L, 2); return v;
}
static int result_bool(lua_State *L, const char *field) {
    lua_getglobal(L, "RESULTS"); lua_getfield(L, -1, field);
    int v = lua_toboolean(L, -1); lua_pop(L, 2); return v;
}

int main(int argc, char **argv) {
    const char *modpath = (argc > 1) ? argv[1] : "hook_typed_test_mod.lua";
    profile_select(1);
    if (lh_init() != 0) { printf("lh_init FAILED\n"); return 1; }
    lua_State *L = lh_state();

    // Hand the mod the target addresses; give it a shared RESULTS table to report through.
    lua_pushnumber(L, (double)(uintptr_t)&t_pp);    lua_setglobal(L, "T_PP");
    lua_pushnumber(L, (double)(uintptr_t)&t_block); lua_setglobal(L, "T_BLOCK");
    lua_pushnumber(L, (double)(uintptr_t)&t_sc);    lua_setglobal(L, "T_SC");
    lua_pushnumber(L, (double)(uintptr_t)&t_this);  lua_setglobal(L, "T_THIS");
    lua_pushnumber(L, (double)(uintptr_t)&t_entry); lua_setglobal(L, "T_ENTRYVA");
    lua_newtable(L);                                lua_setglobal(L, "RESULTS");

    exec_defer_mod("hooktyped", ".", modpath);
    exec_on_safepoint((void *)0x1234);   // captures THIS thread as g_main_tid + runs the mod init (installs hooks)

    printf(">> hooked calls on the engine thread:\n");
    { volatile cd_fp fp = t_pp;    volatile int a = 2, b = 3; check("cdecl   pre*2 then post+100", fp(a, b), 107); }  // (2*2+3)+100
    { volatile cd_fp fp = t_block; volatile int a = 2, b = 3; check("cdecl   blocked -> ret",      fp(a, b), 999); }
    check("cdecl   original NOT run (block)", g_block_calls, 0);
    { volatile sc_fp fp = t_sc;    volatile int a = 9, b = 4; check("stdcall post*10",             fp(a, b), 50); }   // (9-4)*10
    { volatile tc_fp fp = t_this;  volatile int x = 5;
      check("thiscall arg+1 then orig*2", fp((void *)&t_this, x), 12);                                                // (5+1)*2
      check("thiscall ecx(self) captured", result_num(L, "seen_self") == (double)(uintptr_t)&t_this, 1); }

    printf(">> off-thread call must SKIP the chain (main-thread gate):\n");
    int off = -1; HANDLE th = CreateThread(NULL, 0, off_thread, &off, 0, NULL);
    WaitForSingleObject(th, INFINITE); CloseHandle(th);
    check("off-thread = original", off, 5);

    printf(">> cross-tier exclusion + counts:\n");
    check("entry() on a typed VA refused", result_bool(L, "excl_entry_on_typed"), 1);
    check("typed() on an entry VA refused", result_bool(L, "excl_typed_on_entry"), 1);
    check("typed_count", (long)result_num(L, "typed_count"), 4);

    printf(">> %s (fails=%d)\n", g_fails ? "TYPED_HOOK_FAIL" : "TYPED_HOOK_OK", g_fails);
    return g_fails ? 1 : 0;
}
