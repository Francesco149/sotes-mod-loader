// core/config.c — the loader config reader + per-mod config values.  See config.h.

#include "config.h"
#include "loader_internal.h"

#include <windows.h>   // GetTickCount — the save debounce
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// ── oss_loader.cfg (the loader's own settings — read-only, never rewritten) ──
#define MAX_KV 32
static struct { char key[48], val[80]; } g_kv[MAX_KV];
static int g_nkv;

// ── oss_mods.cfg (per-mod values, "<modid>.<setting>" — machine-managed, rewritable) ──
#define MAX_MOD_KV 256
static struct { char key[96], val[192]; } g_mod[MAX_MOD_KV];
static int   g_nmod;
static char  g_cfg_dir[MAX_PATH];
static int   g_mod_dirty;         // a value changed since the last write
static DWORD g_mod_dirty_ms;      // GetTickCount at the last change — debounce the write
#define MOD_SAVE_DEBOUNCE_MS 400  // coalesce a burst of set()s (e.g. a slider drag) into one write

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

// Read a flat key=value file into (key,val) rows of the given widths.  Returns the count.
static int read_kv(const char *path, void *rows, int max, size_t stride, size_t ksz, size_t vsz) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int n = 0;
    char line[512];
    while (fgets(line, sizeof line, f) && n < max) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;      // blank / comment
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        char *krow = (char *)rows + (size_t)n * stride;
        _snprintf(krow, ksz, "%s", k);
        _snprintf(krow + ksz, vsz, "%s", v);
        n++;
    }
    fclose(f);
    return n;
}

void config_load(const char *gamedir) {
    _snprintf(g_cfg_dir, MAX_PATH, "%s", gamedir ? gamedir : "");
    char path[MAX_PATH];

    _snprintf(path, MAX_PATH, "%soss_loader.cfg", g_cfg_dir);
    g_nkv = read_kv(path, g_kv, MAX_KV, sizeof g_kv[0], sizeof g_kv[0].key, sizeof g_kv[0].val);
    if (g_nkv) ml_log("[cfg] loaded %d setting(s) from oss_loader.cfg", g_nkv);
    else       ml_log("[cfg] no oss_loader.cfg — using defaults");

    _snprintf(path, MAX_PATH, "%soss_mods.cfg", g_cfg_dir);
    g_nmod = read_kv(path, g_mod, MAX_MOD_KV, sizeof g_mod[0], sizeof g_mod[0].key, sizeof g_mod[0].val);
    if (g_nmod) ml_log("[cfg] loaded %d mod value(s) from oss_mods.cfg", g_nmod);
}

int config_get_int(const char *key, int def) {
    for (int i = 0; i < g_nkv; i++) if (!strcmp(g_kv[i].key, key)) return atoi(g_kv[i].val);
    return def;
}

// A boolean setting: accepts 1/0 AND true/false/on/yes (same tolerance the mod-config `coerce` in
// lua_host.c uses for oss_mods.cfg).  The launcher's "mod zero" editor writes bools as `true`/`false`
// (matching mods), so a plain atoi read (atoi("true")==0) would misread every bool ON as OFF.
int config_get_bool(const char *key, int def) {
    const char *v = config_get_str(key, NULL);
    if (!v) return def;
    if (!strcmp(v, "true") || !strcmp(v, "on") || !strcmp(v, "yes")) return 1;
    if (!strcmp(v, "false") || !strcmp(v, "off") || !strcmp(v, "no")) return 0;
    return atoi(v) != 0;
}

const char *config_get_str(const char *key, const char *def) {
    for (int i = 0; i < g_nkv; i++) if (!strcmp(g_kv[i].key, key)) return g_kv[i].val;
    return def;
}

const char *config_mod_get(const char *nskey) {
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].key, nskey)) return g_mod[i].val;
    return NULL;
}

// Update a value in memory + mark the store dirty (a debounced flush persists it — see config_mod_flush).
// No file write here, so a burst of set()s (a slider drag) costs nothing until the burst settles.
void config_mod_set(const char *nskey, const char *val) {
    const char *cur = NULL;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].key, nskey)) { cur = g_mod[i].val;
        if (!strcmp(cur, val ? val : "")) return;    // unchanged — no dirty, no churn
        _snprintf(g_mod[i].val, sizeof g_mod[0].val, "%s", val ? val : ""); goto dirty; }
    if (g_nmod >= MAX_MOD_KV) { ml_log("[cfg] oss_mods.cfg full — dropping %s", nskey); return; }
    _snprintf(g_mod[g_nmod].key, sizeof g_mod[0].key, "%s", nskey);
    _snprintf(g_mod[g_nmod].val, sizeof g_mod[0].val, "%s", val ? val : "");
    g_nmod++;
dirty:
    g_mod_dirty = 1; g_mod_dirty_ms = GetTickCount();
}

static int write_mods_file(void) {
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%soss_mods.cfg", g_cfg_dir);
    FILE *f = fopen(path, "w");
    if (!f) { ml_log("[cfg] could not write oss_mods.cfg"); return -1; }
    fprintf(f, "# oss_mods.cfg — per-mod config values (machine-managed; edit via the launcher or in-game).\n");
    fprintf(f, "# key = <modid>.<setting>; the settings + defaults are declared in each mod's mod.toml [config].\n");
    for (int i = 0; i < g_nmod; i++) fprintf(f, "%s = %s\n", g_mod[i].key, g_mod[i].val);
    fclose(f);
    return 0;
}

// Persist pending changes: write iff dirty AND (force, or the debounce window has elapsed since the
// last change).  Called cheaply every safepoint (force=0) + once on shutdown (force=1).  0 = nothing
// to do or write ok; -1 = write failed (stays dirty, retried next tick).
int config_mod_flush(int force) {
    if (!g_mod_dirty) return 0;
    if (!force && (GetTickCount() - g_mod_dirty_ms) < MOD_SAVE_DEBOUNCE_MS) return 0;
    int rc = write_mods_file();
    if (rc == 0) g_mod_dirty = 0;
    return rc;
}
