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
#define OFF_ATK         0x64
#define OFF_DEF         0x68
#define OFF_SPIRIT      0x6c
#define OFF_RESIST      0x70
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
    mem_rd32((void *)(a + OFF_STATBLOCK), &sb);
    mem_rd32((void *)(a + OFF_WORLD_X), &wx);
    mem_rd32((void *)(a + OFF_WORLD_Y), &wy);
    int hpm = 0, mpm = 0;
    if (sb) {
        mem_rd32((void *)(sb + OFF_COMBAT_LV_MAX), &clv);
        mem_rd32((void *)(sb + OFF_ADV_LEVEL), &alv);
        mem_rd32((void *)(sb + OFF_HP_CUR), &hpc);
        mem_rd32((void *)(sb + OFF_MP_CUR), &mpc);
        mem_rd32((void *)(sb + OFF_ATK), &atk);
        mem_rd32((void *)(sb + OFF_DEF), &def);
        mem_rd32((void *)(sb + OFF_SPIRIT), &spi);
        mem_rd32((void *)(sb + OFF_RESIST), &res);
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
    // the 4 combat stats — raw base (docs/findings/save15-live-stats.md in ../OpenSummoners).
    lua_pushinteger(L, (int)atk);           lua_setfield(L, -2, "attack");
    lua_pushinteger(L, (int)def);           lua_setfield(L, -2, "defense");
    lua_pushinteger(L, (int)spi);           lua_setfield(L, -2, "spirit");
    lua_pushinteger(L, (int)res);           lua_setfield(L, -2, "resist");
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
#define VA_DEMO_JL      0x583866    // attract/demo trigger (jl 0x5832e1)
#define BTN_CONFIRM     0x25        // title/picker confirm button id
#define RING_OFF        0x0c        // input mgr -> 64 record-pointer ring
#define RING_SLOT       63          // ordinal 0 = the first-polled ring slot

static int      g_al_target = -1;   // savedataNN index; < 0 = newest/default
static int      g_al_state;         // 0 idle, 1 title-confirm, 2 picker-confirm, 3 done
static uint32_t g_al_pk_mgr;        // captured picker controller
static int      g_al_pk_logged, g_al_done_logged;
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
static void al_mark_done(void) {
    g_al_state = 3;
    al_attract_freeze(0);   // drive done (past the title) — restore the demo so attract works normally again
    if (!g_al_done_logged) { g_al_done_logged = 1; ml_log("[sotes] save.load: scene loaded — gameplay reached, drive stopped"); }
}
static void al_picker_cb(const OssHookCtx *ctx, void *user) {   // picker poll observer: capture ctrl + confirm
    (void)user;
    if (!g_al_pk_logged) { g_al_pk_logged = 1; ml_log("[sotes] save.load: picker open (ctrl=0x%08x) — confirming", (unsigned)ctx->ecx); }
    g_al_pk_mgr = ctx->ecx;
    if (g_al_state == 1) g_al_state = 2;
    if (g_al_state != 2) return;
    if (al_scene_loaded()) { al_mark_done(); return; }
    uint32_t now = 0; mem_rd32((void *)(uintptr_t)(ctx->esp + 8), &now);   // 0x4378d0 arg2 = the poll's now
    al_inject(ctx->ecx, BTN_CONFIRM, now);
}
static void al_title_cb(void *user) {   // title poll drive, every safepoint
    (void)user;
    if (g_al_state == 3) return;
    if (al_scene_loaded()) { al_mark_done(); return; }
    if (g_al_state == 1 && !g_al_pk_mgr) al_inject(exec_ti_mgr(), BTN_CONFIRM, exec_sp_now());
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
    al_attract_freeze(1);                                   // don't cut to the demo mid-drive
    uintptr_t pk = mem_reloc(VA_PICKER_POLL);
    int h = hooks_entry_c(pk, al_picker_cb, NULL);
    exec_on_frame_c(al_title_cb, NULL);
    g_al_state = 1;
    ml_log("[sotes] save.load: armed — picker hook @ 0x%08x (handle %d), slot %d (<0 = newest)", (unsigned)pk, h, g_al_target);
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
    gb_register(&ROSTER);
    gb_register(&COORD);
    gb_register(&MOUSE);
    gb_register(&ATTRACT);
    gb_register(&SAVE);
}
