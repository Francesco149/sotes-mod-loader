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
    uintptr_t   safepoint_va;   // ImageBase-relative per-frame fn (the executor's drain point)
} oss_profile;

// Select the active profile for this host (call once at init).  is_sotes comes from the
// loader's host-exe match; extend as more profiles land.
void               profile_select(int is_sotes);
const oss_profile *profile_current(void);   // NULL if no profile matched

#endif
