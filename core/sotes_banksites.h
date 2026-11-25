// core/sotes_banksites.h — where each edition's code indexes the sprite-bank tables with a runtime index (generated:
// core/gen/sotes_banksites.c by tools/banksites.py).  mod.game.sprites (core/lua/sprites.lua) rebases those disp32
// fields onto larger tables of its own, so mods get sprite banks past the retail arrays' end.
#ifndef OSS_SOTES_BANKSITES_H
#define OSS_SOTES_BANKSITES_H
#include <stdint.h>

typedef struct { uint32_t va, expect; uint8_t kind; } BankSite;   // kind 0 = slots, 1 = descriptors
typedef struct {
    const char *edition;
    uint32_t sys, slots_off, descs_off;     // g_sys + the two arrays inside it
    int count;                              // entries in each
    uint32_t restore_base, restore_count;   // the display-restore loop's two imm32 fields (table, count)
    int nsites;
    const BankSite *sites;
} BankSites;

extern const BankSites g_banksites[3];      // by SYM_ENOLD / SYM_ENSE_EN / SYM_ENSE_JP

#endif
