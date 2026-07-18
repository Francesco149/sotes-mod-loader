// core/config.h — the loader config (oss_loader.cfg, beside the exe).
//
// A tiny key=value file the loader reads at boot.  The launcher (P8) writes it; it's
// hand-editable too.  Kept dead-simple (no TOML dep) since it's read early in C.
// Keys so far:  skip_launcher=0|1  (auto-dismiss the game's #32770 launcher; OFF by default).
#ifndef OSS_CONFIG_H
#define OSS_CONFIG_H

void config_load(const char *gamedir);         // read <gamedir>oss_loader.cfg (missing = defaults)
int  config_get_int(const char *key, int def); // parsed value, or def if absent/unparseable

#endif
