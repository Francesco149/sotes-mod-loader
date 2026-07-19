// core/profile.h — per-game facts the core needs, selected by the host exe.
//
// Keeps the loader core game-agnostic: the SotES safepoint VA / window facts live here
// (mirroring profiles/sotes_en.lua, the declarative source we'll load at runtime later).
// For now a small C table selected on the host-exe match computed in loader.c.
#ifndef OSS_PROFILE_H
#define OSS_PROFILE_H

#include <stdint.h>

typedef struct {
    const char *id;
    uintptr_t   safepoint_va;    // ImageBase-relative per-frame fn (the executor's drain point)
    const char *window_class;    // the game's MAIN window registered class — the robust bootstrap
                                 // target (positively matched, never the launcher/transient); NULL = heuristic
    const char *launcher_class;   // pre-game launcher window class to skip/dismiss (NULL = none)
    int         launch_ctrl_id;   // the launcher's "Launch" button control id (for skip_launcher)
    int         windowed_ctrl_id; // the launcher's "Windowed Mode" radio (0 = none)
    int         fullscreen_ctrl_id; // the launcher's "Fullscreen Mode" radio (0 = none)
} oss_profile;

// Select the active profile for this host (call once at init).  is_sotes comes from the
// loader's host-exe match; extend as more profiles land.
void               profile_select(int is_sotes);
const oss_profile *profile_current(void);   // NULL if no profile matched

#endif
