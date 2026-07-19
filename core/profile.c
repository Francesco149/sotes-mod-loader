// core/profile.c — the active game profile.  See profile.h.

#include "profile.h"
#include <stddef.h>

// SotES EN-SE: per-frame input poll 0x437c70 (thiscall; ecx = the input manager) — the
// executor's safepoint.  ImageBase-relative; the executor reloc's it by the ASLR delta.
// The engine's MAIN window class is "CLASS_LIZSOFT_SOTES" (unique, positively matched so the
// bootstrap never latches a transient/launcher window — hwnd verified live, title "Fortune
// Summoners Ver2...").  The SE shows a #32770 launcher dialog first; controls (engine-quirks
// #3): 10003 "Launch", 10020 "Windowed Mode" radio, 10022 "Fullscreen Mode" radio.
// present_va = zdd_present (thiscall; ECX = the ZDD screen ctx, arg1 = HWND) — fires once per
// frame; switches on the mode field *(ECX+0x164) (0 Full/Flip .. 2 Windowed/BitBlt(SRCCOPY)); the
// finished back-buffer surface is *(*(ECX+0x16c)+0x2c).  The ddraw capture backend hooks it.
// NB: **EN Special Edition (sotes-ense-en.exe, md5 3fe1bc9f) address = 0x5e1470**, found by
// signature (the +0x164 jump-table + GetDC/BitBlt(0xcc0020) case).  OpenSummoners' RE by-address
// files (`5b8fc0.c`) are for the BASE-game unpack (sotes.unpacked.exe, md5 278bad) — a DIFFERENT
// edition whose ddraw code is shifted; only EN-SE-validated addresses work on our target.
static const oss_profile SOTES_EN = { "sotes_en", 0x437c70, "CLASS_LIZSOFT_SOTES", "#32770", 10003, 10020, 10022, 0x5e1470 };

static const oss_profile *g_current;

void profile_select(int is_sotes) {
    g_current = is_sotes ? &SOTES_EN : NULL;
}
const oss_profile *profile_current(void) { return g_current; }
