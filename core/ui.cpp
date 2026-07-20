// core/ui.cpp — the ImGui/DX11 UI host (P5, decoupled).  See ui.h for the contract.
//
// A loader-owned companion window on its own thread, fed by a LOCK-FREE bidirectional pipeline
// to/from the game's engine thread — the whole point being that the UI never stalls the game and
// the game never stalls the UI (an earlier lock-based version coupled the two and caused lag).
//
//   engine thread  --(ui_build)-->  records a plain-data SNAPSHOT of the mod.ui panels, publishes
//                                   it through a lock-free TRIPLE BUFFER (latest-wins; an
//                                   unconsumed frame is simply overwritten).  Runs the mod Lua
//                                   here — Lua stays single-threaded on the engine thread.
//   UI thread      <--(snapshot)--  fetches the latest snapshot lock-free and REPLAYS it into
//                                   ImGui; interactions go back through per-widget ATOMIC SLOTS
//                                   (latest value wins; button clicks sticky-OR until drained).
//                                   Event-driven: it sleeps until input / a dirty signal / a
//                                   heartbeat, so idle cost is ~zero.  Never touches lua_State.
//
// The mod-facing `mod.ui` API is immediate-mode on the surface (panel/window + widget calls) but
// snapshot-and-replay underneath.  A UI-thread live-value cache keeps sliders/checkboxes smooth
// between the (throttled) engine builds; input has ~1 build-cycle latency, imperceptible.
//
// The in-game overlay is deferred (companion window only) — it will later consume the SAME
// snapshot through a renderer-hook backend, so mods won't change.

#include "ui.h"

#include "loader_internal.h"   // ml_log, OSS_ML_NAME / OSS_ML_VERSION
#include "lua_host.h"          // lh_state (engine thread only)
#include "executor.h"          // exec_armed / exec_main_tid / exec_ti_mgr
#include "ddraw_present.h"     // ddp_draw_background — mirror the captured game frame behind the UI

extern "C" {
#include "lua.h"
#include "lauxlib.h"
}

#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <atomic>
#include <vector>
#include <string>

#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern "C" int config_get_int(const char *key, int def);   // oss_loader.cfg reader (config.c)

// ════════════════════════════ the snapshot data model ════════════════════════════
enum CmdType {
    C_TEXT, C_DISABLED, C_WRAPPED, C_COLORED, C_BULLET,
    C_SEP, C_SPACING, C_SAMELINE,
    C_BUTTON, C_SMALLBUTTON, C_CHECKBOX, C_SLIDER_INT, C_SLIDER_FLOAT, C_PROGRESS,
};
struct Cmd {
    CmdType t;
    std::string s;         // label / text
    float a, b, c, d;      // value/min/max  or  r,g,b,a
    int   ix;              // interactive-widget index within the surface (-1 = non-interactive)
};
struct Surface {
    std::string name;
    bool is_window;
    bool collapsed;   // panels only: start collapsed (mod.ui.panel(name, fn, true))
    std::vector<Cmd> cmds;
};
struct SnapStatus { int armed; uint32_t tid, ti_mgr; int npanel, nwindow; };
struct Snapshot {
    SnapStatus status;
    std::vector<Surface> surfaces;
};

// ── lock-free triple buffer (engine publishes, UI consumes; latest-wins) ──────
// g_ctl packs [fresh:1 @bit2][mid:2 @bits0-1].  g_write_ix is engine-private, g_read_ix UI-private;
// the three indices are always a permutation of {0,1,2}, so producer + consumer never touch the
// same buffer.  Publish/fetch swap their private index with `mid` via a CAS on g_ctl.
static Snapshot g_buf[3];
static std::atomic<uint32_t> g_ctl{ (0u << 2) | 2u };   // fresh=0, mid=2
static int g_write_ix = 0;      // engine-thread private
static int g_read_ix  = 1;      // UI-thread private

static void snap_publish(void) {          // engine: buf[g_write_ix] is filled — make it the latest
    uint32_t old = g_ctl.load(std::memory_order_relaxed), neu;
    do { neu = (1u << 2) | (uint32_t)g_write_ix; }
    while (!g_ctl.compare_exchange_weak(old, neu, std::memory_order_release, std::memory_order_relaxed));
    g_write_ix = (int)(old & 3u);          // the displaced mid becomes our next write buffer
}
static bool snap_fetch(void) {             // UI: take the latest if fresh; returns true if updated
    if (!(g_ctl.load(std::memory_order_acquire) & (1u << 2))) return false;
    uint32_t old = g_ctl.load(std::memory_order_relaxed), neu;
    do { neu = (0u << 2) | (uint32_t)g_read_ix; }
    while (!g_ctl.compare_exchange_weak(old, neu, std::memory_order_acquire, std::memory_order_relaxed));
    g_read_ix = (int)(old & 3u);           // the displaced mid (freshly published) becomes our read buffer
    return true;
}

// ── per-widget atomic input slots (UI -> engine; latest-wins, clicks sticky) ──
#define MAX_SURFACES 64
#define MAX_WIDGETS  128
#define N_SLOTS      (MAX_SURFACES * MAX_WIDGETS)
static const uint64_t IN_CLICKED = 1ull << 32;
static const uint64_t IN_CHANGED = 1ull << 33;
static std::atomic<uint64_t> g_input[N_SLOTS];   // [value:32][changed@32][clicked@33]

static inline int slot_id(int surf, int wix) {
    if (surf < 0 || surf >= MAX_SURFACES || wix < 0 || wix >= MAX_WIDGETS) return -1;
    return surf * MAX_WIDGETS + wix;
}
static void input_click(int surf, int wix) {          // UI: accumulate a click
    int id = slot_id(surf, wix); if (id >= 0) g_input[id].fetch_or(IN_CLICKED, std::memory_order_release);
}
static void input_value(int surf, int wix, uint32_t bits) {   // UI: latest value wins
    int id = slot_id(surf, wix); if (id >= 0) g_input[id].store((uint64_t)bits | IN_CHANGED, std::memory_order_release);
}
static uint64_t input_take(int surf, int wix) {       // engine: read + clear
    int id = slot_id(surf, wix); return id >= 0 ? g_input[id].exchange(0, std::memory_order_acquire) : 0;
}

// ════════════════════════════ engine-side: the build ════════════════════════════
// The mod registry (touched only on the engine thread: registered from mod init, read at build).
struct Reg { std::string name; int ref; bool is_window; bool used; bool collapsed; };
static std::vector<Reg> g_reg;

static bool  g_building = false;       // true only inside ui_build, on the engine thread
static std::vector<Cmd> *g_cur = NULL; // the current surface's cmd list
static int   g_cur_surf = -1;          // current surface index (for input slot ids)
static int   g_cur_wix  = 0;           // interactive-widget counter within the current surface

static volatile LONG g_started = 0;    // ui_start ran
static DWORD g_build_ms = 0;           // last build tick
static DWORD g_build_interval = 33;    // ~30 Hz
static uint64_t g_last_hash = 0;
static HANDLE g_dirty = NULL;          // engine SetEvents on a changed publish; UI waits on it

static uint64_t snap_hash(const Snapshot &s) {   // FNV-1a over the visible content (cheap change test)
    uint64_t h = 1469598103934665603ull;
    #define MIX(p, n) do { const unsigned char *bp=(const unsigned char*)(p); for (size_t i=0;i<(n);i++){ h^=bp[i]; h*=1099511628211ull; } } while (0)
    MIX(&s.status, sizeof s.status);
    for (const Surface &sf : s.surfaces) {
        MIX(sf.name.data(), sf.name.size()); MIX(&sf.is_window, sizeof sf.is_window); MIX(&sf.collapsed, sizeof sf.collapsed);
        for (const Cmd &c : sf.cmds) { MIX(&c.t, sizeof c.t); MIX(c.s.data(), c.s.size()); MIX(&c.a, 4*sizeof(float)); MIX(&c.ix, sizeof c.ix); }
    }
    #undef MIX
    return h;
}

void ui_build(void) {
    if (!g_started) return;
    lua_State *L = lh_state();
    if (!L) return;
    DWORD now = GetTickCount();
    if (now - g_build_ms < g_build_interval) return;   // throttle the engine-side cost
    g_build_ms = now;

    Snapshot &snap = g_buf[g_write_ix];
    snap.surfaces.clear();
    snap.status.armed  = exec_armed();
    snap.status.tid    = exec_main_tid();
    snap.status.ti_mgr = exec_ti_mgr();
    int npanel = 0, nwindow = 0;

    g_building = true;
    int surf = 0;
    for (Reg &r : g_reg) {
        if (!r.used) continue;
        if (surf >= MAX_SURFACES) break;
        snap.surfaces.push_back(Surface{});
        Surface &s = snap.surfaces.back();
        s.name = r.name; s.is_window = r.is_window; s.collapsed = r.collapsed;
        r.is_window ? nwindow++ : npanel++;
        g_cur = &s.cmds; g_cur_surf = surf; g_cur_wix = 0;
        lua_rawgeti(L, LUA_REGISTRYINDEX, r.ref);
        if (lua_pcall(L, 0, 0, 0) != 0) {              // a faulting callback disables just this surface
            ml_log("[ui] '%s' draw error (disabling): %s", r.name.c_str(), lua_tostring(L, -1));
            lua_pop(L, 1);
            luaL_unref(L, LUA_REGISTRYINDEX, r.ref);
            r.used = false;
            snap.surfaces.pop_back();
            r.is_window ? nwindow-- : npanel--;
            continue;
        }
        surf++;
    }
    g_building = false; g_cur = NULL; g_cur_surf = -1;
    snap.status.npanel = npanel; snap.status.nwindow = nwindow;

    uint64_t h = snap_hash(snap);          // publish + wake only when something actually changed
    if (h != g_last_hash) {
        g_last_hash = h;
        snap_publish();
        if (g_dirty) SetEvent(g_dirty);
    }
}

// ════════════════════════════ the `mod.ui` Lua binding ════════════════════════════
// Registration runs during a mod's init on the engine thread (single-threaded Lua).
static int reg_common(lua_State *L, bool is_window) {
    const char *name = luaL_optstring(L, 1, "(unnamed)");
    if (!lua_isfunction(L, 2)) return luaL_error(L, "mod.ui.panel/window(name, draw_fn): draw_fn must be a function");
    lua_pushvalue(L, 2);
    int ref = luaL_ref(L, LUA_REGISTRYINDEX);
    int idx = -1;
    for (size_t i = 0; i < g_reg.size(); i++) if (!g_reg[i].used) { idx = (int)i; break; }
    // panels take an optional 3rd arg: collapsed-by-default (windows ignore it).
    Reg r; r.name = name; r.ref = ref; r.is_window = is_window; r.used = true;
    r.collapsed = !is_window && lua_toboolean(L, 3);
    if (idx >= 0) g_reg[idx] = r; else { idx = (int)g_reg.size(); g_reg.push_back(r); }
    lua_pushinteger(L, idx);
    return 1;
}
static int l_ui_panel(lua_State *L)  { return reg_common(L, false); }
static int l_ui_window(lua_State *L) { return reg_common(L, true);  }

// Widgets — active only while building (engine thread, inside ui_build).  Off-build they are inert
// no-ops that return sensible defaults, so calling one from the wrong place can't crash.
static void emit(const Cmd &c) { if (g_cur) g_cur->push_back(c); }
static Cmd mk(CmdType t) { Cmd c; c.t = t; c.a = c.b = c.c = c.d = 0; c.ix = -1; return c; }

static int l_ui_text(lua_State *L)          { if (g_building) { Cmd c=mk(C_TEXT);     c.s=luaL_optstring(L,1,""); emit(c); } return 0; }
static int l_ui_text_disabled(lua_State *L) { if (g_building) { Cmd c=mk(C_DISABLED); c.s=luaL_optstring(L,1,""); emit(c); } return 0; }
static int l_ui_text_wrapped(lua_State *L)  { if (g_building) { Cmd c=mk(C_WRAPPED);  c.s=luaL_optstring(L,1,""); emit(c); } return 0; }
static int l_ui_bullet(lua_State *L)        { if (g_building) { Cmd c=mk(C_BULLET);   c.s=luaL_optstring(L,1,""); emit(c); } return 0; }
static int l_ui_separator(lua_State *L)     { (void)L; if (g_building) emit(mk(C_SEP));     return 0; }
static int l_ui_spacing(lua_State *L)       { (void)L; if (g_building) emit(mk(C_SPACING)); return 0; }
static int l_ui_same_line(lua_State *L)     { (void)L; if (g_building) emit(mk(C_SAMELINE));return 0; }
static int l_ui_text_colored(lua_State *L) {
    if (g_building) {
        Cmd c = mk(C_COLORED);
        c.a=(float)luaL_optnumber(L,1,1); c.b=(float)luaL_optnumber(L,2,1);
        c.c=(float)luaL_optnumber(L,3,1); c.d=(float)luaL_optnumber(L,4,1);
        c.s = luaL_optstring(L,5,""); emit(c);
    }
    return 0;
}
static int l_ui_progress(lua_State *L) {
    if (g_building) { Cmd c=mk(C_PROGRESS); c.a=(float)luaL_optnumber(L,1,0); c.s=luaL_optstring(L,2,""); emit(c); }
    return 0;
}
static int l_ui_button(lua_State *L) {
    bool clicked = false;
    if (g_building) {
        Cmd c = mk(C_BUTTON); c.s = luaL_optstring(L,1,"button");
        c.a = (float)luaL_optnumber(L,2,0); c.b = (float)luaL_optnumber(L,3,0);
        c.ix = g_cur_wix++;
        clicked = (input_take(g_cur_surf, c.ix) & IN_CLICKED) != 0;
        emit(c);
    }
    lua_pushboolean(L, clicked); return 1;
}
static int l_ui_small_button(lua_State *L) {
    bool clicked = false;
    if (g_building) {
        Cmd c = mk(C_SMALLBUTTON); c.s = luaL_optstring(L,1,"button"); c.ix = g_cur_wix++;
        clicked = (input_take(g_cur_surf, c.ix) & IN_CLICKED) != 0;
        emit(c);
    }
    lua_pushboolean(L, clicked); return 1;
}
static int l_ui_checkbox(lua_State *L) {     // (label, value) -> new_value, changed
    const char *s = luaL_optstring(L,1,"checkbox");
    bool v = lua_toboolean(L,2) != 0, changed = false;
    if (g_building) {
        int wix = g_cur_wix++;
        uint64_t r = input_take(g_cur_surf, wix);
        if (r & IN_CHANGED) { v = (uint32_t)(r & 0xffffffffu) != 0; changed = true; }
        Cmd c = mk(C_CHECKBOX); c.s = s; c.a = v ? 1.f : 0.f; c.ix = wix; emit(c);
    }
    lua_pushboolean(L, v); lua_pushboolean(L, changed); return 2;
}
static int l_ui_slider_int(lua_State *L) {   // (label, v, min, max) -> new_v, changed
    const char *s = luaL_optstring(L,1,"slider");
    int v = (int)luaL_optinteger(L,2,0), lo=(int)luaL_optinteger(L,3,0), hi=(int)luaL_optinteger(L,4,100);
    bool changed = false;
    if (g_building) {
        int wix = g_cur_wix++;
        uint64_t r = input_take(g_cur_surf, wix);
        if (r & IN_CHANGED) { v = (int)(uint32_t)(r & 0xffffffffu); changed = true; }
        Cmd c = mk(C_SLIDER_INT); c.s = s; c.a = (float)v; c.b = (float)lo; c.c = (float)hi; c.ix = wix; emit(c);
    }
    lua_pushinteger(L, v); lua_pushboolean(L, changed); return 2;
}
static int l_ui_slider_float(lua_State *L) { // (label, v, min, max) -> new_v, changed
    const char *s = luaL_optstring(L,1,"slider");
    float v=(float)luaL_optnumber(L,2,0), lo=(float)luaL_optnumber(L,3,0), hi=(float)luaL_optnumber(L,4,1);
    bool changed = false;
    if (g_building) {
        int wix = g_cur_wix++;
        uint64_t r = input_take(g_cur_surf, wix);
        if (r & IN_CHANGED) { uint32_t bits=(uint32_t)(r & 0xffffffffu); memcpy(&v,&bits,4); changed = true; }
        Cmd c = mk(C_SLIDER_FLOAT); c.s = s; c.a = v; c.b = lo; c.c = hi; c.ix = wix; emit(c);
    }
    lua_pushnumber(L, v); lua_pushboolean(L, changed); return 2;
}

static int g_ui_ref = LUA_NOREF;
extern "C" void ui_init(lua_State *L) {
    lua_newtable(L);
    #define FN(name, fn) do { lua_pushcfunction(L, fn); lua_setfield(L, -2, name); } while (0)
    FN("panel", l_ui_panel);          FN("window", l_ui_window);
    FN("text", l_ui_text);            FN("text_disabled", l_ui_text_disabled);
    FN("text_wrapped", l_ui_text_wrapped); FN("text_colored", l_ui_text_colored);
    FN("bullet", l_ui_bullet);        FN("separator", l_ui_separator);
    FN("spacing", l_ui_spacing);      FN("same_line", l_ui_same_line);
    FN("button", l_ui_button);        FN("small_button", l_ui_small_button);
    FN("checkbox", l_ui_checkbox);    FN("slider_int", l_ui_slider_int);
    FN("slider_float", l_ui_slider_float); FN("progress", l_ui_progress);
    #undef FN
    g_ui_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ml_log("[ui] mod.ui table built");
}
extern "C" void ui_push_table(lua_State *L) {
    if (g_ui_ref == LUA_NOREF) { lua_pushnil(L); return; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_ui_ref);
}

// ════════════════════════════ UI thread: window + DX11 + replay ════════════════════════════
static HWND                    g_hwnd;
static ID3D11Device           *g_dev;
static ID3D11DeviceContext    *g_devctx;
static IDXGISwapChain         *g_swap;
static ID3D11RenderTargetView *g_rtv;
static bool                    g_visible = true;
static int                     g_key_toggle = VK_F10;   // F8 collides with SotES' in-game menu key; ui_key overrides

// UI-thread live-value cache — lets sliders/checkboxes stay smooth between the (throttled) engine
// builds: ImGui owns the value while the user drags; we only re-seed from the snapshot when the mod
// changed it AND the widget isn't being actively edited (so the engine's lagged sample can't fight
// an in-progress drag).
struct Live { bool init; bool active; float val; float seed; };
static Live g_live[N_SLOTS];

static bool create_rtv(void) {
    ID3D11Texture2D *back = NULL;
    if (FAILED(g_swap->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
    HRESULT hr = g_dev->CreateRenderTargetView(back, NULL, &g_rtv); back->Release();
    return SUCCEEDED(hr);
}
static void cleanup_rtv(void) { if (g_rtv) { g_rtv->Release(); g_rtv = NULL; } }
static bool create_device(HWND h) {
    DXGI_SWAP_CHAIN_DESC sd; ZeroMemory(&sd, sizeof sd);
    sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.OutputWindow = h;
    sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, lv, 2,
                    D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_devctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, lv, 2,
                    D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_devctx);
    if (FAILED(hr)) return false;
    return create_rtv();
}
static void destroy_device(void) {
    cleanup_rtv();
    if (g_swap)   { g_swap->Release();   g_swap = NULL; }
    if (g_devctx) { g_devctx->Release(); g_devctx = NULL; }
    if (g_dev)    { g_dev->Release();    g_dev = NULL; }
}

static LRESULT WINAPI wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (ImGui_ImplWin32_WndProcHandler(h, m, w, l)) return true;
    switch (m) {
    case WM_SIZE:
        if (g_dev && w != SIZE_MINIMIZED) { cleanup_rtv(); g_swap->ResizeBuffers(0, (UINT)LOWORD(l), (UINT)HIWORD(l), DXGI_FORMAT_UNKNOWN, 0); create_rtv(); ui_mark_resized(); }
        return 0;
    case WM_CLOSE:  g_visible = false; ShowWindow(h, SW_HIDE); return 0;   // hide, not destroy (toggle re-shows)
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

static void apply_theme(void) {
    ImGui::StyleColorsDark();
    ImGuiStyle &s = ImGui::GetStyle();
    s.WindowRounding = 6; s.FrameRounding = 4; s.GrabRounding = 4; s.ScrollbarRounding = 4;
    s.WindowPadding = ImVec2(12,12); s.FramePadding = ImVec2(8,4); s.ItemSpacing = ImVec2(8,6);
    ImVec4 *c = s.Colors;
    c[ImGuiCol_TitleBgActive] = ImVec4(0.10f,0.22f,0.30f,1);
    c[ImGuiCol_Header]        = ImVec4(0.14f,0.30f,0.40f,0.7f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.18f,0.40f,0.52f,0.9f);
    c[ImGuiCol_HeaderActive]  = ImVec4(0.20f,0.46f,0.60f,1);
    c[ImGuiCol_Button]        = ImVec4(0.16f,0.32f,0.42f,0.9f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.20f,0.42f,0.55f,1);
    c[ImGuiCol_ButtonActive]  = ImVec4(0.24f,0.52f,0.66f,1);
    c[ImGuiCol_CheckMark]     = ImVec4(0.35f,0.75f,0.90f,1);
    c[ImGuiCol_SliderGrab]    = ImVec4(0.30f,0.65f,0.82f,1);
    c[ImGuiCol_FrameBg]       = ImVec4(0.12f,0.14f,0.17f,1);
}

// Replay one surface's recorded cmds into ImGui, routing interactions back to the atomic slots.
static void replay_cmds(int surf, const std::vector<Cmd> &cmds) {
    for (const Cmd &c : cmds) {
        switch (c.t) {
        case C_TEXT:     ImGui::TextUnformatted(c.s.c_str()); break;
        case C_DISABLED: ImGui::TextDisabled("%s", c.s.c_str()); break;
        case C_WRAPPED:  ImGui::TextWrapped("%s", c.s.c_str()); break;
        case C_COLORED:  ImGui::TextColored(ImVec4(c.a,c.b,c.c,c.d), "%s", c.s.c_str()); break;
        case C_BULLET:   ImGui::BulletText("%s", c.s.c_str()); break;
        case C_SEP:      ImGui::Separator(); break;
        case C_SPACING:  ImGui::Spacing(); break;
        case C_SAMELINE: ImGui::SameLine(); break;
        case C_PROGRESS: ImGui::ProgressBar(c.a, ImVec2(-1,0), c.s.empty()?NULL:c.s.c_str()); break;
        case C_BUTTON:      if (ImGui::Button(c.s.c_str(), ImVec2(c.a,c.b))) input_click(surf, c.ix); break;
        case C_SMALLBUTTON: if (ImGui::SmallButton(c.s.c_str()))            input_click(surf, c.ix); break;
        case C_CHECKBOX: {
            int id = slot_id(surf, c.ix); Live &lv = g_live[id >= 0 ? id : 0];
            if (!lv.init || (c.a != lv.seed && !lv.active)) { lv.val = c.a; lv.init = true; }
            lv.seed = c.a;
            bool v = lv.val != 0;
            if (ImGui::Checkbox(c.s.c_str(), &v)) { lv.val = v?1.f:0.f; input_value(surf, c.ix, v?1u:0u); }
            lv.active = ImGui::IsItemActive();
            break; }
        case C_SLIDER_INT: {
            int id = slot_id(surf, c.ix); Live &lv = g_live[id >= 0 ? id : 0];
            if (!lv.init || (c.a != lv.seed && !lv.active)) { lv.val = c.a; lv.init = true; }
            lv.seed = c.a;
            int v = (int)lv.val;
            if (ImGui::SliderInt(c.s.c_str(), &v, (int)c.b, (int)c.c)) { lv.val = (float)v; input_value(surf, c.ix, (uint32_t)v); }
            lv.active = ImGui::IsItemActive();
            break; }
        case C_SLIDER_FLOAT: {
            int id = slot_id(surf, c.ix); Live &lv = g_live[id >= 0 ? id : 0];
            if (!lv.init || (c.a != lv.seed && !lv.active)) { lv.val = c.a; lv.init = true; }
            lv.seed = c.a;
            float v = lv.val;
            if (ImGui::SliderFloat(c.s.c_str(), &v, c.b, c.c)) { lv.val = v; uint32_t bits; memcpy(&bits,&v,4); input_value(surf, c.ix, bits); }
            lv.active = ImGui::IsItemActive();
            break; }
        }
    }
}

// A window/monitor resize happened — nudge every ImGui window back on-screen for ONE frame, so a
// window near the old right/bottom edge doesn't end up stranded off the smaller client.  Set from the
// resize paths (companion WM_SIZE + the windowed-takeover resize, via ui_mark_resized), cleared after
// the next draw.  Same-thread within each mode (companion: UI thread; overlay: engine thread).
static bool g_reclamp = false;
extern "C" void ui_mark_resized(void) { g_reclamp = true; }

// Clamp the CURRENT ImGui window's top-left so its body stays inside the display (called between Begin
// and End; only acts when g_reclamp is set).  Keeps at least the whole window on-screen when it fits.
static void reclamp_current_window(void) {
    if (!g_reclamp) return;
    ImVec2 ds = ImGui::GetIO().DisplaySize, pos = ImGui::GetWindowPos(), sz = ImGui::GetWindowSize();
    float maxx = ds.x - sz.x, maxy = ds.y - sz.y;
    if (maxx < 0) maxx = 0;
    if (maxy < 0) maxy = 0;
    ImVec2 cl = pos;
    if (cl.x > maxx) cl.x = maxx;
    if (cl.x < 0)    cl.x = 0;
    if (cl.y > maxy) cl.y = maxy;
    if (cl.y < 0)    cl.y = 0;
    if (cl.x != pos.x || cl.y != pos.y) ImGui::SetWindowPos(cl);
}

// Draw the whole UI from the latest snapshot (UI thread; no Lua).  Returns true if ImGui is busy
// (an item active) so the loop keeps rendering for smooth interaction.
static bool draw_from_snapshot(const Snapshot &snap) {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float MARGIN = 16.0f, HOST_W = 460.0f, HOST_H = 620.0f;
    // The loader's host window spawns on the RIGHT edge (mods' own windows tile from the left, below).
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - HOST_W - MARGIN, vp->WorkPos.y + MARGIN), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(HOST_W, HOST_H), ImGuiCond_FirstUseEver);
    if (ImGui::Begin(OSS_ML_NAME "##host")) {
        ImGui::TextUnformatted(OSS_ML_NAME "  " OSS_ML_VERSION);
        ImGui::TextDisabled("%s   |   panels %d   windows %d",
            snap.status.armed ? "executor armed" : "executor idle", snap.status.npanel, snap.status.nwindow);
        ImGui::Separator();
        for (size_t i = 0; i < snap.surfaces.size(); i++) {
            const Surface &s = snap.surfaces[i];
            if (s.is_window) continue;
            char label[160]; snprintf(label, sizeof label, "%s##oss_panel_%zu", s.name.c_str(), i);
            if (ImGui::CollapsingHeader(label, s.collapsed ? ImGuiTreeNodeFlags_None : ImGuiTreeNodeFlags_DefaultOpen)) replay_cmds((int)i, s.cmds);
        }
    }
    reclamp_current_window();
    ImGui::End();

    // Mods' floating windows: TILE them so they don't stack on boot — a column down the left, wrapping
    // to the next column when it runs past the bottom.  FirstUseEver, so the user can rearrange freely.
    const float WIN_W = 360.0f, WIN_H = 300.0f, top = vp->WorkPos.y + MARGIN;
    const float bottom = vp->WorkPos.y + vp->WorkSize.y - MARGIN;
    float tx = vp->WorkPos.x + MARGIN, ty = top;
    for (size_t i = 0; i < snap.surfaces.size(); i++) {
        const Surface &s = snap.surfaces[i];
        if (!s.is_window) continue;
        if (ty > top && ty + WIN_H > bottom) { ty = top; tx += WIN_W + MARGIN; }   // wrap to next column
        char label[160]; snprintf(label, sizeof label, "%s##oss_win_%zu", s.name.c_str(), i);
        ImGui::SetNextWindowPos(ImVec2(tx, ty), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(WIN_W, WIN_H), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(label)) replay_cmds((int)i, s.cmds);
        reclamp_current_window();
        ImGui::End();
        ty += WIN_H + MARGIN;
    }
    g_reclamp = false;   // one-shot: every window was nudged this frame
    return ImGui::IsAnyItemActive();
}

// ════════════════════════ engine-thread in-game overlay (Phase C) ════════════════════════
// In borderless takeover the game window IS the display, so we draw mod.ui ON TOP of the game frame
// in the game window's swapchain — retiring the companion window.  ONE ImGui context, owned by the
// ENGINE thread (the companion UI thread is NOT started in takeover — see ui_start), so ImGui's
// global current-context is never raced.  We replay the SAME snapshot the companion would (mod.ui is
// unchanged): here the engine thread is BOTH producer (ui_build) and consumer, so the lock-free
// triple buffer just degenerates to same-thread latest-wins — no contention, no new machinery.
static bool g_ov_started = false;      // engine-thread ImGui context + backends are up
static bool g_ov_failed  = false;      // init failed once — don't retry every present
static bool g_ov_visible = true;       // F10 show/hide (toggled from ui_overlay_wndproc)
static bool g_ov_have    = false;      // a snapshot has been fetched at least once

static bool overlay_init(void *dev_, void *ctx_, void *hwnd_) {
    ID3D11Device *dev = (ID3D11Device *)dev_;
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)ctx_;
    HWND hwnd = (HWND)hwnd_;
    if (!dev || !ctx || !hwnd) return false;
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    // MOUSE-ONLY on purpose: no keyboard nav, so gameplay keys belong entirely to the game (you can
    // move the character with the overlay up).  The game ignores the mouse, so the mouse is ours.
    ImGuiIO &io = ImGui::GetIO(); io.IniFilename = NULL;
    io.MouseDrawCursor = true;   // borderless-fullscreen: draw ImGui's own cursor (don't trust the OS one)
    { const char *f = "C:\\Windows\\Fonts\\segoeui.ttf";
      if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(f, 18.0f); else io.Fonts->AddFontDefault(); }
    apply_theme();
    if (!ImGui_ImplWin32_Init(hwnd)) { ml_log("[ui] overlay ImGui_ImplWin32_Init failed"); ImGui::DestroyContext(); return false; }
    if (!ImGui_ImplDX11_Init(dev, ctx)) { ml_log("[ui] overlay ImGui_ImplDX11_Init failed"); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext(); return false; }
    ml_log("[ui] in-game overlay up (engine-thread ImGui on the takeover device; F10 toggles)");
    return true;
}

// ENGINE THREAD (ddp_engine_present): draw the overlay over the just-drawn game frame, into the
// currently-bound render target, right before the swapchain Present.  Lazy one-time bring-up.
extern "C" void ui_overlay_present(void *dev, void *ctx, void *hwnd) {
    if (!g_started || g_ov_failed) return;                 // ui=0 (no snapshot) or a prior init failure
    if (!g_ov_started) {
        if (!overlay_init(dev, ctx, hwnd)) { g_ov_failed = true; return; }
        g_ov_started = true;
    }
    if (!g_ov_visible) return;   // hidden (toggled in ui_overlay_wndproc) — game frame only, no overlay

    if (snap_fetch()) g_ov_have = true;    // adopt the freshest snapshot (same-thread producer/consumer)

    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (g_ov_have) draw_from_snapshot(g_buf[g_read_ix]);
    else { ImGui::Begin(OSS_ML_NAME "##host"); ImGui::TextDisabled("waiting for the game..."); ImGui::End(); }
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());   // draws into the bound RTV (the game back buffer)
}

// ENGINE THREAD (executor boot_wndproc): route a game-window message to the overlay's ImGui.
extern "C" int ui_overlay_wndproc(void *hwnd, unsigned msg, uintptr_t wparam, intptr_t lparam) {
    if (!g_ov_started) return 0;                           // overlay not up yet — game owns all input
    // The toggle key (F10 default) — handle it HERE, not by polling: consume it (return 1) so it
    // reaches neither the game nor DefWindowProc (F10 is a system key that would enter menu mode).
    // F10 arrives as WM_SYSKEY*; guard bit 30 (previous-key-state) so auto-repeat doesn't re-toggle.
    if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) && (int)wparam == g_key_toggle) {
        if (!(lparam & (1 << 30))) g_ov_visible = !g_ov_visible;
        return 1;
    }
    if ((msg == WM_KEYUP || msg == WM_SYSKEYUP) && (int)wparam == g_key_toggle) return 1;
    if (!g_ov_visible) return 0;                           // hidden: MOUSE + keyboard both go to the game
    // Visible: feed ImGui.  It records mouse/keyboard into io but only CONSUMES WM_SETCURSOR (returns
    // 1), so gameplay keys still fall through to the game — mouse drives the overlay, keyboard the game.
    return ImGui_ImplWin32_WndProcHandler((HWND)hwnd, msg, (WPARAM)wparam, (LPARAM)lparam) ? 1 : 0;
}

static bool g_have_snap = false;
static bool render_frame(void) {   // returns busy
    if (!g_rtv) return false;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    bool busy = false;
    if (g_have_snap) busy = draw_from_snapshot(g_buf[g_read_ix]);
    else { ImGui::Begin(OSS_ML_NAME "##host"); ImGui::TextDisabled("waiting for the game..."); ImGui::End(); }
    ImGui::Render();
    const float clr[4] = { 0.09f, 0.10f, 0.12f, 1.0f };
    g_devctx->OMSetRenderTargets(1, &g_rtv, NULL);
    g_devctx->ClearRenderTargetView(g_rtv, clr);
    RECT rc; GetClientRect(g_hwnd, &rc);   // mirror the captured game frame (aspect-fit) behind the UI
    ddp_draw_background(g_dev, g_devctx, (int)(rc.right - rc.left), (int)(rc.bottom - rc.top));
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    g_swap->Present(1, 0);
    return busy;
}

static volatile bool g_want_quit = false;

static DWORD WINAPI ui_thread(void *unused) {
    (void)unused;
    WNDCLASSEXW wc; ZeroMemory(&wc, sizeof wc);
    wc.cbSize = sizeof wc; wc.style = CS_CLASSDC; wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleW(NULL); wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = L"OSSModLoaderUI";
    if (!RegisterClassExW(&wc)) { ml_log("[ui] RegisterClassExW failed (%lu)", GetLastError()); return 1; }
    g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"SotES Mod Loader", WS_OVERLAPPEDWINDOW,
                             80, 80, 500, 720, NULL, NULL, wc.hInstance, NULL);
    if (!g_hwnd || !create_device(g_hwnd)) {
        ml_log("[ui] window/device creation FAILED — UI disabled");
        destroy_device(); if (g_hwnd) DestroyWindow(g_hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }
    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO(); io.IniFilename = NULL; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    { const char *f = "C:\\Windows\\Fonts\\segoeui.ttf";
      if (GetFileAttributesA(f) != INVALID_FILE_ATTRIBUTES) io.Fonts->AddFontFromFileTTF(f, 18.0f); else io.Fonts->AddFontDefault(); }
    apply_theme();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX11_Init(g_dev, g_devctx);
    // In borderless takeover the game window IS the display, so the companion (a separate window that
    // would only duplicate it) starts HIDDEN — F8 shows it as a fallback UI until the in-game overlay lands.
    int hide = config_get_int("ddraw_takeover", 0);
    ShowWindow(g_hwnd, hide ? SW_HIDE : SW_SHOWNORMAL); UpdateWindow(g_hwnd);
    g_visible = !hide;
    ml_log("[ui] companion window %s (hwnd %p)", hide ? "hidden — takeover owns the screen; F10 to show" : "up", (void *)g_hwnd);

    bool running = true, busy = false;
    int prev_key = 0;
    while (running && !g_want_quit) {
        // Event-driven: wait for input, a fresh snapshot, or a heartbeat.  busy -> spin (smooth drag).
        DWORD timeout = g_visible ? (busy ? 0u : 250u) : INFINITE;
        MsgWaitForMultipleObjects(1, &g_dirty, FALSE, timeout, QS_ALLINPUT);

        bool had_input = false;
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) running = false;
            UINT m = msg.message;
            if (m == WM_MOUSEMOVE || m == WM_LBUTTONDOWN || m == WM_LBUTTONUP || m == WM_RBUTTONDOWN ||
                m == WM_MOUSEWHEEL || m == WM_KEYDOWN || m == WM_KEYUP || m == WM_CHAR) had_input = true;
        }
        if (!running || g_want_quit) break;

        int k = (GetAsyncKeyState(g_key_toggle) & 0x8000) ? 1 : 0;   // toggle the window
        if (k && !prev_key) { g_visible = !g_visible; ShowWindow(g_hwnd, g_visible ? SW_SHOW : SW_HIDE); if (g_visible) SetForegroundWindow(g_hwnd); }
        prev_key = k;

        bool fresh = snap_fetch();      // lock-free: point g_read_ix at the latest snapshot
        if (fresh) g_have_snap = true;
        static uint32_t last_fseq = 0;  // a fresh CAPTURED game frame is also a reason to render (mirror)
        uint32_t fseq = ddp_frame_seq(); bool new_frame = (fseq != last_fseq); last_fseq = fseq;
        if (!g_visible) { busy = false; continue; }
        if (fresh || had_input || busy || new_frame) busy = render_frame();   // render only when there's a reason
    }

    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    destroy_device(); if (g_hwnd) DestroyWindow(g_hwnd); UnregisterClassW(wc.lpszClassName, wc.hInstance);
    ml_log("[ui] UI thread exited");
    return 0;
}

extern "C" void ui_start(int key_toggle, int build_hz) {
    if (InterlockedCompareExchange(&g_started, 1, 0) != 0) return;
    if (key_toggle) g_key_toggle = key_toggle;
    if (build_hz > 0) g_build_interval = (DWORD)(1000 / build_hz);
    g_dirty = CreateEvent(NULL, FALSE, FALSE, NULL);   // auto-reset: a changed publish wakes the UI
    // In borderless takeover the in-game overlay (engine thread, ui_overlay_present) IS the UI — one
    // ImGui context drawn on top of the game frame.  The companion window would be a SECOND ImGui
    // context on a SECOND thread (racing ImGui's global current-context), so we DON'T start it here;
    // ui_build still runs from the safepoint to produce the snapshot the overlay replays.
    if (config_get_int("ddraw", 1) && config_get_int("ddraw_takeover", 0)) {
        ml_log("[ui] in-game overlay mode (takeover) — companion window disabled; mod.ui draws in the game window");
        return;
    }
    HANDLE t = CreateThread(NULL, 0, ui_thread, NULL, 0, NULL);
    if (t) CloseHandle(t); else ml_log("[ui] CreateThread failed (%lu)", GetLastError());
}

// Present hook -> wake the UI thread so the game mirror renders at the game's present rate (not the
// throttled snapshot rate).  Only when visible: hidden, we leave it asleep (INFINITE wait, zero cost).
extern "C" void ui_wake(void) { if (g_visible && g_dirty) SetEvent(g_dirty); }

extern "C" void ui_shutdown(void) {
    g_want_quit = true;
    if (g_dirty) SetEvent(g_dirty);                    // nudge the loop out of its wait
    if (g_hwnd)  PostMessageW(g_hwnd, WM_CLOSE, 0, 0);
}
