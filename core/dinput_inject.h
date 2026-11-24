// core/dinput_inject.h — scripted keyboard input via DirectInput (edition-agnostic).  mod.input.
#ifndef OSS_DINPUT_INJECT_H
#define OSS_DINPUT_INJECT_H
struct lua_State;
int  di_install(void);                 // engine thread, after MH_Initialize; 1 = armed
void di_frame(void);                   // engine thread, once per frame (frame hook)
void di_push_lua(struct lua_State *L); // the C half of mod.input (core/lua/input.lua adds play/keys)
#endif
