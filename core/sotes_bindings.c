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
#include "hooks.h"            // hooks_entry_c — the picker-poll observer (mod.game.save)
#include "oss_mod_api.h"      // OssHookCtx
#include "prof.h"
#include "ddraw_present.h"   // ddp_cursor_game — cursor -> game 640x480 screen space (mod.game.mouse)
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
// name + the 4 combat stats + levels + exp — all RE'd live vs save 15 (docs/findings/save15-live-stats.md
// in ../OpenSummoners).  The 4 combat stats are the RAW BASE; the status screen's shown base = this +
// armor/passive, and the shown total additionally folds in the weapon (so a mod that boosts these
// changes the character's intrinsic stat).  Character "Level" = combat_level + adventurer_level.
#define OFF_NAME        0x00       // char[8], NUL-terminated ("Arche"/"Sana"/"Stella")
#define OFF_ATK         0x64       // combat stats: RAW value (before growth/equipment)
#define OFF_DEF         0x68
#define OFF_SPIRIT      0x6c
#define OFF_RESIST      0x70
// combat-stat modifiers (RE'd live vs Stella's sheet 2026-07-20, all 4 stats exact): the status
// screen shows "effective/base" where base = raw + growth and effective = raw + growth + equipment.
// Same 3-cluster layout as HP/MP (equip 0x84/0x88, buff 0x9c/0xa0 sit after the atk/def/spi/res ones).
#define OFF_ATK_EQUIP   0x74       // equipment bonus (weapon WP / armor AP) — the +N in "eff/base"
#define OFF_DEF_EQUIP   0x78
#define OFF_SPI_EQUIP   0x7c
#define OFF_RES_EQUIP   0x80
#define OFF_ATK_GROWTH  0x8c       // level/innate growth — folded into the displayed base
#define OFF_DEF_GROWTH  0x90
#define OFF_SPI_GROWTH  0x94
#define OFF_RES_GROWTH  0x98
#define OFF_COMBAT_LV_MAX 0xe0     // max combat level (the HUD stars' "N"; "Combat Level M/N")
#define OFF_ADV_LEVEL   0xe4       // adventurer level ("Adventurer Level")
#define OFF_EXP_CUR     0xec       // current EXP
#define OFF_EXP_MAX     0xf0       // EXP to the next level
#define OFF_INPUT_CHAIN   0xc7a4   // actor -> input chain; *(*(actor+0xc7a4)) == input mgr iff CONTROLLED
// camera / view object (render_root + OFF_CAM) — for cursor world-space mapping (trainer SE_CODE_MAP)
#define OFF_CAM           0x104c   // render_root -> camera/view object POINTER
#define CAM_VIEW_TOP      0x5c     // camera -> eased scroll top  (world centi-px) = view top edge
#define CAM_VIEW_LEFT     0x60     // camera -> eased scroll left (world centi-px) = view left edge
#define CAM_VP_W          0x64     // camera -> viewport width  (centi-px; 64000 = 640 px)
#define CAM_VP_H          0x68     // camera -> viewport height (centi-px; 48000 = 480 px)

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
// Two-term stat sum (raw + growth) — the combat-stat "base" (2nd) number, effective minus equipment.
static int stat_sum2(uintptr_t sb, int off_a, int off_b) {
    uint32_t a = 0, b = 0;
    mem_rd32((void *)(sb + off_a), &a);
    mem_rd32((void *)(sb + off_b), &b);
    int m = (int)a + (int)b;
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

// ── the roster cache (g_ros[k] = actor for CODES[k]) ─────────────────────────
static uintptr_t g_ros[3];

// ── render-root band scan (a bounded, RE-informed alternative to the full heap walk) ──
// render_root = *(0x92dd38) (0 at title/menu).  OpenSummoners RE (findings/door-gate.md:41,52-54)
// confirms two SE render bands, each an ARRAY of entity pointers off render_root: EFFECT (+0x1160,
// 32) and CHARACTER (+0x11e0, 128).  We scan those ~160 slots for a party code (entity+0x1d4) —
// microseconds vs the full-heap word scan.  NOTE: the base-game map_obj+0x4030 party array (the
// "canonical" roster) does NOT reach the SE party via any render_root+off single/double indirection
// we could pin live, so this band scan is the pragmatic fast path; the heap walk stays as the
// ultimate fallback for anything the bands miss.  See PORT-DEBT(sotes-roster-heapscan).
#define VA_RENDER_ROOT 0x92dd38
#define BAND_EFFECT    0x1160      // 32 entity-ptr slots
#define BAND_CHARACTER 0x11e0      // 128 entity-ptr slots

// Scan the two render bands for the party; fill g_ros for any found.  Returns 1 if any resolved.
static int roster_bands(uint32_t root) {
    static const struct { uint32_t off; int n; } BANDS[2] = { { BAND_EFFECT, 32 }, { BAND_CHARACTER, 128 } };
    uintptr_t found[3] = { 0, 0, 0 };
    for (int b = 0; b < 2; b++) {
        for (int i = 0; i < BANDS[b].n; i++) {
            uint32_t e = 0, code = 0;
            if (!mem_rd32((void *)(root + BANDS[b].off + i * 4), &e) || e <= 0x10000) continue;
            if (!mem_rd32((void *)(uintptr_t)(e + OFF_CODE), &code)) continue;
            int k = code_index(code & 0xffff);
            if (k >= 0 && !found[k] && actor_valid(e, CODES[k])) found[k] = e;
        }
    }
    int any = 0;
    for (int k = 0; k < 3; k++) if (found[k]) { g_ros[k] = found[k]; any = 1; }
    return any;
}

// Refresh the roster.  Fast path: no scene -> empty (no scan); in-scene -> the direct party array.
// The full-heap walk is now only a throttled FALLBACK for a scene the direct chain can't resolve.
static void ensure_roster_impl(void) {
    int need_walk = 0;
    for (int k = 0; k < 3; k++) {
        if (g_ros[k] && !actor_valid(g_ros[k], CODES[k])) g_ros[k] = 0;
        if (!g_ros[k]) need_walk = 1;
    }

    // No scene loaded (title/menu) -> no party -> clear + do NOTHING (kills the ~65ms menu walk that
    // used to churn hunting for actors that don't exist yet).
    uint32_t root = 0;
    if (!mem_rd32((void *)mem_reloc(VA_RENDER_ROOT), &root) || root == 0) { g_ros[0] = g_ros[1] = g_ros[2] = 0; return; }
    if (!need_walk) return;                              // all present + valid

    // Bounded band scan (~160 reads) — the common-case fast path in place of the heap walk.
    if (roster_bands(root)) return;

    // Fallback: the throttled full-heap walk, only if the bands didn't resolve the party — a safety
    // net (and the bootstrap on scenes the bands miss).
    static DWORD last_walk;
    DWORD now = GetTickCount();
    if (last_walk && (now - last_walk) < 120) return;   // ~8x/sec cold-scan cap
    last_walk = now;
    ml_log("[sotes] roster: direct chain miss — heap-scan fallback");

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
// Timed wrapper — the roster refresh is the suspected per-frame spike (the full-heap walk when a
// slot goes empty, e.g. after a scene change); prof_add captures it under `profile=1`.
static void ensure_roster(void) { uint64_t t = prof_now(); ensure_roster_impl(); prof_add(PROF_ROSTER, t); }

// ── Lua: mod.game.roster ─────────────────────────────────────────────────────
static void push_member(lua_State *L, int k) {
    uintptr_t a = g_ros[k];
    uint32_t sb = 0, wx = 0, wy = 0, clv = 0, alv = 0, hpc = 0, mpc = 0;
    uint32_t atk = 0, def = 0, spi = 0, res = 0, expc = 0, expm = 0;
    int atk_b = 0, def_b = 0, spi_b = 0, res_b = 0;   // combat-stat BASE (2nd number) = raw + growth
    mem_rd32((void *)(a + OFF_STATBLOCK), &sb);
    mem_rd32((void *)(a + OFF_WORLD_X), &wx);
    mem_rd32((void *)(a + OFF_WORLD_Y), &wy);
    int hpm = 0, mpm = 0;
    if (sb) {
        mem_rd32((void *)(sb + OFF_COMBAT_LV_MAX), &clv);
        mem_rd32((void *)(sb + OFF_ADV_LEVEL), &alv);
        mem_rd32((void *)(sb + OFF_HP_CUR), &hpc);
        mem_rd32((void *)(sb + OFF_MP_CUR), &mpc);
        atk = (uint32_t)stat_max(sb, OFF_ATK, OFF_ATK_EQUIP, OFF_ATK_GROWTH);   // effective (w/ equip)
        def = (uint32_t)stat_max(sb, OFF_DEF, OFF_DEF_EQUIP, OFF_DEF_GROWTH);
        spi = (uint32_t)stat_max(sb, OFF_SPIRIT, OFF_SPI_EQUIP, OFF_SPI_GROWTH);
        res = (uint32_t)stat_max(sb, OFF_RESIST, OFF_RES_EQUIP, OFF_RES_GROWTH);
        atk_b = stat_sum2(sb, OFF_ATK, OFF_ATK_GROWTH);                          // base = raw + growth
        def_b = stat_sum2(sb, OFF_DEF, OFF_DEF_GROWTH);
        spi_b = stat_sum2(sb, OFF_SPIRIT, OFF_SPI_GROWTH);
        res_b = stat_sum2(sb, OFF_RESIST, OFF_RES_GROWTH);
        mem_rd32((void *)(sb + OFF_EXP_CUR), &expc);
        mem_rd32((void *)(sb + OFF_EXP_MAX), &expm);
        hpm = stat_max(sb, OFF_HP_BASE, OFF_HP_EQUIP, OFF_HP_BUFF);
        mpm = stat_max(sb, OFF_MP_BASE, OFF_MP_EQUIP, OFF_MP_BUFF);
    }
    lua_newtable(L);
    lua_pushinteger(L, (int)CODES[k]);      lua_setfield(L, -2, "code");
    lua_pushstring(L, NAMES[k]);            lua_setfield(L, -2, "name");
    lua_pushnumber(L, (double)a);           lua_setfield(L, -2, "actor");
    lua_pushnumber(L, (double)sb);          lua_setfield(L, -2, "stat_block");
    // `level` stays = combat_level for backward-compat, but it IS the combat level (see combat_level
    // below).  The big character "Level" on the status screen = combat_level + adventurer_level.
    lua_pushinteger(L, (int)clv);           lua_setfield(L, -2, "level");
    lua_pushinteger(L, (int)clv);           lua_setfield(L, -2, "combat_level");
    lua_pushinteger(L, (int)alv);           lua_setfield(L, -2, "adventurer_level");
    lua_pushinteger(L, (int)(clv + alv));   lua_setfield(L, -2, "char_level");
    lua_pushinteger(L, (int)wx);            lua_setfield(L, -2, "x");
    lua_pushinteger(L, (int)wy);            lua_setfield(L, -2, "y");
    lua_pushinteger(L, (int)hpc);           lua_setfield(L, -2, "hp");
    lua_pushinteger(L, hpm);                lua_setfield(L, -2, "hp_max");
    lua_pushinteger(L, (int)mpc);           lua_setfield(L, -2, "mp");
    lua_pushinteger(L, mpm);                lua_setfield(L, -2, "mp_max");
    // the 4 combat stats — the status screen's "effective/base" (effective = with equipment; base =
    // raw + growth).  RE'd live vs Stella's sheet 2026-07-20 (all 4 exact).  `attack` = effective.
    lua_pushinteger(L, (int)atk);           lua_setfield(L, -2, "attack");
    lua_pushinteger(L, atk_b);              lua_setfield(L, -2, "attack_base");
    lua_pushinteger(L, (int)def);           lua_setfield(L, -2, "defense");
    lua_pushinteger(L, def_b);              lua_setfield(L, -2, "defense_base");
    lua_pushinteger(L, (int)spi);           lua_setfield(L, -2, "spirit");
    lua_pushinteger(L, spi_b);              lua_setfield(L, -2, "spirit_base");
    lua_pushinteger(L, (int)res);           lua_setfield(L, -2, "resist");
    lua_pushinteger(L, res_b);              lua_setfield(L, -2, "resist_base");
    lua_pushinteger(L, (int)expc);          lua_setfield(L, -2, "exp");
    lua_pushinteger(L, (int)expm);          lua_setfield(L, -2, "exp_max");
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

// ── Lua: mod.game.mouse ──────────────────────────────────────────────────────
// mod.game.mouse.get() -> { screen_x, screen_y, over, world_x, world_y }.  screen_* is the cursor in the
// game's own 640x480 space (the ddraw layer undoes the borderless pillarbox / windowed client scale);
// world_* is world centi-px via the camera (view origin + screen*viewport-span), omitted at the title/
// menu (no scene/camera).  The game ignores the mouse itself — this is for the trainer + mouse mods.
static int l_mouse_get(lua_State *L) {
    if (!gb_enabled("mouse")) { lua_pushnil(L); return 1; }
    float gx = 0, gy = 0;
    int over = ddp_cursor_game(&gx, &gy);
    lua_newtable(L);
    lua_pushnumber(L, gx);    lua_setfield(L, -2, "screen_x");
    lua_pushnumber(L, gy);    lua_setfield(L, -2, "screen_y");
    lua_pushboolean(L, over); lua_setfield(L, -2, "over");
    // world-space: view origin (camera) + the screen fraction of the viewport span (cpx).
    uint32_t root = 0, cam = 0, vleft = 0, vtop = 0, vpw = 0, vph = 0;
    if (mem_rd32((void *)mem_reloc(VA_RENDER_ROOT), &root) && root &&
        mem_rd32((void *)(uintptr_t)(root + OFF_CAM), &cam) && cam &&
        mem_rd32((void *)(uintptr_t)(cam + CAM_VIEW_LEFT), &vleft) &&
        mem_rd32((void *)(uintptr_t)(cam + CAM_VIEW_TOP), &vtop) &&
        mem_rd32((void *)(uintptr_t)(cam + CAM_VP_W), &vpw) &&
        mem_rd32((void *)(uintptr_t)(cam + CAM_VP_H), &vph) && vpw && vph) {
        int wx = (int)vleft + (int)(gx * (float)vpw / 640.0f);   // vpw≈64000 for 640px => screen*100
        int wy = (int)vtop  + (int)(gy * (float)vph / 480.0f);
        lua_pushinteger(L, wx);  lua_setfield(L, -2, "world_x");
        lua_pushinteger(L, wy);  lua_setfield(L, -2, "world_y");
    }
    return 1;
}
static void install_mouse(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_mouse_get); lua_setfield(L, -2, "get");
}

// ══════════════ semantic loader ops — RE'd menu-drive, exposed so mods ORCHESTRATE it ══════════════
// The attract-mode toggle + auto-load-a-save mechanism (input-record ring, the demo trigger, the
// title/picker poll VAs) lives HERE in the loader; a mod just says mod.game.save.load(slot).  This is
// the old core/sotes_autoload.c, retired into a proper binding — same mechanism, now mod-facing.
// Input-record contract: a record is {id@0, now@4, state=1@8}; its pointer is published into the input
// manager's 64-slot ring at mgr+0x0c, slot 63 (first polled); the poll matches id + state==1 +
// (poll_now - record_now) <= 0x64.  `now` = the poll's own timestamp.  Everything runs engine-thread.
#define VA_PICKER_POLL  0x4378d0    // save-slot picker input poll (thiscall; ecx = picker ctrl, arg2 = now)
#define VA_MENU_NAV     0x5e7fe0    // menu_ctrl nav/action (thiscall; ecx = menu_ctrl, arg1 = action code) — the
                                    //   EN-SE cursor controller the title steers its selection through.  DISASM-
                                    //   VERIFIED in the SHIPPING exe (sha bed4e1): clean entry `push esi;mov esi,ecx`,
                                    //   reads list@+0x174 / rows@+0x17c, dispatches action (0/1 nav, 9 confirm) to the
                                    //   mode-1 handler 0x5e7ad0.  Runs ONLY when a nav/confirm button is consumed, so
                                    //   we inject one nav to provoke a call + capture ecx.  (The old 0x43ce50 was a
                                    //   base-game VA = a struct-init here; hooking it corrupted code -> crash.  See
                                    //   ../OpenSummoners title-menu-state.md correction.)
#define VA_DEMO_JL      0x583866    // attract/demo trigger (jl 0x5832e1)
#define BTN_CONFIRM     0x25        // save-slot PICKER confirm id (the picker reads input via its own path)
#define BTN_TITLE_OK    0x25        // TITLE commit — DISASM-VERIFIED @ 0x583714 (`push 0x25;call input_poll_consume
                                    //   0x437c70` -> menu_ctrl action 9 = confirm -> commit rows[cursor]).  The title
                                    //   polls {0x22 back, 0x02/0x04 nav, 0x25 confirm} — it NEVER polls 0x24 (the old
                                    //   "0x24 commit" note was base-game RE, wrong for EN-SE).
#define BTN_TITLE_NAV   0x04        // TITLE nav — injected only to PROVOKE a 0x5e7fe0 call (it runs only on a consumed
                                    //   button) so we can capture menu_ctrl; verified polled @ 0x5836da (-> action 1).
#define ACT_START       0x1a        // title row action: Start (new game) — used as the title-menu signature (row 0)
#define ACT_CONTINUE    0x1c        // title row action: Continue -> opens the save-data list (row 1; the load path)
#define RING_OFF        0x0c        // input mgr -> 64 record-pointer ring
#define RING_SLOT       63          // ordinal 0 = the first-polled ring slot
// menu_ctrl / list-header layout (../OpenSummoners findings/menu-list.md; ctrl = 0x5e7fe0's ecx) — the layout is
// EN-SE-VALID (base-game & EN-SE share it: 0x5e8e70/0x5e7ad0 read list@+0x174, rows@+0x17c, stride list+0x0c):
#define MC_SUB          0x00        //  *(ctrl+0x00)  = input "ready" gate sub-object
#define MC_MODE         0x08        //   ctrl+0x08    = mode (1 = cursor-nav list — the title's mode)
#define MC_LIST         0x174       //  *(ctrl+0x174) = list header
#define MC_ROWS         0x17c       //  *(ctrl+0x17c) = rows array (menu_row 0x10 B; action @ +0x04)
#define SUB_READY       0x54        //  *(sub+0x54) == MENU_READY once the menu is interactive
#define LH_STRIDE       0x0c        //   list+0x0c = page stride
#define LH_COUNT        0x10        //   list+0x10 = row count
#define LH_CURSOR       0x14        //   list+0x14 = selection index (what commit reads)
#define LH_SEL2         0x18        //   list+0x18 = page-top (floor(cursor/stride)*stride)
#define MENU_READY      0x3e8       //   sub->ready interactive sentinel (1000)
#define ROW_SZ          0x10
#define ROW_ACTION      0x04

static int      g_al_target = -1;   // savedataNN index; < 0 = newest/default
static int      g_al_state;         // 0 idle, 1 title-confirm, 2 picker-confirm, 3 done
static uint32_t g_al_pk_mgr;        // captured picker controller
static uint32_t g_al_title_ctrl;    // captured TITLE menu_ctrl (0x5e7fe0's ecx; validated: mode 1 + Start & Continue)
static int      g_al_committed;     // 1 once the title's Continue was committed — stop driving the title, let the
                                    //   save-list picker (al_picker_cb) confirm the save.  Also GATES al_picker_cb
                                    //   (only auto-confirm 0x4378d0 after Continue, never the new-game/other menus).
static int      g_al_pk_h, g_al_nav_h;   // picker-poll + menu-nav hook handles — both dropped when the drive ends
static int      g_al_pk_logged, g_al_done_logged, g_al_ctrl_logged, g_al_commit_logged;
static uint8_t  g_al_rec[8][16];    // rotating record buffers (static -> always game-readable)
static int      g_al_recn;
static int      g_al_demo_frozen;
static uint8_t  g_al_demo_orig[6];  // original demo-trigger bytes (restore on attract ON)

static int al_scene_loaded(void) {
    uint32_t root = 0;
    return mem_rd32((void *)mem_reloc(VA_RENDER_ROOT), &root) && root != 0;
}
static void al_inject(uint32_t mgr, uint32_t id, uint32_t now) {   // publish {id,now,1} into ring slot 63
    if (!mgr) return;
    uintptr_t slot = mgr + RING_OFF + RING_SLOT * 4;
    if (!mem_writable((void *)slot, 4)) return;
    uint8_t *rec = g_al_rec[g_al_recn++ & 7];
    *(uint32_t *)(rec + 0) = id; *(uint32_t *)(rec + 4) = now; *(uint32_t *)(rec + 8) = 1;
    *(uint32_t *)slot = (uint32_t)(uintptr_t)rec;
}
// Freeze / restore the attract-demo trigger (0f 8c .. jl  <->  e9 .. 90 jmp+nop) so the title stays up.
static void al_attract_freeze(int freeze) {
    void *va = (void *)mem_reloc(VA_DEMO_JL);
    DWORD old;
    if (freeze) {
        if (g_al_demo_frozen) return;
        static const uint8_t want[2] = { 0x0f, 0x8c };
        if (!mem_readable(va, 6) || memcmp(va, want, 2) != 0) { ml_log("[sotes] attract: demo bytes unexpected — not frozen"); return; }
        memcpy(g_al_demo_orig, va, 6);
        static const uint8_t patch[6] = { 0xe9, 0x76, 0xfa, 0xff, 0xff, 0x90 };   // jmp 0x5832e1 + nop
        if (VirtualProtect(va, 6, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(va, patch, 6); VirtualProtect(va, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), va, 6);
            g_al_demo_frozen = 1; ml_log("[sotes] attract OFF — demo trigger frozen @ %p", va);
        }
    } else {
        if (!g_al_demo_frozen) return;
        if (VirtualProtect(va, 6, PAGE_EXECUTE_READWRITE, &old)) {
            memcpy(va, g_al_demo_orig, 6); VirtualProtect(va, 6, old, &old); FlushInstructionCache(GetCurrentProcess(), va, 6);
            g_al_demo_frozen = 0; ml_log("[sotes] attract ON — demo trigger restored @ %p", va);
        }
    }
}
static void al_stop(void) {   // tear the drive down (restore attract, drop hooks); safe to call once
    g_al_state = 3;
    al_attract_freeze(0);     // past the title — restore the demo so attract works normally again
    if (g_al_nav_h) { hooks_remove(g_al_nav_h); g_al_nav_h = 0; }
    if (g_al_pk_h)  { hooks_remove(g_al_pk_h);  g_al_pk_h  = 0; }
}
static void al_mark_done(void) {
    al_stop();
    if (!g_al_done_logged) {
        g_al_done_logged = 1;
        // We now force the cursor onto Continue before committing (below), so a NEW GAME can no longer be
        // started by accident — the only remaining outcomes are "loaded via the picker" or a direct load.
        if (g_al_pk_mgr)
            ml_log("[sotes] save.load: scene loaded via the save picker — gameplay reached, drive stopped");
        else
            ml_log("[sotes] save.load: scene loaded (Continue committed; the picker hook never fired — a "
                   "direct Continue->load?) — drive stopped");
    }
}
static void al_picker_cb(const OssHookCtx *ctx, void *user) {   // picker poll observer: capture ctrl + confirm
    (void)user;
    // 0x4378d0 is a SHARED input poll (many menus call it), so we must only auto-confirm the SAVE-DATA list.
    // We gate on the DRIVE STATE, not the caller VA: al_picker_cb confirms only AFTER the title's Continue is
    // committed (g_al_committed), by which point the only menu up is the save-data list.  (The old caller gate —
    // "confirm only ~0x436xxx" — was a base-game address; in the EN-SE shipping exe the save-list scene polls
    // 0x4378d0 from 0x586a13 (ret 0x586a18), which that gate REJECTED, so the save never loaded.)
    if (!g_al_committed) return;                        // not driving into a save yet — ignore other 0x4378d0 users
    if (!g_al_pk_logged) { g_al_pk_logged = 1;
        ml_log("[sotes] save.load: save-list poll (ctrl=0x%08x, caller=0x%08x) — confirming", (unsigned)ctx->ecx, (unsigned)ctx->ret); }
    g_al_pk_mgr = ctx->ecx;
    if (g_al_state == 1) g_al_state = 2;
    if (g_al_state != 2) return;
    if (al_scene_loaded()) { al_mark_done(); return; }
    uint32_t now = 0; mem_rd32((void *)(uintptr_t)(ctx->esp + 8), &now);   // 0x4378d0 arg2 = the poll's now
    al_inject(ctx->ecx, BTN_CONFIRM, now);
}
// Classify the captured menu_ctrl: >=0 = the Continue row index; -1 = NOT the title menu (keep waiting);
// -2 = it IS the title but has no Continue row (no save to load — must NOT fall back to Start).  Requiring
// the Start row (0x1a) as a signature keeps us from ever writing the cursor of some unrelated menu that
// happens to be navigating while we drive.
static int al_title_continue_row(uint32_t ctrl) {
    uint32_t mode = 0, list = 0, rows = 0, count = 0;
    if (!ctrl) return -1;
    if (!mem_rd32((void *)(uintptr_t)(ctrl + MC_MODE), &mode) || mode != 1)               return -1;
    if (!mem_rd32((void *)(uintptr_t)(ctrl + MC_LIST), &list) || !list)                   return -1;
    if (!mem_rd32((void *)(uintptr_t)(ctrl + MC_ROWS), &rows) || !rows)                   return -1;
    if (!mem_rd32((void *)(uintptr_t)(list + LH_COUNT), &count) || !count || count > 16)  return -1;
    int cont = -1, start = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t act = 0;
        if (!mem_rd32((void *)(uintptr_t)(rows + i * ROW_SZ + ROW_ACTION), &act))         return -1;
        if (act == ACT_START)    start = 1;
        if (act == ACT_CONTINUE) cont  = (int)i;
    }
    if (!start) return -1;          // not the title menu
    return cont >= 0 ? cont : -2;   // title, but Continue may be absent (no save present)
}
// Write the title cursor onto `row` (and keep the page-top consistent) so the next commit selects it.
static void al_title_set_cursor(uint32_t ctrl, int row) {
    uint32_t list = 0, stride = 0;
    if (!mem_rd32((void *)(uintptr_t)(ctrl + MC_LIST), &list) || !list) return;
    if (mem_writable((void *)(uintptr_t)(list + LH_CURSOR), 4))
        *(uint32_t *)(uintptr_t)(list + LH_CURSOR) = (uint32_t)row;
    if (mem_rd32((void *)(uintptr_t)(list + LH_STRIDE), &stride) && stride &&
        mem_writable((void *)(uintptr_t)(list + LH_SEL2), 4))
        *(uint32_t *)(uintptr_t)(list + LH_SEL2) = (uint32_t)((row / (int)stride) * (int)stride);
}
// 0x5e7fe0 (menu_ctrl nav/action) entry observer — it runs ONLY when a nav/confirm button is consumed, so this
// is how we grab the live title menu_ctrl (its ecx), but only while driving the title and only if it validates.
// 0x5e7fe0 is a real function entry (clean `push esi;mov esi,ecx` prologue), so — unlike the old 0x43ce50 — the
// MinHook patch sits on a proper boundary and leaving it installed after the drive is a harmless no-op.
static void al_menu_nav_cb(const OssHookCtx *ctx, void *user) {
    (void)user;
    if (g_al_state != 1) return;
    if (al_title_continue_row(ctx->ecx) == -1) return;   // not the title menu (or unreadable) — ignore
    g_al_title_ctrl = ctx->ecx;
    if (!g_al_ctrl_logged) { g_al_ctrl_logged = 1;
        ml_log("[sotes] save.load: title menu_ctrl captured (0x%08x)", (unsigned)ctx->ecx); }
}
// Title drive (INJECTION ONLY — the only hooks are the picker poll + the nav observer, both real entries): provoke
// one 0x5e7fe0 call with a nav (0x04) to CAPTURE the title menu_ctrl, force the cursor onto the Continue row, then
// KEEP injecting the confirm (0x25) every frame WHILE the title is up — a single 0x25 doesn't reliably land (the
// title polls it on a specific frame), so we re-commit until the title transitions to the save-data list.  Once the
// title menu_ctrl stops validating (the save-list replaced it), we STOP (do NOT fall back to nav — that would spam
// the save-list) and hand off to al_picker_cb, which confirms the save (gated on g_al_committed).  We inject with
// the safepoint's live mgr+now (the title's own poll is 0x437c70 = the safepoint), so records pass its freshness check.
static void al_title_cb(void *user) {
    (void)user;
    if (g_al_state == 3) return;
    if (al_scene_loaded()) { al_mark_done(); return; }
    if (g_al_state != 1 || g_al_pk_mgr) return;    // picker owns the drive now -> stop
    uint32_t mgr = exec_ti_mgr(), now = exec_sp_now();
    if (!mgr) return;

    if (!g_al_title_ctrl) {
        if (g_al_committed) return;         // committed once + the title ctrl is gone -> the save-list is up; wait for
        al_inject(mgr, BTN_TITLE_NAV, now); //   the picker (never re-provoke/nav — that spams the save-list).  Pre-
        return;                             //   commit: provoke a nav call so al_menu_nav_cb can capture menu_ctrl.
    }
    int row = al_title_continue_row(g_al_title_ctrl);
    if (row == -2) {                        // the title has no Continue row -> nothing to load
        if (!g_al_done_logged) { g_al_done_logged = 1;
            ml_log("[sotes] save.load: title has no 'Continue' row (no save present?) — refusing to start "
                   "a NEW GAME; drive stopped."); }
        al_stop();
        return;
    }
    if (row < 0) { g_al_title_ctrl = 0; return; }   // ctrl no longer the title — re-capture (or, if committed, the
                                                    //   next frame's !g_al_title_ctrl branch hands off to the picker)
    al_title_set_cursor(g_al_title_ctrl, row);       // force the cursor onto Continue (capture => interactive)
    al_inject(mgr, BTN_TITLE_OK, now);               // commit Continue -> opens the save-data list (re-injected each frame)
    if (!g_al_committed) { g_al_committed = 1;        // first commit: enable al_picker_cb + log once
        ml_log("[sotes] save.load: cursor -> Continue (row %d) — committing 0x25 until the save-list opens", row); }
}

// mod.game.attract.set(on): on=false freezes the attract/demo (keeps the title up); on=true restores.
static int l_attract_set(lua_State *L) {
    if (!gb_enabled("attract")) return 0;
    al_attract_freeze(lua_toboolean(L, 1) ? 0 : 1);
    return 0;
}
// mod.game.save.load([slot]) -> ok: drive the title + picker menus into a save (slot < 0 / nil =
// newest/default).  Runs on the engine thread (mod init / on_frame), so it installs the picker hook +
// the per-frame drive directly; arms once (a second call while driving is a no-op that returns false).
static int l_save_load(lua_State *L) {
    if (!gb_enabled("save")) { lua_pushboolean(L, 0); return 1; }
    if (g_al_state != 0) { lua_pushboolean(L, 0); return 1; }
    g_al_target = lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : -1;
    g_al_title_ctrl = 0; g_al_ctrl_logged = 0; g_al_commit_logged = 0; g_al_committed = 0;   // fresh drive state
    al_attract_freeze(1);                                   // don't cut to the demo mid-drive
    uintptr_t pk = mem_reloc(VA_PICKER_POLL), nv = mem_reloc(VA_MENU_NAV);
    g_al_pk_h  = hooks_entry_c(pk, al_picker_cb,   NULL);   // auto-confirm the save PICKER once it opens
    g_al_nav_h = hooks_entry_c(nv, al_menu_nav_cb, NULL);   // capture the title menu_ctrl to steer the cursor onto Continue
    exec_on_frame_c(al_title_cb, NULL);
    g_al_state = 1;
    ml_log("[sotes] save.load: armed — picker @ 0x%08x (h%d), nav @ 0x%08x (h%d), slot %d",
           (unsigned)pk, g_al_pk_h, (unsigned)nv, g_al_nav_h, g_al_target);
    lua_pushboolean(L, 1);
    return 1;
}
static void install_attract(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_attract_set); lua_setfield(L, -2, "set");
}
static void install_save(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_save_load); lua_setfield(L, -2, "load");
}

// ── Lua: mod.game.input — inject a UI button into the captured input manager ──────────────────────
// The engine polls buttons via a 64-slot record ring on the input manager (mgr+0x0c, slot 63 first-
// polled); each record is {id@0, now@4, state=1@8} and the poll matches id + state==1 + (poll_now −
// record_now) <= 0x64.  The executor captures the live input mgr at the safepoint (exec_ti_mgr) and
// the poll's timestamp (exec_sp_now).  al_inject (used by save.load) publishes one record — expose it
// as mod.game.input.press(id) so a mod can drive ARBITRARY menu navigation (title, picker, in-game
// menus), not just the built-in save-load.  Button ids (SotES title, RE-verified @ 0x56b80f): 0x01 up /
// 0x03 down, 0x24 confirm/commit, 0x22 abort/back (0x02/0x04 are page-up/down = no-ops on the 1-page title).
// Runs on the engine thread (call from mod.main / mod.on_frame).
static int l_input_press(lua_State *L) {
    if (!gb_enabled("input")) { lua_pushboolean(L, 0); return 1; }
    uint32_t id = (uint32_t)luaL_checkinteger(L, 1);
    uint32_t mgr = lua_isnumber(L, 2) ? (uint32_t)lua_tonumber(L, 2) : exec_ti_mgr();  // default = captured mgr
    if (!mgr) { lua_pushboolean(L, 0); return 1; }   // no safepoint capture yet (not in a menu/scene)
    al_inject(mgr, id, exec_sp_now());
    lua_pushboolean(L, 1);
    return 1;
}
// mod.game.input.mgr() -> the input manager the safepoint captured (0 until the first frame); for RE /
// reading live menu state.  mod.game.input.now() -> the poll's timestamp (arg to a fresh press()).
static int l_input_mgr(lua_State *L) { lua_pushnumber(L, (double)exec_ti_mgr()); return 1; }
static int l_input_now(lua_State *L) { lua_pushnumber(L, (double)exec_sp_now()); return 1; }
static void install_input(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_input_press); lua_setfield(L, -2, "press");
    lua_pushcfunction(L, l_input_mgr);   lua_setfield(L, -2, "mgr");
    lua_pushcfunction(L, l_input_now);   lua_setfield(L, -2, "now");
}

// ── registration ─────────────────────────────────────────────────────────────
void sotes_bindings_register(void) {
    static const gb_def ROSTER = {
        "roster", "party members: name/coords/hp/mp + attack/defense/spirit/resist, combat & adventurer level, exp",
        install_roster, 1
    };
    static const gb_def COORD = {
        "coordinates", "member world x/y in centi-px",
        install_coordinates, 1
    };
    static const gb_def MOUSE = {
        "mouse", "cursor in game 640x480 screen-space + world centi-px (screen_x/y, world_x/y, over)",
        install_mouse, 1
    };
    static const gb_def ATTRACT = {
        "attract", "attract/demo-mode toggle — .set(on) (off keeps the title up)",
        install_attract, 1
    };
    static const gb_def SAVE = {
        "save", "auto-load a save by driving the menus — .load(slot) (<0 = newest)",
        install_save, 1
    };
    static const gb_def INPUT = {
        "input", "inject UI buttons for arbitrary menu navigation — .press(id[,mgr]) / .mgr() / .now()",
        install_input, 1
    };
    gb_register(&ROSTER);
    gb_register(&COORD);
    gb_register(&MOUSE);
    gb_register(&ATTRACT);
    gb_register(&SAVE);
    gb_register(&INPUT);
}
