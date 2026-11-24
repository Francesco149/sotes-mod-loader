// core/sotes_layout.h — per-edition STRUCT OFFSETS the SotES bindings read (the edition layer's other half:
// syms.h gives VAs, this gives field offsets).  enold values = OpenSummoners' retail-layout headers
// (src/world_layout.h, party.h, camera_follow.h — static-asserted against retail there); SE values =
// tools/fieldxw.py (aligned enold↔SE function pairs, the displacement each SE site uses) + live checks
// under wine (tests/*.lua via tools/sml).  ense-jp == ense-en for every field below (fieldxw --to ense-jp).
//
// Reproduce:  python3 tools/fieldxw.py 0x2784 0x4030 0x9f4 0x9c4 0xa10 0x750 0x200c 0x104c 0xc7a4
//             python3 tools/fieldxw.py --fn 0x426fd0 0xd4 0xd8 0xdc 0xe0 0xe8 0xec   (the stat block)
//   (enold port names: +0xd8 "level_bonus" = combat level, +0xdc "star_count" = its max, +0xe0 "combat_level_max"
//    = adventurer level — the port's display formula +0xe0 + +0xd8 is the same sum.)
#ifndef OSS_SOTES_LAYOUT_H
#define OSS_SOTES_LAYOUT_H
#include <stdint.h>

typedef struct sotes_layout {
    const char *edition;
    // world = *g_world (wl_world)
    uint16_t world_view;      // camera_view*          enold 0x104c  SE 0x104c
    uint16_t world_chars;     // wl_char*[32]          0x1160        0x1160
    uint16_t world_actors;    // wl_entity*[128]       0x11e0        0x11e0
    uint16_t world_room;      // wl_room* (current)    0x2784        0x3788   (SE grew the bands by 0x1004)
    // room (wl_room)
    uint16_t room_flags;      // {id,state}[1024]      0x0008        0x0008
    uint16_t room_nflags;     // u16                   0x2008        0x2008
    uint16_t room_leader;     // wl_pslot*             0x200c        0x200c
    uint16_t room_slots;      // wl_pslot*[8]          0x4030        0x4030
    // party slot (wl_pslot, operator_new(0xeec) on enold)
    uint16_t ps_in;           // input_mgr*            0x000         0x000
    uint16_t ps_code;         // character code        0x004         0x004
    uint16_t ps_active;       // 1 = in use            0x9c4         0x9e4
    uint16_t ps_ch;           // wl_char*              0x9f4         0xa14
    // character / entity
    uint16_t ent_body;        // wl_body* (x +4, y +8) 0x040         0x040
    uint16_t ent_code;        // behaviour code        0x1d4         0x1d4    (50010 Arche / 50011 Sana / 50012 Stella)
    uint16_t ch_stats;        // wl_party* stat block  0x750         0x760
    uint16_t ch_held;         // input chain / held    0xc7a4        0xc7b4
    // stat block (wl_party): identical up to 0xa4; SE inserted one dword before 0xd4 (fieldxw on 0x426fd0)
    uint16_t st_combat_lv;    // combat level (M)      0xd8          0xdc     ┐ HUD "Lv" = combat + adventurer
    uint16_t st_combat_max;   // max combat level (N)  0xdc          0xe0     │ (SE live, slot "Weathervane Tower":
    uint16_t st_adv_lv;       // adventurer level      0xe0          0xe4     ┘  3 + 12 = Lv15, 5 stars)
    uint16_t st_exp;          // exp                   0xe8          0xec
    uint16_t st_exp_max;      // exp to next           0xec          0xf0
} sotes_layout;

// The running edition's layout (NULL until sym_init identified the build, or for an unknown build).
const sotes_layout *sotes_layout_get(void);

struct lua_State;
void sotes_layout_push_lua(struct lua_State *L);   // mod.game.layout: the table above, read-only by convention

#endif
