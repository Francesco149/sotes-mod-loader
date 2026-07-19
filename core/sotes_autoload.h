// core/sotes_autoload.h — dev harness: auto-load a SotES save so in-game testing/profiling reaches
// actual gameplay instead of sitting at the title.
//
// Ports the sotes_trainer's VERIFIED "menu-drive": inject synthetic confirm records into the game's
// own title + save-picker input polls so the GAME runs its native load chain (robust; no direct
// engine calls, which crash on the scene transition).  SotES-specific; enabled by `autoload=1` in
// oss_loader.cfg (`autoload_slot=N` optional — currently the newest/default save always loads).
#ifndef OSS_SOTES_AUTOLOAD_H
#define OSS_SOTES_AUTOLOAD_H

// Register the drive to ARM on the engine thread at the first safepoint (call from the loader when
// the host is SotES + autoload is configured).  slot < 0 = the picker's default (newest) save.
void sotes_autoload_enable(int slot);

#endif
