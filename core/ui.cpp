// core/ui.cpp — the ImGui/DX11 UI host (P5).  See ui.h for the contract.
//
// Layout of this file:
//   - one dedicated UI thread owns TWO ImGui contexts, each with its OWN DX11 device + swapchain:
//       g_main : a normal WS_OVERLAPPEDWINDOW (opaque; the default UI)   — toggle: F8
//       g_over : a borderless, top-most, DWM-transparent window tracked   — toggle: INSERT
//                over the game's client area (the in-game overlay MIRROR)
//   - both windows share one WndProc; the current ImGui context is selected per-HWND before
//     the Win32 backend handler runs (each window has its own backend data — multi-context).
//   - the mod-facing `mod.ui.*` table: panel/window registration + a curated widget set.  A
//     registered draw callback (a Lua closure ref) is invoked in BOTH contexts each frame, so a
//     panel appears identically in the window and the overlay.
//   - EVERY frame's draw callbacks run under the Lua Big Lock (lua_host.h), which the engine
//     thread also holds around all of ITS Lua — so the shared lua_State is single-threaded still.
//
// Stability notes (the north star):
//   - We never hook or share the game's renderer; each window is an independent DX11 device.
//   - Widget calls are inert unless we are mid-draw on the UI thread (drawing()), so a mod that
//     calls mod.ui.text() from init (the engine thread) is a harmless no-op, not a crash.
//   - A draw callback runs inside pcall; a fault disables just that panel (+ logs), like on_frame.
//   - The DX11/Win32 boilerplate is the stock Dear ImGui example (the trainer / osr_view model).

#include "ui.h"

#include "loader_internal.h"   // ml_log, OSS_ML_NAME / OSS_ML_VERSION
#include "lua_host.h"          // lh_lock / lh_unlock / lh_state  (the Lua Big Lock)
#include "executor.h"          // exec_armed / exec_main_tid / exec_ti_mgr

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <stdio.h>
#include <string.h>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

// forward-declared by imgui_impl_win32.h; the raw handler we feed messages to per-context.
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ── one managed window = its own device + swapchain + ImGui context ──────────
struct UiWindow {
    HWND                    hwnd;
    ID3D11Device           *dev;
    ID3D11DeviceContext    *devctx;
    IDXGISwapChain         *swap;
    ID3D11RenderTargetView *rtv;
    ImGuiContext           *imctx;
    bool                    overlay;   // true = the transparent in-game mirror
    bool                    visible;
};
static UiWindow g_main;   // the loader-owned window
static UiWindow g_over;   // the in-game overlay

static DWORD  g_ui_tid;                 // the UI thread (widgets are inert off it)
static bool   g_in_draw;                // true only between NewFrame and Render, on the UI thread
static HWND   g_game_hwnd;              // the game window the overlay tracks (may be NULL)
static int    g_key_main = VK_F8;       // main-window toggle
static int    g_key_over = VK_INSERT;   // overlay toggle
static volatile LONG g_started;         // ui_start idempotency
static volatile bool g_want_quit;       // ui_shutdown request
static volatile bool g_req_toggle_over; // a "toggle overlay" request from a button/other thread

// Only act on ImGui inside a live frame on the UI thread with a current context.  This makes
// every widget wrapper safe to call from anywhere: off-thread / outside a frame -> a no-op.
static inline bool drawing(void) {
    return g_in_draw && GetCurrentThreadId() == g_ui_tid && ImGui::GetCurrentContext() != NULL;
}

// ── the mod panel / window registries (LBL-protected: written from Lua on the engine thread
//    during init, read on the UI thread during draw — both hold the Lua Big Lock) ───────────
struct DrawReg { char name[96]; int ref; bool used; };
#define MAX_REG 64
static DrawReg g_panel[MAX_REG]; static int g_npanel;
static DrawReg g_win[MAX_REG];   static int g_nwin;

static int reg_add(DrawReg *arr, int *n, const char *name, int ref) {
    int idx = -1;
    for (int i = 0; i < *n; i++) if (!arr[i].used) { idx = i; break; }   // reuse a freed slot
    if (idx < 0) { if (*n >= MAX_REG) return -1; idx = (*n)++; }
    snprintf(arr[idx].name, sizeof arr[idx].name, "%s", name && *name ? name : "(unnamed)");
    arr[idx].ref = ref; arr[idx].used = true;
    return idx;
}
static int reg_used(DrawReg *arr, int n) { int c = 0; for (int i = 0; i < n; i++) if (arr[i].used) c++; return c; }

// ── DX11 device / swapchain (per window) — the stock Dear ImGui example helper ─
static bool create_rtv(UiWindow *w) {
    ID3D11Texture2D *back = NULL;
    if (FAILED(w->swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    HRESULT hr = w->dev->CreateRenderTargetView(back, NULL, &w->rtv);
    back->Release();
    return SUCCEEDED(hr);
}
static void cleanup_rtv(UiWindow *w) { if (w->rtv) { w->rtv->Release(); w->rtv = NULL; } }

static bool create_device(UiWindow *w, HWND h) {
    DXGI_SWAP_CHAIN_DESC sd; ZeroMemory(&sd, sizeof sd);
    sd.BufferCount        = 2;
    sd.BufferDesc.Format  = DXGI_FORMAT_R8G8B8A8_UNORM;   // has alpha -> the overlay can clear transparent
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = h;
    sd.SampleDesc.Count   = 1;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL lvls[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, lvls, 2,
                    D3D11_SDK_VERSION, &sd, &w->swap, &w->dev, &got, &w->devctx);
    if (FAILED(hr))   // no HW device (headless / RDP) -> the WARP software rasterizer
        hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, lvls, 2,
                    D3D11_SDK_VERSION, &sd, &w->swap, &w->dev, &got, &w->devctx);
    if (FAILED(hr)) return false;
    return create_rtv(w);
}
static void destroy_device(UiWindow *w) {
    cleanup_rtv(w);
    if (w->swap)   { w->swap->Release();   w->swap   = NULL; }
    if (w->devctx) { w->devctx->Release(); w->devctx = NULL; }
    if (w->dev)    { w->dev->Release();    w->dev    = NULL; }
}

// ── the transparent overlay window: DWM composites the alpha we clear to ──────
// A borderless top-most window whose whole client area is "glass" (extend-frame -1) with a
// null blur region -> DWM honours the per-pixel alpha of our DX surface, so clearing the render
// target to (0,0,0,0) shows the game through, and opaque ImGui panels float on top.  No
// WS_EX_LAYERED (we want the window INTERACTIVE while shown, not click-through), no renderer hook.
static void make_transparent(HWND h) {
    MARGINS m = { -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(h, &m);
    DWM_BLURBEHIND bb; ZeroMemory(&bb, sizeof bb);
    bb.dwFlags  = DWM_BB_ENABLE | DWM_BB_BLURREGION;
    bb.hRgnBlur = CreateRectRgn(0, 0, -1, -1);   // empty region: enable per-pixel alpha, no actual blur
    bb.fEnable  = TRUE;
    DwmEnableBlurBehindWindow(h, &bb);
    if (bb.hRgnBlur) DeleteObject(bb.hRgnBlur);
}

// Keep the overlay sized/positioned over the game's client area.  Only touch the window when the
// rect actually changes (a per-frame SetWindowPos would spam WM_SIZE -> swapchain churn).
static void track_overlay(void) {
    if (!g_over.hwnd || !g_game_hwnd || !IsWindow(g_game_hwnd)) return;
    RECT rc; if (!GetClientRect(g_game_hwnd, &rc)) return;
    POINT tl = { rc.left, rc.top };
    if (!ClientToScreen(g_game_hwnd, &tl)) return;
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    static int lx, ly, lw, lh;
    if (tl.x == lx && tl.y == ly && w == lw && h == lh) return;
    lx = tl.x; ly = tl.y; lw = w; lh = h;
    SetWindowPos(g_over.hwnd, HWND_TOPMOST, tl.x, tl.y, w, h, SWP_NOACTIVATE);
}

// ── the shared WndProc (routes to the right ImGui context by HWND) ────────────
static LRESULT WINAPI wndproc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    UiWindow *w = (UiWindow *)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (m == WM_NCCREATE) {                                   // stash our UiWindow* for every later msg
        CREATESTRUCTW *cs = (CREATESTRUCTW *)lp;
        w = (UiWindow *)cs->lpCreateParams;
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)w);
    }
    if (w && w->imctx) {                                      // select this window's context, then the backend
        ImGui::SetCurrentContext(w->imctx);
        if (ImGui_ImplWin32_WndProcHandler(h, m, wp, lp)) return true;
    }
    switch (m) {
    case WM_SIZE:
        if (w && w->dev && wp != SIZE_MINIMIZED) {
            cleanup_rtv(w);
            w->swap->ResizeBuffers(0, (UINT)LOWORD(lp), (UINT)HIWORD(lp), DXGI_FORMAT_UNKNOWN, 0);
            create_rtv(w);
        }
        return 0;
    case WM_CLOSE:                                            // hide, don't destroy (the toggle re-shows it)
        if (w) { w->visible = false; ShowWindow(h, SW_HIDE); }
        if (w == &g_over && g_game_hwnd) SetForegroundWindow(g_game_hwnd);
        return 0;
    case WM_DESTROY:
        if (w == &g_main) PostQuitMessage(0);                 // closing the main window ends the UI thread
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

// ── ImGui look ───────────────────────────────────────────────────────────────
static void apply_theme(void) {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6.0f; s.FrameRounding = 4.0f; s.GrabRounding = 4.0f; s.ScrollbarRounding = 4.0f;
    s.WindowBorderSize = 1.0f; s.WindowPadding = ImVec2(12, 12);
    s.FramePadding = ImVec2(8, 4); s.ItemSpacing = ImVec2(8, 6);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f, 0.22f, 0.30f, 1.0f);
    c[ImGuiCol_Header]        = ImVec4(0.14f, 0.30f, 0.40f, 0.70f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f, 0.40f, 0.52f, 0.90f);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.20f, 0.46f, 0.60f, 1.0f);
    c[ImGuiCol_Button]        = ImVec4(0.16f, 0.32f, 0.42f, 0.90f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.20f, 0.42f, 0.55f, 1.0f);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.24f, 0.52f, 0.66f, 1.0f);
    c[ImGuiCol_CheckMark]     = ImVec4(0.35f, 0.75f, 0.90f, 1.0f);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.30f, 0.65f, 0.82f, 1.0f);
    c[ImGuiCol_FrameBg]       = ImVec4(0.12f, 0.14f, 0.17f, 1.0f);
}
static void load_font(ImGuiIO &io) {
    const char *seg = "C:\\Windows\\Fonts\\segoeui.ttf";
    if (GetFileAttributesA(seg) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(seg, 18.0f);
    else io.Fonts->AddFontDefault();
}

// ── invoke one mod draw callback (a Lua closure ref) under the LBL ────────────
// Called from draw_frame, which already holds the LBL.  A fault disables just this callback.
static void call_draw(lua_State *L, DrawReg *r) {
    if (!L) return;
    lua_rawgeti(L, LUA_REGISTRYINDEX, r->ref);
    if (lua_pcall(L, 0, 0, 0) != 0) {
        ml_log("[ui] draw callback '%s' error (disabling): %s", r->name, lua_tostring(L, -1));
        lua_pop(L, 1);
        luaL_unref(L, LUA_REGISTRYINDEX, r->ref);   // safe: we hold the LBL
        r->used = false;
    }
}

// ── the built-in loader panel (pure C++, reads C loader state; no Lua) ────────
static void draw_builtin(UiWindow *w) {
    ImGui::TextUnformatted(OSS_ML_NAME "  " OSS_ML_VERSION);
    ImGui::TextDisabled("%s  |  %s", w->overlay ? "in-game overlay (mirror)" : "loader window",
                        exec_armed() ? "executor armed" : "executor idle");
    ImGui::Separator();
    ImGui::Text("engine thread : %u", exec_main_tid());
    ImGui::Text("input manager : 0x%08x", exec_ti_mgr());
    ImGui::Text("mod panels    : %d      mod windows : %d", reg_used(g_panel, g_npanel), reg_used(g_win, g_nwin));
    ImGui::Spacing();
    if (g_over.hwnd) {
        ImGui::Text("overlay: %s", g_over.visible ? "shown" : "hidden");
        ImGui::SameLine();
        if (ImGui::SmallButton(g_over.visible ? "hide##ov" : "show##ov")) g_req_toggle_over = true;
    } else {
        ImGui::TextDisabled("overlay: unavailable (no game window)");
    }
    ImGui::Spacing();
}

// MAIN + OVERLAY share this.  Runs under the LBL (set by draw_window).  Builds the host window
// with the built-in panel + every registered mod panel (as collapsing headers), then each
// registered mod window as its own floating window.  Identical in both contexts = the mirror.
static void draw_frame(UiWindow *w) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 24, vp->WorkPos.y + 24), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(460, 620), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(OSS_ML_NAME "##oss_host")) {
        draw_builtin(w);
        lua_State *L = lh_state();
        for (int i = 0; i < g_npanel; i++) {
            if (!g_panel[i].used) continue;
            char label[128];
            snprintf(label, sizeof label, "%s##oss_panel_%d", g_panel[i].name, i);   // unique id, shown name
            if (ImGui::CollapsingHeader(label, ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::PushID(i);
                call_draw(L, &g_panel[i]);
                ImGui::PopID();
            }
        }
    }
    ImGui::End();

    lua_State *L = lh_state();
    for (int i = 0; i < g_nwin; i++) {
        if (!g_win[i].used) continue;
        char label[128];
        snprintf(label, sizeof label, "%s##oss_win_%d", g_win[i].name, i);
        ImGui::SetNextWindowSize(ImVec2(360, 300), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(label)) {
            ImGui::PushID(1000 + i);
            call_draw(L, &g_win[i]);
            ImGui::PopID();
        }
        ImGui::End();
    }
}

static void render_window(UiWindow *w) {
    if (!w->rtv) return;
    ImGui::SetCurrentContext(w->imctx);
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    lh_lock();                 // LBL: the draw callbacks touch the shared lua_State
    g_in_draw = true;
    draw_frame(w);
    g_in_draw = false;
    lh_unlock();

    ImGui::Render();
    const float opaque[4] = { 0.09f, 0.10f, 0.12f, 1.0f };
    const float clear0[4] = { 0.0f,  0.0f,  0.0f,  0.0f };   // transparent for the overlay
    w->devctx->OMSetRenderTargets(1, &w->rtv, NULL);
    w->devctx->ClearRenderTargetView(w->rtv, w->overlay ? clear0 : opaque);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    w->swap->Present(1, 0);    // vsync — no busy-spin
}

// ══════════════════════════════════════════════════════════════════════════════
//  the `mod.ui.*` Lua binding (registration + a curated widget set)
// ══════════════════════════════════════════════════════════════════════════════
// Registration (mod.ui.panel / mod.ui.window): called during a mod's init on the engine thread,
// already under the LBL.  We ref the draw closure and slot it into the registry the UI thread reads.
static int reg_common(lua_State *L, DrawReg *arr, int *n) {
    const char *name = luaL_optstring(L, 1, "(unnamed)");
    if (!lua_isfunction(L, 2)) { luaL_error(L, "mod.ui.panel/window(name, draw_fn): draw_fn must be a function"); return 0; }
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int idx = reg_add(arr, n, name, ref);
    if (idx < 0) { luaL_unref(L, LUA_REGISTRYINDEX, ref); ml_log("[ui] registry full — dropping '%s'", name); lua_pushnil(L); return 1; }
    lua_pushinteger(L, idx);
    return 1;
}
static int l_ui_panel(lua_State *L)  { return reg_common(L, g_panel, &g_npanel); }
static int l_ui_window(lua_State *L) { return reg_common(L, g_win,   &g_nwin);   }

// Widgets — inert unless mid-draw on the UI thread (see drawing()).  Kept minimal + curated;
// grows as mods need it.  Each returns sensible defaults when inert so a mod's `if button then`
// logic never misbehaves off-frame.
static int l_ui_text(lua_State *L) {
    if (drawing()) { const char *s = lua_tostring(L, 1); ImGui::TextUnformatted(s ? s : ""); }
    return 0;
}
static int l_ui_text_disabled(lua_State *L) {
    if (drawing()) { const char *s = lua_tostring(L, 1); ImGui::TextDisabled("%s", s ? s : ""); }
    return 0;
}
static int l_ui_text_wrapped(lua_State *L) {
    if (drawing()) { const char *s = lua_tostring(L, 1); ImGui::TextWrapped("%s", s ? s : ""); }
    return 0;
}
static int l_ui_text_colored(lua_State *L) {
    if (drawing()) {
        float r = (float)luaL_optnumber(L, 1, 1), g = (float)luaL_optnumber(L, 2, 1),
              b = (float)luaL_optnumber(L, 3, 1), a = (float)luaL_optnumber(L, 4, 1);
        const char *s = lua_tostring(L, 5);
        ImGui::TextColored(ImVec4(r, g, b, a), "%s", s ? s : "");
    }
    return 0;
}
static int l_ui_bullet(lua_State *L) {
    if (drawing()) { const char *s = lua_tostring(L, 1); ImGui::BulletText("%s", s ? s : ""); }
    return 0;
}
static int l_ui_separator(lua_State *L)  { (void)L; if (drawing()) ImGui::Separator();  return 0; }
static int l_ui_spacing(lua_State *L)    { (void)L; if (drawing()) ImGui::Spacing();    return 0; }
static int l_ui_same_line(lua_State *L)  { (void)L; if (drawing()) ImGui::SameLine();   return 0; }

static int l_ui_button(lua_State *L) {
    bool r = false;
    if (drawing()) {
        const char *s = luaL_optstring(L, 1, "button");
        float w = (float)luaL_optnumber(L, 2, 0), h = (float)luaL_optnumber(L, 3, 0);
        r = ImGui::Button(s, ImVec2(w, h));
    }
    lua_pushboolean(L, r); return 1;
}
static int l_ui_small_button(lua_State *L) {
    bool r = false;
    if (drawing()) r = ImGui::SmallButton(luaL_optstring(L, 1, "button"));
    lua_pushboolean(L, r); return 1;
}
static int l_ui_checkbox(lua_State *L) {   // (label, bool) -> new_bool, changed
    const char *s = luaL_optstring(L, 1, "checkbox");
    bool v = lua_toboolean(L, 2) != 0, changed = false;
    if (drawing()) changed = ImGui::Checkbox(s, &v);
    lua_pushboolean(L, v); lua_pushboolean(L, changed); return 2;
}
static int l_ui_slider_int(lua_State *L) {   // (label, v, min, max) -> new_v, changed
    const char *s = luaL_optstring(L, 1, "slider");
    int v = (int)luaL_optinteger(L, 2, 0), lo = (int)luaL_optinteger(L, 3, 0), hi = (int)luaL_optinteger(L, 4, 100);
    bool changed = false;
    if (drawing()) changed = ImGui::SliderInt(s, &v, lo, hi);
    lua_pushinteger(L, v); lua_pushboolean(L, changed); return 2;
}
static int l_ui_slider_float(lua_State *L) {   // (label, v, min, max) -> new_v, changed
    const char *s = luaL_optstring(L, 1, "slider");
    float v = (float)luaL_optnumber(L, 2, 0), lo = (float)luaL_optnumber(L, 3, 0), hi = (float)luaL_optnumber(L, 4, 1);
    bool changed = false;
    if (drawing()) changed = ImGui::SliderFloat(s, &v, lo, hi);
    lua_pushnumber(L, v); lua_pushboolean(L, changed); return 2;
}
static int l_ui_header(lua_State *L) {   // (label) -> open (a CollapsingHeader inside a panel)
    bool open = false;
    if (drawing()) open = ImGui::CollapsingHeader(luaL_optstring(L, 1, "header"));
    lua_pushboolean(L, open); return 1;
}
static int l_ui_progress(lua_State *L) {   // (frac [, text])
    if (drawing()) {
        float f = (float)luaL_optnumber(L, 1, 0);
        const char *t = lua_tostring(L, 2);
        ImGui::ProgressBar(f, ImVec2(-1, 0), t);
    }
    return 0;
}

// Overlay controls (callable from any thread — they just set request flags the UI thread reads).
static int l_ui_overlay_toggle(lua_State *L)  { (void)L; g_req_toggle_over = true; return 0; }
static int l_ui_overlay_visible(lua_State *L) { lua_pushboolean(L, g_over.hwnd && g_over.visible); return 1; }

static int g_ui_ref = LUA_NOREF;
extern "C" void ui_init(lua_State *L) {
    lua_newtable(L);
    #define FN(name, fn) do { lua_pushcfunction(L, fn); lua_setfield(L, -2, name); } while (0)
    FN("panel",           l_ui_panel);
    FN("window",          l_ui_window);
    FN("text",            l_ui_text);
    FN("text_disabled",   l_ui_text_disabled);
    FN("text_wrapped",    l_ui_text_wrapped);
    FN("text_colored",    l_ui_text_colored);
    FN("bullet",          l_ui_bullet);
    FN("separator",       l_ui_separator);
    FN("spacing",         l_ui_spacing);
    FN("same_line",       l_ui_same_line);
    FN("button",          l_ui_button);
    FN("small_button",    l_ui_small_button);
    FN("checkbox",        l_ui_checkbox);
    FN("slider_int",      l_ui_slider_int);
    FN("slider_float",    l_ui_slider_float);
    FN("header",          l_ui_header);
    FN("progress",        l_ui_progress);
    FN("overlay_toggle",  l_ui_overlay_toggle);
    FN("overlay_visible", l_ui_overlay_visible);
    #undef FN
    g_ui_ref = luaL_ref(L, LUA_REGISTRYINDEX);   // keep the shared table alive
    ml_log("[ui] mod.ui table built");
}
extern "C" void ui_push_table(lua_State *L) {
    if (g_ui_ref == LUA_NOREF) { lua_pushnil(L); return; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_ui_ref);
}

// ══════════════════════════════════════════════════════════════════════════════
//  the UI thread
// ══════════════════════════════════════════════════════════════════════════════
static bool init_imgui(UiWindow *w) {
    IMGUI_CHECKVERSION();
    w->imctx = ImGui::CreateContext();               // a context PER window (multi-context)
    ImGui::SetCurrentContext(w->imctx);
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = NULL;                            // don't litter the game dir with imgui.ini
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    load_font(io);
    apply_theme();
    if (!ImGui_ImplWin32_Init(w->hwnd)) return false;
    if (!ImGui_ImplDX11_Init(w->dev, w->devctx)) return false;
    return true;
}
static void shutdown_imgui(UiWindow *w) {
    if (!w->imctx) return;
    ImGui::SetCurrentContext(w->imctx);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext(w->imctx);
    w->imctx = NULL;
}

static void toggle_window(UiWindow *w) {
    if (!w->hwnd) return;
    w->visible = !w->visible;
    if (w->visible) {
        if (w->overlay) track_overlay();
        ShowWindow(w->hwnd, SW_SHOW);
        SetForegroundWindow(w->hwnd);
    } else {
        ShowWindow(w->hwnd, SW_HIDE);
        if (w->overlay && g_game_hwnd) SetForegroundWindow(g_game_hwnd);
    }
}

static void poll_hotkeys(void) {
    static int pm, po;
    int m = (GetAsyncKeyState(g_key_main) & 0x8000) ? 1 : 0;
    if (m && !pm) toggle_window(&g_main);
    pm = m;
    int o = g_over.hwnd ? ((GetAsyncKeyState(g_key_over) & 0x8000) ? 1 : 0) : 0;
    if ((o && !po) || g_req_toggle_over) { g_req_toggle_over = false; if (g_over.hwnd) toggle_window(&g_over); }
    po = o;
}

static DWORD WINAPI ui_thread(void *unused) {
    (void)unused;
    g_ui_tid = GetCurrentThreadId();

    WNDCLASSEXW wc; ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc; wc.style = CS_CLASSDC; wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL); wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;   // DX paints everything; no GDI background (needed for the glass overlay)
    wc.lpszClassName = L"OSSModLoaderUI";
    if (!RegisterClassExW(&wc)) { ml_log("[ui] RegisterClassExW failed (%lu)", GetLastError()); return 1; }

    // ── the main loader window ────────────────────────────────────────────────
    g_main.overlay = false;
    g_main.hwnd = CreateWindowExW(0, wc.lpszClassName, L"SotES Mod Loader",
                                  WS_OVERLAPPEDWINDOW, 80, 80, 500, 720,
                                  NULL, NULL, wc.hInstance, &g_main);
    if (!g_main.hwnd || !create_device(&g_main, g_main.hwnd)) {
        ml_log("[ui] main window/device creation FAILED — UI disabled");
        destroy_device(&g_main);
        if (g_main.hwnd) DestroyWindow(g_main.hwnd);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    if (!init_imgui(&g_main)) { ml_log("[ui] main ImGui init FAILED — UI disabled"); return 1; }
    ShowWindow(g_main.hwnd, SW_SHOWNORMAL); UpdateWindow(g_main.hwnd);
    g_main.visible = true;
    ml_log("[ui] main window up (hwnd %p)", (void *)g_main.hwnd);

    // ── the in-game overlay (best-effort: if it fails, the main window still works) ──
    if (g_game_hwnd) {
        g_over.overlay = true;
        RECT rc = { 0, 0, 640, 480 };
        POINT tl = { 0, 0 };
        if (GetClientRect(g_game_hwnd, &rc)) { tl.x = rc.left; tl.y = rc.top; ClientToScreen(g_game_hwnd, &tl); }
        int ow = rc.right - rc.left, oh = rc.bottom - rc.top; if (ow <= 0) ow = 640; if (oh <= 0) oh = 480;
        g_over.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, wc.lpszClassName, L"",
                                      WS_POPUP, tl.x, tl.y, ow, oh, NULL, NULL, wc.hInstance, &g_over);
        if (g_over.hwnd && create_device(&g_over, g_over.hwnd) && init_imgui(&g_over)) {
            make_transparent(g_over.hwnd);
            ShowWindow(g_over.hwnd, SW_HIDE);   // hidden until the hotkey
            g_over.visible = false;
            ml_log("[ui] overlay ready (hwnd %p) — toggle with the overlay hotkey", (void *)g_over.hwnd);
        } else {
            ml_log("[ui] overlay creation FAILED — main window only");
            shutdown_imgui(&g_over);
            destroy_device(&g_over);
            if (g_over.hwnd) { DestroyWindow(g_over.hwnd); g_over.hwnd = NULL; }
        }
    }

    // ── the pump + render loop ────────────────────────────────────────────────
    bool running = true;
    while (running && !g_want_quit) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running || g_want_quit) break;

        poll_hotkeys();

        bool drew = false;
        if (g_main.visible) { render_window(&g_main); drew = true; }
        if (g_over.hwnd && g_over.visible) { track_overlay(); render_window(&g_over); drew = true; }
        if (!drew) Sleep(60);   // nothing visible -> idle (no Present to throttle us)
    }

    shutdown_imgui(&g_over);  destroy_device(&g_over);  if (g_over.hwnd) DestroyWindow(g_over.hwnd);
    shutdown_imgui(&g_main);  destroy_device(&g_main);  if (g_main.hwnd) DestroyWindow(g_main.hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ml_log("[ui] UI thread exited");
    return 0;
}

extern "C" void ui_start(void *game_hwnd, int key_main, int key_overlay) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;   // once
    g_game_hwnd = (HWND)game_hwnd;
    if (key_main)    g_key_main = key_main;
    if (key_overlay) g_key_over = key_overlay;
    HANDLE t = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else ml_log("[ui] CreateThread failed (%lu)", GetLastError());
}

extern "C" void ui_shutdown(void) {
    g_want_quit = true;
    if (g_main.hwnd) PostMessageW(g_main.hwnd, WM_CLOSE, 0, 0);   // nudge the pump out of any wait
}
