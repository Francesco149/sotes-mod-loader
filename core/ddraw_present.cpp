// core/ddraw_present.cpp — DirectDraw present interception + D3D11 re-present (Phase A).  See .h.
//
// ENGINE THREAD: ddp_on_present() runs from the zdd_present hook (executor).  It Locks the finished
// 16bpp back-buffer, copies it into a lock-free triple buffer, and measures the frame-delivery pace.
// UI THREAD: ddp_draw_background() uploads the latest frame to a dynamic texture and draws it with a
// fullscreen-triangle shader (aspect-correct) into the companion window, behind the ImGui overlay.
// Nothing locks between the two threads: the triple buffer is single-writer/single-reader latest-wins
// (an unconsumed frame is dropped, never queued) — the same discipline as the UI snapshot pipeline.

#include "ddraw_present.h"
#include "loader_internal.h"   // ml_log (extern "C")

#define DIRECTDRAW_VERSION 0x0700
#include <windows.h>
#include <ddraw.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

// ════════════════════════════ the captured frame (lock-free triple buffer) ════════════════════════
// SotES back-buffers are 640x480x16bpp; a frame is copied tightly packed (pitch = w*2).  Single
// writer (engine thread) / single reader (UI thread): the writer publishes an index with a FRESH bit
// via InterlockedExchange, taking back whatever buffer the reader last handed over; the reader claims
// the freshest the same way.  {write, read, published} stay a permutation of {0,1,2} — always 3
// distinct buffers, so neither thread ever touches the other's live buffer.
struct Frame {
    void    *buf;                     // tightly packed pixels (pitch = w * bpp/8)
    size_t   cap;                     // bytes allocated
    int      w, h, bpp;               // bpp: 16 (RGB565) / 24 (RGB888) / 32 (XRGB8888)
    uint32_t rmask, gmask, bmask;     // source pixel-format masks
};
static Frame        g_fr[3];
static volatile LONG g_latest   = 2;  // published index (| 0x100 FRESH); starts index 2, not fresh
static LONG         g_write_ix  = 0;  // engine-thread owned
static LONG         g_read_ix   = 1;  // UI-thread owned
static volatile LONG g_have     = 0;  // a real frame has been claimed at least once
static volatile uint32_t g_pubseq;    // published-frame counter (UI polls it to render the mirror)

// ── frame-pace measurement (engine thread) ────────────────────────────────────
static LARGE_INTEGER  g_qpf, g_last_qpc;
static volatile uint32_t g_interval_us;
static uint32_t       g_pcount;

extern "C" uint32_t ddp_present_interval_us(void) { return g_interval_us; }
extern "C" int      ddp_have_frame(void)          { return g_have ? 1 : 0; }
extern "C" uint32_t ddp_frame_seq(void)           { return g_pubseq; }

// ════════════════════════════ ENGINE THREAD: capture ════════════════════════════
extern "C" void ddp_on_present(void *screen_ctx, void *hwnd) {
    (void)hwnd;

    // Frame-pace: interval between presents (EMA), independent of whether the capture below succeeds.
    if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    LARGE_INTEGER now; QueryPerformanceCounter(&now);
    if (g_last_qpc.QuadPart && g_qpf.QuadPart) {
        uint64_t d = (uint64_t)(now.QuadPart - g_last_qpc.QuadPart) * 1000000ull / (uint64_t)g_qpf.QuadPart;
        uint32_t prev = g_interval_us;
        g_interval_us = prev ? (uint32_t)((uint64_t)prev * 7 / 8 + d / 8) : (uint32_t)d;
    }
    g_last_qpc = now;

    static int diag = 1;   // one-shot: confirm the hook fires + the derived back-buffer pointer chain
    if (diag && screen_ctx) {
        diag = 0;
        char *z = *(char **)((char *)screen_ctx + 0x16c);
        void *s = z ? *(void **)(z + 0x2c) : NULL;
        ml_log("[ddraw] present hook live: ctx=%p mode=%d back_zdd=%p surf=%p", screen_ctx,
               *(int *)((char *)screen_ctx + 0x164), (void *)z, s);
    }

    if (!screen_ctx) return;
    char *ctx = (char *)screen_ctx;

    if ((++g_pcount % 300) == 0 && g_interval_us) {   // ~5 s @ 60 Hz: is the LOOP slow, or only the present?
        int mode = *(int *)(ctx + 0x164);             // 0 Full/Flip, 1 Safe, 2 Windowed/BitBlt, 3 DB, 4 Zoom
        ml_log("[ddraw] present pace ~%u us (~%u fps), game mode=%d", g_interval_us, 1000000u / g_interval_us, mode);
    }

    // back-buffer ZDDObject at ctx+0x16c; its IDirectDrawSurface7 at +0x2c (mode-independent — RE).
    char *zddobj = *(char **)(ctx + 0x16c);
    if (!zddobj) return;
    IDirectDrawSurface7 *surf = *(IDirectDrawSurface7 **)(zddobj + 0x2c);
    if (!surf) return;

    DDSURFACEDESC2 dd; memset(&dd, 0, sizeof dd); dd.dwSize = sizeof dd;
    HRESULT hr = surf->Lock(NULL, &dd, DDLOCK_READONLY | DDLOCK_WAIT, NULL);
    if (FAILED(hr) || !dd.lpSurface) {
        static int warned = 0; if (!warned) { warned = 1; ml_log("[ddraw] back-buffer Lock failed hr=0x%08lx — capture off", (unsigned long)hr); }
        return;
    }
    int w = (int)dd.dwWidth, h = (int)dd.dwHeight, srcPitch = (int)dd.lPitch;
    int bpp = (int)dd.ddpfPixelFormat.dwRGBBitCount;
    if (w > 0 && h > 0 && (bpp == 16 || bpp == 24 || bpp == 32)) {
        int bypp = bpp / 8;
        Frame &f = g_fr[g_write_ix];
        size_t rowbytes = (size_t)w * bypp, need = rowbytes * h;
        if (f.cap < need) { void *nb = realloc(f.buf, need); if (nb) { f.buf = nb; f.cap = need; } }
        if (f.buf && f.cap >= need) {
            const char *src = (const char *)dd.lpSurface;
            char *dst = (char *)f.buf;
            for (int y = 0; y < h; y++) memcpy(dst + (size_t)y * rowbytes, src + (size_t)y * srcPitch, rowbytes);
            f.w = w; f.h = h; f.bpp = bpp;
            f.rmask = dd.ddpfPixelFormat.dwRBitMask;
            f.gmask = dd.ddpfPixelFormat.dwGBitMask;
            f.bmask = dd.ddpfPixelFormat.dwBBitMask;
            LONG prev = InterlockedExchange(&g_latest, g_write_ix | 0x100);   // publish (fresh)
            g_write_ix = prev & 0xff;                                         // take the reader's old buffer
            g_pubseq++;
            static int announced = 0;
            if (!announced) { announced = 1; ml_log("[ddraw] capturing back-buffer %dx%d %dbpp masks R=%08x G=%08x B=%08x", w, h, bpp, f.rmask, f.gmask, f.bmask); }
        }
    } else {
        static int warned = 0; if (!warned) { warned = 1; ml_log("[ddraw] back-buffer %dx%d bpp=%d unsupported — capture skipped", w, h, bpp); }
    }
    surf->Unlock(NULL);
}

// ════════════════════════════ UI THREAD: D3D11 re-present ════════════════════════════
static ID3D11Texture2D          *g_tex;
static ID3D11ShaderResourceView *g_srv;
static ID3D11VertexShader       *g_vs;
static ID3D11PixelShader        *g_ps;
static ID3D11SamplerState       *g_smp;
static int                       g_tex_w, g_tex_h;
static bool                      g_pipe_ready;
static uint32_t                  g_lut5[32], g_lut6[64];   // 5/6-bit -> 8-bit expand (RGB565 decode)

static const char *VS_SRC =
    "struct VSOut{float4 pos:SV_Position;float2 uv:TEXCOORD0;};"
    "VSOut main(uint id:SV_VertexID){VSOut o;float2 uv=float2((id<<1)&2,id&2);"
    "o.uv=uv;o.pos=float4(uv*float2(2,-2)+float2(-1,1),0,1);return o;}";
static const char *PS_SRC =
    "Texture2D t:register(t0);SamplerState s:register(s0);"
    "float4 main(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target{return t.Sample(s,uv);}";

static Frame *fetch_latest(void) {
    LONG cur = g_latest;
    if (cur >= 0 && (cur & 0x100)) {
        LONG prev = InterlockedExchange(&g_latest, g_read_ix);   // hand back our buffer (clear fresh)
        g_read_ix = prev & 0xff;                                 // adopt the freshest
        g_have = 1;
    }
    return g_have ? &g_fr[g_read_ix] : NULL;
}

static bool ensure_pipeline(ID3D11Device *dev) {
    if (g_pipe_ready) return true;
    for (int i = 0; i < 32; i++) g_lut5[i] = (uint32_t)((i * 255 + 15) / 31);
    for (int i = 0; i < 64; i++) g_lut6[i] = (uint32_t)((i * 255 + 31) / 63);
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    if (FAILED(D3DCompile(VS_SRC, strlen(VS_SRC), NULL, NULL, NULL, "main", "vs_4_0", 0, 0, &vsb, &err))) {
        ml_log("[ddraw] VS compile failed: %s", err ? (char *)err->GetBufferPointer() : "?"); if (err) err->Release(); return false; }
    if (FAILED(D3DCompile(PS_SRC, strlen(PS_SRC), NULL, NULL, NULL, "main", "ps_4_0", 0, 0, &psb, &err))) {
        ml_log("[ddraw] PS compile failed: %s", err ? (char *)err->GetBufferPointer() : "?"); if (err) err->Release(); vsb->Release(); return false; }
    bool ok = SUCCEEDED(dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &g_vs))
           && SUCCEEDED(dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &g_ps));
    vsb->Release(); psb->Release();
    if (!ok) { ml_log("[ddraw] shader create failed"); return false; }
    D3D11_SAMPLER_DESC sd; memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;                 // bilinear (spike); sharp-bilinear = Phase B
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(dev->CreateSamplerState(&sd, &g_smp))) { ml_log("[ddraw] sampler create failed"); return false; }
    g_pipe_ready = true;
    ml_log("[ddraw] D3D11 re-present pipeline ready");
    return true;
}

static bool ensure_texture(ID3D11Device *dev, int w, int h) {
    if (g_tex && g_tex_w == w && g_tex_h == h) return true;
    if (g_srv) { g_srv->Release(); g_srv = NULL; }
    if (g_tex) { g_tex->Release(); g_tex = NULL; }
    D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof td);
    td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(dev->CreateTexture2D(&td, NULL, &g_tex))) { ml_log("[ddraw] texture create failed %dx%d", w, h); return false; }
    if (FAILED(dev->CreateShaderResourceView(g_tex, NULL, &g_srv))) { g_tex->Release(); g_tex = NULL; return false; }
    g_tex_w = w; g_tex_h = h;
    return true;
}

extern "C" void ddp_draw_background(void *dev_, void *ctx_, int dstW, int dstH) {
    ID3D11Device *dev = (ID3D11Device *)dev_;
    ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)ctx_;
    if (!dev || !ctx || dstW <= 0 || dstH <= 0) return;
    Frame *f = fetch_latest();
    if (!f || !f->buf || f->w <= 0 || f->h <= 0) return;
    if (!ensure_pipeline(dev) || !ensure_texture(dev, f->w, f->h)) return;

    // upload: RGB565/888/X888 -> R8G8B8A8 into the mapped dynamic texture.  DirectDraw stores these
    // little-endian (B in the low byte), so the source byte order is B,G,R; the DXGI R8G8B8A8 dst
    // wants byte order R,G,B,A => the uint32 is 0xAABBGGRR.
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    const int bypp = f->bpp / 8;
    for (int y = 0; y < f->h; y++) {
        uint32_t *drow = (uint32_t *)((char *)map.pData + (size_t)y * map.RowPitch);
        const uint8_t *srow = (const uint8_t *)f->buf + (size_t)y * f->w * bypp;
        if (f->bpp == 16) {
            const uint16_t *s16 = (const uint16_t *)srow;
            for (int x = 0; x < f->w; x++) {
                uint16_t p = s16[x];
                uint32_t r = g_lut5[(p >> 11) & 0x1f], g = g_lut6[(p >> 5) & 0x3f], b = g_lut5[p & 0x1f];
                drow[x] = 0xff000000u | (b << 16) | (g << 8) | r;
            }
        } else if (f->bpp == 24) {
            for (int x = 0; x < f->w; x++) {
                const uint8_t *p = srow + x * 3;   // B,G,R
                drow[x] = 0xff000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2];
            }
        } else {   // 32bpp XRGB (B,G,R,X)
            const uint32_t *s32 = (const uint32_t *)srow;
            for (int x = 0; x < f->w; x++) {
                uint32_t p = s32[x];             // 0xXXRRGGBB
                drow[x] = 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00u) | ((p >> 16) & 0xff);
            }
        }
    }
    ctx->Unmap(g_tex, 0);

    // aspect-correct fit within dstW x dstH (letterbox / pillarbox)
    float srcA = (float)f->w / (float)f->h, dstA = (float)dstW / (float)dstH, vw, vh;
    if (dstA > srcA) { vh = (float)dstH; vw = vh * srcA; } else { vw = (float)dstW; vh = vw / srcA; }
    D3D11_VIEWPORT vp;
    vp.TopLeftX = ((float)dstW - vw) * 0.5f; vp.TopLeftY = ((float)dstH - vh) * 0.5f;
    vp.Width = vw; vp.Height = vh; vp.MinDepth = 0; vp.MaxDepth = 1;
    ctx->RSSetViewports(1, &vp);

    ctx->IASetInputLayout(NULL);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(g_vs, NULL, 0);
    ctx->PSSetShader(g_ps, NULL, 0);
    ctx->PSSetShaderResources(0, 1, &g_srv);
    ctx->PSSetSamplers(0, 1, &g_smp);
    ctx->Draw(3, 0);                                            // fullscreen triangle (no vertex buffer)
}
