// core/devserver.c — the dev / agent control server (opt-in).  See devserver.h.
//
// One TCP listener on 127.0.0.1:<port>, one client at a time, line protocol:
//   request  = "<cmd> [<arg>]\n"
//   response = "ok <payload>\n" | "err <message>\n"     (payload / message: \\ and \n escaped)
// commands:
//   ping            answered on the server thread (no Lua): "frame=<n> armed=<0|1> edition=<tag>"
//   lua <hex>       hex-encoded Lua chunk, run on the ENGINE thread at the next safepoint in a
//                   persistent dev env (globals survive between calls); returns dev.repr(results...)
// Lua never runs on this thread (loader invariant #1): the chunk is queued to the executor and this
// thread waits on an event (timeout → "err timeout", the job still runs later and its result is dropped).
#include <winsock2.h>   // before windows.h (pulled in by the headers below)
#include <ws2tcpip.h>
#include "devserver.h"
#include "executor.h"
#include "config.h"
#include "syms.h"
#include "lua_host.h"
#include "loader_internal.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"

typedef struct {
    char   *code;          // owned by the job until done
    size_t  ncode;
    char   *out;           // malloc'd result (escaped later)
    int     ok;
    HANDLE  done;
    volatile LONG abandoned;   // the server timed out; the job frees itself
} dev_job;

static int g_env_ref = LUA_NOREF;
static int g_port;

static void job_free(dev_job *j) {
    free(j->code); free(j->out);
    CloseHandle(j->done);
    free(j);
}

// ENGINE THREAD: run the chunk in the persistent dev env.
static void run_job(void *user) {
    dev_job *j = (dev_job *)user;
    lua_State *L = lh_state();
    if (!L) { j->out = _strdup("no Lua state"); j->ok = 0; goto fin; }
    if (g_env_ref == LUA_NOREF) {
        lh_push_env(L, "dev", "");                 // env (mod = the "dev" mod table)
        if (luaL_loadstring(L, lh_dev_lua()) == 0) {   // the dev helpers (repr, keys, ...), run in that env
            lua_pushvalue(L, -2); lua_setfenv(L, -2);
            if (lua_pcall(L, 0, 0, 0) != 0) { ml_log("[dev] helper chunk failed: %s", lua_tostring(L, -1)); lua_pop(L, 1); }
        } else { ml_log("[dev] helper chunk load failed: %s", lua_tostring(L, -1)); lua_pop(L, 1); }
        g_env_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    }
    int top = lua_gettop(L);
    // "return <expr>" first (REPL convenience), else the chunk as statements
    size_t n = j->ncode + 8;
    char *ret = (char *)malloc(n);
    _snprintf(ret, n, "return %s", j->code);
    int rc = luaL_loadbuffer(L, ret, strlen(ret), "=dev");
    free(ret);
    if (rc != 0) { lua_pop(L, 1); rc = luaL_loadbuffer(L, j->code, j->ncode, "=dev"); }
    if (rc != 0) { j->out = _strdup(lua_tostring(L, -1)); j->ok = 0; lua_settop(L, top); goto fin; }
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_env_ref);
    lua_setfenv(L, -2);
    if (lua_pcall(L, 0, LUA_MULTRET, 0) != 0) {
        const char *e = lua_tostring(L, -1);
        j->out = _strdup(e ? e : "error"); j->ok = 0; lua_settop(L, top); goto fin;
    }
    int nres = lua_gettop(L) - top;
    // dev.repr(results...) → one string
    lua_rawgeti(L, LUA_REGISTRYINDEX, g_env_ref);
    lua_getfield(L, -1, "dev");
    lua_getfield(L, -1, "repr");
    lua_remove(L, -2); lua_remove(L, -2);        // results..., repr
    lua_insert(L, top + 1);                      // repr, results...
    if (lua_pcall(L, nres, 1, 0) != 0) { j->out = _strdup(lua_tostring(L, -1)); j->ok = 0; }
    else { const char *s = lua_tostring(L, -1); j->out = _strdup(s ? s : ""); j->ok = 1; }
    lua_settop(L, top);
fin:
    if (j->abandoned) job_free(j);
    else SetEvent(j->done);
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void send_all(SOCKET s, const char *p, size_t n) {
    while (n) { int k = send(s, p, (int)n, 0); if (k <= 0) return; p += k; n -= (size_t)k; }
}
static void reply(SOCKET s, int ok, const char *msg) {
    size_t n = strlen(msg);
    char *b = (char *)malloc(n * 2 + 8), *o = b;
    memcpy(o, ok ? "ok " : "err ", ok ? 3 : 4); o += ok ? 3 : 4;
    for (size_t i = 0; i < n; i++) {
        if (msg[i] == '\\') { *o++ = '\\'; *o++ = '\\'; }
        else if (msg[i] == '\n') { *o++ = '\\'; *o++ = 'n'; }
        else if (msg[i] == '\r') { *o++ = '\\'; *o++ = 'r'; }
        else *o++ = msg[i];
    }
    *o++ = '\n';
    send_all(s, b, (size_t)(o - b));
    free(b);
}

static void handle_line(SOCKET s, char *line) {
    char *arg = strchr(line, ' ');
    if (arg) *arg++ = 0; else arg = line + strlen(line);
    if (!strcmp(line, "ping")) {
        char b[160];
        _snprintf(b, sizeof b, "frame=%lu armed=%d edition=%s", (unsigned long)exec_frame(), exec_armed(), sym_edition_tag());
        reply(s, 1, b);
    } else if (!strcmp(line, "lua")) {
        size_t hn = strlen(arg);
        if (hn % 2) { reply(s, 0, "odd hex length"); return; }
        dev_job *j = (dev_job *)calloc(1, sizeof *j);
        j->ncode = hn / 2;
        j->code = (char *)malloc(j->ncode + 1);
        for (size_t i = 0; i < j->ncode; i++) {
            int a = hexval(arg[2 * i]), b = hexval(arg[2 * i + 1]);
            if (a < 0 || b < 0) { free(j->code); free(j); reply(s, 0, "bad hex"); return; }
            j->code[i] = (char)(a * 16 + b);
        }
        j->code[j->ncode] = 0;
        j->done = CreateEventA(NULL, TRUE, FALSE, NULL);
        if (!exec_armed()) { reply(s, 0, "executor not armed (no safepoint yet)"); job_free(j); return; }
        exec_enqueue_c(run_job, j);
        if (WaitForSingleObject(j->done, 15000) != WAIT_OBJECT_0) {
            if (InterlockedExchange(&j->abandoned, 1) == 0) { reply(s, 0, "timeout (no safepoint in 15 s — game stalled?)"); return; }
        }
        reply(s, j->ok, j->out ? j->out : "");
        job_free(j);
    } else {
        reply(s, 0, "unknown command (ping | lua <hex>)");
    }
}

static DWORD WINAPI server_thread(void *unused) {
    (void)unused;
    WSADATA wd;
    if (WSAStartup(MAKEWORD(2, 2), &wd)) { ml_log("[dev] WSAStartup failed"); return 0; }
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    BOOL one = TRUE;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((u_short)g_port); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 4)) {
        ml_log("[dev] cannot listen on 127.0.0.1:%d (err %d)", g_port, WSAGetLastError());
        closesocket(ls); return 0;
    }
    ml_log("[dev] devserver listening on 127.0.0.1:%d", g_port);
    for (;;) {
        SOCKET c = accept(ls, NULL, NULL);
        if (c == INVALID_SOCKET) { Sleep(100); continue; }
        size_t cap = 1 << 16, len = 0;
        char *buf = (char *)malloc(cap);
        for (;;) {
            if (len + 4096 > cap) { cap *= 2; buf = (char *)realloc(buf, cap); }
            int k = recv(c, buf + len, 4096, 0);
            if (k <= 0) break;
            len += (size_t)k;
            char *nl;
            while ((nl = (char *)memchr(buf, '\n', len)) != NULL) {
                *nl = 0;
                if (nl > buf && nl[-1] == '\r') nl[-1] = 0;
                handle_line(c, buf);
                size_t used = (size_t)(nl - buf) + 1;
                memmove(buf, nl + 1, len - used); len -= used;
            }
        }
        free(buf);
        closesocket(c);
    }
    return 0;
}

void devserver_start(void) {
    char env[16] = "";
    g_port = config_get_int("devserver", 0);
    if (GetEnvironmentVariableA("OSS_DEVSERVER", env, sizeof env)) g_port = atoi(env);
    if (g_port <= 0) return;
    HANDLE t = CreateThread(NULL, 0, server_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
