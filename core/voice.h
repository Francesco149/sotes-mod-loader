// core/voice.h — built-in Japanese voice restore (EN-SE / JP-SE), applied in the early-boot phase.
//
// Ports OpenSummoners' `ennse_voice` standalone patch into the loader as a core, config-toggled
// feature.  It restores the Japanese dialogue + deluxe combat voices (EN) and the monster SFX (both
// editions) with two signature-gated in-memory patches, applied AFTER SteamStub decrypts .text and
// BEFORE the boot sound-def registrar runs — the exe on disk is never touched.
//
// Boot-time only: the sound defs register ONCE at boot, so there is no runtime toggle.  The launcher
// config key `voice` (oss_loader.cfg) gates it; default OFF (opt-in — it needs the user-supplied
// sotesx_s.dll voice bank beside the exe).  Absent bank / wrong edition / build shift => fail safe.
#ifndef OSS_VOICE_H
#define OSS_VOICE_H

// Arm the voice patch iff `voice=1` in oss_loader.cfg and the host is a supported sotes edition.
// Called ONCE from the loader's early-boot phase (loader_thread, right after config_load).  Spawns a
// short-lived waiter that polls for .text decrypt then applies the patches, and returns immediately
// (so the loader thread never blocks on it).  A no-op when disabled or on an unrecognized host.
void voice_early_init(void);

#endif
