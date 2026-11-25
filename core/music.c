// core/music.c — one looping music stream for mods (mod.game.sound.music_*, api 1.11).
//
// The game's own BGM is DirectShow playing WMA resources; a mod's track is a plain PCM .wav on disk (often made by
// its first-time setup from another game the user owns).  We play it from a DirectSound device of our own: one
// static secondary buffer holding the whole file, looping.  The Lua side (core/lua/script.lua) silences the game's
// BGM while a track plays by answering the room loop's "which BGM belongs here" (bgm_pick) with 0.
#include "loader_internal.h"
#include "executor.h"
#include <stdio.h>
#include <string.h>
#define COBJMACROS
#include <mmsystem.h>
#include <dsound.h>
#include "lua.h"
#include "lauxlib.h"

static IDirectSound8      *g_ds;
static IDirectSoundBuffer *g_buf;
static char                g_path[MAX_PATH];
static long                g_vol;          // 1/100 dB, <= 0

static void music_stop(void) {
    if (g_buf) { IDirectSoundBuffer_Stop(g_buf); IDirectSoundBuffer_Release(g_buf); g_buf = NULL; }
    g_path[0] = 0;
}
static int device(void) {
    if (g_ds) return 1;
    if (FAILED(DirectSoundCreate8(NULL, &g_ds, NULL))) { g_ds = NULL; return 0; }
    HWND h = (HWND)exec_game_hwnd();
    if (!h) h = GetDesktopWindow();
    IDirectSound8_SetCooperativeLevel(g_ds, h, DSSCL_PRIORITY);
    return 1;
}
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
// "RIFF....WAVE" + chunks: 'fmt ' (a WAVEFORMATEX prefix, PCM) and 'data'
static const char *music_play(const char *path, int loop, long vol) {
    if (g_buf && lstrcmpiA(path, g_path) == 0) return NULL;      // already playing this
    music_stop();
    if (!device()) return "no DirectSound device";
    FILE *f = fopen(path, "rb");
    if (!f) return "cannot open the file";
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = (n > 44 && n < 512 * 1024 * 1024) ? (unsigned char *)malloc((size_t)n) : NULL;
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return "cannot read the file"; }
    fclose(f);
    const char *err = NULL;
    if (memcmp(d, "RIFF", 4) || memcmp(d + 8, "WAVE", 4)) err = "not a RIFF WAVE file";
    WAVEFORMATEX wf; memset(&wf, 0, sizeof wf);
    const unsigned char *pcm = NULL; unsigned npcm = 0;
    for (long o = 12; !err && o + 8 <= n; ) {
        unsigned sz = rd32(d + o + 4);
        if ((unsigned long)o + 8 + sz > (unsigned long)n) sz = (unsigned)(n - o - 8);
        if (!memcmp(d + o, "fmt ", 4) && sz >= 16) memcpy(&wf, d + o + 8, 16);
        else if (!memcmp(d + o, "data", 4)) { pcm = d + o + 8; npcm = sz; }
        o += 8 + (long)sz + (sz & 1);
    }
    if (!err && (!pcm || !npcm || wf.wFormatTag != WAVE_FORMAT_PCM)) err = "not PCM audio";
    if (!err) {
        wf.cbSize = 0;
        DSBUFFERDESC bd; memset(&bd, 0, sizeof bd);
        bd.dwSize = sizeof bd;
        bd.dwFlags = DSBCAPS_CTRLVOLUME | DSBCAPS_GLOBALFOCUS | DSBCAPS_GETCURRENTPOSITION2;
        bd.dwBufferBytes = npcm - npcm % wf.nBlockAlign;
        bd.lpwfxFormat = &wf;
        if (FAILED(IDirectSound8_CreateSoundBuffer(g_ds, &bd, &g_buf, NULL))) { g_buf = NULL; err = "CreateSoundBuffer failed"; }
        else {
            void *p1, *p2; DWORD n1, n2;
            if (SUCCEEDED(IDirectSoundBuffer_Lock(g_buf, 0, bd.dwBufferBytes, &p1, &n1, &p2, &n2, 0))) {
                memcpy(p1, pcm, n1);
                if (p2 && n2) memcpy(p2, pcm + n1, n2);
                IDirectSoundBuffer_Unlock(g_buf, p1, n1, p2, n2);
                g_vol = vol > 0 ? 0 : vol < -10000 ? -10000 : vol;
                IDirectSoundBuffer_SetVolume(g_buf, g_vol);
                IDirectSoundBuffer_Play(g_buf, 0, 0, loop ? DSBPLAY_LOOPING : 0);
                _snprintf(g_path, MAX_PATH, "%s", path);
                ml_log("[music] playing %s (%u Hz, %u ch, %u B%s)", path, (unsigned)wf.nSamplesPerSec, wf.nChannels,
                       (unsigned)bd.dwBufferBytes, loop ? ", looping" : "");
            } else { IDirectSoundBuffer_Release(g_buf); g_buf = NULL; err = "Lock failed"; }
        }
    }
    free(d);
    return err;
}

static int l_play(lua_State *L) {
    const char *err = music_play(luaL_checkstring(L, 1), lua_isnone(L, 2) ? 1 : lua_toboolean(L, 2), (long)luaL_optnumber(L, 3, 0));
    if (err) { lua_pushnil(L); lua_pushstring(L, err); return 2; }
    lua_pushboolean(L, 1);
    return 1;
}
static int l_stop(lua_State *L) { music_stop(); lua_pushboolean(L, 1); return 1; }
static int l_volume(lua_State *L) {
    if (g_buf && lua_isnumber(L, 1)) {
        long v = (long)lua_tonumber(L, 1);
        g_vol = v > 0 ? 0 : v < -10000 ? -10000 : v;
        IDirectSoundBuffer_SetVolume(g_buf, g_vol);
    }
    lua_pushnumber(L, g_vol);
    return 1;
}
static int l_status(lua_State *L) {
    if (!g_buf) { lua_pushnil(L); return 1; }
    DWORD st = 0, pos = 0;
    IDirectSoundBuffer_GetStatus(g_buf, &st);
    IDirectSoundBuffer_GetCurrentPosition(g_buf, &pos, NULL);
    lua_newtable(L);
    lua_pushstring(L, g_path);                              lua_setfield(L, -2, "file");
    lua_pushboolean(L, (st & DSBSTATUS_PLAYING) != 0);      lua_setfield(L, -2, "playing");
    lua_pushnumber(L, pos);                                 lua_setfield(L, -2, "position");
    lua_pushnumber(L, g_vol);                               lua_setfield(L, -2, "volume");
    return 1;
}
void music_push_lua(lua_State *L) {
    lua_newtable(L);
    lua_pushcfunction(L, l_play);   lua_setfield(L, -2, "play");
    lua_pushcfunction(L, l_stop);   lua_setfield(L, -2, "stop");
    lua_pushcfunction(L, l_volume); lua_setfield(L, -2, "volume");
    lua_pushcfunction(L, l_status); lua_setfield(L, -2, "status");
}
