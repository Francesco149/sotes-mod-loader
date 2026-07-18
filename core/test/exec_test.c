// core/test/exec_test.c — drives the executor's drain WITHOUT the game.
//
// The real safepoint hook (MinHook on 0x437c70) + WndProc bootstrap need the live game,
// but the executor's LOGIC — deferred mod init runs on the safepoint (not before),
// on_frame fires every frame, mod.main runs its job, and the safepoint `this` is
// captured — is testable natively by calling exec_on_safepoint() directly.  Links the
// executor + Lua host TUs and supplies ml_log (normally in loader.c).  Run via WSLInterop.

#include <stdio.h>
#include <stdarg.h>
#include "lua_host.h"
#include "executor.h"
#include "profile.h"

void ml_log(const char *fmt, ...) {   // stand-in for loader.c's flush-to-file log
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}

int main(int argc, char **argv) {
    const char *modpath = (argc > 1) ? argv[1] : "exec_test_mod.lua";
    profile_select(1);
    if (lh_init() != 0) { printf("lh_init FAILED\n"); return 1; }

    exec_defer_mod("t", ".", modpath);
    printf(">> deferred a mod; NO safepoint yet — its init must NOT have run\n");

    for (int i = 1; i <= 3; i++) {
        printf(">> --- safepoint %d ---\n", i);
        exec_on_safepoint((void *)0xABCD1234);   // simulate the per-frame hook (captures this)
    }
    printf(">> exec_ti_mgr() = 0x%08x  (want 0xabcd1234)\n", exec_ti_mgr());
    printf(">> EXEC_OK\n");
    return 0;
}
