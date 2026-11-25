// core/resov.h — the resource overlay (mod.res): see resov.c.
#ifndef OSS_RESOV_H
#define OSS_RESOV_H
#include <windows.h>
#include <stddef.h>
struct lua_State;
// type / name: "#123" (integer id) or a name ("DATA"); hmod NULL = any module.  Copies `data`.
int  resov_set(const char *type, const char *name, HMODULE hmod, const void *data, size_t size);
int  resov_clear(const char *type, const char *name, HMODULE hmod);
HRSRC resov_find(HMODULE hmod, LPCSTR name, LPCSTR type);   // overlay first, then the real one
void resov_push_lua(struct lua_State *L);
#endif
