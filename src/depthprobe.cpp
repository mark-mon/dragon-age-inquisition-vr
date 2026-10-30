#include "depthprobe.h"
#include <windows.h>
#include <cmath>
#include <cstdint>
#include "log.h"
#include "settings.h"

namespace mea {

static volatile bool g_want = false;
static int g_eye = -1;
static ID3D11Resource* g_dsvRes = nullptr;   // largest depth buffer bound during eye 0 (not AddRef'd beyond the frame)
static UINT g_dsvArea = 0;
static const int kRing = 3;
static ID3D11Texture2D* g_stage[kRing] = {};
static bool g_pend[kRing] = {};
static float g_projPend[kRing][16] = {};     // the eye projection of the frame copied (row vectors)
static volatile float g_ray[6] = {0, 0, 0, 0, 0, -1};   // aim ray in the left eye's frame (real m; x right, y up, z back)
static volatile float g_hitT = 0;
static volatile DWORD g_hitTick = 0;
void DepthProbeSetRay(const float o[3], const float d[3]) { for (int i = 0; i < 3; ++i) { g_ray[i] = o[i]; g_ray[3 + i] = d[i]; } }
bool DepthProbeHit(float& t) {
    if (!g_hitTick || GetTickCount() - g_hitTick > 300 || !(g_hitT > 0)) return false;
    t = g_hitT;
    return true;
}
static D3D11_TEXTURE2D_DESC g_sd{};
static int g_pos = 0;
static volatile float g_dist = 0;
static volatile DWORD g_distTick = 0;
static int g_frame = 0;

void DepthProbeRequest(bool on) { g_want = on; }
bool DepthProbeDistance(float& d) {
    if (!g_distTick || GetTickCount() - g_distTick > 300 || !(g_dist > 0)) return false;
    d = g_dist;
    return true;
}

void DepthProbeBeginEye(int eye) {
    g_eye = eye;
    if (eye == 0) { g_dsvRes = nullptr; g_dsvArea = 0; }
}

static ID3D11DeviceContext* g_ctx = nullptr;
void DepthProbeNoteDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv) {
    if (g_eye != 0 || !dsv || !g_want) return;
    g_ctx = c;
    ID3D11Resource* r = nullptr;
    dsv->GetResource(&r);
    if (!r) return;
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
        if (d.Width * d.Height > g_dsvArea && d.SampleDesc.Count == 1 && d.ArraySize == 1) { g_dsvArea = d.Width * d.Height; g_dsvRes = r; }
    }
    r->Release();   // the game keeps it alive for the frame
}

static DXGI_FORMAT StageFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT: case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

void DepthProbeEndEye(const float* P) {
    ID3D11DeviceContext* c = g_ctx;
    int eye = g_eye;
    g_eye = -1;
    if (eye != 0 || !g_want || !g_dsvRes || !c) return;
    ++g_frame;
    // read back the oldest copy
    int rd = (g_pos + 1) % kRing;
    if (g_pend[rd] && g_stage[rd]) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(c->Map(g_stage[rd], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) {
            const float* P = g_projPend[rd];
            auto ndcAt = [&](UINT x, UINT y) -> float {
                const uint8_t* px = (const uint8_t*)m.pData + y * m.RowPitch;
                if (g_sd.Format == DXGI_FORMAT_R24G8_TYPELESS) return (((const uint32_t*)px)[x] & 0xFFFFFF) / 16777215.f;
                if (g_sd.Format == DXGI_FORMAT_R32G8X24_TYPELESS) return ((const float*)px)[x * 2];
                return ((const float*)px)[x];
            };
            // view z of the surface from NDC depth (row vectors: clip.z = z*P22 + P32, clip.w = z*P23 + P33)
            auto surfZ = [&](float ndc) -> float {
                float den = ndc * P[11] - P[10];
                return std::fabs(den) > 1e-9f ? (P[14] - ndc * P[15]) / den : -1e30f;
            };
            float ndc0 = ndcAt(g_sd.Width / 2, g_sd.Height / 2);
            float dist = -surfZ(ndc0);
            bool sky = !(dist > 0.05f) || dist > 5000.f || ndc0 <= 0.f || ndc0 >= 1.f;
            g_dist = sky ? 0.f : dist;
            g_distTick = GetTickCount();
            // march the aim ray through the depth buffer: the first point that goes behind a visible surface (after
            // having been in front of one - Ryder herself can hide the start of the ray) is the hit
            float sc = g_cfg.worldScale > 0.01f ? g_cfg.worldScale : 1.f;
            float o[3] = {g_ray[0], g_ray[1], g_ray[2]}, d[3] = {g_ray[3], g_ray[4], g_ray[5]};
            auto behind = [&](float t, int& state) -> bool {   // state: 1 in front, 0 behind, -1 off screen
                float v[3] = {(o[0] + d[0] * t) * sc, (o[1] + d[1] * t) * sc, (o[2] + d[2] * t) * sc};
                float cx = v[0] * P[0] + v[1] * P[4] + v[2] * P[8] + P[12];
                float cy = v[0] * P[1] + v[1] * P[5] + v[2] * P[9] + P[13];
                float cw = v[0] * P[3] + v[1] * P[7] + v[2] * P[11] + P[15];
                if (cw <= 1e-4f) { state = -1; return false; }
                float nx = cx / cw, ny = cy / cw;
                if (nx < -1 || nx > 1 || ny < -1 || ny > 1) { state = -1; return false; }
                UINT px = (UINT)((nx * 0.5f + 0.5f) * (g_sd.Width - 1)), py = (UINT)((0.5f - ny * 0.5f) * (g_sd.Height - 1));
                float n = ndcAt(px, py);
                if (n <= 0.f || n >= 1.f) { state = 1; return false; }   // sky: never behind
                state = v[2] <= surfZ(n) ? 0 : 1;   // more negative z = further away
                return state == 0;
            };
            float hit = 0;
            const float tMin = 3.f / 1.f;   // real metres: skip the player's own character
            int st = 1, prevSt = -2;
            float prevT = tMin;
            bool seenFront = false;
            for (float t = tMin; t < 400.f; t *= 1.035f) {
                behind(t, st);
                if (st == 1) seenFront = true;
                if (st == 0 && seenFront && prevSt == 1) {
                    float a = prevT, b = t;   // refine between the last point in front and this one
                    for (int i = 0; i < 6; ++i) { float mid = 0.5f * (a + b); int s2; if (behind(mid, s2)) b = mid; else a = mid; }
                    hit = b;
                    break;
                }
                prevSt = st; prevT = t;
            }
            g_hitT = hit;
            g_hitTick = GetTickCount();
            c->Unmap(g_stage[rd], 0);
            g_pend[rd] = false;
            static DWORD lt = 0;
            if (GetTickCount() - lt > 3000) {
                lt = GetTickCount();
                LOGI("depthprobe: centre %.2f m%s, aim ray hit %.2f m (ray o %.2f %.2f %.2f d %.2f %.2f %.2f)", dist, sky ? " (sky)" : "", hit,
                     o[0], o[1], o[2], d[0], d[1], d[2]);
            }
        }
    }
    // copy this frame's depth (a depth buffer can only be copied whole)
    D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)g_dsvRes)->GetDesc(&d);
    DXGI_FORMAT sf = StageFormat(d.Format);
    if (sf == DXGI_FORMAT_UNKNOWN) {
        static bool warned = false;
        if (!warned) { warned = true; LOGW("depthprobe: depth format %d not supported", (int)d.Format); }
        return;
    }
    if (d.Width != g_sd.Width || d.Height != g_sd.Height || sf != g_sd.Format) {
        for (auto& s : g_stage) if (s) { s->Release(); s = nullptr; }
        for (auto& p : g_pend) p = false;
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        g_sd = D3D11_TEXTURE2D_DESC{d.Width, d.Height, 1, 1, sf, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
        for (auto& s : g_stage) if (FAILED(dev->CreateTexture2D(&g_sd, nullptr, &s))) s = nullptr;
        dev->Release();
        LOGI("depthprobe: eye depth %ux%u format %d", d.Width, d.Height, (int)d.Format);
    }
    if (g_stage[g_pos]) {
        c->CopySubresourceRegion(g_stage[g_pos], 0, 0, 0, 0, g_dsvRes, 0, nullptr);
        g_pend[g_pos] = true;
        for (int i = 0; i < 16; ++i) g_projPend[g_pos][i] = P[i];
    }
    g_pos = (g_pos + 1) % kRing;
}

}
