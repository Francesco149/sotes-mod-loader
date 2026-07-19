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
#include "ui.h"                // ui_overlay_present — draw mod.ui over the game frame (takeover)

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

// ── shared pixel decode: RGB565/888/X888 (DirectDraw little-endian, B in low byte) -> R8G8B8A8 ──
static uint32_t g_lut5[32], g_lut6[64];        // 5/6-bit -> 8-bit expand (RGB565)
static void ensure_luts(void) {
    static int done = 0; if (done) return; done = 1;
    for (int i = 0; i < 32; i++) g_lut5[i] = (uint32_t)((i * 255 + 15) / 31);
    for (int i = 0; i < 64; i++) g_lut6[i] = (uint32_t)((i * 255 + 31) / 63);
}
static void convert_to_rgba(void *dstv, int dstPitch, const Frame *f) {
    ensure_luts();
    const int bypp = f->bpp / 8;
    for (int y = 0; y < f->h; y++) {
        uint32_t *drow = (uint32_t *)((char *)dstv + (size_t)y * dstPitch);
        const uint8_t *srow = (const uint8_t *)f->buf + (size_t)y * f->w * bypp;
        if (f->bpp == 16) {
            const uint16_t *s = (const uint16_t *)srow;
            for (int x = 0; x < f->w; x++) { uint16_t p = s[x];
                uint32_t r = g_lut5[(p >> 11) & 0x1f], g = g_lut6[(p >> 5) & 0x3f], b = g_lut5[p & 0x1f];
                drow[x] = 0xff000000u | (b << 16) | (g << 8) | r; }
        } else if (f->bpp == 24) {
            for (int x = 0; x < f->w; x++) { const uint8_t *p = srow + x * 3;   // B,G,R
                drow[x] = 0xff000000u | ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]; }
        } else {   // 32bpp XRGB (0xXXRRGGBB)
            const uint32_t *s = (const uint32_t *)srow;
            for (int x = 0; x < f->w; x++) { uint32_t p = s[x];
                drow[x] = 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00u) | ((p >> 16) & 0xff); }
        }
    }
}

// ── Phase B/C: own the game window — present into it + skip the game's own present ──
// g_tk_mode (config ddraw_takeover): 0 = off (mirror only), 1 = borderless-fullscreen, 2 = windowed
// (a normal resizable window).  Both takeover modes drive the window with our vsync'd sharp-bilinear
// present + the in-game overlay; they differ only in the window's style/size and resize handling.
static int  g_tk_mode;       // 0 off / 1 borderless / 2 windowed
static int  g_e_ready;       // the engine-thread device/swapchain on the game window is up
static HWND g_game_hwnd;     // the game window (captured from the present hook) — for cursor mapping + resize
static Frame g_e_lastframe;  // shallow copy of the last presented frame (buf points into g_fr[], stable at
static int  g_e_haveframe;   // 640x480 so it never reallocs) — re-presented on a windowed resize (drag stays live)
static void ddp_engine_present(HWND hwnd, const Frame *f);   // defined below
extern "C" void ddp_set_takeover(int mode) { g_tk_mode = mode; }
extern "C" int  ddp_takeover_active(void)  { return (g_tk_mode && g_e_ready) ? 1 : 0; }   // skip the game present only if we ARE presenting

// ════════════════════════════ ENGINE THREAD: capture ════════════════════════════
extern "C" void ddp_on_present(void *screen_ctx, void *hwnd) {
    if (hwnd) g_game_hwnd = (HWND)hwnd;

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
            // Phase B takeover: present THIS just-captured buffer straight into the game window on the
            // engine thread (the write buffer is ours until we publish, so no reader can race it).
            if (g_tk_mode && hwnd) ddp_engine_present((HWND)hwnd, &f);
            LONG prev = InterlockedExchange(&g_latest, g_write_ix | 0x100);   // publish (fresh, for the companion)
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
    if (g_tk_mode && g_e_ready) return;   // the game window owns the display in takeover — no redundant mirror
    Frame *f = fetch_latest();
    if (!f || !f->buf || f->w <= 0 || f->h <= 0) return;
    if (!ensure_pipeline(dev) || !ensure_texture(dev, f->w, f->h)) return;

    // upload the captured frame -> the dynamic texture (shared decode helper)
    D3D11_MAPPED_SUBRESOURCE map;
    if (FAILED(ctx->Map(g_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &map))) return;
    convert_to_rgba(map.pData, (int)map.RowPitch, f);
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

// ════════════════════════ Phase B: present into the game window (engine thread) ════════════════════════
// Own the game window: skip the game's own (unsynced ~130fps GDI BitBlt) present and drive the window
// ourselves — borderless-fullscreen, vsync'd (Present(1,0) both smooths AND paces the loop), sharp-bilinear
// upscale, aspect-correct.  Its own D3D11 device on the ENGINE thread (the thread that owns the window).
static ID3D11Device           *g_e_dev;   static ID3D11DeviceContext      *g_e_ctx;
static IDXGISwapChain         *g_e_swap;  static ID3D11RenderTargetView   *g_e_rtv;
static ID3D11Texture2D        *g_e_tex;   static ID3D11ShaderResourceView *g_e_srv;
static ID3D11VertexShader     *g_e_vs;    static ID3D11PixelShader        *g_e_ps;
static ID3D11SamplerState     *g_e_smp;   static ID3D11Buffer             *g_e_cb;
static int g_e_bbw, g_e_bbh, g_e_tw, g_e_th;

// sharp-bilinear ("sharp-bilinear-simple"): integer-prescale then bilinear only across the ~1px texel edge
// — crisp like nearest, no shimmer, no blur.  cbuffer: source size + the on-screen (scaled) image size.
static const char *PS_SHARP =
    "cbuffer P:register(b0){float2 srcSize;float2 outSize;};"
    "Texture2D t:register(t0);SamplerState s:register(s0);"
    "float4 main(float4 pos:SV_Position,float2 uv:TEXCOORD0):SV_Target{"
    "float2 sc=max(floor(outSize/srcSize),1.0);float2 tx=uv*srcSize;float2 tf=floor(tx);"
    "float2 cd=(tx-tf)-0.5;float2 rr=0.5-0.5/sc;float2 f=(cd-clamp(cd,-rr,rr))*sc+0.5;"
    "return t.Sample(s,(tf+f)/srcSize);}";

static bool e_create(HWND hwnd) {
    HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi; mi.cbSize = sizeof mi; if (!GetMonitorInfoA(mon, &mi)) return false;
    // The game keeps rendering to its fixed 640x480 back-buffer surface (unaffected by window size) — we
    // just present it scaled.  Two takeover shapes: borderless-fullscreen (WS_POPUP over the monitor) or
    // a normal RESIZABLE window (2x game res, centered on the work area).  mw/mh = the swapchain (client)
    // size either way.
    int mw, mh;
    LONG st = GetWindowLongA(hwnd, GWL_STYLE);
    if (g_tk_mode == 2) {
        LONG wst = (st & ~WS_POPUP) | WS_OVERLAPPEDWINDOW;
        SetWindowLongA(hwnd, GWL_STYLE, wst);
        RECT wa = mi.rcWork; int aw = wa.right - wa.left, ah = wa.bottom - wa.top;
        RECT r = { 0, 0, 1280, 960 }; AdjustWindowRect(&r, wst, FALSE);   // 2x 640x480 client -> window rect
        int winw = r.right - r.left, winh = r.bottom - r.top;
        if (winw > aw) winw = aw;
        if (winh > ah) winh = ah;
        SetWindowPos(hwnd, HWND_TOP, wa.left + (aw - winw) / 2, wa.top + (ah - winh) / 2, winw, winh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        RECT cr; GetClientRect(hwnd, &cr); mw = cr.right - cr.left; mh = cr.bottom - cr.top;
        if (mw <= 0 || mh <= 0) { mw = 1280; mh = 960; }
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, (st & ~(WS_CAPTION|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU|WS_DLGFRAME|WS_BORDER)) | WS_POPUP);
        mw = mi.rcMonitor.right - mi.rcMonitor.left; mh = mi.rcMonitor.bottom - mi.rcMonitor.top;
        SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mw, mh, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    }

    DXGI_SWAP_CHAIN_DESC sd; ZeroMemory(&sd, sizeof sd);
    sd.BufferCount = 2; sd.BufferDesc.Width = mw; sd.BufferDesc.Height = mh;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL lv[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, lv, 2, D3D11_SDK_VERSION, &sd, &g_e_swap, &g_e_dev, &got, &g_e_ctx);
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, lv, 2, D3D11_SDK_VERSION, &sd, &g_e_swap, &g_e_dev, &got, &g_e_ctx);
    if (FAILED(hr)) { ml_log("[ddraw] takeover device create FAILED hr=0x%08lx", (unsigned long)hr); return false; }
    g_e_bbw = mw; g_e_bbh = mh;
    // We own the window (esp. in windowed mode): stop DXGI from hijacking Alt+Enter into its own
    // exclusive-fullscreen toggle, which would fight our borderless/windowed presentation.
    { IDXGIFactory *fac = NULL;
      if (SUCCEEDED(g_e_swap->GetParent(IID_PPV_ARGS(&fac))) && fac) { fac->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER); fac->Release(); } }
    ID3D11Texture2D *bb = NULL;
    if (SUCCEEDED(g_e_swap->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) { g_e_dev->CreateRenderTargetView(bb, NULL, &g_e_rtv); bb->Release(); }
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    if (FAILED(D3DCompile(VS_SRC, strlen(VS_SRC), NULL, NULL, NULL, "main", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(PS_SHARP, strlen(PS_SHARP), NULL, NULL, NULL, "main", "ps_4_0", 0, 0, &psb, &err))) {
        ml_log("[ddraw] takeover shader compile failed: %s", err ? (char *)err->GetBufferPointer() : "?");
        if (err) err->Release();
        if (vsb) vsb->Release();
        return false;
    }
    g_e_dev->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), NULL, &g_e_vs);
    g_e_dev->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), NULL, &g_e_ps);
    vsb->Release(); psb->Release();
    D3D11_SAMPLER_DESC sm; memset(&sm, 0, sizeof sm); sm.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sm.AddressU = sm.AddressV = sm.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; sm.MaxLOD = D3D11_FLOAT32_MAX;
    g_e_dev->CreateSamplerState(&sm, &g_e_smp);
    D3D11_BUFFER_DESC bd; memset(&bd, 0, sizeof bd); bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    g_e_dev->CreateBuffer(&bd, NULL, &g_e_cb);
    if (!g_e_rtv || !g_e_vs || !g_e_ps || !g_e_smp || !g_e_cb) { ml_log("[ddraw] takeover resource create failed"); return false; }
    ml_log("[ddraw] TAKEOVER (%s): game window %p -> %dx%d, presenting via vsync'd D3D11 (sharp-bilinear)",
           g_tk_mode == 2 ? "windowed" : "borderless", (void *)hwnd, mw, mh);
    return true;
}

static bool e_tex(int w, int h) {
    if (g_e_tex && g_e_tw == w && g_e_th == h) return true;
    if (g_e_srv) { g_e_srv->Release(); g_e_srv = NULL; } if (g_e_tex) { g_e_tex->Release(); g_e_tex = NULL; }
    D3D11_TEXTURE2D_DESC td; memset(&td, 0, sizeof td); td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(g_e_dev->CreateTexture2D(&td, NULL, &g_e_tex))) return false;
    if (FAILED(g_e_dev->CreateShaderResourceView(g_e_tex, NULL, &g_e_srv))) { g_e_tex->Release(); g_e_tex = NULL; return false; }
    g_e_tw = w; g_e_th = h; return true;
}

// Draw one captured frame into the swapchain: the game frame scaled (sharp-bilinear, aspect-fit
// pillarbox within the client), then the in-game overlay, then Present (vsync — smooths + paces the
// loop).  Shared by the per-frame present and the windowed-resize re-present.
static void e_present_frame(HWND hwnd, const Frame *f) {
    if (!g_e_rtv || !e_tex(f->w, f->h)) return;
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_e_ctx->Map(g_e_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    convert_to_rgba(m.pData, (int)m.RowPitch, f);
    g_e_ctx->Unmap(g_e_tex, 0);

    // aspect-correct fit within the client (pillarbox); the on-screen image size feeds sharp-bilinear.
    float srcA = (float)f->w / (float)f->h, dstA = (float)g_e_bbw / (float)g_e_bbh, vw, vh;
    if (dstA > srcA) { vh = (float)g_e_bbh; vw = vh * srcA; } else { vw = (float)g_e_bbw; vh = vw / srcA; }
    D3D11_MAPPED_SUBRESOURCE cm;
    if (SUCCEEDED(g_e_ctx->Map(g_e_cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &cm))) {
        float *p = (float *)cm.pData; p[0] = (float)f->w; p[1] = (float)f->h; p[2] = vw; p[3] = vh; g_e_ctx->Unmap(g_e_cb, 0); }
    D3D11_VIEWPORT vp; vp.TopLeftX = ((float)g_e_bbw - vw) * 0.5f; vp.TopLeftY = ((float)g_e_bbh - vh) * 0.5f;
    vp.Width = vw; vp.Height = vh; vp.MinDepth = 0; vp.MaxDepth = 1;

    const float clr[4] = { 0, 0, 0, 1 };
    g_e_ctx->OMSetRenderTargets(1, &g_e_rtv, NULL);
    g_e_ctx->ClearRenderTargetView(g_e_rtv, clr);
    g_e_ctx->RSSetViewports(1, &vp);
    g_e_ctx->IASetInputLayout(NULL); g_e_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_e_ctx->VSSetShader(g_e_vs, NULL, 0); g_e_ctx->PSSetShader(g_e_ps, NULL, 0);
    g_e_ctx->PSSetShaderResources(0, 1, &g_e_srv); g_e_ctx->PSSetSamplers(0, 1, &g_e_smp);
    g_e_ctx->PSSetConstantBuffers(0, 1, &g_e_cb);
    g_e_ctx->Draw(3, 0);
    ui_overlay_present(g_e_dev, g_e_ctx, hwnd);   // the in-game overlay: mod.ui on top of the game frame
    g_e_swap->Present(1, 0);
}

static void ddp_engine_present(HWND hwnd, const Frame *f) {
    if (!f || !f->buf || f->w <= 0 || f->h <= 0) return;
    if (!g_e_ready) { if (!e_create(hwnd)) return; g_e_ready = 1; }   // create fail -> stays 0 -> game presents (fallback)
    g_e_lastframe = *f; g_e_haveframe = 1;   // remember for a windowed-resize re-present (buf stable at 640x480)
    e_present_frame(hwnd, f);
}

// Windowed takeover only: the user resized the game window — resize the swapchain to the new client and
// re-present the last frame so the drag stays live (during a resize the game loop is parked in the modal
// size loop, so no capture arrives).  Called from the executor's window subclass (WM_SIZE), engine thread.
extern "C" void ddp_on_resize(int w, int h) {
    if (g_tk_mode != 2 || !g_e_ready || w <= 0 || h <= 0) return;
    if (w == g_e_bbw && h == g_e_bbh) return;                          // no real change
    if (g_e_rtv) { g_e_rtv->Release(); g_e_rtv = NULL; }
    HRESULT hr = g_e_swap->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { ml_log("[ddraw] ResizeBuffers %dx%d failed hr=0x%08lx", w, h, (unsigned long)hr); return; }
    ID3D11Texture2D *bb = NULL;
    if (SUCCEEDED(g_e_swap->GetBuffer(0, IID_PPV_ARGS(&bb))) && bb) { g_e_dev->CreateRenderTargetView(bb, NULL, &g_e_rtv); bb->Release(); }
    g_e_bbw = w; g_e_bbh = h;
    ui_mark_resized();                                                 // nudge overlay windows back on-screen
    if (g_e_haveframe) e_present_frame(g_game_hwnd, &g_e_lastframe);   // repaint live during the drag
}

// ── cursor -> game-screen (640x480) mapping ──────────────────────────────────────────────────────────
// Undo the on-screen transform (the takeover's aspect-fit pillarbox, or the game's windowed client
// scale) so a mod / the trainer gets the cursor in the game's own 640x480 screen space.  On-demand from
// the mod.game.mouse binding (engine thread).  Returns 1 if the cursor is over the game area (else 0,
// but still fills gx/gy with the extrapolated position so a drag past the edge stays continuous).
extern "C" int ddp_cursor_game(float *gx, float *gy) {
    if (gx) *gx = 0;
    if (gy) *gy = 0;
    HWND h = g_game_hwnd;
    if (!h) return 0;
    POINT p; if (!GetCursorPos(&p)) return 0;
    ScreenToClient(h, &p);
    RECT rc; if (!GetClientRect(h, &rc)) return 0;
    float cw = (float)(rc.right - rc.left), ch = (float)(rc.bottom - rc.top);
    if (cw <= 0 || ch <= 0) return 0;
    float ox = 0, oy = 0, sx, sy;
    if (g_tk_mode && g_e_ready) {           // takeover: aspect-fit pillarbox within the client (monitor OR window)
        float srcA = 640.0f / 480.0f, dstA = cw / ch, vw, vh;
        if (dstA > srcA) { vh = ch; vw = vh * srcA; } else { vw = cw; vh = vw / srcA; }
        ox = (cw - vw) * 0.5f; oy = (ch - vh) * 0.5f; sx = vw / 640.0f; sy = vh / 480.0f;
    } else {                                 // windowed: the game BitBlt's 640x480 to the full client
        sx = cw / 640.0f; sy = ch / 480.0f;
    }
    float x = (p.x - ox) / sx, y = (p.y - oy) / sy;
    if (gx) *gx = x;
    if (gy) *gy = y;
    return (x >= 0 && y >= 0 && x < 640 && y < 480) ? 1 : 0;
}
