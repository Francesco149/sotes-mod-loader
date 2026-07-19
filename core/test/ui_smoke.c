// core/test/ui_smoke.c — stand up the UI host WITHOUT the game (main window only).
//
// The overlay needs the live game window to track, but the main window + its DX11 device + the
// two-thread Lua draw path (a Lua panel/window callback rendered on the UI thread under the LBL)
// are all exercisable here.  Registers a panel + a floating window through the real mod.ui table,
// runs the render loop for a few seconds, then shuts down.  Proves device creation (HW or the WARP
// fallback), ImGui init, and that the draw callbacks run without faulting.  Run via WSLInterop:
//   make -C core ../build/ui_smoke.exe && (cd build && ./ui_smoke.exe 3)
// A window appears on the Windows desktop for the duration; "UI_SMOKE_OK" = the loop ran + tore
// down cleanly.  (Headless CI with no D3D at all logs a graceful "device creation FAILED" instead.)

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <windows.h>

#include "lua_host.h"
#include "ui.h"

#include "lua.h"
#include "lauxlib.h"

void ml_log(const char *fmt, ...) {   // stand-in for loader.c's flush-to-file log
    va_list ap; va_start(ap, fmt); vprintf(fmt, ap); va_end(ap);
    fputc('\n', stdout); fflush(stdout);
}

static const char *PANEL_LUA =
    "ui.panel('Smoke Panel', function()\n"
    "  ui.text('hello from a Lua panel on the UI thread')\n"
    "  ui.separator()\n"
    "  if ui.button('click me') then end\n"
    "  ui.same_line()\n"
    "  local v = ui.checkbox('a toggle', _G.smk_toggle)\n"
    "  _G.smk_toggle = v\n"
    "  _G.smk_slider = ui.slider_int('a slider', _G.smk_slider or 0, 0, 100)\n"
    "  ui.text_disabled('rendered under the Lua Big Lock')\n"
    "end)\n"
    "ui.window('Smoke Window', function()\n"
    "  ui.text_wrapped('a floating mod window, mirrored into the overlay in-game')\n"
    "  ui.bullet('one')\n"
    "  ui.bullet('two')\n"
    "end)\n";

int main(int argc, char **argv) {
    int secs = (argc > 1) ? atoi(argv[1]) : 3;
    if (lh_init() != 0) { printf("lh_init FAILED\n"); return 1; }

    // Register a panel + a window through the real mod.ui table (exposed as global `ui` here).
    lua_State *L = lh_state();
    ui_push_table(L);
    lua_setglobal(L, "ui");
    if (luaL_dostring(L, PANEL_LUA) != 0) {
        printf("panel register FAILED: %s\n", lua_tostring(L, -1));
        return 1;
    }
    printf(">> registered a panel + a window; starting the UI host (main window only)\n");

    ui_start(NULL, 0, 0);          // NULL game hwnd -> main window only, no overlay
    printf(">> UI up; rendering for %d s (a window should appear on the desktop)\n", secs);
    Sleep((DWORD)secs * 1000);

    printf(">> requesting shutdown\n");
    ui_shutdown();
    Sleep(800);                    // let the UI thread tear down its device/window
    printf(">> UI_SMOKE_OK\n");
    return 0;
}
