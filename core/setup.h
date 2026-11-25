// core/setup.h — first-time setup for mods (api 1.11): consent, progress, the boot gate.
//
// A Lua mod that ships `setup.lua` beside its init.lua has a ONE-TIME setup step (typically: read assets
// from another game the user owns and convert them into the mod's data folder).  Before it ever runs the
// loader shows a security warning naming the mod and what it says it will do; only on "Yes" does setup.lua
// run (on the loader thread, behind a progress window), and only when it succeeds does the mod's init.lua
// load.  The result is recorded in <game>\mod_data\<mod>\setup.state, so the step runs once per
// `[setup] version`.
//
// While any of that is pending the game's main thread is HELD at its first window (launcher dialog / main
// window): the dialogs never fight a fullscreen game and every mod is initialized before the first frame.
#ifndef OSS_SETUP_H
#define OSS_SETUP_H

#ifdef __cplusplus
extern "C" {
#endif

struct lua_State;

// DllMain (process attach, loader lock held: file reads + hooks only).  If some mods\<name>\setup.lua has
// not completed for its current [setup] version, hook the window-creation entry points so the main thread
// (`main_tid`) waits in its first call until setup_gate_open().  No pending setup → nothing is hooked.
void setup_gate_arm_if_needed(unsigned long main_tid);
// Loader thread, after the mods scan: release the main thread (idempotent).
void setup_gate_open(void);

// Loader thread, per Lua mod, before its init is deferred.  Returns 1 = load the mod (no setup step, or it
// is done / just completed), 0 = skip the mod this session (declined, failed).
int  setup_mod(const char *name, const char *dir);

// <game>\mod_data\<name>\ (trailing '\'); not created here.
void setup_data_dir(const char *name, char *out, unsigned n);
// 1 when <name>'s recorded setup state matches its mod.toml [setup] version (or it has no setup.lua).
int  setup_is_done(const char *name, const char *dir);

// mod.setup = { progress(frac[, text]), log(text), version, done, running }  (pushed onto the Lua stack)
void setup_push_lua(struct lua_State *L, const char *name, const char *dir);

#ifdef __cplusplus
}
#endif
#endif
