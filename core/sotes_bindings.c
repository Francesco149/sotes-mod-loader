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
#include <windows.h>

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

static int on_engine(void);
static int wr32(uintptr_t a, uint32_t v);
static int refuse_write(lua_State *L, const char *what);
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

// mod.game.roster.set(code|nil, {hp=, mp=}) -> true — the member's CURRENT hp / mp (stat block +0x54 / +0x5c, what
// the HUD shows; max = base + equip + buff is untouched).  code nil = the controlled member.  Engine thread only.
static const member_t *member_pick(member_t m[NSLOTS], int n, lua_State *L, int arg) {
    int lead = lua_isnoneornil(L, arg);
    uint32_t code = lead ? 0 : (uint32_t)luaL_checkinteger(L, arg);
    for (int i = 0; i < n; i++) if (lead ? m[i].leader : m[i].code == code) return &m[i];
    return NULL;
}
static int l_roster_set(lua_State *L) {
    if (!gb_enabled("roster")) { lua_pushnil(L); return 1; }
    luaL_checktype(L, 2, LUA_TTABLE);
    member_t m[NSLOTS];
    int n = party_members(m);
    const member_t *p = member_pick(m, n, L, 1);
    if (!p || !p->stats || !on_engine()) return refuse_write(L, "roster.set");
    lua_getfield(L, 2, "hp");
    if (lua_isnumber(L, -1)) wr32(p->stats + OFF_HP_CUR, (uint32_t)lua_tointeger(L, -1));
    lua_getfield(L, 2, "mp");
    if (lua_isnumber(L, -1)) wr32(p->stats + OFF_MP_CUR, (uint32_t)lua_tointeger(L, -1));
    lua_pop(L, 2);
    lua_pushboolean(L, 1);
    return 1;
}
static void install_roster(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_roster_set);     lua_setfield(L, -2, "set");
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

// mod.game.coordinates.set(code|nil, x, y) -> true — teleport a member: its body box top-left (world centi-px; the
// position the physics integrates — x sticks, y then settles under gravity).  code nil = the controlled member.
static int l_coord_set(lua_State *L) {
    if (!gb_enabled("coordinates")) { lua_pushnil(L); return 1; }
    int x = (int)luaL_checkinteger(L, 2), y = (int)luaL_checkinteger(L, 3);
    member_t m[NSLOTS];
    int n = party_members(m);
    const member_t *p = member_pick(m, n, L, 1);
    const sotes_layout *l = sotes_layout_get();
    uintptr_t body = p ? rd(p->ch + l->ent_body) : 0;
    if (!body || !on_engine()) return refuse_write(L, "coordinates.set");
    wr32(body + BODY_X, (uint32_t)x);
    wr32(body + BODY_Y, (uint32_t)y);
    lua_pushboolean(L, 1);
    return 1;
}
static void install_coordinates(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_coord_set);    lua_setfield(L, -2, "set");
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
// mod.game.map.rooms() -> {{key, map, exits = {{gate, to}, ...}}, ...} — the area table = every room record the
// scene loop can load (warp destinations); exits = the record's 20 gate slots (gate id, the room it leads to).
// map.room(key) -> one of those | nil.
// Can load_room load this room's map?  Retail's own lookup (load_room 0x586b9c; SE 0x5ad169): SE record +0x148 ≠ 0
// → the W-map file; else FindResourceA(module, record +0xc, "DATA") with module = the area table's override (+4
// after its count) or map_module (0 = the exe).  Not found = retail's fatal "The map data is not found." + exit —
// the area table has such rooms (unused / cut), so map.warp refuses them.
static int map_data_ok(const sotes_layout *l, uintptr_t areas, uintptr_t rec) {
    if (l->rec_wmap && rd(rec + l->rec_wmap)) return 1;
    uintptr_t mm = sym_va("map_module"), hmod = rd(areas + l->area_count + 4);
    if (!hmod && mm) hmod = rd(mm);
    if (!mm && !hmod) return 1;                       // unknown build: can't tell
    return FindResourceA((HMODULE)hmod, MAKEINTRESOURCEA(rd16(rec + 0xc)), "DATA") != NULL;
}
static void push_room_rec(lua_State *L, const sotes_layout *l, uintptr_t areas, uintptr_t rec) {
    lua_newtable(L);
    lua_pushboolean(L, map_data_ok(l, areas, rec)); lua_setfield(L, -2, "loadable");
    lua_pushinteger(L, rdi(rec));     lua_setfield(L, -2, "key");
    lua_pushinteger(L, rdi(rec + 4)); lua_setfield(L, -2, "map");
    lua_newtable(L);
    int k = 0;
    for (int i = 0; i < 20; i++) {
        uintptr_t z = rec + l->rec_exits + 12 * i;
        uint16_t gate = rd16(z);
        int32_t to = rdi(z + 4);
        if (!gate && !to) continue;
        lua_newtable(L);
        lua_pushinteger(L, gate); lua_setfield(L, -2, "gate");
        lua_pushinteger(L, to);   lua_setfield(L, -2, "to");
        lua_rawseti(L, -2, ++k);
    }
    lua_setfield(L, -2, "exits");
}
static uintptr_t areas_any(const sotes_layout **lo) {
    const sotes_layout *l = sotes_layout_get();
    uintptr_t w = world_ptr();
    if (lo) *lo = l;
    return (l && w) ? rd(w + l->world_areas) : 0;
}
static int l_map_rooms(lua_State *L) {
    if (!gb_enabled("map")) { lua_pushnil(L); return 1; }
    const sotes_layout *l;
    uintptr_t areas = areas_any(&l);
    if (!areas) { lua_pushnil(L); return 1; }
    uint16_t n = rd16(areas + l->area_count);
    lua_newtable(L);
    for (uint32_t i = 0; i < n && i < 1024; i++) {
        push_room_rec(L, l, areas, areas + 4 + l->area_size * i);
        lua_rawseti(L, -2, (int)i + 1);
    }
    return 1;
}
static int l_map_room(lua_State *L) {
    if (!gb_enabled("map")) { lua_pushnil(L); return 1; }
    const sotes_layout *l;
    uintptr_t areas = areas_any(&l), rec = areas ? area_find_rec(l, areas, (int32_t)luaL_checkinteger(L, 1)) : 0;
    if (!rec) { lua_pushnil(L); return 1; }
    push_room_rec(L, l, areas, rec);
    return 1;
}
// map.warp(key[, gate[, force]]) -> true | nil, why — the engine's own warp: the room-loop request the warp item
// writes (0x5b3850: +0x10 = 1, +0x900 key, +0x904 gate, +0x908 face, +0x90c kind; SE +0x14 / +0x944..), kind 0 =
// no arrival message, face 1 (as warp_request).  room_warp_tick then applies retail's rules (world +0x1034 /
// +0x1030 == 2 / +0x2768 (SE +0x3768, + SE +0x37b8 in 2..3) / room +0x4080 / no retry point +0x40d0 → "For some
// reason, you cannot teleport here." and the request dropped), plays the dissolve (ui_lock 160), and returns 10:
// the scene loop loads `key` at `gate` (0 / unknown = the room's first gate).
// force = skip those rules + the dissolve: the countdown armed at 1 (no request), so room_warp_tick's own
// `--ui_lock == 0 → return 10` fires on the next room tick.  Engine thread only.
// An event script owns the game (script_begin 0x49cb60 sets world +0x2760 = 1, script_end clears it; SE +0x3760 — fieldxw): its own
// frame loop calls room_warp_tick too but drops the result, so a warp armed then would be consumed and lost —
// map.warp refuses while one runs.  map.event() -> bool.
static int event_running(const sotes_layout *l) {
    uintptr_t w = world_ptr();
    return l && w && rd(w + l->world_script) != 0;
}
static int l_map_event(lua_State *L) {
    const sotes_layout *l = sotes_layout_get();
    if (!l || !world_ptr()) { lua_pushnil(L); return 1; }
    lua_pushboolean(L, event_running(l));
    return 1;
}
static uintptr_t cur_rl(const sotes_layout **lo) {
    const sotes_layout *l = sotes_layout_get();
    uintptr_t w = world_ptr();
    if (lo) *lo = l;
    return (l && w && rd(w + l->world_room)) ? rd(w + l->world_rl) : 0;
}
static int l_map_warp(lua_State *L) {
    if (!gb_enabled("map")) { lua_pushnil(L); return 1; }
    int32_t key = (int32_t)luaL_checkinteger(L, 1);
    int32_t gate = (int32_t)luaL_optinteger(L, 2, 0), face = 1;
    int force = lua_toboolean(L, 3);
    const sotes_layout *l;
    uintptr_t rl = cur_rl(&l);
    if (!rl || !on_engine()) return refuse_write(L, "map.warp");
    uintptr_t areas = areas_any(NULL), rec = area_find_rec(l, areas, key);
    if (!rec) { lua_pushnil(L); lua_pushfstring(L, "map.warp: no room %d", key); return 2; }
    if (!map_data_ok(l, areas, rec)) { lua_pushnil(L); lua_pushfstring(L, "map.warp: room %d has no map data", key); return 2; }
    if (event_running(l)) { lua_pushnil(L); lua_pushstring(L, "map.warp: an event is running (map.event())"); return 2; }
    if (rd16(rl + l->rl_ui_lock) || rd(rl + l->rl_warp_req)) { lua_pushnil(L); lua_pushstring(L, "map.warp: a warp is already running"); return 2; }
    wr32(rl + l->rl_warp + 0x0, (uint32_t)key);
    wr32(rl + l->rl_warp + 0x4, (uint32_t)gate);
    wr32(rl + l->rl_warp + 0x8, (uint32_t)face);
    wr32(rl + l->rl_warp + 0xc, 0);
    if (force) {                                      // u16 ui_lock (the dword's high half is the next field)
        uintptr_t a = rl + l->rl_ui_lock;
        if (!mem_writable((void *)a, 2)) return refuse_write(L, "map.warp");
        *(volatile uint16_t *)a = 1;
    } else
        wr32(rl + l->rl_warp_req, 1);
    lua_pushboolean(L, 1);
    return 1;
}
// map.warp_status() -> 'idle' | 'requested' (room_warp_tick not run yet) | 'warping' (the dissolve, ui_lock), key
static int l_map_warp_status(lua_State *L) {
    const sotes_layout *l;
    uintptr_t rl = cur_rl(&l);
    if (!rl) { lua_pushstring(L, "idle"); return 1; }
    const char *st = rd(rl + l->rl_warp_req) ? "requested" : rd16(rl + l->rl_ui_lock) ? "warping" : "idle";
    lua_pushstring(L, st);
    lua_pushinteger(L, rdi(rl + l->rl_warp));
    return 2;
}
static void install_map(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_map_get);         lua_setfield(L, -2, "get");
    lua_pushcfunction(L, l_map_rooms);       lua_setfield(L, -2, "rooms");
    lua_pushcfunction(L, l_map_room);        lua_setfield(L, -2, "room");
    lua_pushcfunction(L, l_map_warp);        lua_setfield(L, -2, "warp");
    lua_pushcfunction(L, l_map_warp_status); lua_setfield(L, -2, "warp_status");
    lua_pushcfunction(L, l_map_event);       lua_setfield(L, -2, "event");
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
// mod.game.items.give(id[, count]) -> index | nil, why — the engine's own room_item_add (thiscall room, id, &idx,
// count, 0, 0): the item-definition lookup, the edition's capacity rules (enold: 512 records, 48 gear; SE: per
// category), count <= 0 = the definition's default.  No message (the script's item_give without its say).
typedef int (__attribute__((thiscall)) *room_item_add_t)(void *room, int32_t id, uint16_t *idx, int32_t count,
                                                          uint8_t flag, int32_t dry);
static int l_items_give(lua_State *L) {
    if (!gb_enabled("items")) { lua_pushnil(L); return 1; }
    int32_t id = (int32_t)luaL_checkinteger(L, 1), count = (int32_t)luaL_optinteger(L, 2, 0);
    const sotes_layout *l;
    uintptr_t room = cur_room_any(&l), fn = sym_va("room_item_add");
    if (!room || !fn || !on_engine()) return refuse_write(L, "items.give");
    uint16_t idx = 0xffff;
    int ok = ((room_item_add_t)fn)((void *)room, id, &idx, count, 0, 0);
    if (!ok || idx == 0xffff) { lua_pushnil(L); lua_pushfstring(L, "items.give: refused by the game (id %d unknown or inventory full)", id); return 2; }
    lua_pushinteger(L, idx);
    return 1;
}
// mod.game.items.catalog() -> {{id, name}, ...} — every item definition (the static table; SE also the extension
// table item_def searches), names in the edition's encoding.
static void catalog_push(lua_State *L, const sotes_layout *l, uintptr_t t, uint32_t max, int *k) {
    for (uint32_t i = 0; t && i < max; i++) {
        uintptr_t d = t + (uintptr_t)l->item_def_size * i;
        int32_t id = rdi(d);
        if (!id) break;
        char nm[64] = { 0 };
        if (mem_readable((void *)(d + 4), sizeof nm - 1)) memcpy(nm, (void *)(d + 4), sizeof nm - 1);
        lua_newtable(L);
        lua_pushinteger(L, id); lua_setfield(L, -2, "id");
        lua_pushstring(L, nm);  lua_setfield(L, -2, "name");
        lua_rawseti(L, -2, ++*k);
    }
}
static int l_items_catalog(lua_State *L) {
    if (!gb_enabled("items")) { lua_pushnil(L); return 1; }
    const sotes_layout *l = sotes_layout_get();
    uintptr_t t = sym_va("item_table"), ext = sym_va("item_table_ext");
    if (!l || !t) { lua_pushnil(L); return 1; }
    int k = 0;
    lua_newtable(L);
    catalog_push(L, l, t, 4096, &k);
    if (ext) catalog_push(L, l, rd(ext), 0x200, &k);
    return 1;
}
static void install_items(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_items_give);     lua_setfield(L, -2, "give");
    lua_pushcfunction(L, l_items_catalog);  lua_setfield(L, -2, "catalog");
    lua_pushcfunction(L, l_items_list);     lua_setfield(L, -2, "list");
    lua_pushcfunction(L, l_items_gold);     lua_setfield(L, -2, "gold");
    lua_pushcfunction(L, l_items_set_gold); lua_setfield(L, -2, "set_gold");
}

// ══════════════ semantic loader ops — save.load / attract.set ══════════════
// Implemented in core/lua/title.lua (typed hooks on the title driver's own functions, every edition);
// lua_host.c fills these tables once the hook layer is up.  Empty here so the toggle registry knows them.
static void install_attract(lua_State *L) { lua_newtable(L); }
static void install_save(lua_State *L) { lua_newtable(L); }

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
        "attract", "attract/demo-mode toggle — .set(on) / .get() (off keeps the title up)",
        install_attract, 1
    };
    static const gb_def SAVE = {
        "save", "load a save from the title — .load(slot) (<0 = the list's default) / .status() / .cancel()",
        install_save, 1
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
}
