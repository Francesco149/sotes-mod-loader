// core/sotes_itemsites.h — where each edition's code holds the static item table's address (generated:
// core/gen/sotes_itemsites.c by tools/itemsites.py).  mod.game.items (core/lua/items.lua) rebases those dwords onto
// a larger copy of the table, so mods get item definitions of their own past the retail rows.
#ifndef OSS_SOTES_ITEMSITES_H
#define OSS_SOTES_ITEMSITES_H
#include <stdint.h>

typedef struct {
    const char *edition;
    uint32_t table;         // the static item table
    int row_size;           // 736 enold / 940 SE
    int rows;               // retail rows before the 0-id end row
    int icon_bank;          // the sprite bank of the item icons (32 x 32 cells; frame = u16 row +0x54)
    int pickup_kind;        // pickup_spawn's kind for an item lying on the floor
    int nsites;
    const uint32_t *sites;  // each holds `table`
} ItemSites;

extern const ItemSites g_itemsites[3];      // by SYM_ENOLD / SYM_ENSE_EN / SYM_ENSE_JP

#endif
