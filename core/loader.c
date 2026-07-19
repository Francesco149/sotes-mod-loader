// core/loader.c — SotES Mod Loader host (proxy version.dll).  [P0]
//
// The game exe imports version.dll, so Windows loads THIS in its place.  We forward
// all 17 version.dll exports to realver.dll (a renamed copy of the user's own
// SysWOW64\version.dll, via version.def) so the exe's imports resolve, then load
// everything under mods\:
//   - native mods   mods\<name>.dll        -> LoadLibraryEx (its own DllMain runs)
//   - Lua mods      mods\<name>\init.lua   -> the LuaJIT host (lua_host.c)
//
// The host is GENERIC — no engine addresses (those live in profiles\<exe>.lua and in
// individual mods), so a game update never breaks the loader.  It loads only when a
// mods\ folder sits beside the host exe, so dropping the shim next to some unrelated
// app that also imports version.dll does nothing.
//
//   nix develop --command make -C core        # -> ../build/version.dll
//
// Install (beside the exe): version.dll (this) + realver.dll (copy of the system
// version.dll) + mods\...  Log: oss_modloader.log beside the exe.

#include "loader_internal.h"
#include "lua_host.h"
#include "sotes_bindings.h"
#include "executor.h"
#include "profile.h"
#include "config.h"
#include "ui.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

char        g_gamedir[MAX_PATH];   // our dir (= game dir), trailing '\'
static char g_logpath[MAX_PATH];
static int  g_is_sotes;            // host exe matches the SotES profile (register its bindings)

// tiny case-insensitive substring (avoid linking shlwapi for StrStrIA)
static const char *istr(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return hay;
    }
    return NULL;
}

void ml_log(const char *fmt, ...) {
    FILE *f = fopen(g_logpath, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Load one native mod DLL from mods\.  LOAD_WITH_ALTERED_SEARCH_PATH so a mod may
// ship co-located deps in its own folder without polluting the game dir.  If it exports
// OssModInit it's an ABI mod: defer its init to the engine thread (like a Lua mod's init).
// If not, it's a legacy standalone native DLL — its DllMain ran; nothing more to do.
static int load_native(const char *filename) {
    char full[MAX_PATH];
    _snprintf(full, MAX_PATH, "%smods\\%s", g_gamedir, filename);
    HMODULE m = LoadLibraryExA(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { ml_log("[loader] native FAILED mods\\%s (err %lu)", filename, GetLastError()); return 0; }
    FARPROC pf = GetProcAddress(m, OSS_MOD_INIT_NAME);
    OssModInitFn init = NULL;
    if (pf) memcpy(&init, &pf, sizeof init);   // FARPROC -> typed fn ptr, no -Wcast-function-type
    if (init) { exec_defer_native(filename, init); ml_log("[loader] native mods\\%s -> %p (OssModInit deferred to safepoint)", filename, (void *)m); }
    else        ml_log("[loader] native mods\\%s -> %p (no OssModInit — standalone DLL)", filename, (void *)m);
    return 1;
}

// A directory mod is a Lua mod iff it contains init.lua.  We DEFER its init to the
// executor so it runs on the engine (main) thread, never here on the loader thread.
static int load_lua_dir(const char *dirname) {
    char init[MAX_PATH], moddir[MAX_PATH];
    _snprintf(moddir, MAX_PATH, "%smods\\%s", g_gamedir, dirname);
    _snprintf(init,   MAX_PATH, "%s\\init.lua", moddir);
    if (GetFileAttributesA(init) == INVALID_FILE_ATTRIBUTES) return 0;  // not a Lua mod
    exec_defer_mod(dirname, moddir, init);
    return 1;
}

// Scan mods\ once: native *.dll files + directories that hold an init.lua.  Runs on
// a worker thread (never DllMain — LoadLibrary there risks loader-lock deadlock),
// spawned from attach so it starts once the loader lock frees: late enough to be
// safe, early enough for hooks.
static DWORD WINAPI loader_thread(void *unused) {
    (void)unused;

    config_load(g_gamedir);   // oss_loader.cfg (skip_launcher etc.), before the executor arms

    // Register the profile's native game bindings BEFORE the Lua game table finalizes,
    // so mod.game.<id> resolves them (game-agnostic core; SotES knowledge stays in its TU).
    if (g_is_sotes) { sotes_bindings_register(); ml_log("[loader] SotES profile — game bindings registered"); }
    profile_select(g_is_sotes);   // pick the game profile (safepoint VA etc.) for the executor

    if (lh_init() != 0) ml_log("[loader] Lua host down — Lua mods skipped, native still load");

    char pat[MAX_PATH];
    _snprintf(pat, MAX_PATH, "%smods\\*", g_gamedir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) { ml_log("[loader] no mods (empty %smods\\)", g_gamedir); return 0; }

    int nn = 0, nl = 0;
    do {
        const char *nm = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
            nl += load_lua_dir(nm);
        } else {
            size_t len = strlen(nm);
            if (len > 4 && _stricmp(nm + len - 4, ".dll") == 0) nn += load_native(nm);
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    // Arm the main-thread executor: Lua mods init on the ENGINE thread at the first
    // safepoint.  If there's no game window / no profile, fall back to running them here
    // on the loader thread (degraded, but non-game hosts + the test stub still work).
    if (exec_bootstrap()) {
        ml_log("[loader] executor armed — %d Lua mod(s) will init on the main thread", nl);
        // Stand up the ImGui UI host (loader-owned companion window) on its own thread.  It's fed
        // by a lock-free snapshot the executor builds each frame (ui_build), so it never stalls the
        // game.  Opt-out with ui=0; ui_key is the VK toggle (0 = F8); ui_hz the refresh (0 = 30).
        if (config_get_int("ui", 1)) {
            ui_start(config_get_int("ui_key", 0), config_get_int("ui_hz", 0));
            ml_log("[loader] UI host starting — companion window (lock-free snapshot pipeline)");
        } else ml_log("[loader] UI disabled (ui=0)");
    } else {
        ml_log("[loader] executor not armed — running %d Lua mod(s) on the loader thread (fallback)", nl);
        exec_run_deferred_inline();
    }

    ml_log("[loader] done: %d native + %d Lua mod(s) from %smods\\", nn, nl, g_gamedir);
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        // our own path (we sit beside the exe as version.dll) -> the game dir.
        GetModuleFileNameA(h, g_gamedir, MAX_PATH); g_gamedir[MAX_PATH - 1] = 0;
        char *bs = strrchr(g_gamedir, '\\');
        if (bs) bs[1] = 0; else g_gamedir[0] = 0;
        _snprintf(g_logpath, MAX_PATH, "%soss_modloader.log", g_gamedir);

        char host[MAX_PATH] = ""; GetModuleFileNameA(NULL, host, MAX_PATH);
        char *hslash = strrchr(host, '\\'); const char *hbase = hslash ? hslash + 1 : host;
        g_is_sotes = istr(hbase, "sotes") != NULL;   // profile select (stub — generalizes later)
        ml_log("[loader] %s %s — attach host=%s dir=%s sotes=%d", OSS_ML_NAME, OSS_ML_VERSION, host, g_gamedir, g_is_sotes);

        // Load only if a mods\ folder sits beside us (game-agnostic: no exe-name gate,
        // but don't act as a random version.dll shim in some unrelated app's folder).
        char modsdir[MAX_PATH];
        _snprintf(modsdir, MAX_PATH, "%smods", g_gamedir);
        DWORD a = GetFileAttributesA(modsdir);
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            CreateThread(NULL, 0, loader_thread, NULL, 0, NULL);
        } else {
            ml_log("[loader] no mods\\ beside the exe — nothing to load");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // reserved != NULL => process is terminating (skip cleanup — loader-lock unsafe);
        // reserved == NULL => explicit FreeLibrary unload (clean up).
        if (reserved == NULL) lh_shutdown();
    }
    return TRUE;
}
