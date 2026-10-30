// NVIDIA VRS fixed foveation for the stereo eye views (see sharedtools/vr-mod-toolkit.md, "VRS").
// A shading-rate image (R8_UINT, one texel per 16x16 tile) per eye indexes a per-viewport table:
// 0 = 1x1 (centre), 1 = 2x2 (middle ring), 2 = 4x4 (outer ring). It is bound while a scene pass renders: render
// targets of the eye render size with a depth buffer (G-buffer, forward, sky); off for everything else (shadow maps,
// half-res and post passes, the UI). The game's lighting is compute and is not affected.
#include <windows.h>
#include <d3d11.h>
#include <cmath>
#include <cstdio>
#include <vector>
#include "vrs.h"
#include "settings.h"
#include "log.h"

namespace mea {

// ---- NVAPI (declared here, as in nvapi.h)
typedef int NvAPI_Status;
typedef void* (__cdecl* PFN_QI)(unsigned int);
struct ID3D11NvShadingRateResourceView;   // opaque COM object (IUnknown)
struct NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1 {
    unsigned version;
    DXGI_FORMAT Format;
    int ViewDimension;                    // NV_SRRV_DIMENSION_TEXTURE2D = 4
    unsigned MipSlice, pad0, pad1;        // union (Texture2D / Texture2DArray)
};
struct NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1 {
    bool enableVariablePixelShadingRate;
    int shadingRateTable[16];             // NV_PIXEL_SHADING_RATE
};
struct NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1 {
    unsigned version;
    unsigned numViewports;
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1* pViewports;
};
enum { NV_PIXEL_X1_PER_RASTER_PIXEL = 5, NV_PIXEL_X1_PER_2X2_RASTER_PIXELS = 8, NV_PIXEL_X1_PER_4X4_RASTER_PIXELS = 11 };
typedef NvAPI_Status(__cdecl* PFN_Init)();
typedef NvAPI_Status(__cdecl* PFN_CreateSRRV)(ID3D11Device*, ID3D11Resource*, NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1*, ID3D11NvShadingRateResourceView**);
typedef NvAPI_Status(__cdecl* PFN_SetSRRV)(IUnknown*, ID3D11NvShadingRateResourceView*);
typedef NvAPI_Status(__cdecl* PFN_SetRates)(IUnknown*, NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1*);
static PFN_CreateSRRV p_Create = nullptr;
static PFN_SetSRRV p_SetSrrv = nullptr;
static PFN_SetRates p_SetRates = nullptr;

static bool g_ok = false, g_tried = false;
static char g_status[96] = "VRS: not initialised";
static ID3D11Device* g_dev = nullptr;          // the game's device (from the context the scene passes use)
static ID3D11DeviceContext* g_ctx = nullptr;   // that context (immediate)
struct EyeImg { ID3D11Texture2D* tex = nullptr; ID3D11NvShadingRateResourceView* srrv = nullptr; UINT w = 0, h = 0; float key[4] = {-1, -1, -1, -1}; };
static volatile float g_gaze[2][2] = {{0.5f, 0.5f}, {0.5f, 0.5f}};   // per eye: foveation centre in the image (0..1)
static volatile bool g_gazeOn[2] = {false, false};
static EyeImg g_img[2];
static int g_eye = -1;
static UINT g_mainW = 0, g_mainH = 0;   // largest depth buffer seen in the previous eye render = the eye render size
static UINT g_curW = 0, g_curH = 0;
static bool g_bound = false;

void VrsInit(ID3D11Device* dev) {
    if (g_tried) return;
    g_tried = true;   // dev is the temporary hook-install device: not kept (the game's device comes from its context)
    (void)dev;
    HMODULE m = LoadLibraryW(L"nvapi64.dll");
    auto qi = m ? (PFN_QI)GetProcAddress(m, "nvapi_QueryInterface") : nullptr;
    if (!qi) { snprintf(g_status, sizeof g_status, "VRS: no NVAPI (not an NVIDIA GPU?)"); LOGI("%s", g_status); return; }
    auto init = (PFN_Init)qi(0x0150E828u);
    p_Create = (PFN_CreateSRRV)qi(0x99CA2DFFu);
    p_SetSrrv = (PFN_SetSRRV)qi(0x1B0C2F83u);
    p_SetRates = (PFN_SetRates)qi(0x34F7938Fu);
    if (!init || !p_Create || !p_SetSrrv || !p_SetRates || init() != 0) {
        snprintf(g_status, sizeof g_status, "VRS: NVAPI functions unavailable");
        LOGW("%s", g_status);
        return;
    }
    g_ok = true;
    snprintf(g_status, sizeof g_status, "VRS: ready");
    LOGI("vrs: NVAPI variable rate shading available");
}

static void Release(EyeImg& e) {
    if (e.srrv) ((IUnknown*)e.srrv)->Release();
    if (e.tex) e.tex->Release();
    e = EyeImg();
}

// tiles: radial distance from the image centre, normalised so 1.0 = the half-height
static bool Ensure(int eye, UINT w, UINT h) {
    EyeImg& e = g_img[eye];
    UINT tw = (w + 15) / 16, th = (h + 15) / 16;
    // foveation centre, snapped to whole tiles (only re-upload when it moves a tile)
    float gu = g_gazeOn[eye] ? g_gaze[eye][0] : 0.5f, gv = g_gazeOn[eye] ? g_gaze[eye][1] : 0.5f;
    float cxp = std::floor(gu * w / 16.f) * 16.f + 8.f, cyp = std::floor(gv * h / 16.f) * 16.f + 8.f;
    float key[4] = {g_cfg.vrsInner, g_cfg.vrsOuter, cxp, cyp};
    bool same = e.tex && e.w == w && e.h == h && key[0] == e.key[0] && key[1] == e.key[1] && key[2] == e.key[2] && key[3] == e.key[3];
    if (same) return true;
    std::vector<uint8_t> px((size_t)tw * th);
    for (UINT y = 0; y < th; ++y)
        for (UINT x = 0; x < tw; ++x) {
            float fx = ((x + 0.5f) * 16.f - cxp) / (h * 0.5f), fy = ((y + 0.5f) * 16.f - cyp) / (h * 0.5f);
            float r = std::sqrt(fx * fx + fy * fy);
            px[(size_t)y * tw + x] = r < g_cfg.vrsInner ? 0 : r < g_cfg.vrsOuter ? 1 : 2;
        }
    if (!e.tex || e.w != w || e.h != h) {
        Release(e);
        LOGI("vrs: creating eye %d rate image (%ux%u tiles)", eye, tw, th);
        D3D11_TEXTURE2D_DESC d{tw, th, 1, 1, DXGI_FORMAT_R8_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
        D3D11_SUBRESOURCE_DATA sd{px.data(), tw, 0};
        if (FAILED(g_dev->CreateTexture2D(&d, &sd, &e.tex))) { LOGE("vrs: rate texture failed"); return false; }
        NV_D3D11_SHADING_RATE_RESOURCE_VIEW_DESC_V1 vd{};
        vd.version = sizeof(vd) | (1u << 16);
        vd.Format = DXGI_FORMAT_R8_UINT;
        vd.ViewDimension = 4;
        if (p_Create(g_dev, e.tex, &vd, &e.srrv) != 0 || !e.srrv) { LOGE("vrs: CreateShadingRateResourceView failed"); Release(e); return false; }
        LOGI("vrs: eye %d rate image %ux%u tiles for %ux%u (inner %.2f, outer %.2f)", eye, tw, th, w, h, g_cfg.vrsInner, g_cfg.vrsOuter);
    } else {
        g_ctx->UpdateSubresource(e.tex, 0, nullptr, px.data(), tw, 0);
    }
    e.w = w; e.h = h;
    for (int i = 0; i < 4; ++i) e.key[i] = key[i];
    return true;
}

static void Bind(ID3D11DeviceContext* c, bool on) {
    if (on == g_bound) return;
    NV_D3D11_VIEWPORT_SHADING_RATE_DESC_V1 vp[16] = {};
    for (int i = 0; i < 16; ++i) {
        vp[i].enableVariablePixelShadingRate = on;
        for (int k = 0; k < 16; ++k) vp[i].shadingRateTable[k] = NV_PIXEL_X1_PER_RASTER_PIXEL;
        vp[i].shadingRateTable[1] = NV_PIXEL_X1_PER_2X2_RASTER_PIXELS;
        vp[i].shadingRateTable[2] = NV_PIXEL_X1_PER_4X4_RASTER_PIXELS;
    }
    NV_D3D11_VIEWPORTS_SHADING_RATE_DESC_V1 d{};
    d.version = sizeof(d) | (1u << 16);
    d.numViewports = 16;
    d.pViewports = vp;
    static bool first = true;
    if (on && first) { first = false; LOGI("vrs: first bind (eye %d)", g_eye); }
    if (on) p_SetSrrv(c, g_img[g_eye].srrv);
    p_SetRates(c, &d);
    if (!on) p_SetSrrv(c, nullptr);
    g_bound = on;
    static int binds = 0;
    static DWORD tick = 0;
    if (on) ++binds;
    DWORD now = GetTickCount();
    if (on && now - tick > 10000) { tick = now; LOGI("vrs: %d scene-pass binds in the last period (eye size %ux%u)", binds, g_mainW, g_mainH); binds = 0; }
}

static UINT g_fixW = 0, g_fixH = 0;   // DAI: the eye render size (back buffer); shadow maps are larger than the scene
void VrsSetMainSize(UINT w, UINT h) { g_fixW = w; g_fixH = h; }
static ID3D11Resource* g_exclude = nullptr;   // never foveate passes into this target (DAI: the back buffer - composite + UI)
void VrsSetExclude(ID3D11Resource* r) { g_exclude = r; }
static bool IsExcluded(ID3D11View* v) {
    if (!g_exclude || !v) return false;
    ID3D11Resource* r = nullptr; v->GetResource(&r);
    bool x = r == g_exclude;
    if (r) r->Release();
    return x;
}

void VrsBeginEye(int eye) {
    g_eye = -1;
    if (!g_ok || !g_cfg.vrsEnabled || eye < 0 || eye > 1) return;
    g_eye = eye;
    g_curW = g_curH = 0;
}

void VrsEndEye() {
    if (g_bound && g_ctx) Bind(g_ctx, false);
    if (g_eye >= 0 && g_curW) { g_mainW = g_curW; g_mainH = g_curH; }
    g_eye = -1;
}

static void Dims(ID3D11View* v, UINT& w, UINT& h) {
    w = h = 0;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) { D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d); w = d.Width; h = d.Height; }
    r->Release();
}

void VrsOnSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (!g_ok) return;
    if (n == D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) return;
    if (g_eye >= 0 && c != g_ctx) {
        ID3D11Device* d = nullptr;
        c->GetDevice(&d);
        if (!d) return;
        if (d != g_dev) {   // new device: drop the rate images made on the old one
            for (auto& e : g_img) Release(e);
            g_bound = false;
            if (g_dev) g_dev->Release();
            g_dev = d;
        } else d->Release();
        g_ctx = c;   // owned by the game's device, which we hold
        LOGI("vrs: using device %p context %p", (void*)g_dev, (void*)c);
    }
    bool scene = false;
    if (g_eye >= 0 && dsv) {
        UINT dw, dh; Dims(dsv, dw, dh);
        if (dw * dh > g_curW * g_curH) { g_curW = dw; g_curH = dh; }
        UINT mw = g_fixW ? g_fixW : g_mainW, mh = g_fixW ? g_fixH : g_mainH;
        if (n >= 1 && rtv && rtv[0] && dw == mw && dh == mh && dw >= 256) {
            UINT rw, rh; Dims(rtv[0], rw, rh);
            scene = rw == dw && rh == dh && !IsExcluded(rtv[0]) && Ensure(g_eye, dw, dh);
        }
    }
    if (scene && !g_bound)
        snprintf(g_status, sizeof g_status, "VRS: on (%ux%u, inner %.2f, outer %.2f)", g_mainW, g_mainH, g_cfg.vrsInner, g_cfg.vrsOuter);
    if (scene || g_bound) Bind(c, scene);
}

void VrsSetGaze(int eye, bool on, float u, float v) {
    if (eye < 0 || eye > 1) return;
    g_gaze[eye][0] = u; g_gaze[eye][1] = v;
    g_gazeOn[eye] = on;
}

const char* VrsStatus() { return g_status; }

}
