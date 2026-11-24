// core/sotes_charcodes.h — the character codes the running edition's char_init accepts (generated table:
// core/gen/sotes_charcodes.c by tools/charcodes.py, a static walk of char_init's own code dispatch).  A code
// outside it reaches retail's default arm: "Unknown Object Type Character." + exit — mod.game.actors.spawn
// refuses those.
#ifndef OSS_SOTES_CHARCODES_H
#define OSS_SOTES_CHARCODES_H
#include <stdint.h>

typedef struct { int32_t lo, hi; } charcode_iv;

// edition = SYM_ENOLD / SYM_ENSE_EN / SYM_ENSE_JP; NULL + *n = 0 for anything else
const charcode_iv *charcodes_get(int edition, int *n);

#endif
