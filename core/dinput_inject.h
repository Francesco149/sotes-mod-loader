// core/dinput_inject.h — scripted keyboard input via DirectInput (edition-agnostic).  mod.input.
#ifndef OSS_DINPUT_INJECT_H
#define OSS_DINPUT_INJECT_H
#ifdef __cplusplus
extern "C" {
#endif
struct lua_State;
int  di_install(void);                 // engine thread, after MH_Initialize; 1 = armed
void di_frame(void);                   // engine thread, once per frame (frame hook)
void di_auto(unsigned char dik, int on); // engine thread, before di_frame: a loader-owned hold (separate from scripts / input.hold)
void di_push_lua(struct lua_State *L);
// Keyboard capture (any thread): while on, the game sees NO real keys (levels zero, buffered events dropped) — the UI
// sets it while one of its text fields has focus, so typing into a search box doesn't play the game.  Scripted /
// loader-owned input still goes through.
void di_set_capture(int on);
#ifdef __cplusplus
}
#endif // the C half of mod.input (core/lua/input.lua adds play/keys)
#endif
