// core/lua_host.c — LuaJIT runtime backing the mod.* API (P0 surface).
//
// One shared lua_State, JIT forced off (interpreter + FFI only — the stable mode
// for an injected DLL: no JIT code pages in the game's address space; FFI stays on
// for later native calls / struct cdefs).  Each Lua mod runs inside its own
// environment table + a protected call, so a faulting mod is caught + logged and
// never takes down the game or the other mods (loader invariant #7).
//
// P0 `mod` table:  mod.name  mod.dir  mod.log(...)  mod.loader  mod.loader_version
// Later phases add mod.mem / mod.hook / mod.main / mod.on_frame / mod.ui onto the
// same table (docs/DESIGN.md).  P0 runs init.lua on the loader thread; the
// main-thread executor (P2) will marshal engine-touching work onto the game thread.

#include "loader_internal.h"
#include "lua_host.h"
#include "mem.h"
#include "game_bindings.h"
#include "executor.h"
#include "hooks.h"
#include "ui.h"
#include "config.h"

#include <string.h>
#include <stdio.h>

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"
#include "luajit.h"

// The shared Lua state.  Touched ONLY on the engine thread: mod init + on_frame + hook
// dispatch (executor safepoint) and the per-frame UI snapshot build (ui_build, also on the
// safepoint) all run there.  The UI thread never touches it — it consumes a lock-free snapshot
// (ui.cpp), so there is no cross-thread lua_State access and no lock (P5, decoupled model).
static lua_State *g_L;

// mod.config factory + mod.toml [loader]-meta reader (embedded Lua closures, built once at lh_init):
// make_mod_config(modid, toml) -> a per-mod config table; read_meta(toml) -> { api=, min_version= }
// for the API-compat load gate.  See CONFIG_LUA below.  LUA_NOREF until built.
static int g_mkconfig_ref = LUA_NOREF;
static int g_meta_ref     = LUA_NOREF;

// mod.log(...) — like print(): tostring() each arg, space-join, route to the loader
// log with the mod's name (carried as an upvalue so every mod's log is attributed).
static int l_mod_log(lua_State *L) {
    const char *name = lua_tostring(L, lua_upvalueindex(1));
    int n = lua_gettop(L);
    char buf[1024];
    size_t off = 0;
    for (int i = 1; i <= n; i++) {
        lua_getglobal(L, "tostring");
        lua_pushvalue(L, i);
        lua_call(L, 1, 1);
        const char *s = lua_tostring(L, -1);
        if (!s) s = "?";
        off += (size_t)_snprintf(buf + off, sizeof buf - off, "%s%s", (i > 1 ? " " : ""), s);
        lua_pop(L, 1);
        if (off >= sizeof buf - 1) break;
    }
    buf[sizeof buf - 1] = 0;
    ml_log("[mod] %s: %s", name ? name : "?", buf);
    return 0;
}

// Build + push a fresh per-mod `mod` table (its own name/dir/log closure).
static void push_mod_table(lua_State *L, const char *name, const char *dir) {
    lua_newtable(L);

    lua_pushstring(L, name);                 lua_setfield(L, -2, "name");
    lua_pushstring(L, dir);                  lua_setfield(L, -2, "dir");
    lua_pushstring(L, OSS_ML_NAME);          lua_setfield(L, -2, "loader");
    lua_pushstring(L, OSS_ML_VERSION);       lua_setfield(L, -2, "loader_version");
    lua_pushstring(L, OSS_API_VERSION);      lua_setfield(L, -2, "api_version");   // the mod API contract (MAJ.MIN)

    lua_pushstring(L, name);                 // upvalue for the log closure
    lua_pushcclosure(L, l_mod_log, 1);       lua_setfield(L, -2, "log");

    mem_push_lua(L);                         lua_setfield(L, -2, "mem");   // shared mem service (P1)
    gb_push_lua(L);                          lua_setfield(L, -2, "game");  // shared game bindings
    exec_push_main(L);                       lua_setfield(L, -2, "main");     // run on the main thread (P2)
    exec_push_on_frame(L);                   lua_setfield(L, -2, "on_frame"); // per-frame callback (P2)
    hooks_push_table(L);                     lua_setfield(L, -2, "hook");     // chained hook registry (P3)
    ui_push_table(L);                        lua_setfield(L, -2, "ui");       // shared ImGui UI host (P5)

    // mod.config — the mod's own settings, schema from <dir>\mod.toml [config], values namespaced in
    // oss_mods.cfg.  Built by the embedded factory (CONFIG_LUA).  get/set/schema; nil if the factory
    // failed to build (a mod with no mod.toml still gets a working config with an empty schema).
    if (g_mkconfig_ref != LUA_NOREF) {
        char tp[MAX_PATH];
        _snprintf(tp, MAX_PATH, "%s\\mod.toml", dir);
        lua_rawgeti(L, LUA_REGISTRYINDEX, g_mkconfig_ref);   // make_mod_config
        lua_pushstring(L, name);
        lua_pushstring(L, tp);
        if (lua_pcall(L, 2, 1, 0) == 0) lua_setfield(L, -2, "config");
        else { ml_log("[cfg] mod.config build failed for %s: %s", name, lua_tostring(L, -1)); lua_pop(L, 1); }
    }
}

// ── mod.config bridge: the raw namespaced value store (oss_mods.cfg, via config.c) ──
static int l_cfg_raw_get(lua_State *L) {
    const char *v = config_mod_get(luaL_checkstring(L, 1));
    if (v) lua_pushstring(L, v); else lua_pushnil(L);
    return 1;
}
static int l_cfg_raw_set(lua_State *L) { config_mod_set(luaL_checkstring(L, 1), luaL_optstring(L, 2, "")); return 0; }

// The mod.config factory, in Lua: parses a mod.toml [config] schema (a small TOML subset) and returns
// a config table bound to the mod's id.  get(key) resolves the stored value (coerced by the declared
// type) or the schema default; set(key, v) clamps to min/max, writes the namespaced value + persists;
// schema() returns (schema, order) for a generic editor (the overlay / the launcher).  Chunk args
// (...) are the C raw get/set/save.  Runs in the global env, so its closures keep io/string/math even
// when called from a sandboxed mod.
static const char *CONFIG_LUA =
    "local raw_get, raw_set = ...\n"
    "local function pval(v)\n"
    "  local q=v:match('^\"(.-)\"'); if q then return q end\n"    // quoted: up to the first closing quote (drops a trailing #comment)
    "  v=(v:gsub('%s*#.*$','')):gsub('%s+$','')\n"                 // unquoted: strip an inline comment + ws
    "  if v=='true' then return true elseif v=='false' then return false end\n"
    "  local arr=v:match('^%[(.*)%]$')\n"
    "  if arr then local t={} for it in arr:gmatch('[^,]+') do it=it:gsub('^%s+',''):gsub('%s+$','')\n"
    "    t[#t+1]=(it:match('^\"(.-)\"')) or tonumber(it) or it end return t end\n"
    "  return tonumber(v) or v\n"
    "end\n"
    "local function parse(path)\n"
    "  local f=io.open(path,'r'); if not f then return {},{} end\n"
    "  local schema,order={},{}; local cur=nil\n"
    "  for line in f:lines() do\n"
    "    line=line:gsub('^%s+',''):gsub('%s+$','')\n"
    "    if line~='' and line:sub(1,1)~='#' then\n"
    "      local sect=line:match('^%[config%.([%w_%-]+)%]$')\n"
    "      if sect then cur={key=sect}; schema[sect]=cur; order[#order+1]=sect\n"
    "      elseif line:sub(1,1)=='[' then cur=nil\n"
    "      elseif cur then local k,v=line:match('^([%w_]+)%s*=%s*(.+)$'); if k then cur[k]=pval(v) end end\n"
    "    end\n"
    "  end\n"
    "  f:close(); return schema,order\n"
    "end\n"
    "local function coerce(spec,raw)\n"
    "  if raw==nil then return nil end\n"
    "  local t=spec and spec.type\n"
    "  if t=='bool' then return raw=='true' or raw=='1'\n"
    "  elseif t=='int' then return math.floor(tonumber(raw) or 0)\n"
    "  elseif t=='float' then return tonumber(raw) or 0 end\n"
    "  return raw\n"
    "end\n"
    "local function read_meta(path)\n"                 // extract [loader] (api, min_version) for the load gate
    "  local f=io.open(path,'r'); if not f then return {} end\n"
    "  local m={}; local inl=false\n"
    "  for line in f:lines() do\n"
    "    line=line:gsub('^%s+',''):gsub('%s+$','')\n"
    "    if line:sub(1,1)=='[' then inl=(line=='[loader]')\n"
    "    elseif inl then local k,v=line:match('^([%w_]+)%s*=%s*(.+)$'); if k then m[k]=(v:match('^\"(.-)\"')) or (v:gsub('%s*#.*$','')) end end\n"
    "  end\n"
    "  f:close(); return m\n"
    "end\n"
    "local function make_config(modid, toml_path)\n"
    "  local schema,order=parse(toml_path)\n"
    "  local cfg={}\n"
    "  function cfg.schema() return schema, order end\n"
    "  function cfg.get(key)\n"
    "    local spec=schema[key]; local v=coerce(spec, raw_get(modid..'.'..key))\n"
    "    if v==nil and spec then return spec.default end\n"
    "    return v\n"
    "  end\n"
    "  function cfg.set(key, value)\n"
    "    local spec=schema[key]\n"
    "    if spec and (spec.type=='int' or spec.type=='float') and type(value)=='number' then\n"
    "      if spec.min and value<spec.min then value=spec.min end\n"
    "      if spec.max and value>spec.max then value=spec.max end\n"
    "      if spec.type=='int' then value=math.floor(value) end\n"
    "    end\n"
    "    local s; if type(value)=='boolean' then s=(value and 'true' or 'false') else s=tostring(value) end\n"
    "    raw_set(modid..'.'..key, s); return cfg.get(key)\n"
    "  end\n"
    "  return cfg\n"
    "end\n"
    "return make_config, read_meta\n";

static void config_lua_init(lua_State *L) {
    if (luaL_loadstring(L, CONFIG_LUA) != 0) { ml_log("[cfg] CONFIG_LUA load failed: %s", lua_tostring(L, -1)); lua_pop(L, 1); return; }
    lua_pushcfunction(L, l_cfg_raw_get);
    lua_pushcfunction(L, l_cfg_raw_set);
    if (lua_pcall(L, 2, 2, 0) != 0) { ml_log("[cfg] mod-config factory init failed: %s", lua_tostring(L, -1)); lua_pop(L, 1); return; }
    g_meta_ref     = luaL_ref(L, LUA_REGISTRYINDEX);   // read_meta   (top)
    g_mkconfig_ref = luaL_ref(L, LUA_REGISTRYINDEX);   // make_config (below it)
    ml_log("[cfg] mod.config ready (mod.toml [config] schema + oss_mods.cfg values); API %s", OSS_API_VERSION);
}

// ── mod API-version gate: refuse a mod whose declared mod.toml [loader] api is incompatible ──
// Compatible iff same MAJOR (breaking axis) AND the mod's MINOR <= ours (we have the features it needs).
// Absent/unparseable-but-present handled by the caller.  Fills `reason` on refusal.
static int api_compatible(const char *want, char *reason, size_t rn) {
    int wmaj = OSS_API_MAJOR, wmin = 0;
    if (want && *want && sscanf(want, "%d.%d", &wmaj, &wmin) < 1) { _snprintf(reason, rn, "unparseable api \"%s\"", want); return 0; }
    if (wmaj != OSS_API_MAJOR) { _snprintf(reason, rn, "built for API major %d, loader is %s (breaking)", wmaj, OSS_API_VERSION); return 0; }
    if (wmin > OSS_API_MINOR)  { _snprintf(reason, rn, "needs API %d.%d, loader is %s", wmaj, wmin, OSS_API_VERSION); return 0; }
    return 1;
}
// Read mod.toml [loader].api and check compatibility.  Returns 1 = load it, 0 = refuse (logs the reason).
static int mod_api_ok(const char *name, const char *dir) {
    if (g_meta_ref == LUA_NOREF) return 1;               // no parser (shouldn't happen) — don't block
    char tp[MAX_PATH]; _snprintf(tp, MAX_PATH, "%s\\mod.toml", dir);
    char apibuf[32] = ""; const char *want = NULL;
    lua_rawgeti(g_L, LUA_REGISTRYINDEX, g_meta_ref);
    lua_pushstring(g_L, tp);
    if (lua_pcall(g_L, 1, 1, 0) == 0) {
        if (lua_istable(g_L, -1)) {
            lua_getfield(g_L, -1, "api");
            if (lua_isstring(g_L, -1)) { _snprintf(apibuf, sizeof apibuf, "%s", lua_tostring(g_L, -1)); want = apibuf; }
            lua_pop(g_L, 1);
        }
    } else { ml_log("[lua] %s: mod.toml meta read error: %s", name, lua_tostring(g_L, -1)); }
    lua_pop(g_L, 1);
    char reason[128];
    if (!api_compatible(want, reason, sizeof reason)) {
        ml_log("[lua] REFUSED mod '%s' — %s. Set mod.toml [loader] api to a compatible version.", name, reason);
        return 0;
    }
    return 1;
}

int lh_init(void) {
    g_L = luaL_newstate();
    if (!g_L) { ml_log("[lua] luaL_newstate FAILED"); return 1; }
    luaL_openlibs(g_L);
    // Belt-and-suspenders: the lib is already built with LUAJIT_DISABLE_JIT, but
    // force the engine off at runtime too so no path ever JIT-compiles.
    luaJIT_setmode(g_L, 0, LUAJIT_MODE_ENGINE | LUAJIT_MODE_OFF);

    mem_init();                 // host base / PE ImageBase / ASLR delta
    mem_install_lua(g_L);       // the shared mod.mem table
    gb_finalize_lua(g_L);       // the shared mod.game table (bindings registered before this)
    exec_init(g_L);             // the main-thread executor (mod.main / mod.on_frame)
    hooks_init(g_L);            // the chained hook registry (mod.hook.entry / remove)
    ui_init(g_L);               // the ImGui UI host — builds the shared mod.ui table (P5)
    config_lua_init(g_L);       // the mod.config factory (mod.toml [config] schema + oss_mods.cfg)

    ml_log("[lua] LuaJIT up (%s, JIT off, FFI on)", LUAJIT_VERSION);
    return 0;
}

void lh_run_mod(const char *name, const char *dir, const char *init_lua_path) {
    if (!g_L) { ml_log("[lua] no state — skip mod %s", name); return; }

    if (!mod_api_ok(name, dir)) return;   // API-version gate: refuse an incompatible mod before running it

    // Load the chunk; a syntax error is caught here (not propagated).
    if (luaL_loadfile(g_L, init_lua_path) != 0) {
        ml_log("[lua] LOAD FAILED %s: %s", name, lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return;
    }

    // Fresh sandbox environment: the mod's writes land here (mods don't clobber
    // each other's globals); reads fall through to _G via the metatable, so the
    // Lua stdlib (string/table/math/print/...) is available.  Lua 5.1 setfenv.
    lua_newtable(g_L);                                    // env
    push_mod_table(g_L, name, dir);                      // env, mod
    lua_setfield(g_L, -2, "mod");                        // env.mod = <table>
    lua_newtable(g_L);                                   // env, meta
    lua_getglobal(g_L, "_G");                            // env, meta, _G
    lua_setfield(g_L, -2, "__index");                    // meta.__index = _G
    lua_setmetatable(g_L, -2);                           // setmetatable(env, meta)
    lua_setfenv(g_L, -2);                                // setfenv(chunk, env)

    // Protected call — a runtime error is caught, the mod flagged, loader lives.
    if (lua_pcall(g_L, 0, 0, 0) != 0) {
        ml_log("[lua] RUNTIME ERROR %s: %s", name, lua_tostring(g_L, -1));
        lua_pop(g_L, 1);
        return;
    }
    ml_log("[lua] mod ok: %s", name);
}

lua_State *lh_state(void) { return g_L; }

void lh_shutdown(void) {
    config_mod_flush(1);   // persist any pending (debounced) mod-config changes before we go
    ui_shutdown();   // signal the UI thread to stop touching the state before we close it (best-effort)
    if (g_L) { lua_close(g_L); g_L = NULL; ml_log("[lua] state closed"); }
}
