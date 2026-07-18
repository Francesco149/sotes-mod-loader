// core/sotes_bindings.h — register the SotES (sotes_en) game-knowledge bindings.
//
// Profile-gated native bindings for Fortune Summoners EN-SE, registered into the
// generic game-binding registry (game_bindings.h) so they surface as mod.game.<id>.
// Kept in its own TU so the core stays game-agnostic; a future profile-DLL mechanism
// (P4 native bridge) can move these out of the core entirely.
#ifndef OSS_SOTES_BINDINGS_H
#define OSS_SOTES_BINDINGS_H

// Register the SotES bindings (roster, coordinates, ...).  Call once at loader init
// when the host exe matches the SotES profile, BEFORE the Lua game table finalizes.
void sotes_bindings_register(void);

#endif
