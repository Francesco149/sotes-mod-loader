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
#include "voice.h"
#include "mem.h"
#include "native_bridge.h"
#include "ui.h"
#include "prof.h"

#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>

char        g_gamedir[MAX_PATH];   // our dir (= game dir), trailing '\'
static char g_logpath[MAX_PATH];
static int  g_is_sotes;            // host exe matches the SotES profile (register its bindings)

// ml_log is called from several threads (loader, executor, UI, the voice early-boot waiter).  It
// re-opens the file per line, so two concurrent opens would collide and Windows drops one (a lost
// line).  Serialize the whole append behind a critical section, initialized in DllMain before the
// first log.  (g_log_cs_init guards the pre-init window — there is none today, but it's cheap.)
static CRITICAL_SECTION g_log_cs;
static int              g_log_cs_init;

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
    if (g_log_cs_init) EnterCriticalSection(&g_log_cs);
    FILE *f = fopen(g_logpath, "a");
    if (f) {
        va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
        fputc('\n', f); fclose(f);
    }
    if (g_log_cs_init) LeaveCriticalSection(&g_log_cs);
}

// Load one native mod DLL from mods\.  It must be a mod written against our C ABI — export
// OssModInit (engine-thread init at the safepoint) and/or OssModEarlyInit (loader-thread patch in
// the early-boot phase, before the game boots).  A plain DLL that is neither (the old trainer, the
// standalone voice patch) does all its work in its own DllMain and would install conflicting hooks
// the instant we LoadLibrary it, so we PROBE the exports first WITHOUT running any of its code
// (DONT_RESOLVE_DLL_REFERENCES maps the image + lets GetProcAddress read exports, but does NOT call
// DllMain) and skip the DLL entirely if neither export is present.  Real load uses
// LOAD_WITH_ALTERED_SEARCH_PATH so a mod may ship co-located deps in its own folder.  Called from the
// early-boot pass (scan_native_mods_early), so OssModEarlyInit runs before the game's own boot code.
static int load_native(const char *filename) {
    char full[MAX_PATH];
    _snprintf(full, MAX_PATH, "%smods\\%s", g_gamedir, filename);

    // Probe: does this DLL export either loader entry?  Map it without running a byte of its code.
    HMODULE probe = LoadLibraryExA(full, NULL, DONT_RESOLVE_DLL_REFERENCES);
    if (!probe) { ml_log("[loader] native FAILED mods\\%s (err %lu)", filename, GetLastError()); return 0; }
    int has_early = GetProcAddress(probe, OSS_MOD_EARLY_INIT_NAME) != NULL;
    int has_init  = GetProcAddress(probe, OSS_MOD_INIT_NAME)       != NULL;
    FreeLibrary(probe);
    if (!has_early && !has_init) {
        ml_log("[loader] SKIP mods\\%s — no %s / %s export (not a mod-API DLL; move it aside or port it)",
               filename, OSS_MOD_INIT_NAME, OSS_MOD_EARLY_INIT_NAME);
        return 0;
    }

    // Real load: runs DllMain once.
    HMODULE m = LoadLibraryExA(full, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { ml_log("[loader] native FAILED mods\\%s (err %lu)", filename, GetLastError()); return 0; }

    // OssModEarlyInit: run NOW, on the loader thread, in the early-boot phase (before the game boots)
    // — for patches that must beat the game's own boot code.  It gets the smaller early vtable.
    if (has_early) {
        FARPROC pe = GetProcAddress(m, OSS_MOD_EARLY_INIT_NAME);
        OssModEarlyInitFn early = NULL;
        if (pe) memcpy(&early, &pe, sizeof early);   // FARPROC -> typed fn ptr, no -Wcast-function-type
        if (early) {
            int rc = early(oss_early_api());
            ml_log("[loader] native mods\\%s OssModEarlyInit -> %d (early-boot phase)", filename, rc);
        }
    }
    // OssModInit: defer to the first safepoint so it runs on the engine thread (like a Lua mod init).
    if (has_init) {
        FARPROC pf = GetProcAddress(m, OSS_MOD_INIT_NAME);
        OssModInitFn init = NULL;
        if (pf) memcpy(&init, &pf, sizeof init);
        if (init) { exec_defer_native(filename, init); ml_log("[loader] native mods\\%s -> %p (OssModInit deferred to safepoint)", filename, (void *)m); }
    }
    return 1;
}

// Early-boot native pass: scan mods\*.dll and load each (running OssModEarlyInit now, deferring
// OssModInit to the safepoint), then start the shared decrypt/boot waiter.  Runs BEFORE lh_init —
// native mods don't need the Lua host, and an early patch must land before the game's boot code.
// Returns the count of native mods loaded (for the summary log).
static int scan_native_mods_early(void) {
    char pat[MAX_PATH];
    _snprintf(pat, MAX_PATH, "%smods\\*.dll", g_gamedir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int nn = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;   // a dir named *.dll — skip
            nn += load_native(fd.cFileName);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    // Every early mod has now registered its when_text_ready callbacks — start the ONE shared poll
    // thread (keeps registration [loader thread] and polling [waiter thread] disjoint; see the bridge).
    oss_early_waiter_start();
    return nn;
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
    mem_init();               // host base / PE ImageBase / ASLR delta — the early phase needs mem_reloc

    // Early-boot phase: BEFORE the game's own boot code runs, some patches must land (the built-in
    // Japanese voice restore seeds the bank + fixes the monster-SFX registrar before the sound-def
    // registrar runs — normal mod init at the first-frame safepoint is far too late).  Kept minimal
    // (config + mem are up; nothing else yet) so a waiter it spawns beats the registrar, matching the
    // standalone patch's timing.  A no-op unless `voice=1` (see voice.c).
    voice_early_init();

    // The general early-boot ABI for mods: native mods that export OssModEarlyInit run HERE (loader
    // thread, before the game boots) with the smaller early vtable (oss_early_api) — same phase as
    // the built-in voice restore.  This also loads the native mods and defers each OssModInit to the
    // safepoint (the Lua-directory scan stays below, after the Lua host is up).
    int nn = scan_native_mods_early();

    prof_enable(config_get_int("profile", 0));   // opt-in per-frame profiler (dev; profile=1)

    // Register the profile's native game bindings BEFORE the Lua game table finalizes,
    // so mod.game.<id> resolves them (game-agnostic core; SotES knowledge stays in its TU).
    if (g_is_sotes) { sotes_bindings_register(); ml_log("[loader] SotES profile — game bindings registered"); }
    profile_select(g_is_sotes);   // pick the game profile (safepoint VA etc.) for the executor

    // Keep the game ticking while its window is unfocused (the game idles its input poll when it
    // loses foreground).  DEFAULT-ON built-in: the companion UI window steals foreground when you
    // click it, which would otherwise freeze the game loop + the UI snapshot.  keepactive=0 opts out.
    // (The auto-load-a-save feature is now the `autoload` MOD — mod.game.save.load — not a core flag.)
    if (config_get_int("keepactive", 1)) exec_keepactive();

    if (lh_init() != 0) ml_log("[loader] Lua host down — Lua mods skipped, native still load");

    // Lua mods: directories under mods\ that hold an init.lua (native *.dll already loaded in the
    // early-boot pass above).  Deferred to the engine thread; the Lua host is up now.
    char pat[MAX_PATH];
    _snprintf(pat, MAX_PATH, "%smods\\*", g_gamedir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int nl = 0;
    if (h == INVALID_HANDLE_VALUE) {
        // No mods\ folder at all (renamed/missing) — still arm the executor + UI below.  The
        // built-in QoL (focus-keep, launcher dismiss, the overlay/UI) is independent of any mods.
        ml_log("[loader] no mods\\ folder — built-ins only (executor + UI still start)");
    } else {
        do {
            const char *nm = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;   // native *.dll handled early
            if (strcmp(nm, ".") == 0 || strcmp(nm, "..") == 0) continue;
            nl += load_lua_dir(nm);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }

    // Arm the main-thread executor: Lua mods init on the ENGINE thread at the first
    // safepoint.  If there's no game window / no profile, fall back to running them here
    // on the loader thread (degraded, but non-game hosts + the test stub still work).
    if (exec_bootstrap()) {
        ml_log("[loader] executor armed — %d Lua mod(s) will init on the main thread", nl);
        // Stand up the ImGui UI host (loader-owned companion window) on its own thread.  It's fed
        // by a lock-free snapshot the executor builds each frame (ui_build), so it never stalls the
        // game.  Opt-out with ui=0; ui_key is the VK toggle (0 = F10); ui_hz the refresh (0 = 30).
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
        InitializeCriticalSection(&g_log_cs); g_log_cs_init = 1;   // before the first ml_log
        // our own path (we sit beside the exe as version.dll) -> the game dir.
        GetModuleFileNameA(h, g_gamedir, MAX_PATH); g_gamedir[MAX_PATH - 1] = 0;
        char *bs = strrchr(g_gamedir, '\\');
        if (bs) bs[1] = 0; else g_gamedir[0] = 0;
        _snprintf(g_logpath, MAX_PATH, "%soss_modloader.log", g_gamedir);

        char host[MAX_PATH] = ""; GetModuleFileNameA(NULL, host, MAX_PATH);
        char *hslash = strrchr(host, '\\'); const char *hbase = hslash ? hslash + 1 : host;
        g_is_sotes = istr(hbase, "sotes") != NULL;   // profile select (stub — generalizes later)
        ml_log("[loader] %s %s — attach host=%s dir=%s sotes=%d", OSS_ML_NAME, OSS_ML_VERSION, host, g_gamedir, g_is_sotes);

        // Come up if there's anything for us to do beside the exe: a mods\ folder, a recognized
        // game profile (built-in QoL — focus-keep, launcher dismiss, the overlay/UI — runs even
        // with zero mods), or an oss_loader.cfg.  A bare version.dll dropped next to some unrelated
        // app that also imports version.dll matches none of these and stays inert.
        char modsdir[MAX_PATH], cfgpath[MAX_PATH];
        _snprintf(modsdir, MAX_PATH, "%smods", g_gamedir);
        _snprintf(cfgpath, MAX_PATH, "%soss_loader.cfg", g_gamedir);
        DWORD ma = GetFileAttributesA(modsdir);
        int have_mods = (ma != INVALID_FILE_ATTRIBUTES) && (ma & FILE_ATTRIBUTE_DIRECTORY);
        int have_cfg  = GetFileAttributesA(cfgpath) != INVALID_FILE_ATTRIBUTES;
        if (have_mods || g_is_sotes || have_cfg) {
            CreateThread(NULL, 0, loader_thread, NULL, 0, NULL);
        } else {
            ml_log("[loader] not a known game, no mods\\, no cfg — nothing to do");
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        // reserved != NULL => process is terminating (skip cleanup — loader-lock unsafe);
        // reserved == NULL => explicit FreeLibrary unload (clean up).
        if (reserved == NULL) lh_shutdown();
    }
    return TRUE;
}
