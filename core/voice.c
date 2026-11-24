// core/voice.c — built-in monster-SFX fix for the SE voice bank.  See voice.h.
//
// v1 (a port of OpenSummoners/tools/ennse_voice) targeted the pre-2026-08 SE build: an EN "seed" (the old EN
// localizer had dropped the sotesx_s.dll load) + this registrar fix, both at hard-coded VAs that no longer exist.
// The 2026-08-09 builds load the bank on EN too (boot loader, ense-en 0x5d7a47 → world+0x318 / 0x92b220), so only
// the registrar bug is left, on both editions.  The registrar (`sound_registrar`: ense-en 0x59b3b0, ense-jp
// 0x599ab0; enold 0x57b280 = the port's al_57b280, no bank there) walks the sound-def table
// ({valid, ?, group u16 +8, normal_id u16 +0xa, count-2 u16 +0x14, module_sel +0x18, deluxe_id u16 +0x1c}, 0x24 B):
//
//   cmp  [bank], ebx            ; 39 1D <bank>        bank loaded?
//   jz   NONDELUXE              ; 74 xx
//   mov  ecx, [settings_pp]     ; 8B 0D <pp>          settings record
//   mov  edx, [ecx]             ; 8B 11
//   cmp  [edx+254h], ebx        ; 39 9A 54 02 00 00   deluxe voices switched off?
//   jnz  NONDELUXE              ; 75 xx
//   mov  cx, deluxe_id[eax]     ; 66 8B 88 <table+0x1c>
//   cmp  cx, bx                 ; 66 3B CB
//   jz   SKIP                   ; 0F 84 <rel32>       <- deluxe_id == 0: registers NOTHING (the bug)
//   cmp  cx, 7FFFh              ; 66 81 F9 FF 7F      "skip in deluxe" sentinel — left alone
//   jz   SKIP                   ; 74 xx
//
// Fix: that rel32 → NONDELUXE, so deluxe_id == 0 rows register their normal id from their normal module.  Register
// state there (eax = row offset, ebx = 0, esi = group arg, edi = sotesd) is what NONDELUXE's other two predecessors
// leave (ecx / edx are reloaded first thing).  The block is matched byte-for-byte except the operands, inside the
// hash-verified registrar, and the two short jumps must agree on NONDELUXE / SKIP — anything else ⇒ no patch.

#include "voice.h"
#include "config.h"
#include "syms.h"
#include "mem.h"
#include "loader_internal.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>

#define VOICE_POLL_MS     1         // SteamStub decrypts .text at the OEP; the registrar runs soon after
#define VOICE_TIMEOUT_MS  120000

static voice_info g_v = { VOICE_OFF, "not started", 0, 0, 0, 0, 0, 0, 0, 0, 0 };
static int g_ed = -1;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

// Find the block in [fn, fn+size); fill g_v's addresses.  Returns 1 = unpatched, 2 = already patched, 0 = none.
static int analyse(const uint8_t *fn, size_t size) {
    static const uint8_t pat[] = {
        0x39, 0x1D, 0, 0, 0, 0,  0x74, 0,  0x8B, 0x0D, 0, 0, 0, 0,  0x8B, 0x11,  0x39, 0x9A, 0x54, 0x02, 0x00, 0x00,
        0x75, 0,  0x66, 0x8B, 0x88, 0, 0, 0, 0,  0x66, 0x3B, 0xCB,  0x0F, 0x84, 0, 0, 0, 0,
        0x66, 0x81, 0xF9, 0xFF, 0x7F,  0x74, 0 };
    static const uint8_t wild[sizeof pat] = {
        0, 0, 1, 1, 1, 1,  0, 1,  0, 0, 1, 1, 1, 1,  0, 0,  0, 0, 0, 0, 0, 0,
        0, 1,  0, 0, 0, 1, 1, 1, 1,  0, 0, 0,  0, 0, 1, 1, 1, 1,
        0, 0, 0, 0, 0,  0, 1 };
    if (!mem_readable(fn, size)) return 0;
    for (size_t i = 0; i + sizeof pat <= size; i++) {
        const uint8_t *p = fn + i;
        size_t k = 0;
        while (k < sizeof pat && (wild[k] || p[k] == pat[k])) k++;
        if (k < sizeof pat) continue;
        uintptr_t nondeluxe = (uintptr_t)p + 8 + (int8_t)p[7];
        uintptr_t skip      = (uintptr_t)p + 47 + (int8_t)p[46];
        uintptr_t je        = (uintptr_t)p + 34;
        uintptr_t je_to     = je + 6 + (int32_t)rd32(p + 36);
        if ((uintptr_t)p + 24 + (int8_t)p[23] != nondeluxe) continue;   // both bank / settings gates → NONDELUXE
        if (je_to != skip && je_to != nondeluxe) continue;
        g_v.registrar = (uintptr_t)fn; g_v.je = je; g_v.skip = skip; g_v.nondeluxe = nondeluxe;
        g_v.bank_global = rd32(p + 2); g_v.settings_pp = rd32(p + 10); g_v.table = rd32(p + 27) - 0x1c;
        // the NONDELUXE / deluxe calls index the slot array: mov ecx, [slots + ecx*4] (8B 0C 8D <slots>)
        for (const uint8_t *q = p + sizeof pat; q < fn + size - 7 && q < p + 0x80; q++)
            if (q[0] == 0x8B && q[1] == 0x0C && q[2] == 0x8D) { g_v.slots = rd32(q + 3); break; }
        return je_to == skip ? 1 : 2;
    }
    return 0;
}

static int locate(int ed) {
    uint32_t size = 0;
    uintptr_t fn = sym_va_early("sound_registrar", ed, &size);
    if (!fn) return -1;                      // not decrypted yet (or a different build)
    return analyse((const uint8_t *)fn, size);
}

static void patch(void) {
    uint8_t *rel = (uint8_t *)g_v.je + 2;
    int32_t to = (int32_t)(g_v.nondeluxe - (g_v.je + 6));
    DWORD old;
    if (!VirtualProtect(rel, 4, PAGE_EXECUTE_READWRITE, &old)) { g_v.state = VOICE_FAILED; g_v.why = "VirtualProtect"; return; }
    memcpy(rel, &to, 4);
    VirtualProtect(rel, 4, old, &old);
    FlushInstructionCache(GetCurrentProcess(), (void *)g_v.je, 6);
    g_v.state = VOICE_PATCHED; g_v.why = NULL;
    ml_log("[voice] registrar %08x: deluxe-skip je @%08x -> %08x (was %08x) — deluxe_id==0 rows (monster SFX) register "
           "their normal id; bank global %08x", (unsigned)g_v.registrar, (unsigned)g_v.je, (unsigned)g_v.nondeluxe,
           (unsigned)g_v.skip, (unsigned)g_v.bank_global);
}

static DWORD WINAPI voice_waiter(void *unused) {
    (void)unused;
    int r = -1;
    for (int ms = 0;; ms += VOICE_POLL_MS) {
        r = locate(g_ed);   // keep polling on 0 too: the hashed head can decrypt before the block does
        if (r > 0) {
            g_v.wait_ms = ms;
            if (r == 1) patch();
            else { g_v.state = VOICE_PATCHED; g_v.why = "already patched"; ml_log("[voice] registrar already patched"); }
            return 0;
        }
        if (ms > VOICE_TIMEOUT_MS) break;
        Sleep(VOICE_POLL_MS);
    }
    g_v.state = VOICE_FAILED;
    g_v.why = r == 0 ? "pattern not found" : "timeout";
    ml_log(r == 0 ? "[voice] sound_registrar verified but the deluxe-skip block is not in it — no patch (fail safe)"
                  : "[voice] TIMEOUT: sound_registrar never matched its hash — no patch (fail safe)");
    return 0;
}

void voice_early_init(void) {
    g_ed = sym_guess_edition();
    if (g_ed != SYM_ENSE_EN && g_ed != SYM_ENSE_JP) { g_v.why = "not an SE edition"; return; }
    if (!config_get_bool("voice", 1)) { g_v.why = "voice=0"; ml_log("[voice] off (voice=0)"); return; }
    g_v.state = VOICE_WAITING; g_v.why = NULL;
    ml_log("[voice] %s: waiting for .text to fix the deluxe-skip in sound_registrar", SYM_EDITIONS[g_ed].tag);
    CreateThread(NULL, 0, voice_waiter, NULL, 0, NULL);
}

const voice_info *voice_get(void) {
    if (g_v.state == VOICE_OFF && !g_v.registrar && g_ed >= 0 && sym_edition() == g_ed) {   // read-only look
        int r = locate(g_ed);
        if (r == 1) g_v.state = VOICE_UNPATCHED;
    }
    return &g_v;
}

int voice_rows(voice_rows_t *o) {
    memset(o, 0, sizeof *o);
    const voice_info *v = voice_get();
    if (!v->table || !v->slots) return 0;
    const uint8_t *t = (const uint8_t *)v->table;
    void *const *slots = (void *const *)v->slots;
    if (mem_readable((void *)v->bank_global, 4)) o->bank = *(const uintptr_t *)v->bank_global;
    const uint8_t *pp = mem_readable((void *)v->settings_pp, 4) ? *(uint8_t *const *)v->settings_pp : NULL;
    const uint8_t *settings = pp && mem_readable(pp, 4) ? *(uint8_t *const *)pp : NULL;
    if (settings && mem_readable(settings + 0x254, 4)) o->deluxe_off = *(const int32_t *)(settings + 0x254) != 0;
    // Rows sharing a slot group: the later registration overwrites the earlier (retail data: group 398 = rows 157 +
    // a later one on both SE builds) — judge only each group's last row, count the rest as shadowed.
    enum { MAXROW = 4096, MAXGROUP = 8192 };
    static int16_t last[MAXGROUP];
    memset(last, 0xff, sizeof last);
    int n = 0;
    for (const uint8_t *r = t; n < MAXROW && mem_readable(r, 0x24) && rd32(r); n++, r += 0x24) {
        uint16_t g; memcpy(&g, r + 0x8, 2);
        if (g < MAXGROUP) last[g] = (int16_t)n;
    }
    for (int i = 0; i < n; i++, t += 0x24) {
        uint16_t group, normal, deluxe;
        memcpy(&group, t + 0x8, 2); memcpy(&normal, t + 0xa, 2); memcpy(&deluxe, t + 0x1c, 2);
        if (!normal || group >= MAXGROUP) continue;
        if (last[group] != i) { o->shadowed++; continue; }
        o->rows++;
        const uint8_t *s = mem_readable(&slots[group], 4) ? (const uint8_t *)slots[group] : NULL;
        uint16_t rid = 0; uintptr_t module = 0;
        if (s && mem_readable(s, 0x18)) { memcpy(&rid, s + 0xc, 2); memcpy(&module, s + 0x10, 4); }  // ar_sound_slot
        if (deluxe == 0) { o->deluxe0++; if (rid == normal) o->deluxe0_ok++; }
        else if (deluxe != 0x7fff) { o->deluxe++; if (rid == deluxe && module && module == o->bank) o->deluxe_ok++; }
    }
    return 1;
}
