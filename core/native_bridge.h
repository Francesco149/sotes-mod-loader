// core/native_bridge.h — the native-mod C ABI bridge (P4).
//
// Fills one OssApi vtable (oss_mod_api.h) from the loader's internal services (mem / executor
// / hooks) and hands it to each native mod's OssModInit.  So a mods\<name>.dll registers into
// the SAME hook + executor registries as Lua mods and the two interleave safely.
#ifndef OSS_NATIVE_BRIDGE_H
#define OSS_NATIVE_BRIDGE_H

#include "oss_mod_api.h"

const OssApi *oss_api(void);   // the shared ABI vtable (const singleton) passed to OssModInit

#endif
