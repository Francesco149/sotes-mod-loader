// core/resov.c — the RESOURCE OVERLAY (`mod.res`): mods add / replace the game's PE resources in memory.
//
// SotES reads its map data (load_room → map_res_load → FindResourceA(map_module, id, "DATA")) and its sprite
// sheets (the bitmap session 0x5b7800: FindResourceA(sotesd, id, "DATA")) through the four kernel32 resource
// calls the EXE imports.  The overlay patches those four slots of the EXE's import table (lazily, on the first
// mod.res.set — a game no mod touches is never patched):
//
//   FindResourceA(hmod, name, type)  → an overlay entry for (type, name[, hmod]) wins (a fake HRSRC = the entry);
//                                      everything else → the real call.
//   LoadResource(hmod, hrsrc)        → overlay entry → its bytes (an HGLOBAL is just the data pointer on Win32).
//   LockResource(hglobal)            → returns its argument for overlay data (and for real resources, as Win32).
//   SizeofResource(hmod, hrsrc)      → overlay entry → its length.
//
// So a mod can REPLACE a stock resource (e.g. a room's map with an extra door) or ADD one under a new id (a new
// room's map, a new sprite sheet) — the game's own loaders then parse it as theirs.  Nothing is written to disk;
// the bytes a mod builds from the user's own game data stay in memory.
//
// Entries are immutable once published (set = a new entry, the old one retired but never freed: a pointer the
// game took from LoadResource stays valid).  Lookups run on whatever thread the game loads on; the table is a
// fixed array of entry pointers swapped atomically.
//
// Lua (every mod's `mod.res`):
//   mod.res.read(type, id[, hmod]) -> string | nil   the REAL resource (bypasses the overlay); hmod default = exe
//   mod.res.set(type, id, data[, hmod])  -> true     add / replace; hmod nil/0 = any module
//   mod.res.clear(type, id[, hmod])      -> bool     drop an overlay entry
//   mod.res.list() -> {{type, id, hmod, size}, ...}
//   mod.res.hits(type, id[, hmod]) -> n              how many times the game found it (FindResourceA)
// `type` / `id`: a number (MAKEINTRESOURCE) or a string ("DATA").
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "lua.h"
#include "lauxlib.h"
#include "loader_internal.h"
#include "resov.h"

#define RES_MAX   256
#define RES_MAGIC 0x564f5352u   // 'RSOV'

typedef struct res_entry {
    uint32_t magic;
    char     type[32];          // "#n" for an integer type, else the name (case-insensitive compare)
    char     name[32];          // same for the id
    HMODULE  hmod;              // 0 = any
    const uint8_t *data;
    DWORD    size;
    volatile LONG hits;
} res_entry;

static res_entry *volatile g_res[RES_MAX];
static volatile LONG g_patched;

typedef HRSRC   (WINAPI *FindResourceA_t)(HMODULE, LPCSTR, LPCSTR);
typedef HGLOBAL (WINAPI *LoadResource_t)(HMODULE, HRSRC);
typedef DWORD   (WINAPI *SizeofResource_t)(HMODULE, HRSRC);
typedef LPVOID  (WINAPI *LockResource_t)(HGLOBAL);
static FindResourceA_t  r_Find;
static LoadResource_t   r_Load;
static SizeofResource_t r_Size;
static LockResource_t   r_Lock;

static void key_of(LPCSTR s, char out[32]) {
    if (IS_INTRESOURCE(s)) { wsprintfA(out, "#%u", (unsigned)(uintptr_t)s); return; }
    lstrcpynA(out, s, 32);
    if (out[0] == '#') return;                       // "#123" = the integer form, as Win32 accepts
}
static int key_eq(const char *a, const char *b) { return lstrcmpiA(a, b) == 0; }

static res_entry *find_entry(HMODULE hmod, const char *type, const char *name, int any_slot_only) {
    for (int i = 0; i < RES_MAX; i++) {
        res_entry *e = g_res[i];
        if (!e || !key_eq(e->type, type) || !key_eq(e->name, name)) continue;
        if (any_slot_only ? e->hmod == hmod : (!e->hmod || e->hmod == hmod)) return e;
    }
    return NULL;
}
static int is_entry(const void *p) {
    if (!p) return 0;
    for (int i = 0; i < RES_MAX; i++) if (g_res[i] == p) return 1;
    // a retired entry (replaced / cleared) still answers: the magic stays, the memory is never freed
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return 0;
    return ((const res_entry *)p)->magic == RES_MAGIC;
}

// ── the four detours ─────────────────────────────────────────────────────────
static HRSRC WINAPI d_Find(HMODULE hmod, LPCSTR name, LPCSTR type) {
    char t[32], n[32];
    key_of(type, t); key_of(name, n);
    res_entry *e = find_entry(hmod, t, n, 0);
    if (e) { InterlockedIncrement(&e->hits); return (HRSRC)e; }
    return r_Find(hmod, name, type);
}
static HGLOBAL WINAPI d_Load(HMODULE hmod, HRSRC h) {
    if (is_entry(h)) return (HGLOBAL)((res_entry *)h)->data;
    return r_Load(hmod, h);
}
static DWORD WINAPI d_Size(HMODULE hmod, HRSRC h) {
    if (is_entry(h)) return ((res_entry *)h)->size;
    return r_Size(hmod, h);
}
static LPVOID WINAPI d_Lock(HGLOBAL g) { return r_Lock(g); }   // Win32: the pointer itself either way

// ── import-table patch (the EXE's own IAT, by imported name) ─────────────────
static int iat_patch(HMODULE m, const char *dll, const char *fn, void *repl, void **orig) {
    uint8_t *base = (uint8_t *)m;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + ((IMAGE_DOS_HEADER *)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dd.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dd.VirtualAddress); d->Name; d++) {
        if (lstrcmpiA((const char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *slots = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, slots++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, fn)) continue;
            DWORD old;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return 0;
            *orig = (void *)slots->u1.Function;
            slots->u1.Function = (uintptr_t)repl;
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

static int resov_arm(void) {
    if (g_patched) return g_patched > 0;
    HMODULE exe = GetModuleHandleA(NULL), k32 = GetModuleHandleA("kernel32.dll");
    // the real functions first (a slot we fail to patch still has its import; the detours need all four)
    r_Find = (FindResourceA_t)(void *)GetProcAddress(k32, "FindResourceA");
    r_Load = (LoadResource_t)(void *)GetProcAddress(k32, "LoadResource");
    r_Size = (SizeofResource_t)(void *)GetProcAddress(k32, "SizeofResource");
    r_Lock = (LockResource_t)(void *)GetProcAddress(k32, "LockResource");
    void *o;
    int n = iat_patch(exe, "KERNEL32.dll", "FindResourceA", (void *)d_Find, &o)
          + iat_patch(exe, "KERNEL32.dll", "LoadResource", (void *)d_Load, &o)
          + iat_patch(exe, "KERNEL32.dll", "SizeofResource", (void *)d_Size, &o)
          + iat_patch(exe, "KERNEL32.dll", "LockResource", (void *)d_Lock, &o);
    ml_log("[res] import table patched: %d/4 resource calls", n);
    g_patched = n >= 3 ? 1 : -1;       // LockResource is a pass-through: optional
    return g_patched > 0;
}

// ── C API ────────────────────────────────────────────────────────────────────
int resov_set(const char *type, const char *name, HMODULE hmod, const void *data, size_t size) {
    if (!resov_arm()) return 0;
    res_entry *e = (res_entry *)VirtualAlloc(NULL, sizeof *e + size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!e) return 0;
    e->magic = RES_MAGIC;
    lstrcpynA(e->type, type, 32); lstrcpynA(e->name, name, 32);
    e->hmod = hmod;
    memcpy((uint8_t *)(e + 1), data, size);
    e->data = (const uint8_t *)(e + 1); e->size = (DWORD)size;
    for (int i = 0; i < RES_MAX; i++) {             // replace in place (same key) …
        res_entry *x = g_res[i];
        if (x && key_eq(x->type, e->type) && key_eq(x->name, e->name) && x->hmod == hmod) {
            InterlockedExchangePointer((PVOID volatile *)&g_res[i], e);   // the old one: retired, never freed
            return 1;
        }
    }
    for (int i = 0; i < RES_MAX; i++)               // … or a free slot
        if (!g_res[i] && !InterlockedCompareExchangePointer((PVOID volatile *)&g_res[i], e, NULL)) return 1;
    ml_log("[res] overlay full (%d)", RES_MAX);
    return 0;
}
int resov_clear(const char *type, const char *name, HMODULE hmod) {
    for (int i = 0; i < RES_MAX; i++) {
        res_entry *x = g_res[i];
        if (x && key_eq(x->type, type) && key_eq(x->name, name) && x->hmod == hmod) {
            InterlockedExchangePointer((PVOID volatile *)&g_res[i], NULL);
            return 1;
        }
    }
    return 0;
}

// FindResourceA as the GAME sees it (overlay first) — for the loader's own "does this resource exist" checks.
HRSRC resov_find(HMODULE hmod, LPCSTR name, LPCSTR type) {
    if (g_patched > 0) return d_Find(hmod, name, type);
    return FindResourceA(hmod, name, type);
}

// ── Lua ──────────────────────────────────────────────────────────────────────
static void lkey(lua_State *L, int idx, char out[32]) {
    if (lua_type(L, idx) == LUA_TNUMBER) wsprintfA(out, "#%u", (unsigned)lua_tointeger(L, idx));
    else lstrcpynA(out, luaL_checkstring(L, idx), 32);
}
static LPCSTR lres(lua_State *L, int idx) {       // a Win32 name: MAKEINTRESOURCE or the string
    if (lua_type(L, idx) == LUA_TNUMBER) return MAKEINTRESOURCEA((WORD)lua_tointeger(L, idx));
    return luaL_checkstring(L, idx);
}
static HMODULE lmod(lua_State *L, int idx, HMODULE dflt) {
    if (lua_isnoneornil(L, idx)) return dflt;
    return (HMODULE)(uintptr_t)luaL_checknumber(L, idx);
}
static int l_read(lua_State *L) {
    HMODULE m = lmod(L, 3, GetModuleHandleA(NULL));
    FindResourceA_t f = r_Find ? r_Find : FindResourceA;
    HRSRC h = f(m, lres(L, 2), lres(L, 1));
    if (!h) { lua_pushnil(L); return 1; }
    HGLOBAL g = LoadResource(m, h);
    DWORD n = SizeofResource(m, h);
    const void *p = g ? LockResource(g) : NULL;
    if (!p) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char *)p, n);
    return 1;
}
static int l_set(lua_State *L) {
    char t[32], n[32]; size_t len;
    lkey(L, 1, t); lkey(L, 2, n);
    const char *d = luaL_checklstring(L, 3, &len);
    lua_pushboolean(L, resov_set(t, n, lmod(L, 4, NULL), d, len));
    return 1;
}
static int l_clear(lua_State *L) {
    char t[32], n[32];
    lkey(L, 1, t); lkey(L, 2, n);
    lua_pushboolean(L, resov_clear(t, n, lmod(L, 3, NULL)));
    return 1;
}
static int l_list(lua_State *L) {
    lua_newtable(L);
    int k = 0;
    for (int i = 0; i < RES_MAX; i++) {
        res_entry *e = g_res[i];
        if (!e) continue;
        lua_newtable(L);
        lua_pushstring(L, e->type);                 lua_setfield(L, -2, "type");
        lua_pushstring(L, e->name);                 lua_setfield(L, -2, "id");
        lua_pushnumber(L, (double)(uintptr_t)e->hmod); lua_setfield(L, -2, "hmod");
        lua_pushinteger(L, e->size);                lua_setfield(L, -2, "size");
        lua_pushinteger(L, e->hits);                lua_setfield(L, -2, "hits");
        lua_rawseti(L, -2, ++k);
    }
    return 1;
}
static int l_hits(lua_State *L) {
    char t[32], n[32];
    lkey(L, 1, t); lkey(L, 2, n);
    res_entry *e = find_entry(lmod(L, 3, NULL), t, n, 1);
    lua_pushinteger(L, e ? e->hits : 0);
    return 1;
}
void resov_push_lua(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_read);  lua_setfield(L, -2, "read");
    lua_pushcfunction(L, l_set);   lua_setfield(L, -2, "set");
    lua_pushcfunction(L, l_clear); lua_setfield(L, -2, "clear");
    lua_pushcfunction(L, l_list);  lua_setfield(L, -2, "list");
    lua_pushcfunction(L, l_hits);  lua_setfield(L, -2, "hits");
}
