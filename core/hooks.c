// core/hooks.c — the chained hook registry.  See hooks.h.

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
    uintptr_t    va;       // absolute target
    void        *tramp;    // MinHook trampoline (the original)
    uint8_t     *thunk;    // our per-VA detour (RWX pool)
    hcb          cb[MAX_CB];
    volatile long busy;    // per-VA reentrancy guard
} hookrec;
static hookrec g_hk[MAX_HOOKS];

// ── per-VA thunk (generated): capture regs -> hook_dispatch(rec, regs) -> jmp original ──
// Layout (24 B):  pushfd; pushad; push esp; push <rec>; call <dispatch>; add esp,8;
//                 popad; popfd; jmp <tramp>
// pushad frame @ [esp] after the two pushes' base = regs: EDI,ESI,EBP,ESP,EBX,EDX,ECX,EAX;
// the caller's stack top (retaddr) is at regs + 0x24.  Stack-balanced + ends in a jmp, so
// the original runs with the caller's exact stack — arg-agnostic, can't modify/block.
#define THUNK_SZ 24
static uint8_t *g_pool; static size_t g_pool_off;

static uint8_t *alloc_thunk(void) {
    if (!g_pool) { g_pool = (uint8_t *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE); g_pool_off = 0; }
    if (!g_pool || g_pool_off + THUNK_SZ > 0x1000) return NULL;
    uint8_t *t = g_pool + g_pool_off; g_pool_off += THUNK_SZ; return t;
}

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

// The dispatcher: walk the chain, call each observer with a register context.  cdecl —
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

static hookrec *find_or_create(uintptr_t va) {
    int freei = -1;
    for (int i = 0; i < MAX_HOOKS; i++) {
        if (g_hk[i].used && g_hk[i].va == va) return &g_hk[i];
        if (!g_hk[i].used && freei < 0) freei = i;
    }
    if (freei < 0) { ml_log("[hook] registry full (%d)", MAX_HOOKS); return NULL; }
    hookrec *rec = &g_hk[freei];
    uint8_t *thunk = alloc_thunk();
    if (!thunk) { ml_log("[hook] thunk pool exhausted"); return NULL; }

    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { ml_log("[hook] MH_Initialize failed (%d)", s); return NULL; }
    void *tramp = NULL;
    s = MH_CreateHook((void *)va, thunk, &tramp);   // detour = our (not-yet-written) thunk
    if (s != MH_OK && s != MH_ERROR_ALREADY_CREATED) { ml_log("[hook] MH_CreateHook @ 0x%08x failed (%d)", (unsigned)va, s); return NULL; }

    memset(rec, 0, sizeof *rec);
    rec->used = 1; rec->va = va; rec->tramp = tramp; rec->thunk = thunk;
    gen_thunk(thunk, rec, tramp);                   // now write the thunk (bakes rec + tramp)
    if (MH_EnableHook((void *)va) != MH_OK) { ml_log("[hook] MH_EnableHook @ 0x%08x failed", (unsigned)va); rec->used = 0; return NULL; }
    ml_log("[hook] installed @ 0x%08x (thunk %p, tramp %p)", (unsigned)va, (void *)thunk, tramp);
    return rec;
}

// ── Lua: mod.hook.entry / remove / count ─────────────────────────────────────
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
    if (rec->used && rec->cb[slot].used) {
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

static int g_hook_ref = LUA_NOREF;
void hooks_init(lua_State *L) {
    g_L = L;
    lua_newtable(L);
    lua_pushcfunction(L, l_hook_entry);  lua_setfield(L, -2, "entry");
    lua_pushcfunction(L, l_hook_remove); lua_setfield(L, -2, "remove");
    lua_pushcfunction(L, l_hook_count);  lua_setfield(L, -2, "count");
    g_hook_ref = luaL_ref(L, LUA_REGISTRYINDEX);
}
void hooks_push_table(lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, g_hook_ref); }
