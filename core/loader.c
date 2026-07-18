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

#include <stdio.h>
#include <string.h>
#include <stdarg.h>

char        g_gamedir[MAX_PATH];   // our dir (= game dir), trailing '\'
static char g_logpath[MAX_PATH];

void ml_log(const char *fmt, ...) {
    FILE *f = fopen(g_logpath, "a"); if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

// Load one native mod DLL from mods\.  LOAD_WITH_ALTERED_SEARCH_PATH so a mod may
// ship co-located deps in its own folder without polluting the game dir.
static int load_native(const char *filename) {
    char full[MAX_PATH];
    _snprintf(full, MAX_PATH, "%smods\\%s", g_gamedir, filename);
    HMODULE m = LoadLibraryExA(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (m) { ml_log("[loader] native mods\\%s -> %p", filename, (void *)m); return 1; }
    ml_log("[loader] native FAILED mods\\%s (err %lu)", filename, GetLastError());
    return 0;
}

// A directory mod is a Lua mod iff it contains init.lua.
static int load_lua_dir(const char *dirname) {
    char init[MAX_PATH], moddir[MAX_PATH];
    _snprintf(moddir, MAX_PATH, "%smods\\%s", g_gamedir, dirname);
    _snprintf(init,   MAX_PATH, "%s\\init.lua", moddir);
    if (GetFileAttributesA(init) == INVALID_FILE_ATTRIBUTES) return 0;  // not a Lua mod
    lh_run_mod(dirname, moddir, init);
    return 1;
}

// Scan mods\ once: native *.dll files + directories that hold an init.lua.  Runs on
// a worker thread (never DllMain — LoadLibrary there risks loader-lock deadlock),
// spawned from attach so it starts once the loader lock frees: late enough to be
// safe, early enough for hooks.
static DWORD WINAPI loader_thread(void *unused) {
    (void)unused;

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
        ml_log("[loader] %s %s — attach host=%s dir=%s", OSS_ML_NAME, OSS_ML_VERSION, host, g_gamedir);

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
