// core/prof.h — a tiny opt-in profiler for the loader's per-frame overhead.
//
// Measures how long the loader itself costs the ENGINE thread each frame — the safepoint drain,
// the UI snapshot build, and the roster heap-scan — and logs periodic avg/max stats so we can see
// the loader's overhead (and the heap-scan's spikes) in real gameplay, not just at the menu.
// Enabled by `profile=1` in oss_loader.cfg; OFF by default (then every call early-returns, no QPC).
#ifndef OSS_PROF_H
#define OSS_PROF_H

#include <stdint.h>

enum { PROF_SAFEPOINT, PROF_UI_BUILD, PROF_ROSTER, PROF_NBUCKET };

void     prof_enable(int on);              // from config (profile=1)
int      prof_enabled(void);
uint64_t prof_now(void);                   // QPC ticks now (0 when disabled)
void     prof_add(int bucket, uint64_t t0);// accumulate (now - t0) into a bucket
void     prof_frame(void);                 // call once per safepoint; logs + resets every ~5 s

#endif
