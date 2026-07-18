// core/sotes_bindings.c — SotES (sotes_en) game-knowledge bindings.  See sotes_bindings.h.
//
// The first two RE'd subsystems exposed to mods as mod.game.roster + mod.game.coordinates,
// lifted from the proven sotes_trainer.  Offsets are for the unpacked EN-SE (ImageBase
// 0x400000); the party actors are located by their entity code (Arche 0xc35a / Sana 0xc35b
// / Stella 0xc35c) at actor+0x1d4.
//
// PORT-DEBT(sotes-roster-heapscan): finding actors is a THROTTLED FULL-HEAP SCAN (walk all
// committed private RW regions for the code word, validate).  Robust (base-independent) but
// fragile + costly, and it can momentarily see a stale "ghost" actor after a cross-region
// warp.  Temporary — replace with a DIRECT roster/party-manager pointer once RE'd (candidate:
// the render_root CHARACTER band at render_root+0x11e0, 128 slots; render_root = *(0x92dd38)).
// Until then this mirrors the trainer's known-good behavior (cache + revalidate + ~8x/s throttle).

#include "sotes_bindings.h"
#include "game_bindings.h"
#include "mem.h"
#include "executor.h"
#include "loader_internal.h"

#include "lua.h"
#include "lauxlib.h"

// ── EN-SE offsets (from sotes_trainer / SE_CODE_MAP.md) ──────────────────────
#define OFF_CODE        0x1d4      // actor -> entity code (0xc35a/b/c for the party)
#define OFF_STATBLOCK   0x760      // actor -> stat_block ptr
#define OFF_WORLD_X     0xc76c     // actor -> world_x (centi-px, derived snapshot)
#define OFF_WORLD_Y     0xc770     // actor -> world_y (centi-px)
#define OFF_BOX         0x40       // actor -> collision AABB ptr (authoritative pos)
#define BOX_X           0x04       // box   -> left X (== world_x while live)
// stat_block offsets
#define OFF_HP_CUR      0x54
#define OFF_HP_BASE     0x58
#define OFF_HP_EQUIP    0x84
#define OFF_HP_BUFF     0x9c
#define OFF_MP_CUR      0x5c
#define OFF_MP_BASE     0x60
#define OFF_MP_EQUIP    0x88
#define OFF_MP_BUFF     0xa0
#define OFF_COMBAT_LV_MAX 0xe0     // max combat level (the HUD stars' "N")
#define OFF_INPUT_CHAIN   0xc7a4   // actor -> input chain; *(*(actor+0xc7a4)) == input mgr iff CONTROLLED

static const uint32_t CODES[3] = { 0xc35a, 0xc35b, 0xc35c };
static const char    *NAMES[3] = { "Arche", "Sana", "Stella" };

static int code_index(uint32_t c) { return (c >= 0xc35a && c <= 0xc35c) ? (int)(c - 0xc35a) : -1; }

static int stat_max(uintptr_t sb, int base_off, int equip_off, int buff_off) {
    uint32_t b = 0, e = 0, f = 0;
    mem_rd32((void *)(sb + base_off), &b);
    mem_rd32((void *)(sb + equip_off), &e);
    mem_rd32((void *)(sb + buff_off), &f);
    int m = (int)b + (int)e + (int)f;
    return m < 0 ? 0 : m;
}

// Validate a party-member actor: code == want + a mutually-sane stat block + coords
// (the code word appears in many non-actor places, so cross-check).  Lifted verbatim
// from the trainer's actor_valid.
static int actor_valid(uintptr_t base, uint32_t want) {
    uint32_t code, sb, lvl, hpc, mpc, wx, wy;
    if (!mem_rd32((void *)(base + OFF_CODE), &code) || code != want) return 0;
    if (!mem_rd32((void *)(base + OFF_STATBLOCK), &sb) || !mem_readable((void *)sb, 0x200)) return 0;
    if (!mem_rd32((void *)(sb + OFF_COMBAT_LV_MAX), &lvl) || lvl < 1 || lvl > 99) return 0;
    int hpmax = stat_max(sb, OFF_HP_BASE, OFF_HP_EQUIP, OFF_HP_BUFF);
    int mpmax = stat_max(sb, OFF_MP_BASE, OFF_MP_EQUIP, OFF_MP_BUFF);
    if (hpmax < 1 || hpmax > 99999) return 0;
    if (mpmax < 0 || mpmax > 9999) return 0;
    if (!mem_rd32((void *)(sb + OFF_HP_CUR), &hpc) || (int)hpc < 0 || (int)hpc > 999999) return 0;
    if (!mem_rd32((void *)(sb + OFF_MP_CUR), &mpc) || (int)mpc < 0 || (int)mpc > 999999) return 0;
    if (!mem_rd32((void *)(base + OFF_WORLD_X), &wx) || !mem_rd32((void *)(base + OFF_WORLD_Y), &wy)) return 0;
    if (abs((int)wx) > 50000000 || abs((int)wy) > 50000000) return 0;
    // reject a stale roster GHOST: its garbage phys-box makes box[+4] read a pointer (>> any coord).
    uint32_t box = 0, bx4 = 0;
    if (mem_rd32((void *)(base + OFF_BOX), &box) && box > 0x10000 && mem_readable((void *)(uintptr_t)box, 0x14)
        && mem_rd32((void *)((uintptr_t)box + BOX_X), &bx4) && abs((int)bx4) > 50000000) return 0;
    return 1;
}
// The LIVE actor's phys-box mirrors world_x (box[+4] == world_x); a stale duplicate's does not —
// used ONLY as a cold-scan preference to pick the live actor over a ghost (never in actor_valid,
// which would tear while moving).  Lifted from the trainer's actor_box_tracks.
static int actor_box_tracks(uintptr_t base) {
    uint32_t box = 0, bx4 = 0, wx = 0;
    if (!mem_rd32((void *)(base + OFF_BOX), &box) || box <= 0x10000 ||
        !mem_readable((void *)(uintptr_t)box, 0x14)) return 0;
    return mem_rd32((void *)((uintptr_t)box + BOX_X), &bx4) &&
           mem_rd32((void *)(base + OFF_WORLD_X), &wx) && bx4 == wx;
}
// Is `actor` the CONTROLLED (player-driven) member? Its input chain resolves to the live
// input manager — captured by the executor's safepoint hook (exec_ti_mgr).  Until the
// executor is armed exec_ti_mgr()==0, so this reports 0 (unknown) rather than guessing.
static int actor_is_active(uintptr_t actor) {
    uint32_t p = 0, mgr = 0, tm = exec_ti_mgr();
    if (!tm) return 0;
    if (!mem_rd32((void *)(actor + OFF_INPUT_CHAIN), &p) || p <= 0x10000) return 0;
    if (!mem_rd32((void *)(uintptr_t)p, &mgr)) return 0;
    return mgr == tm;
}

// ── the throttled roster cache (g_ros[k] = actor for CODES[k]) ────────────────
static uintptr_t g_ros[3];

// Refresh the roster: revalidate cached actors; if any slot is empty, do ONE throttled
// full-heap walk to (re)fill it (preferring the box-tracking live actor over a ghost).
static void ensure_roster(void) {
    int need_walk = 0;
    for (int k = 0; k < 3; k++) {
        if (g_ros[k] && !actor_valid(g_ros[k], CODES[k])) g_ros[k] = 0;
        if (!g_ros[k]) need_walk = 1;
    }
    if (!need_walk) return;

    static DWORD last_walk;
    DWORD now = GetTickCount();
    if (last_walk && (now - last_walk) < 120) return;   // ~8x/sec cold-scan cap
    last_walk = now;

    uintptr_t best[3] = { 0, 0, 0 }, fallback[3] = { 0, 0, 0 };
    uint8_t *addr = 0;
    MEMORY_BASIC_INFORMATION mbi;
    while (VirtualQuery(addr, &mbi, sizeof mbi)) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE &&
            (mbi.Protect & PAGE_READWRITE) && !(mbi.Protect & PAGE_GUARD) &&
            mbi.RegionSize <= 0x4000000) {
            uint32_t *p = (uint32_t *)mbi.BaseAddress, *e = (uint32_t *)(next - 4);
            for (; p <= e; ++p) {
                int k = code_index(*p);
                if (k < 0) continue;
                uintptr_t base = (uintptr_t)p - OFF_CODE;
                if (!actor_valid(base, CODES[k])) continue;
                if (actor_box_tracks(base)) best[k] = base;
                else if (!fallback[k]) fallback[k] = base;
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    for (int k = 0; k < 3; k++)
        if (!g_ros[k]) g_ros[k] = best[k] ? best[k] : fallback[k];
}

// ── Lua: mod.game.roster ─────────────────────────────────────────────────────
static void push_member(lua_State *L, int k) {
    uintptr_t a = g_ros[k];
    uint32_t sb = 0, wx = 0, wy = 0, lvl = 0, hpc = 0, mpc = 0;
    mem_rd32((void *)(a + OFF_STATBLOCK), &sb);
    mem_rd32((void *)(a + OFF_WORLD_X), &wx);
    mem_rd32((void *)(a + OFF_WORLD_Y), &wy);
    int hpm = 0, mpm = 0;
    if (sb) {
        mem_rd32((void *)(sb + OFF_COMBAT_LV_MAX), &lvl);
        mem_rd32((void *)(sb + OFF_HP_CUR), &hpc);
        mem_rd32((void *)(sb + OFF_MP_CUR), &mpc);
        hpm = stat_max(sb, OFF_HP_BASE, OFF_HP_EQUIP, OFF_HP_BUFF);
        mpm = stat_max(sb, OFF_MP_BASE, OFF_MP_EQUIP, OFF_MP_BUFF);
    }
    lua_newtable(L);
    lua_pushinteger(L, (int)CODES[k]);      lua_setfield(L, -2, "code");
    lua_pushstring(L, NAMES[k]);            lua_setfield(L, -2, "name");
    lua_pushnumber(L, (double)a);           lua_setfield(L, -2, "actor");
    lua_pushnumber(L, (double)sb);          lua_setfield(L, -2, "stat_block");
    lua_pushinteger(L, (int)lvl);           lua_setfield(L, -2, "level");
    lua_pushinteger(L, (int)wx);            lua_setfield(L, -2, "x");
    lua_pushinteger(L, (int)wy);            lua_setfield(L, -2, "y");
    lua_pushinteger(L, (int)hpc);           lua_setfield(L, -2, "hp");
    lua_pushinteger(L, hpm);                lua_setfield(L, -2, "hp_max");
    lua_pushinteger(L, (int)mpc);           lua_setfield(L, -2, "mp");
    lua_pushinteger(L, mpm);                lua_setfield(L, -2, "mp_max");
    lua_pushboolean(L, actor_is_active(a)); lua_setfield(L, -2, "active");  // controlled member (needs the executor)
}

// mod.game.roster.members() -> { {code,name,actor,level,x,y,hp,...}, ... } present members.
static int l_roster_members(lua_State *L) {
    if (!gb_enabled("roster")) { lua_pushnil(L); return 1; }   // inert even via a captured ref
    ensure_roster();
    lua_newtable(L);
    int idx = 1;
    for (int k = 0; k < 3; k++) if (g_ros[k]) { push_member(L, k); lua_rawseti(L, -2, idx++); }
    return 1;
}

static void install_roster(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_roster_members); lua_setfield(L, -2, "members");
}

// ── Lua: mod.game.coordinates ────────────────────────────────────────────────
// mod.game.coordinates.get([code]) -> {x,y,actor,code} for a member (default Arche);
// mod.game.coordinates.player() -> Arche's coords.  (A "controlled member" accessor
// arrives with P3 hooks — the active-member predicate needs the input-mgr global.)
static int coord_get_impl(lua_State *L, uint32_t code) {
    int k = code_index(code); if (k < 0) k = 0;
    ensure_roster();
    uintptr_t a = g_ros[k];
    if (!a) { lua_pushnil(L); return 1; }
    uint32_t wx = 0, wy = 0;
    mem_rd32((void *)(a + OFF_WORLD_X), &wx);
    mem_rd32((void *)(a + OFF_WORLD_Y), &wy);
    lua_newtable(L);
    lua_pushinteger(L, (int)wx);         lua_setfield(L, -2, "x");
    lua_pushinteger(L, (int)wy);         lua_setfield(L, -2, "y");
    lua_pushnumber(L, (double)a);        lua_setfield(L, -2, "actor");
    lua_pushinteger(L, (int)CODES[k]);   lua_setfield(L, -2, "code");
    return 1;
}
static int l_coord_get(lua_State *L) {
    if (!gb_enabled("coordinates")) { lua_pushnil(L); return 1; }
    uint32_t code = lua_isnumber(L, 1) ? (uint32_t)lua_tointeger(L, 1) : 0xc35a;
    return coord_get_impl(L, code);
}
static int l_coord_player(lua_State *L) {
    if (!gb_enabled("coordinates")) { lua_pushnil(L); return 1; }
    return coord_get_impl(L, 0xc35a);
}
// coordinates.target() -> the CONTROLLED member's coords (needs the executor's captured
// input mgr); falls back to Arche until the executor is armed.
static int l_coord_target(lua_State *L) {
    if (!gb_enabled("coordinates")) { lua_pushnil(L); return 1; }
    ensure_roster();
    for (int k = 0; k < 3; k++) if (g_ros[k] && actor_is_active(g_ros[k])) return coord_get_impl(L, CODES[k]);
    return coord_get_impl(L, 0xc35a);
}

static void install_coordinates(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_coord_get);    lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_coord_player); lua_setfield(L, -2, "player");
    lua_pushcfunction(L, l_coord_target); lua_setfield(L, -2, "target");
}

// ── registration ─────────────────────────────────────────────────────────────
void sotes_bindings_register(void) {
    static const gb_def ROSTER = {
        "roster", "party members (code/name/level/coords/hp/mp) [temp heap-scan]",
        install_roster, 1
    };
    static const gb_def COORD = {
        "coordinates", "member world x/y in centi-px",
        install_coordinates, 1
    };
    gb_register(&ROSTER);
    gb_register(&COORD);
}
