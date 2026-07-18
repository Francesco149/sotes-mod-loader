// core/profile.c — the active game profile.  See profile.h.

#include "profile.h"
#include <stddef.h>

// SotES EN-SE: per-frame input poll 0x437c70 (thiscall; ecx = the input manager) — the
// executor's safepoint.  ImageBase-relative; the executor reloc's it by the ASLR delta.
static const oss_profile SOTES_EN = { "sotes_en", 0x437c70 };

static const oss_profile *g_current;

void profile_select(int is_sotes) {
    g_current = is_sotes ? &SOTES_EN : NULL;
}
const oss_profile *profile_current(void) { return g_current; }
