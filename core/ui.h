// core/ui.h — the ImGui/DX11 UI host (P5): a loader-owned companion window fed by a LOCK-FREE
// bidirectional pipeline to/from the game's engine thread.  See ui.cpp for the full contract.
//
// Two threads, NOTHING locked between them (the design requirement):
//   - the ENGINE thread (executor safepoint) calls ui_build(): it runs the mod `mod.ui` callbacks,
//     records a plain-data UI snapshot, and publishes it lock-free (a triple buffer, latest-wins —
//     an unconsumed frame is dropped).  It only touches lua_State here (single-threaded Lua).
//   - the UI thread owns the window + DX11 + ImGui: it fetches the latest snapshot lock-free and
//     replays it, and returns interactions through per-widget ATOMIC slots (latest value wins;
//     clicks sticky until drained).  It NEVER touches lua_State.
//
// So a slow UI never stalls the game and a busy game never stalls the UI; stale data is dropped
// rather than queued.  The in-game overlay is deferred (companion window only) — when added it will
// consume the SAME snapshot through a renderer-hook backend, so mods won't change.
//
// This TU is C++ (ImGui + DX11); the interface is `extern "C"` so the C loader drives it.
#ifndef OSS_UI_H
#define OSS_UI_H

#include <stdint.h>   // uintptr_t / intptr_t in the overlay input entry point

struct lua_State;

#ifdef __cplusplus
extern "C" {
#endif

// Bind the shared Lua state + build the `mod.ui` table.  Call from lh_init (like exec_init /
// hooks_init), before any mod runs.
void ui_init(struct lua_State *L);

// Push the shared `ui` table onto L (for push_mod_table -> mod.ui).  No-op-safe before ui_init.
void ui_push_table(struct lua_State *L);

// Spawn the UI thread (the loader-owned companion window).  Idempotent.  key_toggle is the VK code
// to show/hide the window (0 = default F10).  build_hz is the engine-side snapshot rate (0 = 30).
void ui_start(int key_toggle, int build_hz);

// ENGINE THREAD: run the mod.ui callbacks + publish a fresh snapshot.  Called from the executor
// safepoint every frame; self-throttled to build_hz.  A no-op until ui_start ran (UI disabled).
void ui_build(void);

// Wake the UI thread to render one frame (the executor calls this from the present hook so the game
// mirror tracks the game's present rate, not the throttled snapshot rate).  No-op if hidden/disabled.
void ui_wake(void);

// ── in-game overlay (Phase C): draw mod.ui ON TOP of the game frame, in the game window ──────────
// In borderless takeover the game window IS the display, so we replay the SAME snapshot into an
// ImGui context owned by the ENGINE thread and render it over the game frame (the companion window
// is not started in takeover — see ui_start — so ImGui's global current-context is never raced).
//
// ENGINE THREAD (from ddraw_present's ddp_engine_present, each present): lazily brings ImGui up on
// the takeover device, handles the F10 toggle, and — when visible — draws the overlay into the
// currently-bound render target (the game-window back buffer, with the game frame already drawn).
// dev/ctx/hwnd are ID3D11Device*/ID3D11DeviceContext*/HWND (void* to keep d3d11.h off C callers).
void ui_overlay_present(void *dev, void *ctx, void *hwnd);

// ENGINE THREAD (from the executor's game-window subclass, boot_wndproc): feed a window message to
// the overlay's ImGui.  Returns 1 if ImGui consumed it (so the game doesn't ALSO act on it); 0 (and
// no ImGui touch) until the overlay is up / while it's hidden, so the game sees input as before.
int  ui_overlay_wndproc(void *hwnd, unsigned msg, uintptr_t wparam, intptr_t lparam);

// Ask the UI thread to tear down (best-effort, on clean unload).
void ui_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
