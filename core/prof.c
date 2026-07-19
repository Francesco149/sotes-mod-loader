// core/prof.c — the opt-in per-frame profiler.  See prof.h.

#include "prof.h"
#include "loader_internal.h"

#include <windows.h>

static int          g_on;
static LARGE_INTEGER g_freq;
static const char  *NAMES[PROF_NBUCKET] = { "safepoint", "ui_build", "roster" };
static struct { uint64_t n, sum, max; } g_b[PROF_NBUCKET];
static uint32_t     g_frames;
static DWORD        g_last_report;

void prof_enable(int on) {
    g_on = on;
    if (!on) return;
    if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
    g_last_report = GetTickCount();
    ml_log("[prof] profiling ON (per-frame loader overhead; reports every 5s)");
}
int prof_enabled(void) { return g_on; }

uint64_t prof_now(void) {
    if (!g_on) return 0;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    return (uint64_t)t.QuadPart;
}
void prof_add(int bucket, uint64_t t0) {
    if (!g_on || !t0 || bucket < 0 || bucket >= PROF_NBUCKET) return;
    LARGE_INTEGER t; QueryPerformanceCounter(&t);
    uint64_t d = (uint64_t)t.QuadPart - t0;
    g_b[bucket].n++; g_b[bucket].sum += d;
    if (d > g_b[bucket].max) g_b[bucket].max = d;
}

// ticks -> tenths of a microsecond (0.1 us units) — integer-only, so no %f in the log.
static uint32_t tenths_us(uint64_t ticks) {
    return g_freq.QuadPart ? (uint32_t)(ticks * 10000000ull / (uint64_t)g_freq.QuadPart) : 0;
}

void prof_frame(void) {
    if (!g_on) return;
    g_frames++;
    DWORD now = GetTickCount();
    if (now - g_last_report < 5000) return;
    uint32_t ms = now - g_last_report;
    ml_log("[prof] --- %u ms, %u safepoints (%u/s) ---", ms, g_frames, g_frames * 1000u / (ms ? ms : 1));
    for (int i = 0; i < PROF_NBUCKET; i++) {
        if (!g_b[i].n) { ml_log("[prof]   %-10s (no samples)", NAMES[i]); continue; }
        uint32_t avg = tenths_us(g_b[i].sum) / (uint32_t)g_b[i].n;   // 0.1us units
        uint32_t mx  = tenths_us(g_b[i].max);
        ml_log("[prof]   %-10s n=%u  avg=%u.%uus  max=%u.%uus",
               NAMES[i], (unsigned)g_b[i].n, avg/10, avg%10, mx/10, mx%10);
        g_b[i].n = g_b[i].sum = g_b[i].max = 0;
    }
    g_frames = 0; g_last_report = now;
}
