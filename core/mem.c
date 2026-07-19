// core/mem.c — the guarded memory service (P1).  See mem.h.
//
// Guards lifted from the proven sotes_trainer (mem_readable/mem_writable): every read
// and write is VirtualQuery-checked first, so a stale/garbage pointer yields nil/false
// instead of an access violation.  Exposed to Lua as `mod.mem.*`.

#include "mem.h"
#include "loader_internal.h"

#include <string.h>

#include "lua.h"
#include "lauxlib.h"

// ── never-fault guards ───────────────────────────────────────────────────────
int mem_readable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    DWORD prot = mbi.Protect & 0xff;
    if (prot == PAGE_NOACCESS || (mbi.Protect & PAGE_GUARD)) return 0;
    uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)p + n <= end;
}
int mem_writable(const void *p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!p || VirtualQuery(p, &mbi, sizeof mbi) == 0) return 0;
    if (mbi.State != MEM_COMMIT) return 0;
    DWORD w = mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY |
                             PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY);
    if (!w || (mbi.Protect & PAGE_GUARD)) return 0;
    uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    return (uintptr_t)p + n <= end;
}
int mem_rd32(const void *p, uint32_t *out) {
    if (!mem_readable(p, 4)) return 0;
    *out = *(volatile const uint32_t *)p;
    return 1;
}

// ── module base / ASLR reloc ─────────────────────────────────────────────────
static uintptr_t g_main_base, g_main_size, g_image_base, g_delta;

void mem_init(void) {
    g_main_base = (uintptr_t)GetModuleHandleA(NULL);
    g_image_base = g_main_base;
    g_main_size  = 0;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_main_base;
    if (mem_readable(dos, sizeof *dos) && dos->e_magic == IMAGE_DOS_SIGNATURE) {
        IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(g_main_base + dos->e_lfanew);
        if (mem_readable(nt, sizeof *nt) && nt->Signature == IMAGE_NT_SIGNATURE) {
            g_image_base = nt->OptionalHeader.ImageBase;
            g_main_size  = nt->OptionalHeader.SizeOfImage;
        }
    }
    g_delta = g_main_base - g_image_base;
    ml_log("[mem] host base=%p imagebase=%p delta=%p size=%p",
           (void *)g_main_base, (void *)g_image_base, (void *)g_delta, (void *)g_main_size);
}
uintptr_t mem_main_base(void)       { return g_main_base; }
uintptr_t mem_reloc(uintptr_t va)   { return va + g_delta; }

// ── typed Lua reads/writes (type tag carried as an upvalue) ──────────────────
enum { T_U8, T_U16, T_U32, T_I8, T_I16, T_I32, T_F32, T_F64, T_PTR };
static size_t tsize(int t) {
    switch (t) { case T_U8: case T_I8: return 1; case T_U16: case T_I16: return 2;
                 case T_F64: return 8; default: return 4; }
}

static int l_read(lua_State *L) {
    int t = (int)lua_tointeger(L, lua_upvalueindex(1));
    uintptr_t a = (uintptr_t)lua_tonumber(L, 1);
    if (!mem_readable((void *)a, tsize(t))) { lua_pushnil(L); return 1; }
    volatile void *p = (volatile void *)a;
    double v;
    switch (t) {
        case T_U8:              v = *(volatile uint8_t  *)p; break;
        case T_U16:             v = *(volatile uint16_t *)p; break;
        case T_U32: case T_PTR: v = (double)*(volatile uint32_t *)p; break;
        case T_I8:              v = *(volatile int8_t   *)p; break;
        case T_I16:             v = *(volatile int16_t  *)p; break;
        case T_I32:             v = *(volatile int32_t  *)p; break;
        case T_F32:             v = *(volatile float    *)p; break;
        case T_F64:             v = *(volatile double   *)p; break;
        default: lua_pushnil(L); return 1;
    }
    lua_pushnumber(L, v);
    return 1;
}

static int l_write(lua_State *L) {
    int t = (int)lua_tointeger(L, lua_upvalueindex(1));
    uintptr_t a = (uintptr_t)lua_tonumber(L, 1);
    double v = lua_tonumber(L, 2);
    if (!mem_writable((void *)a, tsize(t))) { lua_pushboolean(L, 0); return 1; }
    volatile void *p = (volatile void *)a;
    long long iv = (long long)v;
    switch (t) {
        case T_U8:  *(volatile uint8_t  *)p = (uint8_t)iv;  break;
        case T_U16: *(volatile uint16_t *)p = (uint16_t)iv; break;
        case T_U32: case T_PTR: *(volatile uint32_t *)p = (uint32_t)iv; break;
        case T_I8:  *(volatile int8_t   *)p = (int8_t)iv;   break;
        case T_I16: *(volatile int16_t  *)p = (int16_t)iv;  break;
        case T_I32: *(volatile int32_t  *)p = (int32_t)iv;  break;
        case T_F32: *(volatile float    *)p = (float)v;     break;
        case T_F64: *(volatile double   *)p = v;            break;
        default: lua_pushboolean(L, 0); return 1;
    }
    lua_pushboolean(L, 1);
    return 1;
}

static int l_read_bytes(lua_State *L) {
    uintptr_t a = (uintptr_t)lua_tonumber(L, 1);
    int n = (int)lua_tointeger(L, 2);
    if (n <= 0 || n > (1 << 20) || !mem_readable((void *)a, (size_t)n)) { lua_pushnil(L); return 1; }
    lua_pushlstring(L, (const char *)a, (size_t)n);
    return 1;
}
static int l_read_cstr(lua_State *L) {
    uintptr_t a = (uintptr_t)lua_tonumber(L, 1);
    int max = lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : 260;
    if (max <= 0 || max > (1 << 16)) max = 260;
    luaL_Buffer b; luaL_buffinit(L, &b);
    int i;
    for (i = 0; i < max; i++) {
        if (!mem_readable((void *)(a + i), 1)) { if (i == 0) { lua_pushnil(L); return 1; } break; }
        char c = *(volatile const char *)(a + i);
        if (!c) break;
        luaL_addchar(&b, c);
    }
    luaL_pushresult(&b);
    return 1;
}
static int l_write_bytes(lua_State *L) {
    uintptr_t a = (uintptr_t)lua_tonumber(L, 1);
    size_t n = 0; const char *s = luaL_checklstring(L, 2, &n);
    if (n == 0 || !mem_writable((void *)a, n)) { lua_pushboolean(L, 0); return 1; }
    memcpy((void *)a, s, n);
    lua_pushboolean(L, 1);
    return 1;
}

static int l_readable(lua_State *L) {
    uintptr_t a = (uintptr_t)lua_tonumber(L, 1);
    int n = lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : 1;
    lua_pushboolean(L, mem_readable((void *)a, n > 0 ? (size_t)n : 1));
    return 1;
}
static int l_writable(lua_State *L) {
    uintptr_t a = (uintptr_t)lua_tonumber(L, 1);
    int n = lua_isnumber(L, 2) ? (int)lua_tointeger(L, 2) : 1;
    lua_pushboolean(L, mem_writable((void *)a, n > 0 ? (size_t)n : 1));
    return 1;
}
static int l_module(lua_State *L) {
    const char *name = luaL_optstring(L, 1, NULL);
    HMODULE h = GetModuleHandleA(name);
    if (!h) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, (double)(uintptr_t)h);
    return 1;
}
static int l_reloc(lua_State *L) {
    uintptr_t va = (uintptr_t)lua_tonumber(L, 1);
    lua_pushnumber(L, (double)mem_reloc(va));
    return 1;
}
static int l_base(lua_State *L) { lua_pushnumber(L, (double)g_main_base); return 1; }

// AOB scan over the running exe's image: "48 8B ?? C3" (?? / ? = wildcard).
static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
// C core (shared by Lua l_scan + the native ABI mem_scan): returns the first match VA or 0.
uintptr_t mem_scan_aob(const char *s) {
    if (!s) return 0;
    uint8_t pat[256]; int mask[256], n = 0;
    while (*s && n < 256) {
        while (*s == ' ') s++;
        if (!*s) break;
        if (*s == '?') { mask[n] = 0; pat[n] = 0; n++; s++; if (*s == '?') s++; }
        else {
            int hi = hexv(s[0]), lo = (s[1] ? hexv(s[1]) : -1);
            if (hi < 0 || lo < 0) break;
            pat[n] = (uint8_t)((hi << 4) | lo); mask[n] = 1; n++; s += 2;
        }
    }
    if (n == 0 || !g_main_size) return 0;
    uintptr_t base = g_main_base, end = base + g_main_size;
    uint8_t *addr = (uint8_t *)base;
    MEMORY_BASIC_INFORMATION mbi;
    while ((uintptr_t)addr < end && VirtualQuery(addr, &mbi, sizeof mbi)) {
        uint8_t *next = (uint8_t *)mbi.BaseAddress + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (mbi.Protect & 0xff) != PAGE_NOACCESS) {
            uint8_t *p = (uint8_t *)mbi.BaseAddress;
            if ((uintptr_t)p < base) p = (uint8_t *)base;
            uint8_t *e = next - n;
            for (; p <= e && (uintptr_t)p < end; ++p) {
                int i; for (i = 0; i < n; i++) if (mask[i] && p[i] != pat[i]) break;
                if (i == n) return (uintptr_t)p;
            }
        }
        if (next <= addr) break;
        addr = next;
    }
    return 0;
}
static int l_scan(lua_State *L) {
    uintptr_t hit = mem_scan_aob(luaL_checkstring(L, 1));
    if (hit) lua_pushnumber(L, (double)hit); else lua_pushnil(L);
    return 1;
}

// ── build + share the `mem` table ────────────────────────────────────────────
static int g_mem_ref = LUA_NOREF;

void mem_install_lua(lua_State *L) {
    lua_newtable(L);
#define TYPED(name, fn, tag) do { lua_pushinteger(L, tag); lua_pushcclosure(L, fn, 1); lua_setfield(L, -2, name); } while (0)
    TYPED("read_u8",  l_read, T_U8);  TYPED("read_u16", l_read, T_U16); TYPED("read_u32", l_read, T_U32);
    TYPED("read_i8",  l_read, T_I8);  TYPED("read_i16", l_read, T_I16); TYPED("read_i32", l_read, T_I32);
    TYPED("read_f32", l_read, T_F32); TYPED("read_f64", l_read, T_F64); TYPED("read_ptr", l_read, T_PTR);
    TYPED("write_u8",  l_write, T_U8);  TYPED("write_u16", l_write, T_U16); TYPED("write_u32", l_write, T_U32);
    TYPED("write_i8",  l_write, T_I8);  TYPED("write_i16", l_write, T_I16); TYPED("write_i32", l_write, T_I32);
    TYPED("write_f32", l_write, T_F32); TYPED("write_f64", l_write, T_F64); TYPED("write_ptr", l_write, T_PTR);
#undef TYPED
#define FN(name, fn) do { lua_pushcfunction(L, fn); lua_setfield(L, -2, name); } while (0)
    FN("read_bytes", l_read_bytes); FN("read_cstr", l_read_cstr); FN("write_bytes", l_write_bytes);
    FN("readable", l_readable); FN("writable", l_writable);
    FN("scan", l_scan); FN("module", l_module); FN("reloc", l_reloc); FN("base", l_base);
#undef FN
    g_mem_ref = luaL_ref(L, LUA_REGISTRYINDEX);   // pops the table, keeps it alive
}
void mem_push_lua(lua_State *L) { lua_rawgeti(L, LUA_REGISTRYINDEX, g_mem_ref); }
