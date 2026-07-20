// core/native_bridge.h — the native-mod C ABI bridge (P4).
//
// Fills one OssApi vtable (oss_mod_api.h) from the loader's internal services (mem / executor
// / hooks) and hands it to each native mod's OssModInit.  So a mods\<name>.dll registers into
// the SAME hook + executor registries as Lua mods and the two interleave safely.
#ifndef OSS_NATIVE_BRIDGE_H
#define OSS_NATIVE_BRIDGE_H

#include "oss_mod_api.h"

const OssApi *oss_api(void);   // the shared ABI vtable (const singleton) passed to OssModInit

// ── early-boot ABI (OssModEarlyInit) ─────────────────────────────────────────
// The smaller vtable handed to a native mod's OssModEarlyInit in the loader's early-boot phase
// (loader thread, before the game boots).  See oss_mod_api.h "EARLY-BOOT ABI".
const OssEarlyApi *oss_early_api(void);

// Start the shared decrypt/boot poll thread that fires the when_text_ready callbacks.  Call ONCE,
// on the loader thread, AFTER all early mods have run their OssModEarlyInit (so every registration
// is in before polling begins — that keeps registration and polling on disjoint threads, no lock).
// A no-op if nothing registered.
void oss_early_waiter_start(void);

#endif
