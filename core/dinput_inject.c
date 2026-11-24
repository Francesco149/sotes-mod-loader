// core/dinput_inject.c — scripted keyboard input through DirectInput itself.  See dinput_inject.h.
//
// SotES (every edition) reads the keyboard two ways per update (rhost rh_dinput.c, retail ZDI):
//   GetDeviceState(256)  — held LEVELS (movement, attack holds)
//   GetDeviceData(16-byte DIDEVICEOBJECTDATA) — buffered EDGES (menus, text advance)
// We find dinput's own implementation of those two methods through a probe keyboard device we
// create ourselves (same DLL, same class → same vtable entries as the game's device), MinHook the
// functions, and on the KEYBOARD device (the one polled with cb == 256) merge our scripted levels
// into the state and append synthesized edge records.  No game VAs: works on any build.  When the
// device is unacquired (no focus — always the case under a headless X server) the real call fails;
// while a script is active we answer DI_OK with the scripted input alone, so tests run unfocused.
#define DIRECTINPUT_VERSION 0x0700
#include "dinput_inject.h"
#include "executor.h"
#include "loader_internal.h"

#include <windows.h>
#include <dinput.h>
#include <string.h>

#include "MinHook.h"
#include "lua.h"
#include "lauxlib.h"

typedef HRESULT (WINAPI *gds_fn)(void *self, DWORD cb, void *buf);
typedef HRESULT (WINAPI *gdd_fn)(void *self, DWORD cbobj, void *rg, DWORD *inout, DWORD flags);
static gds_fn g_gds_orig;
static gdd_fn g_gdd_orig;
static void *g_kbd;                       // the game's keyboard device (seen with cb == 256)

// ── the script: a FIFO of segments (a key set held for n frames), plus persistent holds ──
#define MAXSEG 4096
typedef struct { uint8_t keys[8]; uint8_t nkeys; uint32_t frames; } seg_t;
static seg_t g_seg[MAXSEG];
static int g_sh, g_sn;                    // head, count
static uint8_t g_hold[256];               // persistent holds (input.hold / release)
static uint8_t g_auto[256];               // loader-owned holds (di_auto: e.g. the dialogue skip) — not cleared by input.clear
static uint8_t g_level[256], g_prev[256]; // this frame's scripted level, last frame's
static int g_active;                      // a script or hold is (or was this frame) driving input
static uint32_t g_seq;
typedef struct { DWORD ofs, data, ts, seq; } dod7;
static dod7 g_q[256];
static int g_qh, g_qn;
static long g_n_gds, g_n_gdd, g_n_inj;

static void enqueue(DWORD ofs, DWORD data) {
    if (g_qn == 256) { g_qh = (g_qh + 1) & 255; g_qn--; }
    dod7 *e = &g_q[(g_qh + g_qn) & 255];
    e->ofs = ofs; e->data = data; e->ts = GetTickCount(); e->seq = ++g_seq;
    g_qn++;
}

// ENGINE THREAD, once per frame (the frame hook): advance the script, latch levels, queue edges.
void di_frame(void) {
    memcpy(g_prev, g_level, 256);
    int any = 0;
    for (int i = 0; i < 256; i++) { g_level[i] = g_hold[i] | g_auto[i]; any |= g_level[i]; }
    if (g_sn) {
        seg_t *s = &g_seg[g_sh];
        for (int i = 0; i < s->nkeys; i++) g_level[s->keys[i]] = 0x80;
        any = 1;
        if (--s->frames == 0) { g_sh = (g_sh + 1) % MAXSEG; g_sn--; }
    }
    for (int k = 0; k < 256; k++)
        if ((g_level[k] ^ g_prev[k]) & 0x80) enqueue((DWORD)k, g_level[k] & 0x80);
    // stay "active" one extra frame after the script ends so the final releases are delivered
    static int tail;
    if (any) { g_active = 1; tail = 2; }
    else if (tail) { tail--; g_active = tail > 0 || g_qn > 0; }
    else g_active = g_qn > 0;
}

static volatile LONG g_capture;   // the UI owns the keyboard (a focused text field)
void di_set_capture(int on) { InterlockedExchange(&g_capture, on ? 1 : 0); }

static HRESULT WINAPI gds_hook(void *self, DWORD cb, void *buf) {
    HRESULT hr = g_gds_orig(self, cb, buf);
    if (cb != 256 || !buf) return hr;
    g_kbd = self;
    g_n_gds++;
    if (g_capture) { memset(buf, 0, 256); hr = DI_OK; }  // UI typing: no real key levels reach the game
    if (!g_active) return hr;
    if (FAILED(hr)) memset(buf, 0, 256);                 // unacquired: scripted input alone
    uint8_t *st = (uint8_t *)buf;
    for (int i = 0; i < 256; i++) st[i] |= g_level[i];
    g_n_inj++;
    return DI_OK;
}

static HRESULT WINAPI gdd_hook(void *self, DWORD cbobj, void *rg, DWORD *inout, DWORD flags) {
    if (self != g_kbd || !inout || cbobj < 16) return g_gdd_orig(self, cbobj, rg, inout, flags);
    DWORD cap = *inout;
    HRESULT hr = g_gdd_orig(self, cbobj, rg, inout, flags);
    g_n_gdd++;
    if (g_capture && SUCCEEDED(hr)) {                    // UI typing: drop the real buffered events
        if (!(flags & DIGDD_PEEK) && rg && *inout == cap && cap) {   // drain the rest so none surface later
            DWORD n; do { n = cap; if (FAILED(g_gdd_orig(self, cbobj, rg, &n, flags))) break; } while (n == cap);
        }
        *inout = 0;
    }
    if (!g_active && !g_qn) return hr;
    DWORD have = SUCCEEDED(hr) ? *inout : 0;
    int peek = (flags & DIGDD_PEEK) != 0;
    int qh = g_qh, qn = g_qn;
    while (qn && (!rg || have < cap)) {                  // rg == NULL: a "how many" flush query
        if (rg) {
            uint8_t *dst = (uint8_t *)rg + (size_t)have * cbobj;
            memset(dst, 0, cbobj);
            memcpy(dst, &g_q[qh], sizeof(dod7));
        }
        have++; qh = (qh + 1) & 255; qn--;
        if (!rg && cap != INFINITE && have >= cap) break;
    }
    if (!peek) { g_qh = qh; g_qn = qn; }
    *inout = have;
    return DI_OK;
}

int di_install(void) {
    HMODULE m = LoadLibraryA("dinput.dll");
    typedef HRESULT (WINAPI *create_fn)(HINSTANCE, DWORD, REFIID, void **, IUnknown *);
    create_fn create = m ? (create_fn)(void *)GetProcAddress(m, "DirectInputCreateEx") : NULL;
    if (!create) { ml_log("[input] dinput.dll / DirectInputCreateEx unavailable — injection off"); return 0; }
    IDirectInput7A *di = NULL;
    IDirectInputDevice7A *dev = NULL;
    if (FAILED(create(GetModuleHandleA(NULL), 0x700, &IID_IDirectInput7A, (void **)&di, NULL)) || !di) {
        ml_log("[input] DirectInputCreateEx failed — injection off"); return 0;
    }
    if (FAILED(di->lpVtbl->CreateDeviceEx(di, &GUID_SysKeyboard, &IID_IDirectInputDevice7A, (void **)&dev, NULL)) || !dev) {
        di->lpVtbl->Release(di); ml_log("[input] probe keyboard CreateDeviceEx failed — injection off"); return 0;
    }
    void *gds = (void *)dev->lpVtbl->GetDeviceState, *gdd = (void *)dev->lpVtbl->GetDeviceData;
    dev->lpVtbl->Release(dev);
    di->lpVtbl->Release(di);
    MH_STATUS s1 = MH_CreateHook(gds, (LPVOID)gds_hook, (LPVOID *)&g_gds_orig);
    MH_STATUS s2 = MH_CreateHook(gdd, (LPVOID)gdd_hook, (LPVOID *)&g_gdd_orig);
    if (s1 != MH_OK || s2 != MH_OK || MH_EnableHook(gds) != MH_OK || MH_EnableHook(gdd) != MH_OK) {
        ml_log("[input] hooking dinput GetDeviceState/Data failed (%d/%d) — injection off", s1, s2); return 0;
    }
    ml_log("[input] keyboard injection armed (dinput GetDeviceState %p / GetDeviceData %p)", gds, gdd);
    return 1;
}

void di_auto(uint8_t dik, int on) { g_auto[dik] = on ? 0x80 : 0; }

// ── Lua: mod.input ───────────────────────────────────────────────────────────
// input.push({dik...}, frames)   append a segment (an empty list = hold nothing)
// input.hold(dik) / input.release(dik) / input.clear()
// input.pending() -> frames of script left;  input.stats() -> { gds=, gdd=, injected=, kbd= }
static int l_push(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    uint32_t n = (uint32_t)luaL_optnumber(L, 2, 1);
    if (!n) return 0;
    if (g_sn == MAXSEG) return luaL_error(L, "input script full (%d segments)", MAXSEG);
    seg_t *s = &g_seg[(g_sh + g_sn) % MAXSEG];
    memset(s, 0, sizeof *s);
    int len = (int)lua_objlen(L, 1);
    for (int i = 1; i <= len && s->nkeys < 8; i++) {
        lua_rawgeti(L, 1, i);
        s->keys[s->nkeys++] = (uint8_t)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    s->frames = n;
    g_sn++;
    return 0;
}
static int l_hold(lua_State *L)    { g_hold[(uint8_t)luaL_checkinteger(L, 1)] = 0x80; return 0; }
static int l_release(lua_State *L) { g_hold[(uint8_t)luaL_checkinteger(L, 1)] = 0; return 0; }
static int l_clear(lua_State *L)   { (void)L; g_sn = 0; memset(g_hold, 0, 256); return 0; }
static int l_pending(lua_State *L) {
    uint32_t f = 0;
    for (int i = 0; i < g_sn; i++) f += g_seg[(g_sh + i) % MAXSEG].frames;
    lua_pushnumber(L, f);
    return 1;
}
static int l_stats(lua_State *L) {
    lua_newtable(L);
    lua_pushnumber(L, g_n_gds); lua_setfield(L, -2, "gds");
    lua_pushnumber(L, g_n_gdd); lua_setfield(L, -2, "gdd");
    lua_pushnumber(L, g_n_inj); lua_setfield(L, -2, "injected");
    lua_pushboolean(L, g_kbd != NULL); lua_setfield(L, -2, "kbd");
    lua_pushboolean(L, g_capture != 0); lua_setfield(L, -2, "captured");   // a UI text field owns the keyboard
    return 1;
}
void di_push_lua(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_push);    lua_setfield(L, -2, "push");
    lua_pushcfunction(L, l_hold);    lua_setfield(L, -2, "hold");
    lua_pushcfunction(L, l_release); lua_setfield(L, -2, "release");
    lua_pushcfunction(L, l_clear);   lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, l_pending); lua_setfield(L, -2, "pending");
    lua_pushcfunction(L, l_stats);   lua_setfield(L, -2, "stats");
}
