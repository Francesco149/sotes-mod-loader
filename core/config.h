// core/config.h — the loader config + per-mod config values.
//
// TWO tiny key=value files beside the exe (both hand-editable; the launcher writes them):
//   - oss_loader.cfg — the LOADER's own settings (ddraw, ui, keepactive, ...).  Read-only here;
//     hand-commented, so we never rewrite it.
//   - oss_mods.cfg  — per-mod config VALUES, keys namespaced "<modid>.<setting>".  Machine-managed
//     (mod.config.set + the launcher rewrite it freely), so it carries no comments.  The SCHEMA
//     (types/defaults/labels) lives in each mod's `mod.toml [config]`, not here.
#ifndef OSS_CONFIG_H
#define OSS_CONFIG_H

void config_load(const char *gamedir);          // read <gamedir>{oss_loader,oss_mods}.cfg (missing = defaults)
int  config_get_int(const char *key, int def);  // oss_loader.cfg: parsed int, or def if absent/unparseable
const char *config_get_str(const char *key, const char *def);  // oss_loader.cfg: raw string, or def

// Per-mod values (oss_mods.cfg).  `nskey` is the namespaced "<modid>.<setting>".
const char *config_mod_get(const char *nskey);                  // stored value, or NULL if unset
void        config_mod_set(const char *nskey, const char *val); // set/replace in memory + mark dirty (no write)
int         config_mod_flush(int force);                        // debounced write: dirty && (force || idle) -> rewrite

#endif
