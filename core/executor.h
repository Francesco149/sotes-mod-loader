// core/executor.h — the main-thread executor (P2).
//
// Gets mod code onto the ENGINE thread safely.  Bootstrap = subclass the game window
// (guaranteed main thread, no engine VA) -> on the first message install ONE Tier-1
// observer hook on the profile's per-frame safepoint (SotES 0x437c70) -> drain a job
// queue + run on_frame callbacks there every frame.  All Lua runs on this one thread,
// so engine calls from a mod are safe by construction (loader invariant #1).
//
// Lua surface (added to every mod's `mod` table):
//   mod.on_frame(fn)  -- fn() every frame on the main thread
//   mod.main(fn)      -- run fn() once at the next safepoint (fire-and-forget)
//
// Mods' init.lua ALSO runs on the main thread (deferred to the first safepoint), so a
// mod never touches the engine off-thread.  If no game window / no profile is found the
// executor stays disarmed and the loader falls back to running mods on the loader
// thread (degraded: no on_frame, no main-thread guarantee) — keeps non-game hosts working.
#ifndef OSS_EXECUTOR_H
#define OSS_EXECUTOR_H

#include <stdint.h>
#include "oss_mod_api.h"   // OssJobFn, OssModInitFn (native-mod C callbacks)
struct lua_State;

#ifdef __cplusplus
extern "C" {          // ui.cpp asks the executor for the game window + armed state
#endif

void exec_init(struct lua_State *L);          // bind the Lua state + init the queue (call in lh_init)
void exec_push_main(struct lua_State *L);      // push the mod.main closure     (for push_mod_table)
void exec_push_on_frame(struct lua_State *L);  // push the mod.on_frame closure

void exec_defer_mod(const char *name, const char *dir, const char *init_path);  // Lua mod init at first safepoint
void exec_defer_native(const char *name, OssModInitFn init);    // native mod OssModInit at first safepoint
void exec_run_deferred_inline(void);           // fallback: run deferred mods now, on the caller thread

// Native-mod executor callbacks (the C-side of mod.main / mod.on_frame; used by the ABI bridge).
void exec_enqueue_c(OssJobFn fn, void *user);   // run fn(user) once at the next safepoint (thread-safe)
void exec_on_frame_c(OssJobFn fn, void *user);  // run fn(user) every frame (register on the main thread)

int  exec_bootstrap(void);   // find + subclass the game window, post bootstrap; 1 = armed, 0 = fallback
int  exec_armed(void);       // 1 once the safepoint hook is installed
void *exec_game_hwnd(void);  // the subclassed game window (HWND; void* to keep windows.h out of the header); NULL until bootstrap
uint32_t exec_ti_mgr(void);  // the captured input manager (safepoint `this`); 0 until the first frame
uint32_t exec_main_tid(void);// the engine/main thread id (captured at the first safepoint); 0 until then
uint32_t *exec_main_tid_ptr(void); // &g_main_tid — the typed-hook gate thunk cmp's against it (hooks.c)

void exec_on_safepoint(void *ti_mgr);   // MAIN THREAD drain — the hook (or a test) calls this each frame

#ifdef __cplusplus
}
#endif

#endif
