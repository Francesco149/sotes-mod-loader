// core/hooks.c — the chained hook registry (Tier-1 observers + Tier-2 typed).  See hooks.h.

#include "hooks.h"
#include "executor.h"
#include "loader_internal.h"

#include <windows.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "MinHook.h"

static lua_State *g_L;

#define MAX_HOOKS 64
#define MAX_CB    16
typedef struct { int used; int ref; } hcb;
typedef struct {
    int          used;
    int          tier;     // 1 = entry observer (C thunk), 2 = typed (Lua FFI-closure detour)
    uintptr_t    va;       // absolute target
    void        *tramp;    // MinHook trampoline (the original)
    uint8_t     *thunk;    // our per-VA detour (RWX pool): Tier-1 = observer thunk, Tier-2 = main-thread gate
    hcb          cb[MAX_CB];   // Tier-1 only (Tier-2's chain lives Lua-side)
    volatile long busy;    // per-VA reentrancy guard (Tier-1)
} hookrec;
static hookrec g_hk[MAX_HOOKS];

// ── the RWX thunk pool (both tiers' per-VA detours live here) ────────────────
#define THUNK_SZ 24
static uint8_t *g_pool; static size_t g_pool_off;

static uint8_t *alloc_thunk(void) {
    if (!g_pool) { g_pool = (uint8_t *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); g_pool_off = 0; }
    if (!g_pool || g_pool_off + THUNK_SZ > 0x1000) return NULL;
    uint8_t *t = g_pool + g_pool_off; g_pool_off += THUNK_SZ; return t;
}

// ── Tier-1: per-VA thunk — capture regs -> hook_dispatch(rec, regs) -> jmp original ──
// Layout (24 B):  pushfd; pushad; push esp; push <rec>; call <dispatch>; add esp,8;
//                 popad; popfd; jmp <tramp>
// pushad frame @ [esp] after the two pushes' base = regs: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX;
// the caller's stack top (retaddr) is at regs + 0x24.  Stack-balanced + ends in a jmp, so
// the original runs with the caller's exact stack — arg-agnostic, can't modify/block.
static void hook_dispatch(hookrec *rec, uint32_t *regs);   // fwd

static void gen_thunk(uint8_t *t, hookrec *rec, void *tramp) {
    static const uint8_t tmpl[THUNK_SZ] = {
        0x9C, 0x60, 0x54,               // pushfd; pushad; push esp
        0x68, 0,0,0,0,                  // push <rec>            @4
        0xE8, 0,0,0,0,                  // call <dispatch>       @9 (rel from @13)
        0x83, 0xC4, 0x08,               // add esp, 8
        0x61, 0x9D,                     // popad; popfd
        0xE9, 0,0,0,0,                  // jmp <tramp>           @19 (rel from @23)
        0x90                            // pad
    };
    memcpy(t, tmpl, THUNK_SZ);
    *(uint32_t *)(t + 4)  = (uint32_t)(uintptr_t)rec;
    *(uint32_t *)(t + 9)  = (uint32_t)((uintptr_t)&hook_dispatch - ((uintptr_t)t + 13));
    *(uint32_t *)(t + 19) = (uint32_t)((uintptr_t)tramp - ((uintptr_t)t + 23));
    FlushInstructionCache(GetCurrentProcess(), t, THUNK_SZ);
}

// ── Tier-2: per-VA main-thread GATE — the detour of a typed hook ─────────────
// A typed hook's real detour is a LuaJIT FFI closure (the chain dispatcher).  But a
// closure entered on a NON-engine thread would reenter the shared lua_State off the main
// thread and corrupt every mod (invariant #1 / the multi-mod keystone).  So MinHook points
// the VA at this tiny C gate instead, which runs the closure ONLY on the engine thread and
// otherwise tail-jumps the trampoline (the original, no Lua).  EAX + EFLAGS are dead at a
// call boundary (caller-saved), and ECX/EDX/stack are untouched — so thiscall `this` (ecx)
// and every arg flow into the closure (or the original) exactly as the caller passed them.
// Layout (24 B):  mov eax, fs:[0x24] (current tid); cmp eax,[&g_main_tid];
//                 jne +5; jmp <closure>; jmp <tramp>
static void gen_gate_thunk(uint8_t *t, void *closure, void *tramp) {
    static const uint8_t tmpl[THUNK_SZ] = {
        0x64, 0xA1, 0x24,0x00,0x00,0x00,   // mov eax, fs:[0x24]     @0  (TEB.ClientId.UniqueThread)
        0x3B, 0x05, 0,0,0,0,               // cmp eax, [&g_main_tid] @6  (abs addr @8)
        0x75, 0x05,                        // jne +5  (-> jmp tramp) @12
        0xE9, 0,0,0,0,                     // jmp <closure>          @14 (rel from @19)
        0xE9, 0,0,0,0                      // jmp <tramp>            @19 (rel from @24)
    };
    memcpy(t, tmpl, THUNK_SZ);
    *(uint32_t *)(t + 8)  = (uint32_t)(uintptr_t)exec_main_tid_ptr();
    *(uint32_t *)(t + 15) = (uint32_t)((uintptr_t)closure - ((uintptr_t)t + 19));
    *(uint32_t *)(t + 20) = (uint32_t)((uintptr_t)tramp   - ((uintptr_t)t + 24));
    FlushInstructionCache(GetCurrentProcess(), t, THUNK_SZ);
}

// The Tier-1 dispatcher: walk the chain, call each observer with a register context.  cdecl —
// the generated thunk calls this as hook_dispatch(rec, regs).
static void hook_dispatch(hookrec *rec, uint32_t *regs) {
    if (InterlockedExchange(&rec->busy, 1)) return;                 // reentrancy: skip the chain
    if (GetCurrentThreadId() != exec_main_tid() || !g_L) { InterlockedExchange(&rec->busy, 0); return; }  // Lua only on the engine thread

    uintptr_t caller_esp = (uintptr_t)regs + 0x24;                  // above pushfd(4)+pushad(0x20)
    lua_State *L = g_L;
    lua_newtable(L);
    lua_pushnumber(L, regs[6]);              lua_setfield(L, -2, "ecx");
    lua_pushnumber(L, regs[5]);              lua_setfield(L, -2, "edx");
    lua_pushnumber(L, regs[7]);              lua_setfield(L, -2, "eax");
    lua_pushnumber(L, (double)caller_esp);   lua_setfield(L, -2, "esp");
    lua_pushnumber(L, *(uint32_t *)caller_esp); lua_setfield(L, -2, "ret");
    lua_pushnumber(L, (double)rec->va);      lua_setfield(L, -2, "va");
    for (int i = 0; i < MAX_CB; i++) {
        if (!rec->cb[i].used) continue;
        lua_rawgeti(L, LUA_REGISTRYINDEX, rec->cb[i].ref);          // the cb
        lua_pushvalue(L, -2);                                       // ctx
        if (lua_pcall(L, 1, 0, 0) != 0) {                          // a faulting cb is disabled
            ml_log("[hook] cb error @ 0x%08x (disabling): %s", (unsigned)rec->va, lua_tostring(L, -1));
            lua_pop(L, 1);
            luaL_unref(L, LUA_REGISTRYINDEX, rec->cb[i].ref);
            rec->cb[i].used = 0;
        }
    }
    lua_pop(L, 1);                                                  // ctx
    InterlockedExchange(&rec->busy, 0);
}

static hookrec *rec_for_va(uintptr_t va) {
    for (int i = 0; i < MAX_HOOKS; i++) if (g_hk[i].used && g_hk[i].va == va) return &g_hk[i];
    return NULL;
}
static hookrec *alloc_rec(void) {
    for (int i = 0; i < MAX_HOOKS; i++) if (!g_hk[i].used) return &g_hk[i];
    return NULL;
}

// Tier-1 find/create.  Refuses a VA already claimed by a typed (Tier-2) hook — one MinHook
// per target, and the two tiers can't share a detour, so a VA is Tier-1 xor Tier-2.
static hookrec *find_or_create(uintptr_t va) {
    hookrec *rec = rec_for_va(va);
    if (rec) {
        if (rec->tier == 2) { ml_log("[hook] entry refused @ 0x%08x — already a typed hook", (unsigned)va); return NULL; }
        return rec;
    }
    rec = alloc_rec();
    if (!rec) { ml_log("[hook] registry full (%d)", MAX_HOOKS); return NULL; }
    uint8_t *thunk = alloc_thunk();
    if (!thunk) { ml_log("[hook] thunk pool exhausted"); return NULL; }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { ml_log("[hook] MH_Initialize failed (%d)", s); return NULL; }
    void *tramp = NULL;
    s = MH_CreateHook((void *)va, thunk, &tramp);   // detour = our (not-yet-written) thunk
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { ml_log("[hook] MH_CreateHook @ 0x%08x failed (%d)", (unsigned)va, s); return NULL; }

    memset(rec, 0, sizeof *rec);
    rec->used = 1; rec->tier = 1; rec->va = va; rec->tramp = tramp; rec->thunk = thunk;
    gen_thunk(thunk, rec, tramp);                   // now write the thunk (bakes rec + tramp)
    if (MH_EnableHook((void *)va) != MH_OK) { ml_log("[hook] MH_EnableHook @ 0x%08x failed", (unsigned)va); rec->used = 0; return NULL; }
    ml_log("[hook] entry installed @ 0x%08x (thunk %p, tramp %p)", (unsigned)va, (void *)thunk, tramp);
    return rec;
}

// ── Lua: mod.hook.entry / remove / count (Tier-1) ─────────────────────────────
static int l_hook_entry(lua_State *L) {
    uintptr_t va = (uintptr_t)lua_tonumber(L, 1);
    if (!va || !lua_isfunction(L, 2)) { lua_pushnil(L); return 1; }
    hookrec *rec = find_or_create(va);
    if (!rec) { lua_pushnil(L); return 1; }
    int slot = -1;
    for (int i = 0; i < MAX_CB; i++) if (!rec->cb[i].used) { slot = i; break; }
    if (slot < 0) { ml_log("[hook] chain full @ 0x%08x", (unsigned)va); lua_pushnil(L); return 1; }
    lua_pushvalue(L, 2);
    rec->cb[slot].ref = luaL_ref(L, LUA_REGISTRYINDEX);
    rec->cb[slot].used = 1;
    lua_pushinteger(L, (int)((rec - g_hk) << 8 | slot));   // handle = (recidx<<8)|slot
    return 1;
}
static int l_hook_remove(lua_State *L) {
    int handle = (int)lua_tointeger(L, 1);
    int ri = (handle >> 8) & 0xff, slot = handle & 0xff;
    if (ri < 0 || ri >= MAX_HOOKS || slot < 0 || slot >= MAX_CB) return 0;
    hookrec *rec = &g_hk[ri];
    if (rec->used && rec->tier == 1 && rec->cb[slot].used) {
        luaL_unref(L, LUA_REGISTRYINDEX, rec->cb[slot].ref);
        rec->cb[slot].used = 0;
        // NB: the MinHook stays installed even with an empty chain (dispatch is a cheap
        // no-op then).  Quiescent teardown/reclaim is a later refinement.
    }
    return 0;
}
static int l_hook_count(lua_State *L) {
    int n = 0; for (int i = 0; i < MAX_HOOKS; i++) if (g_hk[i].used) n++;
    lua_pushinteger(L, n); return 1;
}

// ── Tier-2 C primitives (used by the embedded Lua below; hidden from mods at init) ──
// __mh_install(va, closure_addr) -> tramp_addr | nil.  Wraps the FFI closure in a
// main-thread gate and installs ONE MinHook on the VA (refusing a VA already Tier-1),
// returning the trampoline so Lua can call the original.
static int l_mh_install(lua_State *L) {
    uintptr_t va      = (uintptr_t)lua_tonumber(L, 1);
    uintptr_t closure = (uintptr_t)lua_tonumber(L, 2);
    if (!va || !closure) { lua_pushnil(L); return 1; }

    hookrec *rec = rec_for_va(va);
    if (rec) {
        if (rec->tier == 1) { ml_log("[hook] typed refused @ 0x%08x — already an entry hook", (unsigned)va); lua_pushnil(L); return 1; }
        lua_pushnumber(L, (double)(uintptr_t)rec->tramp); return 1;   // already typed: reuse the trampoline
    }
    rec = alloc_rec();
    if (!rec) { ml_log("[hook] registry full (%d)", MAX_HOOKS); lua_pushnil(L); return 1; }
    uint8_t *gate = alloc_thunk();
    if (!gate) { ml_log("[hook] thunk pool exhausted"); lua_pushnil(L); return 1; }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { ml_log("[hook] MH_Initialize failed (%d)", s); lua_pushnil(L); return 1; }
    void *tramp = NULL;
    s = MH_CreateHook((void *)va, gate, &tramp);
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { ml_log("[hook] MH_CreateHook @ 0x%08x failed (%d)", (unsigned)va, s); lua_pushnil(L); return 1; }

    memset(rec, 0, sizeof *rec);
    rec->used = 1; rec->tier = 2; rec->va = va; rec->tramp = tramp; rec->thunk = gate;
    gen_gate_thunk(gate, (void *)closure, tramp);   // bake the gate now (closure + tramp + &g_main_tid)
    if (MH_EnableHook((void *)va) != MH_OK) { ml_log("[hook] MH_EnableHook @ 0x%08x failed", (unsigned)va); rec->used = 0; lua_pushnil(L); return 1; }
    ml_log("[hook] typed installed @ 0x%08x (gate %p -> closure 0x%08x, tramp %p)", (unsigned)va, (void *)gate, (unsigned)closure, tramp);
    lua_pushnumber(L, (double)(uintptr_t)tramp);
    return 1;
}
static int l_hook_log(lua_State *L) {   // __log(preformatted_string) -> route to the loader log
    const char *s = lua_tostring(L, 1);
    if (s) ml_log("%s", s);
    return 0;
}

// ── Tier-2 orchestration, in Lua (LuaJIT FFI closures marshal the typed chain) ──
// Receives the shared `hook` table as arg #1.  Adds `hook.typed`, wraps `hook.remove` to
// route typed handles (>= 0x40000000) to the Lua chain, and hides the C primitives.
// Single-quoted literals only (this is embedded as a C string; no escaping games).
static const char TIER2_LUA[] =
    "local hook = ...\n"
    "local ffi = require('ffi')\n"
    "local mh_install = hook.__mh_install\n"
    "local clog = hook.__log\n"
    "hook.__mh_install = nil\n"
    "hook.__log = nil\n"
    "local function logf(...) clog(string.format(...)) end\n"
    "local recs = {}\n"
    "local NEXT = 0x40000000\n"
    "local function to_ctype(sig)\n"
    "  local head, args = string.match(sig, '^%s*(.-)%s*(%b())%s*$')\n"
    "  if not head then error('mod.hook.typed: bad signature: ' .. tostring(sig)) end\n"
    "  local h2, conv = string.match(head, '^(.-)%s+(__%a+)$')\n"
    "  if conv then return string.format('%s (%s *)%s', h2, conv, args) end\n"
    "  return string.format('%s (*)%s', head, args)\n"
    "end\n"
    "local function make_dispatch(rec)\n"
    "  return function(...)\n"
    "    local orig = rec.orig\n"
    "    if rec.busy then return orig(...) end\n"
    "    rec.busy = true\n"
    "    local n = select('#', ...)\n"
    "    local args = { ... }\n"
    "    local ctx = { args = args, n = n, va = rec.va, blocked = false, ret = nil }\n"
    "    local chain = rec.chain\n"
    "    for i = 1, #chain do\n"
    "      local h = chain[i]\n"
    "      if h and not h.dead and h.pre then\n"
    "        local ok, err = pcall(h.pre, ctx)\n"
    "        if not ok then logf('[hook] typed pre error @ 0x%08x (disabling): %s', rec.va, tostring(err)); h.dead = true\n"
    "        elseif ctx.blocked then break end\n"
    "      end\n"
    "    end\n"
    "    local ret\n"
    "    if ctx.blocked then ret = ctx.ret else ret = orig(unpack(args, 1, n)) end\n"
    "    ctx.ret = ret\n"
    "    for i = #chain, 1, -1 do\n"
    "      local h = chain[i]\n"
    "      if h and not h.dead and h.post then\n"
    "        local ok, err = pcall(h.post, ctx)\n"
    "        if not ok then logf('[hook] typed post error @ 0x%08x (disabling): %s', rec.va, tostring(err)); h.dead = true end\n"
    "      end\n"
    "    end\n"
    "    rec.busy = false\n"
    "    if ctx.ret == nil then return 0 end\n"
    "    return ctx.ret\n"
    "  end\n"
    "end\n"
    "local function typed(va, sig, spec)\n"
    "  va = tonumber(va)\n"
    "  if not va or type(sig) ~= 'string' or type(spec) ~= 'table' then\n"
    "    error('mod.hook.typed(va, sig, {pre=,post=}): sig must be a C decl string', 2)\n"
    "  end\n"
    "  local rec = recs[va]\n"
    "  if rec then\n"
    "    if rec.sig ~= sig then error(string.format('mod.hook.typed: VA 0x%08x already typed with a different sig (%s)', va, rec.sig), 2) end\n"
    "  else\n"
    "    local ok, ct = pcall(ffi.typeof, to_ctype(sig))\n"
    "    if not ok then error(string.format('mod.hook.typed: bad signature (%s): %s', sig, tostring(ct)), 2) end\n"
    "    rec = { va = va, sig = sig, chain = {}, busy = false }\n"
    "    local cb = ffi.cast(ct, make_dispatch(rec))\n"
    "    rec.cb = cb\n"
    "    local tramp = mh_install(va, tonumber(ffi.cast('uintptr_t', cb)))\n"
    "    if not tramp then cb:free(); error(string.format('mod.hook.typed: install refused @ 0x%08x (VA already has an entry hook?)', va), 2) end\n"
    "    rec.orig = ffi.cast(ct, ffi.cast('void*', tramp))\n"
    "    recs[va] = rec\n"
    "  end\n"
    "  local id = NEXT; NEXT = NEXT + 1\n"
    "  rec.chain[#rec.chain + 1] = { id = id, pre = spec.pre, post = spec.post, dead = false }\n"
    "  return id\n"
    "end\n"
    "local c_remove = hook.remove\n"
    "local function remove(h)\n"
    "  if type(h) == 'number' and h >= 0x40000000 then\n"
    "    for _, rec in pairs(recs) do\n"
    "      local chain = rec.chain\n"
    "      for i = 1, #chain do\n"
    "        local e = chain[i]\n"
    "        if e and e.id == h then e.dead = true; e.pre = nil; e.post = nil; return true end\n"
    "      end\n"
    "    end\n"
    "    return false\n"
    "  end\n"
    "  return c_remove(h)\n"
    "end\n"
    "hook.typed = typed\n"
    "hook.remove = remove\n"
    "hook.typed_count = function() local n = 0; for _ in pairs(recs) do n = n + 1 end; return n end\n";

static int g_hook_ref = LUA_NOREF;
void hooks_init(lua_State *L) {
    g_L = L;
    lua_newtable(L);
    lua_pushcfunction(L, l_hook_entry);   lua_setfield(L, -2, "entry");
    lua_pushcfunction(L, l_hook_remove);  lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, l_hook_count);   lua_setfield(L, -2, "count");
    lua_pushcfunction(L, l_mh_install);   lua_setfield(L, -2, "__mh_install");   // Tier-2 primitives,
    lua_pushcfunction(L, l_hook_log);     lua_setfield(L, -2, "__log");          // hidden by TIER2_LUA
    // Augment with the Tier-2 layer (adds `typed`, wraps `remove`, hides the primitives).
    if (luaL_loadstring(L, TIER2_LUA) == 0) {
        lua_pushvalue(L, -2);                                     // the hook table -> the chunk's arg
        if (lua_pcall(L, 1, 0, 0) != 0) { ml_log("[hook] Tier-2 init FAILED: %s", lua_tostring(L, -1)); lua_pop(L, 1); }
    } else { ml_log("[hook] Tier-2 chunk load FAILED: %s", lua_tostring(L, -1)); lua_pop(L, 1); }
    g_hook_ref = luaL_ref(L, LUA_REGISTRYINDEX);                  // pops the (augmented) table
}
void hooks_push_table(lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, g_hook_ref); }
