// core/test/ui_smoke.c — exercise the whole decoupled UI pipeline WITHOUT the game.
//
// In-game the executor calls ui_build() at each safepoint; here the smoke's main thread plays that
// role, calling ui_build() in a loop (it runs the Lua panel callbacks + publishes a snapshot), while
// the UI thread renders + replays.  So this drives the full lock-free path: engine build -> triple
// buffer -> UI replay, and input slots back.  A window appears on the desktop for the duration.
//   make -C core ../build/ui_smoke.exe && (cd build && ./ui_smoke.exe 3)
// ">> UI_SMOKE_OK" = it built, published, rendered, and tore down cleanly.

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>

#include "lua_host.h"
#include "ui.h"

#include "lua.h"
#include "lauxlib.h"

void ml_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}

static const char *PANEL_LUA =
    "local n = 0\n"
    "ui.panel('Smoke Panel', function()\n"
    "  n = n + 1\n"
    "  ui.text('build #' .. n)\n"                 // changes each build -> snapshot changes -> UI renders
    "  ui.separator()\n"
    "  if ui.button('click me') then end\n"
    "  ui.same_line()\n"
    "  _G.tog = ui.checkbox('a toggle', _G.tog)\n"
    "  _G.sl  = ui.slider_int('a slider', _G.sl or 0, 0, 100)\n"
    "  ui.text_disabled('decoupled: engine builds, UI replays')\n"
    "end)\n"
    "ui.window('Smoke Window', function()\n"
    "  ui.text_wrapped('a floating mod window (lock-free snapshot)')\n"
    "  ui.bullet('one'); ui.bullet('two')\n"
    "end)\n";

int main(int argc, char **argv) {
    int secs = (argc > 1) ? atoi(argv[1]) : 3;
    if (lh_init() != 0) { printf("lh_init FAILED\n"); return 1; }

    lua_State *L = lh_state();
    ui_push_table(L);
    lua_setglobal(L, "ui");
    if (luaL_dostring(L, PANEL_LUA) != 0) { printf("register FAILED: %s\n", lua_tostring(L, -1)); return 1; }
    printf(">> registered a panel + a window; starting the UI + driving builds for %d s\n", secs);

    ui_start(0, 30);                 // default toggle key (F8), 30 Hz build rate
    DWORD end = GetTickCount() + (DWORD)secs * 1000;
    while (GetTickCount() < end) { ui_build(); Sleep(10); }   // stand in for the engine safepoint

    printf(">> requesting shutdown\n");
    ui_shutdown();
    Sleep(800);
    printf(">> UI_SMOKE_OK\n");
    return 0;
}
