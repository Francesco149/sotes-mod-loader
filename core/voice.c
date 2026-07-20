// core/voice.c — built-in Japanese voice restore.  See voice.h.
//
// A faithful port of OpenSummoners/tools/ennse_voice (the standalone proxy version.dll), now hosted
// by the loader's early-boot phase instead of being its own version.dll.  The engine's voice
// subsystem is intact in both retail Special-Edition builds; two tiny memory patches restore it
// (exe on disk untouched, changes live only in the running process):
//
//   (1) SEED (EN only) — sotes_en.exe's English localizer removed the one line that loads
//       sotesx_s.dll, so the voice-bank global stays null: no dialogue, non-deluxe combat.  We
//       inline-hook the bank-load wrapper and set the global (== that removed line) BEFORE the boot
//       sound-def registrar runs, so party rows take the deluxe branch and the dialogue manager
//       builds.  sotes.exe (JP) loads the bank natively, so it skips the seed.
//   (2) DELUXE-SKIP FIX (both editions) — a shared engine bug: with the bank present the registrar
//       takes the deluxe branch and, for the 64 rows that have a normal sound but NO deluxe variant
//       (every monster), registers NOTHING → their SFX go silent.  We flip ONE byte in the registrar
//       so those `deluxe_id==0` rows fall through to the normal (non-deluxe) path instead of the
//       skip.  (The 0x7fff "skip in deluxe" sentinel is left intact.)
//
// Both patches are opcode/prologue-signature GATED: a wrong edition guess or a future build shift
// fails safe (no patch), never corrupting the game.  Addresses target the retail Steam build
// (sotes_en.exe SHA-256 668f7e1a… / sotes.exe); a build shift needs re-derived addresses or an AOB
// scan (adaptability is a follow-up).  See OpenSummoners docs/findings/ense-voice-combat-init.md.

#include "voice.h"
#include "config.h"
#include "loader_internal.h"

#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>

// tiny case-insensitive substring (avoid linking shlwapi for StrStrIA) — self-contained here.
static const char *vstr_ci(const char *hay, const char *needle) {
    if (!hay || !needle) return NULL;
    for (; *hay; hay++) {
        const char *h = hay, *n = needle;
        while (*h && *n && tolower((unsigned char)*h) == tolower((unsigned char)*n)) { h++; n++; }
        if (!*n) return hay;
    }
    return NULL;
}

#define VOICE_ORIG_BASE       0x400000u   // the exe's PE ImageBase (runtime-relocated; delta below)
#define VOICE_BANK_DLL        "sotesx_s.dll"
#define VOICE_DECRYPT_POLL_MS  1          // SteamStub decrypts .text at OEP; poll until the sig shows
#define VOICE_DECRYPT_TIMEOUT  120000     // give up after ~120 s (never blocks: on the waiter thread)

// One retail edition's addresses (ImageBase-relative).  seed_* are EN-only (JP loads the bank).
typedef struct {
    const char *name;
    int         is_en;
    uintptr_t   seed_hook;    // bank-load wrapper — inline-hook to seed the bank global (EN only)
    uintptr_t   seed_resume;  // seed_hook + 6 (after the stolen `sub esp,0x710`)
    uintptr_t   reg_je;       // registrar deluxe-skip `je` (0f 84 83 00 00 00) — flip byte reg_je+2
    uintptr_t   bank_global;  // the voice-bank pointer the seed sets (EN)
} voice_edition;

// EN-SE (sotes_en.exe): seed (localizer removed the bank load) + deluxe-skip fix.
static const voice_edition ED_EN = { "EN-SE", 1, 0x5d8b10u, 0x5d8b16u, 0x59ccccu, 0x92af80u };
// JP-SE (sotes.exe): loads the bank natively — needs ONLY the deluxe-skip fix.
static const voice_edition ED_JP = { "JP-SE", 0, 0,         0,         0x59b2ecu, 0x926170u };

static const voice_edition *g_ed;      // selected edition (set before the waiter spawns)
static uintptr_t            g_delta;   // runtime base - VOICE_ORIG_BASE (ASLR-safe)
static void               **g_bankg;   // resolved voice-bank global (EN) — seed_once writes it

#define VAP(x) ((void *)((uintptr_t)(x) + g_delta))   // ImageBase-relative VA -> live address

// ── EN seed: set the voice-bank pointer on the game thread when the loader calls the bank-load
// wrapper (before the registrar).  Guarded on the pointer being null → seeds once.  Allocates nothing.
static void __cdecl seed_once(void) {
    if (*g_bankg != 0) return;
    char path[MAX_PATH];
    GetModuleFileNameA(NULL, path, MAX_PATH);
    char *bs = strrchr(path, '\\'); if (bs) bs[1] = 0; else path[0] = 0;
    strncat(path, VOICE_BANK_DLL, MAX_PATH - strlen(path) - 1);
    *g_bankg = LoadLibraryA(path);   // == the one line the EN localizer removed
    ml_log("[voice] seed: voice bank -> %p [%s] (before the registrar -> party deluxe + dialogue mgr)",
           *g_bankg, path);
}

// ── deluxe-skip patch (both editions): registrar `je <skip>` -> `je <non-deluxe path>`.  Both je's
// are `0f 84 83 00 00 00`; the redirect delta is 0x36 either way, so we patch the rel32 low byte
// (reg_je + 2) 0x83 -> 0x36.  Opcode-signature gated ⇒ a wrong build fails safe.
static void install_registrar_patch(void) {
    unsigned char *je = (unsigned char *)VAP(g_ed->reg_je);
    if (!(je[0] == 0x0f && je[1] == 0x84 && je[2] == 0x83 && je[3] == 0x00 && je[4] == 0x00 && je[5] == 0x00)) {
        ml_log("[voice] reg: je sig mismatch @0x%x (%02x %02x %02x %02x %02x %02x) — no patch (fail safe)",
               g_ed->reg_je, je[0], je[1], je[2], je[3], je[4], je[5]);
        return;
    }
    DWORD old;
    if (!VirtualProtect(je + 2, 1, PAGE_EXECUTE_READWRITE, &old)) { ml_log("[voice] reg: VirtualProtect failed"); return; }
    je[2] = 0x36;   // -> je the non-deluxe path (monster SFX register from sotesd with their normal id)
    VirtualProtect(je + 2, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), je, 6);
    ml_log("[voice] reg: deluxe-skip patched @0x%x — deluxe_id==0 rows take the non-deluxe path (monster SFX restored)",
           g_ed->reg_je);
}

// ── EN seed hook: inline-hook seed_hook, steal the 6-byte `sub esp,0x710` (81 EC 10 07 00 00),
// trampoline pushad/pushfd → seed_once → stolen insn → jmp seed_resume.  (Only reached on EN, after
// its prologue signature is confirmed by the waiter.)
static void install_seed_hook(void) {
    unsigned char *fn   = (unsigned char *)VAP(g_ed->seed_hook);
    unsigned char *stub = (unsigned char *)VirtualAlloc(NULL, 64, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!stub) { ml_log("[voice] seed: VirtualAlloc failed — no seed"); return; }
    unsigned char *p = stub;
    #define B(x) (*p++ = (unsigned char)(x))
    #define D(x) (*(void **)p = (void *)(x), p += 4)
    B(0x60); B(0x9c);                          // pushad; pushfd
    B(0xb8); D(&seed_once); B(0xff); B(0xd0);  // mov eax, seed_once; call eax
    B(0x9d); B(0x61);                          // popfd; popad
    memcpy(p, fn, 6); p += 6;                  // stolen: sub esp,0x710 (live copy)
    B(0xff); B(0x25);                          // jmp dword ptr [slot]
    { void *slot = p + 4; D(slot); D(VAP(g_ed->seed_resume)); }
    #undef B
    #undef D
    DWORD old;
    if (!VirtualProtect(fn, 6, PAGE_EXECUTE_READWRITE, &old)) { ml_log("[voice] seed: VirtualProtect failed"); return; }
    fn[0] = 0xE9; *(int32_t *)(fn + 1) = (int32_t)((uintptr_t)stub - ((uintptr_t)fn + 5));
    fn[5] = 0x90;
    VirtualProtect(fn, 6, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn, 6);
    ml_log("[voice] seed: hook @0x%x installed (stub=%p)", g_ed->seed_hook, (void *)stub);
}

// Waiter: SteamStub decrypts the whole .text section at once at the OEP; poll the edition's prologue
// signature until it appears (the retail exe is packed; an unpacked/dev exe just matches sooner or,
// on a different build, never — timing out, still fail-safe), then arm the patches — before the
// registrar runs.  Runs on its own thread so the loader thread never blocks on it.
static DWORD WINAPI voice_waiter(void *unused) {
    (void)unused;
    // For EN the decrypt/build gate is the seed-hook prologue `81 EC 10 07 00 00`; for JP it's the
    // registrar je `0f 84 83 00 00 00` (JP has no seed hook).  Both live in .text.
    unsigned char *probe = (unsigned char *)VAP(g_ed->is_en ? g_ed->seed_hook : g_ed->reg_je);
    const unsigned char en_sig[6] = { 0x81, 0xec, 0x10, 0x07, 0x00, 0x00 };
    const unsigned char jp_sig[6] = { 0x0f, 0x84, 0x83, 0x00, 0x00, 0x00 };
    const unsigned char *sig = g_ed->is_en ? en_sig : jp_sig;

    int ms = 0;
    while (memcmp(probe, sig, 6) != 0) {
        if ((ms += VOICE_DECRYPT_POLL_MS) > VOICE_DECRYPT_TIMEOUT) {
            ml_log("[voice] TIMEOUT: %s .text never showed the expected code — no patch (fail safe)", g_ed->name);
            return 0;
        }
        Sleep(VOICE_DECRYPT_POLL_MS);
    }
    ml_log("[voice] %s .text ready after ~%d ms — arming patches", g_ed->name, ms);
    install_registrar_patch();
    if (g_ed->is_en) install_seed_hook();
    return 0;
}

void voice_early_init(void) {
    if (!config_get_int("voice", 0)) return;   // opt-in (default off)

    char host[MAX_PATH] = "";
    GetModuleFileNameA(NULL, host, MAX_PATH);
    char *slash = strrchr(host, '\\'); const char *hn = slash ? slash + 1 : host;

    // Edition by host exe name (like the standalone): sotes_en.exe -> EN (seed + fix); sotes.exe ->
    // JP (fix only).  NB: a renamed EN exe without "sotes_en" in the name is misfiled as JP.
    if (vstr_ci(hn, "sotes_en"))      g_ed = &ED_EN;
    else if (vstr_ci(hn, "sotes"))    g_ed = &ED_JP;
    else { ml_log("[voice] host '%s' is not a sotes exe — voice off", hn); return; }

    g_delta = (uintptr_t)GetModuleHandleA(NULL) - VOICE_ORIG_BASE;
    g_bankg = (void **)VAP(g_ed->bank_global);
    ml_log("[voice] enabled — %s edition, delta=%p; waiting for .text decrypt then arming (%s)",
           g_ed->name, (void *)g_delta, g_ed->is_en ? "seed + deluxe-skip fix" : "deluxe-skip fix");
    CreateThread(NULL, 0, voice_waiter, NULL, 0, NULL);
}
