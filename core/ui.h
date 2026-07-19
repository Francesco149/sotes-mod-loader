// core/ui.h — the ImGui/DX11 UI host (P5): loader-owned main window + in-game overlay mirror.
//
// Two loader-MANAGED Dear ImGui instances (a mod never creates its own context):
//   (1) a main window — a normal top-level window with its own DX11 device (the trainer's
//       proven, stable model; we never touch the game's renderer).  The default UI surface.
//   (2) an in-game overlay — a SECOND managed instance on a transparent, top-most window
//       tracked over the game's client area, toggled by a hotkey.  It renders the SAME mod
//       draw callbacks into a second render target, so the panels are literally a mirror.
// Both run on ONE dedicated UI thread with one message pump.  Mods add panels/windows via the
// Lua `mod.ui.*` table (built here); the callbacks run on the UI thread under the "Lua Big
// Lock" (lua_host.h) so they never race the engine thread's Lua.  UI callbacks READ game
// memory (guarded) + enqueue engine work through mod.main — they never call the engine directly.
//
// The rest of the loader is C; this TU is C++ (ImGui + DX11).  The interface below is the only
// seam, kept `extern "C"` so lua_host.c / loader.c call it like any other subsystem.
#ifndef OSS_UI_H
#define OSS_UI_H

struct lua_State;

#ifdef __cplusplus
extern "C" {
#endif

// Bind the shared Lua state + build the `mod.ui` table.  Call from lh_init (like exec_init /
// hooks_init), BEFORE any mod runs — so push_mod_table can hand each mod its mod.ui.
void ui_init(struct lua_State *L);

// Push the shared `ui` table onto L (for push_mod_table -> mod.ui).  No-op-safe before ui_init.
void ui_push_table(struct lua_State *L);

// Spawn the UI thread: stand up the main window (+ the overlay over game_hwnd, if non-NULL).
// Idempotent — a second call is ignored.  game_hwnd is the subclassed game window (HWND as
// void* to keep <windows.h> out of the C headers); NULL = main window only, no overlay.
// key_main / key_overlay are the VK_* toggle keys (0 = use the defaults F8 / INSERT).
void ui_start(void *game_hwnd, int key_main, int key_overlay);

// Ask the UI thread to tear down its windows/devices and exit (best-effort, on clean unload).
void ui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
