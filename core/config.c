// core/config.c — the loader config reader.  See config.h.

#include "config.h"
#include "loader_internal.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#define MAX_KV 32
static struct { char key[48], val[80]; } g_kv[MAX_KV];
static int g_nkv;

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

void config_load(const char *gamedir) {
    g_nkv = 0;
    char path[MAX_PATH];
    _snprintf(path, MAX_PATH, "%soss_loader.cfg", gamedir);
    FILE *f = fopen(path, "r");
    if (!f) { ml_log("[cfg] no oss_loader.cfg — using defaults"); return; }
    char line[256];
    while (fgets(line, sizeof line, f) && g_nkv < MAX_KV) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;      // blank / comment
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(s), *v = trim(eq + 1);
        _snprintf(g_kv[g_nkv].key, sizeof g_kv[0].key, "%s", k);
        _snprintf(g_kv[g_nkv].val, sizeof g_kv[0].val, "%s", v);
        g_nkv++;
    }
    fclose(f);
    ml_log("[cfg] loaded %d setting(s) from oss_loader.cfg", g_nkv);
}

int config_get_int(const char *key, int def) {
    for (int i = 0; i < g_nkv; i++) if (!strcmp(g_kv[i].key, key)) return atoi(g_kv[i].val);
    return def;
}
