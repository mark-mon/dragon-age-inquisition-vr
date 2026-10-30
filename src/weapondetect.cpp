#include "weapondetect.h"
#include <windows.h>
#include <cstdint>
#include "settings.h"
#include "log.h"

namespace mea {

static const int kRing = 4;
static ID3D11Texture2D* g_stage[kRing] = {};
static bool g_pending[kRing] = {};
static UINT g_box = 0, g_fmtBytes = 0;
static DXGI_FORMAT g_fmt = DXGI_FORMAT_UNKNOWN;
static int g_pos = 0;
static volatile bool g_out = false;
static int g_onCount = 0, g_offCount = 0;
static DWORD g_lastSample = 0;

bool WeaponOut() { return g_out && GetTickCount() - g_lastSample < 500; }

// Gameplay HUD: the compass across the top centre is shown in normal play and hidden in conversations, cutscenes and
// menus. Sampled like the reticle (box 20 % x 4 % of the UI layer at the top centre).
static ID3D11Texture2D* g_cstage[kRing] = {};
static bool g_cpend[kRing] = {};
static UINT g_cw = 0, g_ch = 0;
static DXGI_FORMAT g_cfmt = DXGI_FORMAT_UNKNOWN;
static volatile bool g_compass = false;
static int g_cOn = 0, g_cOff = 0;
static DWORD g_cSample = 0;
bool HudGameplayVisible() { return g_compass && GetTickCount() - g_cSample < 500; }

static void CompassSample(ID3D11DeviceContext* ctx, ID3D11Resource* res, const D3D11_TEXTURE2D_DESC& d, int pos) {
    UINT w = (d.Width / 5) & ~1u, h = (d.Height / 25) & ~1u, y0 = d.Height / 200;
    if (w < 16 || h < 8) return;
    if (w != g_cw || h != g_ch || d.Format != g_cfmt) {
        for (auto& s : g_cstage) if (s) { s->Release(); s = nullptr; }
        for (auto& p : g_cpend) p = false;
        ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC sd{w, h, 1, 1, d.Format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        for (auto& s : g_cstage) if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s))) s = nullptr;
        dev->Release();
        g_cw = w; g_ch = h; g_cfmt = d.Format;
    }
    int rd = (pos + 1) % kRing;
    if (g_cpend[rd] && g_cstage[rd]) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_cstage[rd], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            UINT lit = 0;
            for (UINT y = 0; y < g_ch; ++y) {
                const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch;
                for (UINT x = 0; x < g_cw; ++x) {
                    const uint8_t* p = row + x * 4;
                    uint8_t mx = p[0] > p[1] ? p[0] : p[1]; if (p[2] > mx) mx = p[2];
                    if (mx > 60 && p[3] > 40) ++lit;
                }
            }
            ctx->Unmap(g_cstage[rd], 0);
            g_cpend[rd] = false;
            g_cSample = GetTickCount();
            bool vis = lit >= 20 && lit * 100 < g_cw * g_ch * 40;
            if (vis) { g_cOff = 0; if (++g_cOn >= 15 && !g_compass) { g_compass = true; LOGI("hud: gameplay HUD (compass) visible (%u lit px)", lit); } }
            else { g_cOn = 0; if (++g_cOff >= 6 && g_compass) { g_compass = false; LOGI("hud: gameplay HUD hidden (%u lit px)", lit); } }
        }
    }
    if (g_cstage[pos]) {
        D3D11_BOX b{(d.Width - w) / 2, y0, 0, (d.Width + w) / 2, y0 + h, 1};
        ctx->CopySubresourceRegion(g_cstage[pos], 0, 0, 0, 0, res, 0, &b);
        g_cpend[pos] = true;
    }
}

static UINT BytesPerPixel(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return 4;
    default: return 0;
    }
}

void WeaponDetectSample(ID3D11DeviceContext* ctx, ID3D11Resource* res) {
    if (!ctx || !res) return;
    D3D11_RESOURCE_DIMENSION dim; res->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return;
    D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)res)->GetDesc(&d);
    UINT bpp = BytesPerPixel(d.Format);
    if (!bpp || d.SampleDesc.Count != 1 || d.Width < 64 || d.Height < 64) return;
    // box: 6% of the height around the centre (the hip reticle is ~3.5%, the aiming crosshair ~1.5%)
    UINT box = (d.Height * 6 / 100) & ~1u;
    if (box < 16) box = 16;
    if (box != g_box || d.Format != g_fmt) {
        for (auto& s : g_stage) if (s) { s->Release(); s = nullptr; }
        for (auto& p : g_pending) p = false;
        ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC sd{box, box, 1, 1, d.Format, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        for (auto& s : g_stage) if (FAILED(dev->CreateTexture2D(&sd, nullptr, &s))) s = nullptr;
        dev->Release();
        g_box = box; g_fmt = d.Format; g_fmtBytes = bpp;
        LOGI("weapon: reticle detector box %ux%u on the %ux%u UI layer", box, box, d.Width, d.Height);
    }
    // read the oldest pending copy (copied kRing-1 frames ago)
    int rd = (g_pos + 1) % kRing;
    if (g_pending[rd] && g_stage[rd]) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ctx->Map(g_stage[rd], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            UINT lit = 0, total = g_box * g_box;
            for (UINT y = 0; y < g_box; ++y) {
                const uint8_t* row = (const uint8_t*)m.pData + y * m.RowPitch;
                for (UINT x = 0; x < g_box; ++x) {
                    const uint8_t* p = row + x * 4;
                    uint8_t mx = p[0] > p[1] ? p[0] : p[1]; if (p[2] > mx) mx = p[2];
                    if (mx > 90 && p[3] > 60) ++lit;   // a bright, fairly opaque UI pixel
                }
            }
            ctx->Unmap(g_stage[rd], 0);
            g_pending[rd] = false;
            g_lastSample = GetTickCount();
            // a reticle: a handful of lit pixels; a full-screen overlay / menu: most of the box
            bool reticle = lit >= 3 && lit * 100 < total * 30;
            if (reticle) { g_offCount = 0; if (++g_onCount >= 6 && !g_out) { g_out = true; LOGI("weapon: out (reticle, %u lit px)", lit); } }
            else { g_onCount = 0; if (++g_offCount >= 45 && g_out) { g_out = false; LOGI("weapon: holstered / no reticle (%u lit px)", lit); } }
        }
    }
    CompassSample(ctx, res, d, g_pos);
    // copy this frame's centre box
    if (g_stage[g_pos]) {
        D3D11_BOX b{(d.Width - g_box) / 2, (d.Height - g_box) / 2, 0, (d.Width + g_box) / 2, (d.Height + g_box) / 2, 1};
        ctx->CopySubresourceRegion(g_stage[g_pos], 0, 0, 0, 0, res, 0, &b);
        g_pending[g_pos] = true;
    }
    g_pos = (g_pos + 1) % kRing;
}

}
