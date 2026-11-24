// core/syms.c — the edition layer.  See syms.h.
//
// Detection is two-stage so it works on the SteamStub-packed retail exe:
//   1. header guess — PE TimeDateStamp + .text VirtualSize (identical packed / unpacked; Steamless only
//      drops the .bind section), readable at DllMain time;
//   2. confirm — once .text is decrypted, the anchor functions' code hashes must match the guessed
//      edition.  Then every function symbol is hash-verified; a mismatch disables just that symbol
//      (sym_va → 0), so a future Steam patch degrades to "feature unavailable", never to hooking garbage.
#include "syms.h"
#include "mem.h"
#include "loader_internal.h"

#include <windows.h>
#include <string.h>
#include <stdlib.h>

static int g_ed = -1;
static int g_guess = -2;
static uint8_t *g_ok;          // per-row: 1 verified in this edition
static int g_nok, g_nbad;
static volatile LONG g_inited;

static uint32_t fnv1a(const uint8_t *p, size_t n) {
    uint32_t h = 0x811C9DC5u;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x01000193u; }
    return h;
}

static const IMAGE_NT_HEADERS32 *nt_headers(void) {
    const uint8_t *base = (const uint8_t *)GetModuleHandleA(NULL);
    if (!base) return NULL;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return NULL;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE ? nt : NULL;
}

int sym_guess_edition(void) {
    if (g_guess != -2) return g_guess;
    g_guess = -1;
    const IMAGE_NT_HEADERS32 *nt = nt_headers();
    if (!nt) return g_guess;
    uint32_t text_vsize = 0;
    const IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++)
        if (!memcmp(s[i].Name, ".text", 6)) text_vsize = s[i].Misc.VirtualSize;
    for (int e = 0; e < SYM_NED; e++)
        if (SYM_EDITIONS[e].timestamp == nt->FileHeader.TimeDateStamp && SYM_EDITIONS[e].text_vsize == text_vsize)
            g_guess = e;
    return g_guess;
}

static int row_hash_ok(const sym_row *r, int e) {
    if (!r->va[e]) return 0;
    if (r->kind != SYM_FUNC) return 1;            // data: mapped by xref votes, no code to hash
    size_t n = r->size[e] < 16 ? r->size[e] : 16;
    if (!n) return 0;
    const uint8_t *p = (const uint8_t *)mem_reloc(r->va[e]);
    if (!mem_readable(p, n)) return 0;
    return fnv1a(p, n) == r->hash[e];
}

static int find_row(const char *name) {
    int lo = 0, hi = SYM_NROWS - 1;                // SYM_ROWS is sorted by name (symgen)
    while (lo <= hi) {
        int mid = (lo + hi) / 2, c = strcmp(name, SYM_ROWS[mid].name);
        if (!c) return mid;
        if (c < 0) hi = mid - 1; else lo = mid + 1;
    }
    return -1;
}

int sym_init(void) {
    if (g_inited) return g_ed;
    int e = sym_guess_edition();
    if (e < 0) { ml_log("[sym] unknown build (header matches no known edition) — engine symbols disabled"); g_inited = 1; return -1; }
    static const char *ANCHORS[] = { "flip", "rand_draw", "camera_follow_step" };
    for (size_t i = 0; i < sizeof ANCHORS / sizeof *ANCHORS; i++) {
        int r = find_row(ANCHORS[i]);
        if (r < 0 || !row_hash_ok(&SYM_ROWS[r], e)) {
            ml_log("[sym] header says %s but anchor '%s' code differs — engine symbols disabled", SYM_EDITIONS[e].tag, ANCHORS[i]);
            g_inited = 1; return -1;
        }
    }
    g_ok = (uint8_t *)calloc((size_t)SYM_NROWS, 1);
    for (int i = 0; i < SYM_NROWS; i++) {
        if (!SYM_ROWS[i].va[e]) continue;
        if (row_hash_ok(&SYM_ROWS[i], e)) { g_ok[i] = 1; g_nok++; }
        else { g_nbad++; ml_log("[sym] %s @%08x: code hash mismatch — disabled", SYM_ROWS[i].name, SYM_ROWS[i].va[e]); }
    }
    g_ed = e;
    g_inited = 1;
    ml_log("[sym] edition %s confirmed — %d symbols verified, %d disabled", SYM_EDITIONS[e].tag, g_nok, g_nbad);
    return e;
}

int sym_edition(void) { return g_ed; }
const char *sym_edition_tag(void) { return g_ed >= 0 ? SYM_EDITIONS[g_ed].tag : "unknown"; }

uintptr_t sym_va(const char *name) {
    if (g_ed < 0 || !name) return 0;
    int r = find_row(name);
    return (r >= 0 && g_ok[r]) ? mem_reloc(SYM_ROWS[r].va[g_ed]) : 0;
}
uintptr_t sym_va_req(const char *name) {
    uintptr_t v = sym_va(name);
    if (!v) ml_log("[sym] required symbol '%s' unavailable on %s", name, sym_edition_tag());
    return v;
}
const char *sym_name_at(uintptr_t va) {
    if (g_ed < 0) return NULL;
    for (int i = 0; i < SYM_NROWS; i++)
        if (g_ok[i] && mem_reloc(SYM_ROWS[i].va[g_ed]) == va) return SYM_ROWS[i].name;
    return NULL;
}
int sym_count(int *nverified, int *nbad) {
    if (nverified) *nverified = g_nok;
    if (nbad) *nbad = g_nbad;
    return SYM_NROWS;
}

// ── Lua: mod.sym ─────────────────────────────────────────────────────────────
#include "lua.h"
#include "lauxlib.h"
static int l_sym_va(lua_State *L) {
    uintptr_t v = sym_va(luaL_checkstring(L, 1));
    if (v) lua_pushnumber(L, (lua_Number)v); else lua_pushnil(L);
    return 1;
}
static int l_sym_name_at(lua_State *L) {
    const char *n = sym_name_at((uintptr_t)luaL_checknumber(L, 1));
    if (n) lua_pushstring(L, n); else lua_pushnil(L);
    return 1;
}
// mod.sym.find(pattern) -> { name = va, ... } for verified symbols whose name matches a Lua pattern
static int l_sym_find(lua_State *L) {
    const char *pat = luaL_checkstring(L, 1);
    lua_newtable(L);
    if (g_ed < 0) return 1;
    for (int i = 0; i < SYM_NROWS; i++) {
        if (!g_ok[i]) continue;
        lua_getglobal(L, "string"); lua_getfield(L, -1, "find"); lua_remove(L, -2);
        lua_pushstring(L, SYM_ROWS[i].name); lua_pushstring(L, pat);
        lua_call(L, 2, 1);
        int hit = !lua_isnil(L, -1); lua_pop(L, 1);
        if (hit) { lua_pushnumber(L, (lua_Number)mem_reloc(SYM_ROWS[i].va[g_ed])); lua_setfield(L, -2, SYM_ROWS[i].name); }
    }
    return 1;
}
// mod.sym.info(name) -> { va=, kind=, size=, conf= } | nil
static int l_sym_info(lua_State *L) {
    int r = find_row(luaL_checkstring(L, 1));
    if (r < 0 || g_ed < 0 || !g_ok[r]) { lua_pushnil(L); return 1; }
    const sym_row *s = &SYM_ROWS[r];
    lua_newtable(L);
    lua_pushnumber(L, (lua_Number)mem_reloc(s->va[g_ed])); lua_setfield(L, -2, "va");
    lua_pushstring(L, s->kind == SYM_FUNC ? "func" : "data"); lua_setfield(L, -2, "kind");
    lua_pushinteger(L, s->size[g_ed]); lua_setfield(L, -2, "size");
    lua_pushinteger(L, s->conf[g_ed]); lua_setfield(L, -2, "conf");
    return 1;
}
void sym_push_lua(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_sym_va);      lua_setfield(L, -2, "va");
    lua_pushcfunction(L, l_sym_name_at); lua_setfield(L, -2, "name_at");
    lua_pushcfunction(L, l_sym_find);    lua_setfield(L, -2, "find");
    lua_pushcfunction(L, l_sym_info);    lua_setfield(L, -2, "info");
    lua_pushstring(L, sym_edition_tag()); lua_setfield(L, -2, "edition");
}
