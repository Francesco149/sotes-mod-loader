// core/crashlog.c — top-level crash tracer.
//
// SotES mods run IN-PROCESS, so a bad mod, a mis-RE'd offset, or one of our own hooks can fault the
// game.  On a crash-class exception we append a report to oss_modloader.log — the exception code, the
// faulting address as module+offset, the registers, and an EBP-chain + raw-scan stack walk — so every
// crash leaves a lead (which module / which VA) instead of just vanishing.
//
// A VECTORED handler is used, not SetUnhandledExceptionFilter: the loader attaches as version.dll
// BEFORE the game's CRT startup, which installs its own top-level filter and would clobber ours — a
// VEH can't be overridden.  It's filtered to genuine crash-class codes (skips C++/SEH control-flow
// exceptions) and de-duped by faulting address, so normal play logs nothing.  Everything is
// crash-safe: formatting is wvsprintfA (stack buffers, user32 — no CRT heap/locale) and the append is
// raw CreateFile/WriteFile (not the ml_log critical section, which the faulting thread may hold).
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include "crashlog.h"

static char      g_cl_log[MAX_PATH];
static uintptr_t g_cl_seen[32];        // faulting addresses already reported (dedupe)
static int       g_cl_nseen;

static void cl_write(const char *s, int n) {
    HANDLE h = CreateFileA(g_cl_log, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote;
    WriteFile(h, s, (DWORD)n, &wrote, NULL);
    CloseHandle(h);
}
static void cl_line(const char *fmt, ...) {
    char buf[600];
    va_list ap;
    va_start(ap, fmt);
    int n = wvsprintfA(buf, fmt, ap);   // user32: %s/%d/%u/%x/%X with width — stack only, no heap
    va_end(ap);
    if (n < 0) n = 0;
    if (n > 598) n = 598;
    buf[n++] = '\r';
    buf[n++] = '\n';
    cl_write(buf, n);
}
static int cl_module_of(uintptr_t addr, char *base_out, uintptr_t *off_out) {
    HMODULE mod = NULL;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)addr, &mod) || !mod)
        return 0;
    char full[MAX_PATH];
    full[0] = 0;
    GetModuleFileNameA(mod, full, MAX_PATH);
    const char *base = full;
    for (const char *p = full; *p; p++)
        if (*p == '\\' || *p == '/') base = p + 1;
    lstrcpynA(base_out, base, 64);
    *off_out = addr - (uintptr_t)mod;
    return 1;
}
static void cl_addr(const char *label, uintptr_t addr) {
    char base[64];
    uintptr_t off;
    if (cl_module_of(addr, base, &off))
        cl_line("  %s%08X  %s+%X", label, (unsigned)addr, base, (unsigned)off);
    else
        cl_line("  %s%08X  (no module)", label, (unsigned)addr);
}

// Human name for the exception code in the crash header (esp. the fail-fast codes a plain AV filter misses).
static const char *cl_code_name(DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:      return "ACCESS_VIOLATION";
        case EXCEPTION_ILLEGAL_INSTRUCTION:   return "ILLEGAL_INSTRUCTION";
        case EXCEPTION_PRIV_INSTRUCTION:      return "PRIV_INSTRUCTION";
        case EXCEPTION_STACK_OVERFLOW:        return "STACK_OVERFLOW";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:    return "INT_DIVIDE_BY_ZERO";
        case EXCEPTION_IN_PAGE_ERROR:         return "IN_PAGE_ERROR";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
        case 0xC0000409u:                     return "STACK_BUFFER_OVERRUN (/GS or __fastfail)";
        case 0xC000041Du:                     return "FATAL_USER_CALLBACK (exception escaped a WndProc/callback)";
        case 0xC0000374u:                     return "HEAP_CORRUPTION";
        case 0xC0000420u:                     return "ASSERTION_FAILURE";
        default:                              return "(unnamed error-severity NTSTATUS)";
    }
}

static LONG CALLBACK cl_veh(EXCEPTION_POINTERS *ep) {
    const EXCEPTION_RECORD *er = ep->ExceptionRecord;
    const DWORD code = er->ExceptionCode;
    // Log genuine crash-class exceptions.  Two nets: (1) the common NAMED codes; (2) a catch-all for ANY
    // STATUS_SEVERITY_ERROR NTSTATUS (top nibble 0xC — severity=ERROR, customer bit clear) so FAIL-FAST
    // crashes that never raise a plain AV are ALSO caught: /GS stack-buffer-overrun (0xC0000409), a fatal
    // user-callback (0xC000041D — an exception escaping a WndProc; the likely F8→title culprit), heap
    // corruption (0xC0000374), etc.  Skips C++ EH (0xE06D7363) + other app-defined 0xE/0xD codes (customer
    // bit set) and info/warning-severity codes (0x0/0x4/0x8) that fire during normal play.
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        case EXCEPTION_ILLEGAL_INSTRUCTION:
        case EXCEPTION_PRIV_INSTRUCTION:
        case EXCEPTION_STACK_OVERFLOW:
        case EXCEPTION_INT_DIVIDE_BY_ZERO:
        case EXCEPTION_IN_PAGE_ERROR:
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
            break;
        default:
            if ((code & 0xF0000000u) != 0xC0000000u) return EXCEPTION_CONTINUE_SEARCH;
            break;   // an error-severity NTSTATUS we don't specifically name — log it anyway (fail-fast, etc.)
    }
    // Dedupe by faulting address so a fault the game raises + handles every frame logs only once.
    uintptr_t at = (uintptr_t)er->ExceptionAddress;
    for (int i = 0; i < g_cl_nseen; i++)
        if (g_cl_seen[i] == at)
            return EXCEPTION_CONTINUE_SEARCH;
    if (g_cl_nseen < (int)(sizeof g_cl_seen / sizeof g_cl_seen[0]))
        g_cl_seen[g_cl_nseen++] = at;

    const CONTEXT *c = ep->ContextRecord;
    cl_line("");
    cl_line("==================== [crash] exception %08X %s (tid %u) ====================",
            (unsigned)code, cl_code_name(code), (unsigned)GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        cl_line("  access  %s @ %08X",
                er->ExceptionInformation[0] == 1 ? "WRITE"
                    : er->ExceptionInformation[0] == 8 ? "EXEC" : "READ",
                (unsigned)er->ExceptionInformation[1]);
    else if (er->NumberParameters)   // fail-fast / callback codes carry the real detail here (e.g. a nested status)
        for (DWORD i = 0; i < er->NumberParameters && i < 4; i++)
            cl_line("  param%u  %08X", (unsigned)i, (unsigned)er->ExceptionInformation[i]);
    cl_addr("fault   ", at);
#if defined(_M_IX86) || defined(__i386__)
    cl_line("  eax %08X  ebx %08X  ecx %08X  edx %08X",
            (unsigned)c->Eax, (unsigned)c->Ebx, (unsigned)c->Ecx, (unsigned)c->Edx);
    cl_line("  esi %08X  edi %08X  ebp %08X  esp %08X",
            (unsigned)c->Esi, (unsigned)c->Edi, (unsigned)c->Ebp, (unsigned)c->Esp);
    cl_line("  eip %08X  eflags %08X", (unsigned)c->Eip, (unsigned)c->EFlags);

    cl_line("  stack (ebp frames):");
    uint32_t *frame = (uint32_t *)c->Ebp;
    for (int i = 0; i < 20 && frame && !IsBadReadPtr(frame, 8); i++) {
        cl_addr("   ret ", frame[1]);
        uint32_t next = frame[0];
        if (next <= (uint32_t)(uintptr_t)frame)
            break; // frames must ascend
        frame = (uint32_t *)next;
    }
    cl_line("  stack (scan for code addrs):");
    uint32_t *sp = (uint32_t *)c->Esp;
    for (int i = 0, shown = 0; i < 400 && shown < 20; i++) {
        if (IsBadReadPtr(sp + i, 4))
            break;
        char base[64];
        uintptr_t off;
        if (sp[i] > 0x10000 && cl_module_of(sp[i], base, &off)) {
            cl_line("   [esp+%03X] %08X  %s+%X", (unsigned)(i * 4), (unsigned)sp[i], base, (unsigned)off);
            shown++;
        }
    }
#endif
    cl_line("==================== [crash] end (see docs; toggle mod.game modules to bisect) ====================");
    return EXCEPTION_CONTINUE_SEARCH; // log only — let normal crash handling proceed
}

void crashlog_init(const char *logpath) {
    lstrcpynA(g_cl_log, logpath && *logpath ? logpath : "oss_crash.log", MAX_PATH);
    AddVectoredExceptionHandler(1 /* call first */, cl_veh);
}
