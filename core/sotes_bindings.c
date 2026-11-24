// core/sotes_bindings.c — SotES game-knowledge bindings (mod.game.*).  See sotes_bindings.h.
//
// v2 (2026-11-24): every read goes through the edition layer — the world singleton by symbol
// (sym_va("g_world")) and struct offsets from sotes_layout.h (enold = the port's retail-layout headers,
// SE = tools/fieldxw.py + live checks).  The party comes from the engine's own structures:
//   world = *g_world → room = world->room → room->slots[8] (wl_pslot) → active? → pslot->ch (wl_char)
//   ch->code (+0x1d4), ch->body->{x +4, y +8}, ch->stats (wl_party: name, hp, mp, combat stats, levels)
//   room->leader = the slot the player controls.
// No heap scan, no hard-coded VA: an unknown build → no layout → every binding here returns nil.

#include "sotes_bindings.h"
#include "sotes_layout.h"
#include "syms.h"
#include "game_bindings.h"
#include "mem.h"
#include "executor.h"
#include "hooks.h"            // hooks_entry_c — the picker-poll observer (mod.game.save, fenced)
#include "oss_mod_api.h"      // OssHookCtx
#include "prof.h"
#include "ddraw_present.h"   // ddp_cursor_game — cursor -> game 640x480 screen space (mod.game.mouse)
#include "loader_internal.h"

#include <string.h>

#include "lua.h"
#include "lauxlib.h"

// stat block (wl_party) offsets identical on every edition (< 0xd4; the edition-specific ones are in the layout)
#define OFF_NAME        0x00       // char[8] ("Arche"/"Sana"/"Stella")
#define OFF_HP_CUR      0x54
#define OFF_HP_BASE     0x58
#define OFF_MP_CUR      0x5c
#define OFF_MP_BASE     0x60
#define OFF_ATK         0x64       // combat stats: RAW value (before growth/equipment)
#define OFF_DEF         0x68
#define OFF_SPIRIT      0x6c
#define OFF_RESIST      0x70
#define OFF_ATK_EQUIP   0x74       // equipment bonus — the +N in the status screen's "eff/base"
#define OFF_DEF_EQUIP   0x78
#define OFF_SPI_EQUIP   0x7c
#define OFF_RES_EQUIP   0x80
#define OFF_HP_EQUIP    0x84
#define OFF_MP_EQUIP    0x88
#define OFF_ATK_GROWTH  0x8c       // level/innate growth — folded into the displayed base
#define OFF_DEF_GROWTH  0x90
#define OFF_SPI_GROWTH  0x94
#define OFF_RES_GROWTH  0x98
#define OFF_HP_BUFF     0x9c
#define OFF_MP_BUFF     0xa0
#define BODY_X          0x04       // body -> x (world centi-px, box top-left)
#define BODY_Y          0x08
// camera_view (world->view): identical on every edition (camera_follow.h; SE verified by the v1 mouse mapping)
#define CAM_VIEW_TOP    0x5c       // eased scroll top  (world centi-px)
#define CAM_VIEW_LEFT   0x60       // eased scroll left
#define CAM_VP_W        0x64       // viewport width  (centi-px; 64000 = 640 px)
#define CAM_VP_H        0x68       // viewport height
#define NSLOTS          8

static const char *code_name(uint32_t c) {
    switch (c) { case 0xc35a: return "Arche"; case 0xc35b: return "Sana"; case 0xc35c: return "Stella"; case 0xc35d: return "Chiffon"; }
    return NULL;
}

static uint32_t rd(uintptr_t a) { uint32_t v = 0; return a > 0x10000 && mem_rd32((void *)a, &v) ? v : 0; }
static int rdi(uintptr_t a) { return (int)rd(a); }
static int sum3(uintptr_t sb, int a, int b, int c) { int m = rdi(sb + a) + rdi(sb + b) + rdi(sb + c); return m < 0 ? 0 : m; }
static int sum2(uintptr_t sb, int a, int b) { int m = rdi(sb + a) + rdi(sb + b); return m < 0 ? 0 : m; }

static uintptr_t world_ptr(void) {
    uintptr_t g = sym_va("g_world");
    return g ? rd(g) : 0;
}
static uintptr_t cur_room(const sotes_layout *l) {
    uintptr_t w = world_ptr();
    return w ? rd(w + l->world_room) : 0;
}

typedef struct { uintptr_t slot, ch, stats; uint32_t code; int leader; } member_t;

// The live party: active room slots with a character.  Returns the count (<= NSLOTS).
static int party_members(member_t out[NSLOTS]) {
    const sotes_layout *l = sotes_layout_get();
    if (!l) return 0;
    uintptr_t room = cur_room(l);
    if (!room) return 0;
    uintptr_t leader = rd(room + l->room_leader);
    int n = 0;
    for (int i = 0; i < NSLOTS; i++) {
        uintptr_t s = rd(room + l->room_slots + 4 * i);
        if (!s || rd(s + l->ps_active) != 1) continue;
        uintptr_t ch = rd(s + l->ps_ch);
        if (!ch) continue;
        out[n].slot = s; out[n].ch = ch;
        out[n].stats = rd(ch + l->ch_stats);
        out[n].code = rd(ch + l->ent_code);
        out[n].leader = (s == leader);
        n++;
    }
    return n;
}
static int member_xy(const sotes_layout *l, uintptr_t ch, int *x, int *y) {
    uintptr_t body = rd(ch + l->ent_body);
    if (!body) return 0;
    *x = rdi(body + BODY_X); *y = rdi(body + BODY_Y);
    return 1;
}

// ── Lua: mod.game.roster ─────────────────────────────────────────────────────
static void push_member(lua_State *L, const member_t *m) {
    const sotes_layout *l = sotes_layout_get();
    uintptr_t sb = m->stats;
    int x = 0, y = 0;
    member_xy(l, m->ch, &x, &y);
    lua_newtable(L);
    lua_pushinteger(L, (int)m->code);           lua_setfield(L, -2, "code");
    const char *id = code_name(m->code), *nm = id;
    char buf[9] = { 0 };
    if (sb && mem_readable((void *)sb, 8)) { memcpy(buf, (void *)(sb + OFF_NAME), 8); if (buf[0]) nm = buf; }
    if (nm) { lua_pushstring(L, nm); lua_setfield(L, -2, "name"); }    // the game's own text (Shift-JIS on ense-jp)
    if (id) { lua_pushstring(L, id); lua_setfield(L, -2, "id"); }      // stable ASCII id by code
    lua_pushnumber(L, (double)m->ch);           lua_setfield(L, -2, "actor");
    lua_pushnumber(L, (double)m->slot);         lua_setfield(L, -2, "slot");
    lua_pushnumber(L, (double)sb);              lua_setfield(L, -2, "stat_block");
    lua_pushinteger(L, x);                      lua_setfield(L, -2, "x");
    lua_pushinteger(L, y);                      lua_setfield(L, -2, "y");
    lua_pushboolean(L, m->leader);              lua_setfield(L, -2, "active");   // the controlled member (room->leader)
    if (sb) {
        int clv = rdi(sb + l->st_combat_lv), cmax = rdi(sb + l->st_combat_max), alv = rdi(sb + l->st_adv_lv);
        lua_pushinteger(L, clv + alv);          lua_setfield(L, -2, "level");            // the HUD / save-list "Lv"
        lua_pushinteger(L, clv + alv);          lua_setfield(L, -2, "char_level");
        lua_pushinteger(L, clv);                lua_setfield(L, -2, "combat_level");     // "Combat Level M/N": M
        lua_pushinteger(L, cmax);               lua_setfield(L, -2, "combat_level_max"); //   N (the HUD stars)
        lua_pushinteger(L, alv);                lua_setfield(L, -2, "adventurer_level");
        lua_pushinteger(L, rdi(sb + OFF_HP_CUR)); lua_setfield(L, -2, "hp");
        lua_pushinteger(L, sum3(sb, OFF_HP_BASE, OFF_HP_EQUIP, OFF_HP_BUFF)); lua_setfield(L, -2, "hp_max");
        lua_pushinteger(L, rdi(sb + OFF_MP_CUR)); lua_setfield(L, -2, "mp");
        lua_pushinteger(L, sum3(sb, OFF_MP_BASE, OFF_MP_EQUIP, OFF_MP_BUFF)); lua_setfield(L, -2, "mp_max");
        lua_pushinteger(L, sum3(sb, OFF_ATK, OFF_ATK_EQUIP, OFF_ATK_GROWTH));    lua_setfield(L, -2, "attack");
        lua_pushinteger(L, sum2(sb, OFF_ATK, OFF_ATK_GROWTH));                   lua_setfield(L, -2, "attack_base");
        lua_pushinteger(L, sum3(sb, OFF_DEF, OFF_DEF_EQUIP, OFF_DEF_GROWTH));    lua_setfield(L, -2, "defense");
        lua_pushinteger(L, sum2(sb, OFF_DEF, OFF_DEF_GROWTH));                   lua_setfield(L, -2, "defense_base");
        lua_pushinteger(L, sum3(sb, OFF_SPIRIT, OFF_SPI_EQUIP, OFF_SPI_GROWTH)); lua_setfield(L, -2, "spirit");
        lua_pushinteger(L, sum2(sb, OFF_SPIRIT, OFF_SPI_GROWTH));                lua_setfield(L, -2, "spirit_base");
        lua_pushinteger(L, sum3(sb, OFF_RESIST, OFF_RES_EQUIP, OFF_RES_GROWTH)); lua_setfield(L, -2, "resist");
        lua_pushinteger(L, sum2(sb, OFF_RESIST, OFF_RES_GROWTH));                lua_setfield(L, -2, "resist_base");
        lua_pushinteger(L, rdi(sb + l->st_exp));     lua_setfield(L, -2, "exp");
        lua_pushinteger(L, rdi(sb + l->st_exp_max)); lua_setfield(L, -2, "exp_max");
    }
}

// mod.game.roster.members() -> { {code,name,actor,slot,x,y,hp,...,active}, ... } in room-slot order.
static int l_roster_members(lua_State *L) {
    if (!gb_enabled("roster")) { lua_pushnil(L); return 1; }   // inert even via a captured ref
    uint64_t t = prof_now();
    member_t m[NSLOTS];
    int n = party_members(m);
    lua_newtable(L);
    for (int i = 0; i < n; i++) { push_member(L, &m[i]); lua_rawseti(L, -2, i + 1); }
    prof_add(PROF_ROSTER, t);
    return 1;
}
// mod.game.roster.world() -> {world, room, view, nflags} (raw pointers; nil outside a scene) — for RE / mods
static int l_roster_world(lua_State *L) {
    const sotes_layout *l = sotes_layout_get();
    uintptr_t w = world_ptr();
    if (!l || !w) { lua_pushnil(L); return 1; }
    uintptr_t room = rd(w + l->world_room);
    lua_newtable(L);
    lua_pushnumber(L, (double)w);                      lua_setfield(L, -2, "world");
    lua_pushnumber(L, (double)room);                   lua_setfield(L, -2, "room");
    lua_pushnumber(L, (double)rd(w + l->world_view));  lua_setfield(L, -2, "view");
    if (room) { lua_pushinteger(L, (int)(rd(room + l->room_nflags) & 0xffff)); lua_setfield(L, -2, "nflags"); }
    return 1;
}

static void install_roster(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_roster_members); lua_setfield(L, -2, "members");
    lua_pushcfunction(L, l_roster_world);   lua_setfield(L, -2, "world");
    sotes_layout_push_lua(L);               lua_setfield(L, -2, "layout");   // this edition's offsets (sotes_layout.h)
}

// ── Lua: mod.game.coordinates ────────────────────────────────────────────────
// get([code]) -> {x,y,actor,code} for a member (default Arche); player() = Arche; target() = the
// controlled member (room->leader).
static int push_coords(lua_State *L, const member_t *m) {
    int x, y;
    if (!m || !member_xy(sotes_layout_get(), m->ch, &x, &y)) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, x);              lua_setfield(L, -2, "x");
    lua_pushinteger(L, y);              lua_setfield(L, -2, "y");
    lua_pushnumber(L, (double)m->ch);   lua_setfield(L, -2, "actor");
    lua_pushinteger(L, (int)m->code);   lua_setfield(L, -2, "code");
    return 1;
}
static int coord_by(lua_State *L, uint32_t code, int want_leader) {
    member_t m[NSLOTS];
    int n = party_members(m);
    for (int i = 0; i < n; i++)
        if (want_leader ? m[i].leader : m[i].code == code) return push_coords(L, &m[i]);
    if (want_leader && n) return push_coords(L, &m[0]);
    lua_pushnil(L);
    return 1;
}
static int l_coord_get(lua_State *L) {
    if (!gb_enabled("coordinates")) { lua_pushnil(L); return 1; }
    return coord_by(L, lua_isnumber(L, 1) ? (uint32_t)lua_tointeger(L, 1) : 0xc35a, 0);
}
static int l_coord_player(lua_State *L) {
    if (!gb_enabled("coordinates")) { lua_pushnil(L); return 1; }
    return coord_by(L, 0xc35a, 0);
}
static int l_coord_target(lua_State *L) {
    if (!gb_enabled("coordinates")) { lua_pushnil(L); return 1; }
    return coord_by(L, 0, 1);
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
// world_* is world centi-px via the camera (view origin + screen*viewport-span), omitted outside a scene.
static int l_mouse_get(lua_State *L) {
    if (!gb_enabled("mouse")) { lua_pushnil(L); return 1; }
    float gx = 0, gy = 0;
    int over = ddp_cursor_game(&gx, &gy);
    lua_newtable(L);
    lua_pushnumber(L, gx);    lua_setfield(L, -2, "screen_x");
    lua_pushnumber(L, gy);    lua_setfield(L, -2, "screen_y");
    lua_pushboolean(L, over); lua_setfield(L, -2, "over");
    const sotes_layout *l = sotes_layout_get();
    uintptr_t w = world_ptr(), cam = (l && w) ? rd(w + l->world_view) : 0;
    int vpw = cam ? rdi(cam + CAM_VP_W) : 0, vph = cam ? rdi(cam + CAM_VP_H) : 0;
    if (vpw && vph) {
        int wx = rdi(cam + CAM_VIEW_LEFT) + (int)(gx * (float)vpw / 640.0f);
        int wy = rdi(cam + CAM_VIEW_TOP)  + (int)(gy * (float)vph / 480.0f);
        lua_pushinteger(L, wx);  lua_setfield(L, -2, "world_x");
        lua_pushinteger(L, wy);  lua_setfield(L, -2, "world_y");
    }
    return 1;
}
static void install_mouse(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_mouse_get); lua_setfield(L, -2, "get");
}

// ══════════════ bindings v2, wave 2: camera / map / flags / items (2026-11-24) ══════════════
// All offsets from sotes_layout.h (identical on enold + SE: fieldxw, see the header).  Reads anywhere;
// WRITES only on the engine thread (mod.on_frame / mod.main / the dev server's `lua`) — else refused, so a
// mod's UI thread can never race the game's own read-modify-write of the same field.
static int on_engine(void) { return exec_main_tid() && GetCurrentThreadId() == exec_main_tid(); }
static int wr32(uintptr_t a, uint32_t v) {
    if (a <= 0x10000 || !mem_writable((void *)a, 4)) return 0;
    *(volatile uint32_t *)a = v;
    return 1;
}
static uint16_t rd16(uintptr_t a) { return (uint16_t)(rd(a & ~(uintptr_t)3) >> ((a & 2) * 8)); }   // a even
static uintptr_t cur_room_any(const sotes_layout **lo) {
    const sotes_layout *l = sotes_layout_get();
    uintptr_t w = world_ptr();
    if (lo) *lo = l;
    return (l && w) ? rd(w + l->world_room) : 0;
}
static int refuse_write(lua_State *L, const char *what) {
    lua_pushnil(L);
    lua_pushfstring(L, "%s: write refused (%s)", what, on_engine() ? "no scene" : "not on the engine thread");
    return 2;
}

// ── mod.game.camera.get() -> {left, top, width, height, map_w, map_h} (world centi-px; nil outside a scene)
static int l_camera_get(lua_State *L) {
    if (!gb_enabled("camera")) { lua_pushnil(L); return 1; }
    const sotes_layout *l = sotes_layout_get();
    uintptr_t w = world_ptr(), cam = (l && w) ? rd(w + l->world_view) : 0;
    if (!cam) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, rdi(cam + CAM_VIEW_LEFT)); lua_setfield(L, -2, "left");
    lua_pushinteger(L, rdi(cam + CAM_VIEW_TOP));  lua_setfield(L, -2, "top");
    lua_pushinteger(L, rdi(cam + CAM_VP_W));      lua_setfield(L, -2, "width");
    lua_pushinteger(L, rdi(cam + CAM_VP_H));      lua_setfield(L, -2, "height");
    lua_pushinteger(L, rdi(cam + 0x0));           lua_setfield(L, -2, "map_w");   // camera_view +0 / +4: the map size
    lua_pushinteger(L, rdi(cam + 0x4));           lua_setfield(L, -2, "map_h");
    lua_pushnumber(L, (double)cam);               lua_setfield(L, -2, "view");
    return 1;
}
static void install_camera(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_camera_get); lua_setfield(L, -2, "get");
}

// ── mod.game.map.get() -> {area, map, record, areas} — the current area id (room +0x4024, what the scene loop
// looks up per room), its map (area_find: the area table's record +4; 336 B enold / 344 B SE), the raw room record.
static uintptr_t area_find_rec(const sotes_layout *l, uintptr_t areas, int32_t id) {   // area_find
    if (!areas) return 0;
    uint16_t n = rd16(areas + l->area_count);
    for (uint32_t i = 0; i < n && i < 1024; i++)
        if (rdi(areas + 4 + l->area_size * i) == id) return areas + 4 + l->area_size * i;
    return 0;
}
static int l_map_get(lua_State *L) {
    if (!gb_enabled("map")) { lua_pushnil(L); return 1; }
    const sotes_layout *l;
    uintptr_t room = cur_room_any(&l), w = world_ptr();
    if (!room) { lua_pushnil(L); return 1; }
    int32_t area = rdi(room + l->room_area);
    uintptr_t areas = rd(w + l->world_areas), rec = area_find_rec(l, areas, area);
    lua_newtable(L);
    lua_pushinteger(L, area);                         lua_setfield(L, -2, "area");
    if (rec) { lua_pushinteger(L, rdi(rec + 4));      lua_setfield(L, -2, "map"); }
    lua_pushnumber(L, (double)rd(w + l->world_rinfo)); lua_setfield(L, -2, "record");
    lua_pushnumber(L, (double)areas);                  lua_setfield(L, -2, "areas");
    lua_pushinteger(L, areas ? rd16(areas + l->area_count) : 0); lua_setfield(L, -2, "nareas");
    return 1;
}
static void install_map(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_map_get); lua_setfield(L, -2, "get");
}

// ── mod.game.flags — two engine tables:
//   story: the persistent FILL table (room +0x40ac: {id, v, v2}[512], count +0x1800) — what the event scripts
//          read / write (flag_find 0x4c57f0 / script_flag_set) and the save carries.
//   area:  the room's area state (room +0x8: {id, state}[1024], u16 count +0x2008; room_flag_set 0x560e90).
// get(id) -> v, v2 (nil = absent); list() -> {{id,v,v2},...}; set(id, v[, v2]) mirrors the engine exactly
// (story: find-or-add into the LAST free slot, v = v2 = 0 clears the id; area: find-or-append, state = v).
static uintptr_t story_tbl(void) {
    const sotes_layout *l;
    uintptr_t room = cur_room_any(&l);
    return room ? rd(room + l->room_fill) : 0;
}
static uintptr_t story_find(uintptr_t t, int32_t id) {
    uint32_t n = rd(t + 0x1800);
    for (uint32_t i = 0; i < n && i < 512; i++) if (rdi(t + 12 * i) == id) return t + 12 * i;
    return 0;
}
static int l_story_get(lua_State *L) {
    if (!gb_enabled("flags")) { lua_pushnil(L); return 1; }
    uintptr_t t = story_tbl(), f = t ? story_find(t, (int32_t)luaL_checkinteger(L, 1)) : 0;
    if (!f) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, rdi(f + 4)); lua_pushinteger(L, rdi(f + 8));
    return 2;
}
static int l_story_list(lua_State *L) {
    if (!gb_enabled("flags")) { lua_pushnil(L); return 1; }
    uintptr_t t = story_tbl();
    if (!t) { lua_pushnil(L); return 1; }
    uint32_t n = rd(t + 0x1800);
    lua_newtable(L);
    int k = 0;
    for (uint32_t i = 0; i < n && i < 512; i++) {
        int32_t id = rdi(t + 12 * i);
        if (!id) continue;                       // a cleared slot (script_flag_set with 0, 0)
        lua_createtable(L, 3, 0);
        lua_pushinteger(L, id);                  lua_rawseti(L, -2, 1);
        lua_pushinteger(L, rdi(t + 12 * i + 4)); lua_rawseti(L, -2, 2);
        lua_pushinteger(L, rdi(t + 12 * i + 8)); lua_rawseti(L, -2, 3);
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}
static int l_story_set(lua_State *L) {
    if (!gb_enabled("flags")) { lua_pushnil(L); return 1; }
    int32_t id = (int32_t)luaL_checkinteger(L, 1), v = (int32_t)luaL_checkinteger(L, 2), v2 = (int32_t)luaL_optinteger(L, 3, 0);
    uintptr_t t = story_tbl();
    if (!t || !on_engine() || !id) return refuse_write(L, "flags.story.set");
    uintptr_t f = story_find(t, id);
    if (!f) {                                    // flag_add 0x4c5830: the LAST free (id 0) slot, else append
        uint32_t n = rd(t + 0x1800);
        for (uint32_t i = 0; i < n && i < 512; i++) if (rdi(t + 12 * i) == 0) f = t + 12 * i;
        if (!f) {
            if (n >= 512) { lua_pushnil(L); lua_pushstring(L, "flags.story.set: table full (512)"); return 2; }
            f = t + 12 * n;
            wr32(t + 0x1800, n + 1);
        }
        wr32(f + 8, 0); wr32(f + 4, 0); wr32(f, (uint32_t)id);
    }
    if (v || v2) { wr32(f + 4, (uint32_t)v); wr32(f + 8, (uint32_t)v2); }
    else wr32(f, 0);                             // script_flag_set: 0, 0 frees the slot
    lua_pushboolean(L, 1);
    return 1;
}
static int l_area_get(lua_State *L) {
    if (!gb_enabled("flags")) { lua_pushnil(L); return 1; }
    const sotes_layout *l;
    uintptr_t room = cur_room_any(&l);
    int32_t id = (int32_t)luaL_checkinteger(L, 1);
    if (room) {
        uint16_t n = rd16(room + l->room_nflags);
        for (uint32_t i = 0; i < n && i < 1024; i++)
            if (rdi(room + l->room_flags + 8 * i) == id) { lua_pushinteger(L, rdi(room + l->room_flags + 8 * i + 4)); return 1; }
    }
    lua_pushnil(L);
    return 1;
}
static int l_area_list(lua_State *L) {
    if (!gb_enabled("flags")) { lua_pushnil(L); return 1; }
    const sotes_layout *l;
    uintptr_t room = cur_room_any(&l);
    if (!room) { lua_pushnil(L); return 1; }
    uint16_t n = rd16(room + l->room_nflags);
    lua_newtable(L);
    for (uint32_t i = 0; i < n && i < 1024; i++) {
        lua_createtable(L, 2, 0);
        lua_pushinteger(L, rdi(room + l->room_flags + 8 * i));     lua_rawseti(L, -2, 1);
        lua_pushinteger(L, rdi(room + l->room_flags + 8 * i + 4)); lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
static int l_area_set(lua_State *L) {
    if (!gb_enabled("flags")) { lua_pushnil(L); return 1; }
    int32_t id = (int32_t)luaL_checkinteger(L, 1), v = (int32_t)luaL_checkinteger(L, 2);
    const sotes_layout *l;
    uintptr_t room = cur_room_any(&l);
    if (!room || !on_engine()) return refuse_write(L, "flags.area.set");
    uint16_t n = rd16(room + l->room_nflags);
    uintptr_t f = 0;
    for (uint32_t i = 0; i < n && i < 1024; i++)
        if (rdi(room + l->room_flags + 8 * i) == id) { f = room + l->room_flags + 8 * i; break; }
    if (!f) {                                    // room_flag_set 0x560e90: append
        if (n >= 0x400) { lua_pushnil(L); lua_pushstring(L, "flags.area.set: table full (1024)"); return 2; }
        f = room + l->room_flags + 8 * n;
        wr32(f, (uint32_t)id);
        uintptr_t a = room + l->room_nflags;    // u16 at an 8-aligned offset: rewrite its dword's low half
        wr32(a, (rd(a) & 0xffff0000u) | (uint16_t)(n + 1));
    }
    wr32(f + 4, (uint32_t)v);
    lua_pushboolean(L, 1);
    return 1;
}
static void install_flags(lua_State *L) {
    lua_newtable(L);
    lua_newtable(L);
    lua_pushcfunction(L, l_story_get);  lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_story_list); lua_setfield(L, -2, "list");
    lua_pushcfunction(L, l_story_set);  lua_setfield(L, -2, "set");
    lua_setfield(L, -2, "story");
    lua_newtable(L);
    lua_pushcfunction(L, l_area_get);   lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_area_list);  lua_setfield(L, -2, "list");
    lua_pushcfunction(L, l_area_set);   lua_setfield(L, -2, "set");
    lua_setfield(L, -2, "area");
}

// ── mod.game.items — the party inventory (room +0x2010: 16-B records {id, owner, u16 count, u8 stored, -,
// def*}, u16 count +0x4010; def = the item table's 736-B row, its name at +4) + the money (room +0x4014 CP,
// cap +0x4020; 100 CP = 1 GP).
//   list() -> {{index, id, name, count, owner, stored}, ...};  gold() -> cp, cap;  set_gold(cp) (clamped to the cap)
static int l_items_list(lua_State *L) {
    if (!gb_enabled("items")) { lua_pushnil(L); return 1; }
    const sotes_layout *l;
    uintptr_t room = cur_room_any(&l);
    if (!room) { lua_pushnil(L); return 1; }
    uint16_t n = rd16(room + l->room_nitems);
    lua_newtable(L);
    for (uint32_t i = 0; i < n && i < 512; i++) {
        uintptr_t r = room + l->room_items + 16 * i, def = rd(r + 0xc);
        lua_newtable(L);
        lua_pushinteger(L, (int)i);                    lua_setfield(L, -2, "index");
        lua_pushinteger(L, rdi(r));                    lua_setfield(L, -2, "id");
        lua_pushinteger(L, rd16(r + 8));               lua_setfield(L, -2, "count");
        lua_pushinteger(L, rdi(r + 4));                lua_setfield(L, -2, "owner");
        lua_pushboolean(L, (rd(r + 8) >> 16) & 0xff);  lua_setfield(L, -2, "stored");
        if (def && mem_readable((void *)(def + 4), 64)) {
            char nm[65]; memcpy(nm, (const void *)(def + 4), 64); nm[64] = 0;
            lua_pushstring(L, nm);                     lua_setfield(L, -2, "name");
        }
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
static int l_items_gold(lua_State *L) {
    if (!gb_enabled("items")) { lua_pushnil(L); return 1; }
    const sotes_layout *l;
    uintptr_t room = cur_room_any(&l);
    if (!room) { lua_pushnil(L); return 1; }
    lua_pushinteger(L, rdi(room + l->room_gold));
    lua_pushinteger(L, rdi(room + l->room_gold_cap));
    return 2;
}
static int l_items_set_gold(lua_State *L) {
    if (!gb_enabled("items")) { lua_pushnil(L); return 1; }
    int32_t v = (int32_t)luaL_checkinteger(L, 1);
    const sotes_layout *l;
    uintptr_t room = cur_room_any(&l);
    if (!room || !on_engine()) return refuse_write(L, "items.set_gold");
    int32_t cap = rdi(room + l->room_gold_cap);
    if (v < 0) v = 0;
    if (cap > 0 && v > cap) v = cap;            // room_gold_add 0x419c00 clamps to the cap too
    wr32(room + l->room_gold, (uint32_t)v);
    lua_pushinteger(L, v);
    return 1;
}
static void install_items(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_items_list);     lua_setfield(L, -2, "list");
    lua_pushcfunction(L, l_items_gold);     lua_setfield(L, -2, "gold");
    lua_pushcfunction(L, l_items_set_gold); lua_setfield(L, -2, "set_gold");
}

// ══════════════ semantic loader ops — RE'd menu-drive, exposed so mods ORCHESTRATE it ══════════════
// The attract-mode toggle + auto-load-a-save mechanism (input-record ring, the demo trigger, the
// title/picker poll VAs) lives HERE in the loader; a mod just says mod.game.save.load(slot).  This is
// the old core/sotes_autoload.c, retired into a proper binding — same mechanism, now mod-facing.
// Input-record contract: a record is {id@0, now@4, state=1@8}; its pointer is published into the input
// manager's 64-slot ring at mgr+0x0c, slot 63 (first polled); the poll matches id + state==1 +
// (poll_now - record_now) <= 0x64.  `now` = the poll's own timestamp.  Everything runs engine-thread.
#define VA_RENDER_ROOT  0x92dd38    // v1: "in a scene" probe (old SE build)
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
static int      g_al_slot_logged;   // one-shot: warned that a specific g_al_target isn't honored yet (fast-follow)
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
// 0x5e7fe0 (menu_ctrl nav/action) entry observer — runs ONLY when a nav/confirm button is consumed, so this is how
// we grab the live TITLE menu_ctrl (its ecx) to steer onto Continue.  0x5e7fe0 is a real function entry (clean
// `push esi;mov esi,ecx`), so — unlike the old 0x43ce50 — the MinHook patch sits on a proper boundary.
// (Steering the save-DATA list onto a SPECIFIC slot is a fast-follow: its scene loads from a selection separate
// from this menu_ctrl's cursor, so a direct cursor write doesn't take — needs the save-scene's own field.)
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
        if (g_al_committed) {               // the title committed -> the save-DATA list is up; al_picker_cb loads it
            if (g_al_target >= 0 && !g_al_slot_logged) { g_al_slot_logged = 1;
                ml_log("[sotes] save.load: slot %d requested, but SPECIFIC-slot auto-load is a fast-follow — "
                       "loading the save-list's default (last-used) save for now", g_al_target); }
            return;                         // wait for al_picker_cb (never nav here — that would spam the save-list)
        }
        al_inject(mgr, BTN_TITLE_NAV, now); // pre-commit: provoke a nav call so al_menu_nav_cb can capture the title
        return;
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
// PORT-DEBT(v1-se-vas): attract / save / input patch + hook VAs from the PRE-2026-08 SE build; the Steam
// update moved that code (0x437c70 is no longer even a function start).  Refused until rebuilt on the
// symbol table (docs/plans/v2-revamp.md step 4) — a stale patch would write into unrelated code.
static int legacy_refuse(lua_State *L, const char *what) {
    ml_log("[game] %s: v1 address set (old SE build) — disabled until rebuilt on symbols (v2 step 4)", what);
    lua_pushboolean(L, 0);
    return 1;
}

static int l_attract_set(lua_State *L) {
    return legacy_refuse(L, "attract.set");
    if (!gb_enabled("attract")) return 0;
    al_attract_freeze(lua_toboolean(L, 1) ? 0 : 1);
    return 0;
}
// mod.game.save.load([slot]) -> ok: drive the title + picker menus into a save (slot < 0 / nil =
// newest/default).  Runs on the engine thread (mod init / on_frame), so it installs the picker hook +
// the per-frame drive directly; arms once (a second call while driving is a no-op that returns false).
static int l_save_load(lua_State *L) {
    return legacy_refuse(L, "save.load");
    if (!gb_enabled("save")) { lua_pushboolean(L, 0); return 1; }
    if (g_al_state != 0) { lua_pushboolean(L, 0); return 1; }
    g_al_target = lua_isnumber(L, 1) ? (int)lua_tointeger(L, 1) : -1;
    g_al_title_ctrl = 0; g_al_ctrl_logged = 0; g_al_commit_logged = 0; g_al_committed = 0;   // fresh drive state
    g_al_slot_logged = 0;
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
    return legacy_refuse(L, "input.press");
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
    static const gb_def CAMERA = {
        "camera", "the camera view: left/top/width/height + map size (world centi-px)", install_camera, 1
    };
    static const gb_def MAP = {
        "map", "the current area id + its map (area table) — .get()", install_map, 1
    };
    static const gb_def FLAGS = {
        "flags", "story flags (persistent fill table) + area state — .story/.area get/list/set", install_flags, 1
    };
    static const gb_def ITEMS = {
        "items", "party inventory (id/name/count/owner/stored) + money — .list() / .gold() / .set_gold(cp)", install_items, 1
    };
    gb_register(&ROSTER);
    gb_register(&CAMERA);
    gb_register(&MAP);
    gb_register(&FLAGS);
    gb_register(&ITEMS);
    gb_register(&COORD);
    gb_register(&MOUSE);
    gb_register(&ATTRACT);
    gb_register(&SAVE);
    gb_register(&INPUT);
}
