// core/game_bindings.c — the togglable game-knowledge registry.  See game_bindings.h.
//
// `mod.game` is one shared table (control fns + a metatable) whose binding ids resolve
// dynamically through __index: an ENABLED binding returns its (lazily built, cached)
// API table; a DISABLED one returns nil.  Toggling is therefore live, and each
// accessor also self-checks gb_enabled() so a reference captured while enabled goes
// inert on disable.

#include "game_bindings.h"
#include "loader_internal.h"

#include <string.h>

#include "lua.h"
#include "lauxlib.h"

#define GB_MAX 32
static struct { gb_def def; int enabled; int module_ref; } g_e[GB_MAX];
static int g_n;

void gb_register(const gb_def *d) {
    if (g_n >= GB_MAX) { ml_log("[game] registry full — dropping binding %s", d->id); return; }
    g_e[g_n].def = *d;
    g_e[g_n].enabled = d->default_enabled ? 1 : 0;
    g_e[g_n].module_ref = LUA_NOREF;
    ml_log("[game] binding registered: %-14s (%s) %s", d->id, g_e[g_n].enabled ? "on" : "off", d->desc ? d->desc : "");
    g_n++;
}
int gb_set_enabled(const char *id, int on) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_e[i].def.id, id)) {
        g_e[i].enabled = on ? 1 : 0;
        ml_log("[game] binding %s -> %s", id, on ? "ENABLED" : "DISABLED");
        return 1;
    }
    return 0;
}
int gb_enabled(const char *id) {
    for (int i = 0; i < g_n; i++) if (!strcmp(g_e[i].def.id, id)) return g_e[i].enabled;
    return 0;
}
int gb_count(void) { return g_n; }

// mod.game metatable __index(t, key): dynamic binding resolve.
static int gb_index(lua_State *L) {
    const char *key = lua_tostring(L, 2);
    if (key) for (int i = 0; i < g_n; i++) if (!strcmp(g_e[i].def.id, key)) {
        if (!g_e[i].enabled) { lua_pushnil(L); return 1; }             // hidden while disabled
        if (g_e[i].module_ref == LUA_NOREF) {                          // build + cache once
            g_e[i].def.install(L);                                     // pushes the module table
            lua_pushvalue(L, -1);
            g_e[i].module_ref = luaL_ref(L, LUA_REGISTRYINDEX);        // ref the dup (pops it)
            return 1;                                                  // original stays for return
        }
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_e[i].module_ref);
        return 1;
    }
    lua_pushnil(L);
    return 1;
}

static int gb_l_enable (lua_State *L) { lua_pushboolean(L, gb_set_enabled(luaL_checkstring(L, 1), 1)); return 1; }
static int gb_l_disable(lua_State *L) { lua_pushboolean(L, gb_set_enabled(luaL_checkstring(L, 1), 0)); return 1; }
static int gb_l_enabled(lua_State *L) { lua_pushboolean(L, gb_enabled(luaL_checkstring(L, 1))); return 1; }
static int gb_l_list   (lua_State *L) {
    lua_newtable(L);
    for (int i = 0; i < g_n; i++) {
        lua_newtable(L);
        lua_pushstring(L, g_e[i].def.id);                 lua_setfield(L, -2, "id");
        lua_pushstring(L, g_e[i].def.desc ? g_e[i].def.desc : ""); lua_setfield(L, -2, "desc");
        lua_pushboolean(L, g_e[i].enabled);               lua_setfield(L, -2, "enabled");
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

static int g_game_ref = LUA_NOREF;

void gb_finalize_lua(lua_State *L) {
    lua_newtable(L);                                                   // game
    lua_pushcfunction(L, gb_l_enable);  lua_setfield(L, -2, "enable");
    lua_pushcfunction(L, gb_l_disable); lua_setfield(L, -2, "disable");
    lua_pushcfunction(L, gb_l_enabled); lua_setfield(L, -2, "enabled");
    lua_pushcfunction(L, gb_l_list);    lua_setfield(L, -2, "list");
    lua_newtable(L);                                                   // meta
    lua_pushcfunction(L, gb_index);     lua_setfield(L, -2, "__index");
    lua_setmetatable(L, -2);
    g_game_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    ml_log("[game] mod.game finalized (%d binding(s))", g_n);
}
void gb_push_lua(lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, g_game_ref); }
