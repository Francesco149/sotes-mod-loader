// core/voice.h — built-in monster-SFX fix for the Special Edition's Japanese voice bank, applied in the
// early-boot phase.
//
// Both SE builds (2026-08-09 ense-en / ense-jp) load the voice bank sotesx_s.dll themselves.  With the bank
// present, the boot sound-def registrar (symbol `sound_registrar`) takes its deluxe branch and, for every row
// that has a normal sound but NO deluxe variant (deluxe_id == 0: every monster), registers NOTHING → those SFX
// are silent.  The fix flips that one `je <skip>` to `je <non-deluxe path>`, found by pattern inside the
// hash-verified registrar (no hard-coded VA), applied after SteamStub decrypts .text and before the registrar
// runs.  The exe on disk is never touched; enold (no voice bank) is left alone.
//
// Boot-time only: the sound defs register ONCE at boot, so there is no runtime toggle.  oss_loader.cfg key
// `voice` (default ON; voice=0 disables).  Unknown build / pattern mismatch ⇒ no patch (fail safe).
#ifndef OSS_VOICE_H
#define OSS_VOICE_H
#include <stdint.h>

// Arm the patch iff `voice` is on and the header says an SE edition.  Called ONCE from the loader's
// early-boot phase (loader_thread, right after config_load + mem_init); spawns a short-lived waiter thread
// and returns immediately.
void voice_early_init(void);

enum { VOICE_OFF, VOICE_WAITING, VOICE_PATCHED, VOICE_UNPATCHED, VOICE_FAILED };

typedef struct {
    int         state;        // VOICE_* (UNPATCHED = analysed with voice=0: the retail registrar as shipped)
    const char *why;          // short reason for OFF / FAILED, else NULL
    int         wait_ms;      // ms the waiter polled before .text was ready
    uintptr_t   registrar, je, skip, nondeluxe;   // live addresses
    uintptr_t   bank_global, settings_pp, table, slots;
} voice_info;

// The current state; on an engine-ready process with voice=0 this analyses the registrar (read-only) so the
// shipped bug can be measured too.
const voice_info *voice_get(void);

// Walk the sound-def table and the registered slots (call once the game has booted).  Counts rows with a
// normal sound; of those: deluxe0 = no deluxe variant (the rows the bug drops) and how many of them hold their
// normal id; deluxe = with a variant (≠ 0x7fff) and how many hold their deluxe id from the bank module.  shadowed =
// rows whose slot group a later row re-registers (not judged).
typedef struct { int rows, shadowed, deluxe0, deluxe0_ok, deluxe, deluxe_ok, deluxe_off; uintptr_t bank; } voice_rows_t;
int voice_rows(voice_rows_t *out);   // 0 = registrar not analysed

#endif
