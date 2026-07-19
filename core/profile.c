// core/profile.c — the active game profile.  See profile.h.

#include "profile.h"
#include <stddef.h>

// SotES EN-SE: per-frame input poll 0x437c70 (thiscall; ecx = the input manager) — the
// executor's safepoint.  ImageBase-relative; the executor reloc's it by the ASLR delta.
// The engine's MAIN window class is "CLASS_LIZSOFT_SOTES" (unique, positively matched so the
// bootstrap never latches a transient/launcher window — hwnd verified live, title "Fortune
// Summoners Ver2...").  The SE shows a #32770 launcher dialog first; controls (engine-quirks
// #3): 10003 "Launch", 10020 "Windowed Mode" radio, 10022 "Fullscreen Mode" radio.
static const oss_profile SOTES_EN = { "sotes_en", 0x437c70, "CLASS_LIZSOFT_SOTES", "#32770", 10003, 10020, 10022 };

static const oss_profile *g_current;

void profile_select(int is_sotes) {
    g_current = is_sotes ? &SOTES_EN : NULL;
}
const oss_profile *profile_current(void) { return g_current; }
