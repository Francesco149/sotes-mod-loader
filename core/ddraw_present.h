// core/ddraw_present.h — DirectDraw present interception + D3D11 re-present (Phase A: capture).
//
// SotES renders through a DirectDraw7 SOFTWARE blitter and PRESENTS once per frame in zdd_present
// (profile present_va 0x5b8fc0; thiscall, ECX = the ZDD "screen" ctx, arg1 = HWND).  The mode field
// (ECX+0x164) picks how it presents — case 0 (Full) = hardware Flip, case 2 (Windowed) = a slow GDI
// BitBlt(GetDC(hwnd), backDC, SRCCOPY).  The finished frame always lives in the back-buffer surface
// at *(*(ECX+0x16c)+0x2c) (mode-independent).
//
// We hook zdd_present on the ENGINE thread (executor), read that back-buffer, and re-present it
// through OUR OWN vsync'd D3D11 swapchain — the shared foundation for borderless-fullscreen scaling
// AND the in-game overlay (both consume this captured frame; the overlay stays a mod-invisible swap).
//
// Phase A (this cut): the engine thread copies the 16bpp back-buffer into a lock-free triple buffer
// (ddp_on_present); the UI thread uploads it to a texture and draws it (aspect-correct) behind the
// ImGui overlay in the companion window (ddp_draw_background).  The game's own present still runs —
// zero game-window takeover, so it's a safe capture proof.  ddp_present_interval_us exposes the
// game's measured frame-delivery pace (does the WINDOWED loop itself lag, or only its present?).
#ifndef OSS_DDRAW_PRESENT_H
#define OSS_DDRAW_PRESENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ENGINE THREAD (from the zdd_present hook): read the just-rendered back-buffer off the screen ctx
// and copy its pixels into the lock-free capture buffer.  Cheap (a ~600 KB memcpy + the surface
// Lock/Unlock).  screen_ctx = the hook's ECX; hwnd = arg1 (reserved for the game-window backend).
void ddp_on_present(void *screen_ctx, void *hwnd);

// UI THREAD (from ui.cpp render): draw the latest captured frame into the currently-bound render
// target, aspect-correct (letterboxed) within dstW x dstH.  No-op until a frame has been captured.
// dev/ctx are ID3D11Device* / ID3D11DeviceContext* (void* to keep d3d11.h off the C callers).
void ddp_draw_background(void *dev, void *ctx, int dstW, int dstH);

// Has at least one frame been captured?  (UI thread — gates the "waiting for the game" placeholder.)
int ddp_have_frame(void);

// Monotonic count of PUBLISHED frames — the UI thread compares it to detect a fresh capture and
// render the mirror (so it tracks the game's present rate).  Starts 0.
uint32_t ddp_frame_seq(void);

// Mean interval between game presents, in microseconds (0 until measured).  ~16666 => a smooth 60 Hz
// loop (so windowed slowness is only the present); much larger / jittery => the loop itself lags.
uint32_t ddp_present_interval_us(void);

// Phase B/C — own the game window: present the captured frame into the game window instead of just
// mirroring it in the companion.  ddp_set_takeover picks the mode (config ddraw_takeover): 0 = off,
// 1 = borderless-fullscreen, 2 = windowed (a normal resizable window).  Both drive the window with our
// vsync'd, sharp-bilinear present + the in-game overlay.  ddp_takeover_active is 1 only once we're
// actually presenting into the window, so the executor knows to SKIP the game's own present.
void ddp_set_takeover(int mode);
int  ddp_takeover_active(void);

// Windowed takeover (mode 2): the game window was resized — resize our swapchain to the new client and
// re-present.  Call from the game window's WndProc (WM_SIZE), on the engine thread.  No-op otherwise.
void ddp_on_resize(int w, int h);

// Map the desktop cursor into the game's 640x480 screen space (undoing the borderless pillarbox / the
// windowed client scale).  Fills gx/gy with the game-screen position; returns 1 if the cursor is over
// the game area.  Used by mod.game.mouse for the screen-space half (world-space adds the camera).
int ddp_cursor_game(float *gx, float *gy);

#ifdef __cplusplus
}
#endif

#endif
