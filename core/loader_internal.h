// core/loader_internal.h — shared decls across the loader host TUs.
//
// The loader host is the proxy version.dll: it forwards the real version.dll,
// scans mods\, loads native mods (*.dll) + Lua mods (<name>/init.lua), and hosts
// the LuaJIT runtime that backs the mod.* API.  Everything game-specific lives in
// profiles\<exe>.lua (loaded later); the core carries NO engine addresses so a
// game update never breaks it.
#ifndef OSS_LOADER_INTERNAL_H
#define OSS_LOADER_INTERNAL_H

#include <windows.h>

#define OSS_ML_NAME    "SotES Mod Loader"
#define OSS_ML_VERSION "0.1.0"           // loader host version (mod.loader_version)

// The mod API version — the loader<->mod contract, versioned MAJOR.MINOR independently of the host
// version above.  MAJOR bumps on a BREAKING change (a mod built for a different major refuses to
// load); MINOR bumps on an ADDITIVE change (older mods still load, newer mods needing the new minor
// refuse on an older loader).  A mod declares what it needs in mod.toml `[loader] api = "MAJ.MIN"`
// (absent = assume "1.0").  Native mods have their own OSS_ABI_VERSION (struct-growth) in oss_mod_api.h.
#define OSS_API_VERSION "1.0"
#define OSS_API_MAJOR   1
#define OSS_API_MINOR   0

#ifdef __cplusplus
extern "C" {          // the UI host (ui.cpp) is C++; give the shared C decls C linkage
#endif

// crash-resilient flush-to-file log (append + close each line), shared by every TU.
void ml_log(const char *fmt, ...);

// the game dir (= our own dir; we sit beside the exe as version.dll), trailing '\'.
extern char g_gamedir[MAX_PATH];

#ifdef __cplusplus
}
#endif

#endif
