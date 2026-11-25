// core/setup.c — first-time setup for mods: the consent dialog, the progress window, the boot gate.
// See setup.h for the model.  Everything here runs on the LOADER thread except the gate detours (the
// game's main thread, which only waits) and setup_gate_arm_if_needed (DllMain: file reads + MinHook).
#include "loader_internal.h"
#include "setup.h"
#include "config.h"
#include "lua_host.h"
#include "MinHook.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

#include "lua.h"
#include "lauxlib.h"

// ── a tiny mod.toml reader: [section] key = value (quoted or bare; arrays returned raw) ──────────────────
static void trim(char *s) {
    char *p = s;
    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);
    size_t n = strlen(s);
    while (n && isspace((unsigned char)s[n - 1])) s[--n] = 0;
}
static int toml_get(const char *path, const char *section, const char *key, char *out, size_t n) {
    FILE *f = fopen(path, "r");
    out[0] = 0;
    if (!f) return 0;
    char line[2048], want[80];
    _snprintf(want, sizeof want, "[%s]", section);
    int in = 0, found = 0;
    while (fgets(line, sizeof line, f)) {
        trim(line);
        if (line[0] == '#' || !line[0]) continue;
        if (line[0] == '[') { in = strcmp(line, want) == 0; continue; }
        if (!in) continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char k[80]; _snprintf(k, sizeof k, "%s", line); trim(k);
        if (strcmp(k, key) != 0) continue;
        char *v = eq + 1;
        trim(v);
        if (*v == '"') {                       // "…" with \" and \n escapes
            size_t o = 0;
            for (v++; *v && *v != '"' && o + 1 < n; v++) {
                if (*v == '\\' && v[1]) { v++; out[o++] = *v == 'n' ? '\n' : *v; }
                else out[o++] = *v;
            }
            out[o] = 0;
        } else {
            char *h = strchr(v, '#');
            if (h) *h = 0;
            trim(v);
            _snprintf(out, n, "%s", v);
        }
        found = 1;
        break;
    }
    fclose(f);
    return found;
}

// ── state: <game>\mod_data\<name>\setup.state = "version=<v>" ────────────────────────────────────────────
void setup_data_dir(const char *name, char *out, unsigned n) { _snprintf(out, n, "%smod_data\\%s\\", g_gamedir, name); }
static void setup_version(const char *dir, char *out, size_t n) {
    char tp[MAX_PATH];
    _snprintf(tp, MAX_PATH, "%s\\mod.toml", dir);
    if (!toml_get(tp, "setup", "version", out, n) || !out[0]) _snprintf(out, n, "1");
}
static int has_setup(const char *dir) {
    char p[MAX_PATH];
    _snprintf(p, MAX_PATH, "%s\\setup.lua", dir);
    return GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES;
}
static int state_matches(const char *name, const char *version) {
    char p[MAX_PATH], line[128] = "";
    setup_data_dir(name, p, MAX_PATH);
    strncat(p, "setup.state", MAX_PATH - strlen(p) - 1);
    FILE *f = fopen(p, "r");
    if (!f) return 0;
    if (!fgets(line, sizeof line, f)) line[0] = 0;
    fclose(f);
    trim(line);
    return strncmp(line, "version=", 8) == 0 && strcmp(line + 8, version) == 0;
}
int setup_is_done(const char *name, const char *dir) {
    if (!has_setup(dir)) return 1;
    char v[64];
    setup_version(dir, v, sizeof v);
    return state_matches(name, v);
}
static int state_write(const char *name, const char *version) {
    char d[MAX_PATH], p[MAX_PATH];
    _snprintf(d, MAX_PATH, "%smod_data", g_gamedir);
    CreateDirectoryA(d, NULL);
    setup_data_dir(name, d, MAX_PATH);
    CreateDirectoryA(d, NULL);
    _snprintf(p, MAX_PATH, "%ssetup.state", d);
    FILE *f = fopen(p, "w");
    if (!f) return 0;
    fprintf(f, "version=%s\n", version);
    fclose(f);
    return 1;
}

// ── the boot gate ────────────────────────────────────────────────────────────────────────────────────────
static HANDLE g_gate_evt;
static DWORD  g_gate_tid;
static volatile LONG g_gate_armed, g_gate_waited;
static void *g_cwa, *g_cww, *g_dba, *g_dbw, *g_cda, *g_cdw;      // hooked entry points
static HWND (WINAPI *o_cwa)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static HWND (WINAPI *o_cww)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
static INT_PTR (WINAPI *o_dba)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
static INT_PTR (WINAPI *o_dbw)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM);
static HWND (WINAPI *o_cda)(HINSTANCE, LPCSTR, HWND, DLGPROC, LPARAM);
static HWND (WINAPI *o_cdw)(HINSTANCE, LPCWSTR, HWND, DLGPROC, LPARAM);

static void gate_wait(void) {
    if (!g_gate_armed || GetCurrentThreadId() != g_gate_tid) return;
    if (InterlockedExchange(&g_gate_waited, 1) == 0) ml_log("[setup] game start held: a mod's first-time setup is pending");
    WaitForSingleObject(g_gate_evt, INFINITE);
}
static HWND WINAPI h_cwa(DWORD a, LPCSTR b, LPCSTR c, DWORD d, int e, int f, int g, int h, HWND i, HMENU j, HINSTANCE k, LPVOID l) {
    gate_wait(); return o_cwa(a, b, c, d, e, f, g, h, i, j, k, l);
}
static HWND WINAPI h_cww(DWORD a, LPCWSTR b, LPCWSTR c, DWORD d, int e, int f, int g, int h, HWND i, HMENU j, HINSTANCE k, LPVOID l) {
    gate_wait(); return o_cww(a, b, c, d, e, f, g, h, i, j, k, l);
}
static INT_PTR WINAPI h_dba(HINSTANCE a, LPCSTR b, HWND c, DLGPROC d, LPARAM e) { gate_wait(); return o_dba(a, b, c, d, e); }
static INT_PTR WINAPI h_dbw(HINSTANCE a, LPCWSTR b, HWND c, DLGPROC d, LPARAM e) { gate_wait(); return o_dbw(a, b, c, d, e); }
static HWND WINAPI h_cda(HINSTANCE a, LPCSTR b, HWND c, DLGPROC d, LPARAM e) { gate_wait(); return o_cda(a, b, c, d, e); }
static HWND WINAPI h_cdw(HINSTANCE a, LPCWSTR b, HWND c, DLGPROC d, LPARAM e) { gate_wait(); return o_cdw(a, b, c, d, e); }

static void *gate_hook(const char *fn, void *detour, void *orig) {
    HMODULE u = GetModuleHandleA("user32.dll");
    void *p = u ? (void *)GetProcAddress(u, fn) : NULL;
    if (!p) return NULL;
    if (MH_CreateHook(p, detour, (LPVOID *)orig) != MH_OK) return NULL;
    if (MH_EnableHook(p) != MH_OK) return NULL;
    return p;
}

static int any_pending(void) {
    char pat[MAX_PATH];
    _snprintf(pat, MAX_PATH, "%smods\\*", g_gamedir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int pending = 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        char dir[MAX_PATH];
        _snprintf(dir, MAX_PATH, "%smods\\%s", g_gamedir, fd.cFileName);
        if (!setup_is_done(fd.cFileName, dir)) { pending = 1; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return pending;
}

void setup_gate_arm_if_needed(unsigned long main_tid) {
    if (!any_pending()) return;
    g_gate_evt = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!g_gate_evt) return;
    g_gate_tid = main_tid;
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) return;
    g_gate_armed = 1;
    g_cwa = gate_hook("CreateWindowExA", (void *)h_cwa, &o_cwa);
    g_cww = gate_hook("CreateWindowExW", (void *)h_cww, &o_cww);
    g_dba = gate_hook("DialogBoxParamA", (void *)h_dba, &o_dba);
    g_dbw = gate_hook("DialogBoxParamW", (void *)h_dbw, &o_dbw);
    g_cda = gate_hook("CreateDialogParamA", (void *)h_cda, &o_cda);
    g_cdw = gate_hook("CreateDialogParamW", (void *)h_cdw, &o_cdw);
    ml_log("[setup] a mod has a first-time setup pending — the game's first window waits for it");
}

void setup_gate_open(void) {
    if (!g_gate_armed) return;
    g_gate_armed = 0;
    SetEvent(g_gate_evt);
    void *h[] = { g_cwa, g_cww, g_dba, g_dbw, g_cda, g_cdw };
    for (unsigned i = 0; i < sizeof h / sizeof h[0]; i++) if (h[i]) MH_DisableHook(h[i]);
    ml_log("[setup] gate open — the game starts%s", g_gate_waited ? " (it was held)" : "");
}

// ── UI: the consent dialog, the progress window ──────────────────────────────────────────────────────────
static WCHAR *widen(const char *s) {                       // UTF-8 → UTF-16 (caller frees)
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    WCHAR *w = (WCHAR *)malloc(sizeof(WCHAR) * (size_t)(n > 0 ? n : 1));
    if (!w) return NULL;
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); else w[0] = 0;
    return w;
}
static int msgbox(const char *title, const char *text, UINT flags) {
    WCHAR *wt = widen(title), *wx = widen(text);
    int r = MessageBoxW(NULL, wx ? wx : L"", wt ? wt : L"", flags | MB_TOPMOST | MB_SETFOREGROUND);
    free(wt); free(wx);
    return r;
}

static struct {
    HWND  hwnd;
    char  title[160], text[256];
    double frac;
    DWORD last_pump;
} g_pw;
static LRESULT CALLBACK pw_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        FillRect(dc, &rc, (HBRUSH)GetStockObject(WHITE_BRUSH));
        HGDIOBJ of = SelectObject(dc, GetStockObject(DEFAULT_GUI_FONT));
        SetBkMode(dc, TRANSPARENT);
        WCHAR *wt = widen(g_pw.title), *wx = widen(g_pw.text);
        RECT r1 = { 16, 14, rc.right - 16, 34 }, r2 = { 16, 78, rc.right - 16, 98 };
        if (wt) DrawTextW(dc, wt, -1, &r1, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        if (wx) DrawTextW(dc, wx, -1, &r2, DT_LEFT | DT_SINGLELINE | DT_PATH_ELLIPSIS | DT_NOPREFIX);
        free(wt); free(wx);
        RECT bar = { 16, 44, rc.right - 16, 66 };
        FrameRect(dc, &bar, (HBRUSH)GetStockObject(GRAY_BRUSH));
        double f = g_pw.frac < 0 ? 0 : g_pw.frac > 1 ? 1 : g_pw.frac;
        RECT fill = { bar.left + 2, bar.top + 2, bar.left + 2 + (int)((bar.right - bar.left - 4) * f), bar.bottom - 2 };
        HBRUSH b = CreateSolidBrush(RGB(0x3a, 0x8e, 0xd6));
        FillRect(dc, &fill, b);
        DeleteObject(b);
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    if (m == WM_CLOSE) return 0;                           // no cancel: the step is short, a half-run is just redone
    return DefWindowProcW(h, m, w, l);
}
static void pw_pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    g_pw.last_pump = GetTickCount();
}
static void pw_open(const char *title) {
    static int reg;
    if (!reg) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = pw_proc; wc.hInstance = GetModuleHandleW(NULL); wc.lpszClassName = L"OssModSetupProgress";
        wc.hCursor = LoadCursorW(NULL, (LPCWSTR)IDC_WAIT);
        RegisterClassW(&wc);
        reg = 1;
    }
    _snprintf(g_pw.title, sizeof g_pw.title, "%s", title);
    g_pw.text[0] = 0; g_pw.frac = 0;
    int w = 480, h = 150;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    g_pw.hwnd = CreateWindowExW(WS_EX_TOPMOST, L"OssModSetupProgress", L"SotES Mod Loader — mod setup",
                                WS_POPUP | WS_CAPTION | WS_VISIBLE, x, y, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    pw_pump();
}
static void pw_set(double frac, const char *text) {
    if (!g_pw.hwnd) return;
    if (frac >= 0) g_pw.frac = frac;
    if (text) _snprintf(g_pw.text, sizeof g_pw.text, "%s", text);
    if (GetTickCount() - g_pw.last_pump < 30 && frac < 1) return;      // repaint at most ~30 times a second
    InvalidateRect(g_pw.hwnd, NULL, FALSE);
    UpdateWindow(g_pw.hwnd);
    pw_pump();
}
static void pw_close(void) {
    if (g_pw.hwnd) { DestroyWindow(g_pw.hwnd); g_pw.hwnd = NULL; pw_pump(); }
}

// ── mod.setup (Lua) ──────────────────────────────────────────────────────────────────────────────────────
static int g_running;   // a setup.lua is executing (loader thread)
static int l_progress(lua_State *L) {
    double f = lua_isnumber(L, 1) ? lua_tonumber(L, 1) : -1;
    const char *t = lua_isstring(L, 2) ? lua_tostring(L, 2) : NULL;
    pw_set(f, t);
    return 0;
}
void setup_push_lua(lua_State *L, const char *name, const char *dir) {
    char v[64];
    setup_version(dir, v, sizeof v);
    lua_newtable(L);
    lua_pushcfunction(L, l_progress);              lua_setfield(L, -2, "progress");
    lua_pushstring(L, v);                          lua_setfield(L, -2, "version");
    lua_pushboolean(L, setup_is_done(name, dir));  lua_setfield(L, -2, "done");
    lua_pushboolean(L, g_running);                 lua_setfield(L, -2, "running");
    lua_pushboolean(L, has_setup(dir));            lua_setfield(L, -2, "needed");
}

// ── the per-mod step ─────────────────────────────────────────────────────────────────────────────────────
static int trusted(const char *name) {                     // oss_loader.cfg setup_trust = a,b | *  (automation)
    const char *t = config_get_str("setup_trust", NULL);
    if (!t) return 0;
    size_t n = strlen(name);
    for (const char *p = t; *p; ) {
        while (*p == ',' || *p == ' ') p++;
        const char *e = p;
        while (*e && *e != ',' && *e != ' ') e++;
        if ((size_t)(e - p) == 1 && *p == '*') return 1;
        if ((size_t)(e - p) == n && strncmp(p, name, n) == 0) return 1;
        p = e;
    }
    return 0;
}

int setup_mod(const char *name, const char *dir) {
    if (!has_setup(dir)) return 1;
    char ver[64], tp[MAX_PATH], sp[MAX_PATH];
    setup_version(dir, ver, sizeof ver);
    if (state_matches(name, ver)) return 1;
    _snprintf(tp, MAX_PATH, "%s\\mod.toml", dir);
    _snprintf(sp, MAX_PATH, "%s\\setup.lua", dir);

    char title[128], mver[64], authors[256], summary[900];
    if (!toml_get(tp, "mod", "name", title, sizeof title) || !title[0]) _snprintf(title, sizeof title, "%s", name);
    toml_get(tp, "mod", "version", mver, sizeof mver);
    toml_get(tp, "mod", "authors", authors, sizeof authors);
    {   // ["a", "b"] → a, b
        char *w = authors;
        for (char *r = authors; *r; r++) if (*r != '[' && *r != ']' && *r != '"') *w++ = *r;
        *w = 0;
    }
    if (!toml_get(tp, "setup", "summary", summary, sizeof summary) || !summary[0])
        _snprintf(summary, sizeof summary, "(the mod does not say)");

    int silent = trusted(name);
    if (!silent) {
        char text[2400];
        _snprintf(text, sizeof text,
            "The mod \"%s\" needs to run a one-time setup step before it can be used.\n\n"
            "Mod: %s %s%s%s\n"
            "Folder: %s\n\n"
            "What the mod says its setup does:\n%s\n\n"
            "SECURITY WARNING\n"
            "Mods are programs. This setup, and the mod itself, run with the same access to your files and your "
            "computer as the game. The mod loader cannot check what a mod really does. Continue only if you trust "
            "this mod and the place you got it from.\n\n"
            "Run the setup now?\n\n"
            "Yes: run the setup, then start the game with this mod.\n"
            "No: start the game without this mod (you will be asked again next time).",
            title, name, mver, authors[0] ? "  by " : "", authors, dir, summary);
        text[sizeof text - 1] = 0;
        ml_log("[setup] %s: asking for consent (setup version %s)", name, ver);
        int r = msgbox("SotES Mod Loader — first-time mod setup", text, MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2);
        if (r != IDYES) { ml_log("[setup] %s: declined — the mod is skipped this session", name); return 0; }
        ml_log("[setup] %s: consent given", name);
    } else {
        ml_log("[setup] %s: pre-approved by oss_loader.cfg setup_trust (no dialog)", name);
    }

    char pt[200], err[700] = "";
    _snprintf(pt, sizeof pt, "Setting up \"%s\" (one time only)…", title);
    pw_open(pt);
    g_running = 1;
    DWORD t0 = GetTickCount();
    int ok = lh_run_setup(name, dir, sp, err, sizeof err);
    g_running = 0;
    pw_close();
    if (!ok) {
        ml_log("[setup] %s: FAILED after %lu ms: %s", name, GetTickCount() - t0, err);
        if (!silent) {
            char text[1400];
            _snprintf(text, sizeof text,
                "The setup of \"%s\" did not finish:\n\n%s\n\n"
                "The game will start without this mod. The setup is offered again next time.\n"
                "(Details: oss_modloader.log beside the game.)", title, err);
            text[sizeof text - 1] = 0;
            msgbox("SotES Mod Loader — mod setup failed", text, MB_OK | MB_ICONERROR);
        }
        return 0;
    }
    if (!state_write(name, ver)) ml_log("[setup] %s: could not record the setup state (it will run again next time)", name);
    ml_log("[setup] %s: done in %lu ms (setup version %s)", name, GetTickCount() - t0, ver);
    return 1;
}
