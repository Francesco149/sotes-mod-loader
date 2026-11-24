// core/sotes_layout.c — per-edition struct offsets.  See sotes_layout.h for provenance of every value.
#include "sotes_layout.h"
#include "syms.h"

#include "lua.h"

static const sotes_layout L_ENOLD = {
    "enold",
    0x104c, 0x1160, 0x11e0, 0x2784, 0x1038, 0x103c, 336, 0x54004,
    0x0008, 0x2008, 0x200c, 0x4030, 0x2010, 0x4010, 0x4014, 0x4020, 0x4024, 0x40ac,
    0x000, 0x004, 0x9c4, 0x9f4,
    0x040, 0x1d4, 0x750, 0xc7a4,
    0xd8, 0xdc, 0xe0, 0xe8, 0xec,
    0x1044, 0x28, 0x10, 0x900, 0x1c, 736, 0x2760, 0,
};
static const sotes_layout L_ENSE = {
    "ense",
    0x104c, 0x1160, 0x11e0, 0x3788, 0x1038, 0x103c, 344, 0x56004,
    0x0008, 0x2008, 0x200c, 0x4030, 0x2010, 0x4010, 0x4014, 0x4020, 0x4024, 0x40ac,
    0x000, 0x004, 0x9e4, 0xa14,
    0x040, 0x1d4, 0x760, 0xc7b4,
    0xdc, 0xe0, 0xe4, 0xec, 0xf0,
    0x1044, 0x2c, 0x14, 0x944, 0x1c, 940, 0x3760, 0x148,
};

const sotes_layout *sotes_layout_get(void) {
    switch (sym_edition()) {
    case SYM_ENOLD:   return &L_ENOLD;
    case SYM_ENSE_EN:
    case SYM_ENSE_JP: return &L_ENSE;
    default:          return 0;
    }
}

void sotes_layout_push_lua(lua_State *L) {
    const sotes_layout *l = sotes_layout_get();
    lua_newtable(L);
    if (!l) return;
#define F(n) lua_pushinteger(L, l->n); lua_setfield(L, -2, #n);
    F(world_view) F(world_chars) F(world_actors) F(world_room) F(world_rinfo) F(world_areas) F(area_size) F(area_count)
    F(room_flags) F(room_nflags) F(room_leader) F(room_slots)
    F(room_items) F(room_nitems) F(room_gold) F(room_gold_cap) F(room_area) F(room_fill)
    F(ps_in) F(ps_code) F(ps_active) F(ps_ch)
    F(ent_body) F(ent_code) F(ch_stats) F(ch_held)
    F(st_combat_lv) F(st_combat_max) F(st_adv_lv) F(st_exp) F(st_exp_max)
    F(world_rl) F(rl_ui_lock) F(rl_warp_req) F(rl_warp) F(rec_exits) F(item_def_size) F(world_script) F(rec_wmap)
#undef F
}
