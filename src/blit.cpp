// Full-screen blit of the game's back buffer into an 8-bit sRGB swapchain image, for back buffer formats that
// cannot be copied directly (e.g. R10G10B10A2_UNORM). Runs in a private ID3DDeviceContextState so the game's
// bound pipeline state is untouched.
#include <windows.h>
#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <unordered_map>
#include "blit.h"
#include "log.h"

namespace mea { namespace blit {

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext1* g_ctx1 = nullptr;
static ID3DDeviceContextState* g_state = nullptr;
static ID3D11VertexShader* g_vs = nullptr;
static ID3D11PixelShader* g_psDecode = nullptr;   // gamma-encoded source -> linear (sRGB RTV re-encodes)
static ID3D11PixelShader* g_psPass = nullptr;     // linear source (float formats)
static ID3D11PixelShader* g_psTone = nullptr;     // eye views: the game's final tone step, then decode
static ID3D11Buffer* g_toneCb = nullptr;
static ID3D11PixelShader* g_psLut = nullptr;
static ID3D11PixelShader* g_psHud = nullptr;
static ID3D11PixelShader* g_psMatte = nullptr;
static ID3D11PixelShader* g_psCompose = nullptr;
static ID3D11SamplerState* g_sampLin = nullptr;
static ID3D11SamplerState* g_samp = nullptr;
static ID3D11RasterizerState* g_rs = nullptr;
// shader-readable copies of the sources, one per (size, format) - the back buffer and the eye views differ
struct CopySlot { ID3D11Texture2D* tex = nullptr; ID3D11ShaderResourceView* srv = nullptr; D3D11_TEXTURE2D_DESC desc{}; };
static CopySlot g_slots[4];
static ID3D11Texture2D* g_copy = nullptr;           // current slot (set by EnsureCopy)
static ID3D11ShaderResourceView* g_srv = nullptr;
static std::unordered_map<ID3D11Texture2D*, ID3D11RenderTargetView*> g_rtvs;
static bool g_ok = false, g_tried = false;

static const char* kHlsl = R"(
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V o; float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t; o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1); return o;
}
Texture2D tex : register(t0); SamplerState smp : register(s0);
Texture2D tex2 : register(t3); Texture2D tex3 : register(t4);
float3 decode(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
float4 psDecode(V i) : SV_Target { return float4(decode(saturate(tex.Sample(smp, i.uv).rgb)), 1); }
float4 psPass(V i) : SV_Target { return float4(tex.Sample(smp, i.uv).rgb, 1); }
cbuffer Tone : register(b0) { float4 tone; };
Texture1D lut : register(t1); SamplerState lsmp : register(s1);
// the game's final colour step (scale, per-channel 1D LUT), linear output for an sRGB target
float4 psLut(V i) : SV_Target {
    float3 c = tex.Sample(smp, i.uv).rgb * tone.x;
    return float4(saturate(float3(lut.Sample(lsmp, c.r).r, lut.Sample(lsmp, c.g).r, lut.Sample(lsmp, c.b).r)), 1);   // one-channel LUT
}
// the game's UI layer as premultiplied colour + coverage, as its final pass blends it: world * (1 - a z)^2 + rgb * y
float4 psHud(V i) : SV_Target {
    float4 u = tex.Sample(smp, i.uv);
    float k = 1 - u.a * tone.z;
    return float4(saturate(u.rgb * tone.y), saturate(1 - k * k));
}
// HUD matte from the UI drawn over black (t0) and over white (t1), both gamma-encoded: premultiplied linear for an
// sRGB target (alpha = 1 - (white - black))
float4 psMatte(V i) : SV_Target {
    float3 b = saturate(tex.Sample(smp, i.uv).rgb), w = saturate(tex2.Sample(smp, i.uv).rgb);
    float a = saturate(1 - dot(w - b, float3(0.3333, 0.3333, 0.3334)));
    if (a < 0.004) return 0;
    return float4(decode(saturate(b / a)) * a, a);
}
// monitor: world (t2) with the UI (black t0 / white t1) blended over it, gamma-encoded output for a UNORM target
float4 psCompose(V i) : SV_Target {
    float3 b = saturate(tex.Sample(smp, i.uv).rgb), w = saturate(tex2.Sample(smp, i.uv).rgb);
    float a = saturate(1 - dot(w - b, float3(0.3333, 0.3333, 0.3334)));
    return float4(saturate(tex3.Sample(smp, i.uv).rgb * (1 - a) + b), 1);
}
float4 psTone(V i) : SV_Target { float3 c = saturate(tex.Sample(smp, i.uv).rgb); return float4(decode(saturate(pow(tone.x * c, tone.y))), 1); }
)";

typedef HRESULT(WINAPI* PFN_Compile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);

static ID3DBlob* Compile(PFN_Compile c, const char* entry, const char* target) {
    ID3DBlob *b = nullptr, *err = nullptr;
    HRESULT hr = c(kHlsl, strlen(kHlsl), "meavr_blit", nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &b, &err);
    if (FAILED(hr)) LOGE("blit: compile %s failed: %s", entry, err ? (const char*)err->GetBufferPointer() : "?");
    if (err) err->Release();
    return b;
}

bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx) {
    if (g_tried) return g_ok;
    g_tried = true;
    g_dev = dev;
    ID3D11Device1* dev1 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&dev1)) ||
        FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_ctx1))) {
        LOGE("blit: D3D11.1 interfaces not available");
        if (dev1) dev1->Release();
        return false;
    }
    D3D_FEATURE_LEVEL fl = dev->GetFeatureLevel();
    HRESULT hr = dev1->CreateDeviceContextState(0, &fl, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_state);
    dev1->Release();
    if (FAILED(hr)) { LOGE("blit: CreateDeviceContextState failed 0x%08X", (unsigned)hr); return false; }

    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = dc ? (PFN_Compile)GetProcAddress(dc, "D3DCompile") : nullptr;
    if (!compile) { LOGE("blit: d3dcompiler_47.dll not available"); return false; }
    ID3DBlob* vs = Compile(compile, "vs", "vs_5_0");
    ID3DBlob* p1 = Compile(compile, "psDecode", "ps_5_0");
    ID3DBlob* p2 = Compile(compile, "psPass", "ps_5_0");
    ID3DBlob* p3 = Compile(compile, "psTone", "ps_5_0");
    ID3DBlob* p4 = Compile(compile, "psLut", "ps_5_0");
    ID3DBlob* p5 = Compile(compile, "psHud", "ps_5_0");
    ID3DBlob* p6 = Compile(compile, "psMatte", "ps_5_0");
    ID3DBlob* p7 = Compile(compile, "psCompose", "ps_5_0");
    if (p6) { dev->CreatePixelShader(p6->GetBufferPointer(), p6->GetBufferSize(), nullptr, &g_psMatte); p6->Release(); }
    if (p7) { dev->CreatePixelShader(p7->GetBufferPointer(), p7->GetBufferSize(), nullptr, &g_psCompose); p7->Release(); }
    if (!vs || !p1 || !p2 || !p3 || !p4 || !p5) return false;
    dev->CreatePixelShader(p4->GetBufferPointer(), p4->GetBufferSize(), nullptr, &g_psLut);
    dev->CreatePixelShader(p5->GetBufferPointer(), p5->GetBufferSize(), nullptr, &g_psHud);
    p4->Release(); p5->Release();
    dev->CreatePixelShader(p3->GetBufferPointer(), p3->GetBufferSize(), nullptr, &g_psTone);
    p3->Release();
    D3D11_BUFFER_DESC cbd{16, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    dev->CreateBuffer(&cbd, nullptr, &g_toneCb);
    dev->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g_vs);
    dev->CreatePixelShader(p1->GetBufferPointer(), p1->GetBufferSize(), nullptr, &g_psDecode);
    dev->CreatePixelShader(p2->GetBufferPointer(), p2->GetBufferSize(), nullptr, &g_psPass);
    vs->Release(); p1->Release(); p2->Release();

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;   // same size: point sampling keeps the image 1:1
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    dev->CreateSamplerState(&sd, &g_samp);
    D3D11_SAMPLER_DESC ld = sd;
    ld.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    dev->CreateSamplerState(&ld, &g_sampLin);
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    dev->CreateRasterizerState(&rd, &g_rs);
    g_ok = g_vs && g_psDecode && g_psPass && g_samp && g_rs;
    LOGI("blit: ready (%s)", g_ok ? "ok" : "FAILED");
    return g_ok;
}

static bool IsLinearFloat(DXGI_FORMAT f) {
    return f == DXGI_FORMAT_R16G16B16A16_FLOAT || f == DXGI_FORMAT_R16G16B16A16_TYPELESS || f == DXGI_FORMAT_R11G11B10_FLOAT ||
           f == DXGI_FORMAT_R32G32B32A32_FLOAT || f == DXGI_FORMAT_R32G32B32A32_TYPELESS;
}

static bool EnsureCopy(const D3D11_TEXTURE2D_DESC& s) {
    for (auto& c : g_slots)
        if (c.tex && c.desc.Width == s.Width && c.desc.Height == s.Height && c.desc.Format == s.Format) { g_copy = c.tex; g_srv = c.srv; return true; }
    CopySlot* slot = nullptr;
    for (auto& c : g_slots) if (!c.tex) { slot = &c; break; }
    if (!slot) { slot = &g_slots[0]; if (slot->srv) slot->srv->Release(); if (slot->tex) slot->tex->Release(); *slot = CopySlot(); }
    D3D11_TEXTURE2D_DESC d = s;
    d.MipLevels = 1; d.ArraySize = 1; d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.CPUAccessFlags = 0; d.MiscFlags = 0;
    switch (d.Format) {   // typeless sources: shader-readable typed copies
        case DXGI_FORMAT_R10G10B10A2_TYPELESS: d.Format = DXGI_FORMAT_R10G10B10A2_UNORM; break;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS: d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; break;
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; break;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS: d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; break;
        case DXGI_FORMAT_R32G32B32A32_TYPELESS: d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; break;
        default: break;
    }
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &slot->tex))) { LOGE("blit: copy texture failed (fmt %d)", (int)d.Format); return false; }
    if (FAILED(g_dev->CreateShaderResourceView(slot->tex, nullptr, &slot->srv))) {
        LOGE("blit: SRV failed (fmt %d)", (int)d.Format); slot->tex->Release(); slot->tex = nullptr; return false;
    }
    slot->desc = s;
    g_copy = slot->tex; g_srv = slot->srv;
    return true;
}

void Reset() {
    for (auto& c : g_slots) { if (c.srv) c.srv->Release(); if (c.tex) c.tex->Release(); c = CopySlot(); }
    g_copy = nullptr; g_srv = nullptr;
}

void ForgetTargets() {
    for (auto& kv : g_rtvs) kv.second->Release();
    g_rtvs.clear();
}

bool Draw(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& sd, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h, const float* tone) {
    if (!g_ok || !EnsureCopy(sd)) return false;
    ID3D11RenderTargetView*& rtv = g_rtvs[dst];
    if (!rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = rtvFmt;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateRenderTargetView(dst, &rv, &rtv))) { LOGE("blit: RTV fmt %d failed", (int)rtvFmt); g_rtvs.erase(dst); return false; }
    }
    g_ctx1->CopyResource(g_copy, src);   // works in either state; done before the swap
    bool useTone = tone && g_psTone && g_toneCb && !IsLinearFloat(sd.Format);
    if (useTone) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g_ctx1->Map(g_toneCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            float v[4] = {tone[0], tone[1], 0, 0};
            memcpy(m.pData, v, 16);
            g_ctx1->Unmap(g_toneCb, 0);
        } else useTone = false;
    }

    ID3DDeviceContextState* prev = nullptr;
    g_ctx1->SwapDeviceContextState(g_state, &prev);
    D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
    g_ctx1->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx1->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx1->OMSetDepthStencilState(nullptr, 0);
    g_ctx1->RSSetState(g_rs);
    g_ctx1->RSSetViewports(1, &vp);
    g_ctx1->IASetInputLayout(nullptr);
    g_ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx1->VSSetShader(g_vs, nullptr, 0);
    g_ctx1->PSSetShader(useTone ? g_psTone : IsLinearFloat(sd.Format) ? g_psPass : g_psDecode, nullptr, 0);
    if (useTone) g_ctx1->PSSetConstantBuffers(0, 1, &g_toneCb);
    g_ctx1->PSSetShaderResources(0, 1, &g_srv);
    g_ctx1->PSSetSamplers(0, 1, &g_samp);
    g_ctx1->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    g_ctx1->PSSetShaderResources(0, 1, &nullSrv);
    ID3D11RenderTargetView* nullRtv = nullptr;
    g_ctx1->OMSetRenderTargets(1, &nullRtv, nullptr);
    g_ctx1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
    return true;
}

static ID3D11RenderTargetView* Rtv(ID3D11Texture2D* dst, DXGI_FORMAT fmt) {
    ID3D11RenderTargetView*& rtv = g_rtvs[dst];
    if (!rtv) {
        D3D11_RENDER_TARGET_VIEW_DESC rv{};
        rv.Format = fmt;
        rv.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        if (FAILED(g_dev->CreateRenderTargetView(dst, &rv, &rtv))) { LOGE("blit: RTV fmt %d failed", (int)fmt); g_rtvs.erase(dst); return nullptr; }
    }
    return rtv;
}

static bool SetTone(float a, float b, float c) {
    D3D11_MAPPED_SUBRESOURCE m;
    if (!g_toneCb || FAILED(g_ctx1->Map(g_toneCb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
    float v[4] = {a, b, c, 0};
    memcpy(m.pData, v, 16);
    g_ctx1->Unmap(g_toneCb, 0);
    return true;
}

static void Run5(ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11PixelShader* ps, ID3D11ShaderResourceView* const* srv5);
static void Run(ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11PixelShader* ps, ID3D11ShaderResourceView* t0, ID3D11ShaderResourceView* t1) {
    ID3D11ShaderResourceView* v[5] = {t0, t1, nullptr, nullptr, nullptr};
    Run5(rtv, w, h, ps, v);
}
static void Run5(ID3D11RenderTargetView* rtv, UINT w, UINT h, ID3D11PixelShader* ps, ID3D11ShaderResourceView* const* srv5) {
    ID3DDeviceContextState* prev = nullptr;
    g_ctx1->SwapDeviceContextState(g_state, &prev);
    D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
    g_ctx1->OMSetRenderTargets(1, &rtv, nullptr);
    g_ctx1->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
    g_ctx1->OMSetDepthStencilState(nullptr, 0);
    g_ctx1->RSSetState(g_rs);
    g_ctx1->RSSetViewports(1, &vp);
    g_ctx1->IASetInputLayout(nullptr);
    g_ctx1->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_ctx1->VSSetShader(g_vs, nullptr, 0);
    g_ctx1->PSSetShader(ps, nullptr, 0);
    g_ctx1->PSSetShaderResources(0, 5, srv5);
    ID3D11SamplerState* ss[2] = {g_samp, g_sampLin};
    g_ctx1->PSSetSamplers(0, 2, ss);
    g_ctx1->PSSetConstantBuffers(0, 1, &g_toneCb);
    g_ctx1->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[5] = {};
    g_ctx1->PSSetShaderResources(0, 5, nulls);
    ID3D11RenderTargetView* nullRtv = nullptr;
    g_ctx1->OMSetRenderTargets(1, &nullRtv, nullptr);
    g_ctx1->SwapDeviceContextState(prev, nullptr);
    if (prev) prev->Release();
}

bool DrawLut(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& sd, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h,
             ID3D11ShaderResourceView* lutSrv, float scale) {
    if (!g_ok || !g_psLut || !lutSrv || !EnsureCopy(sd)) return false;
    ID3D11RenderTargetView* rtv = Rtv(dst, rtvFmt);
    if (!rtv || !SetTone(scale, 0, 0)) return false;
    g_ctx1->CopyResource(g_copy, src);
    Run(rtv, w, h, g_psLut, g_srv, lutSrv);
    return true;
}

bool DrawHud(ID3D11ShaderResourceView* ui, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h, float uiScale, float uiAlpha) {
    if (!g_ok || !g_psHud || !ui) return false;
    ID3D11RenderTargetView* rtv = Rtv(dst, rtvFmt);
    if (!rtv || !SetTone(0, uiScale, uiAlpha)) return false;
    Run(rtv, w, h, g_psHud, ui, nullptr);
    return true;
}

// SRVs for our own textures (cached per texture)
static std::unordered_map<ID3D11Texture2D*, ID3D11ShaderResourceView*> g_srvs;
static ID3D11ShaderResourceView* Srv(ID3D11Texture2D* t) {
    ID3D11ShaderResourceView*& v = g_srvs[t];
    if (!v && FAILED(g_dev->CreateShaderResourceView(t, nullptr, &v))) { v = nullptr; g_srvs.erase(t); }
    return v;
}
void ForgetTexture(ID3D11Texture2D* t) {
    auto it = g_srvs.find(t);
    if (it != g_srvs.end()) { if (it->second) it->second->Release(); g_srvs.erase(it); }
    auto r = g_rtvs.find(t);
    if (r != g_rtvs.end()) { if (r->second) r->second->Release(); g_rtvs.erase(r); }
}
bool DrawMatte(ID3D11Texture2D* black, ID3D11Texture2D* white, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h) {
    if (!g_ok || !g_psMatte) return false;
    ID3D11RenderTargetView* rtv = Rtv(dst, rtvFmt);
    ID3D11ShaderResourceView* b = Srv(black), *wt = Srv(white);
    if (!rtv || !b || !wt) return false;
    ID3D11ShaderResourceView* v[5] = {b, nullptr, nullptr, wt, nullptr};
    Run5(rtv, w, h, g_psMatte, v);
    return true;
}
bool Compose(ID3D11Texture2D* world, ID3D11Texture2D* black, ID3D11Texture2D* white, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h) {
    if (!g_ok || !g_psCompose) return false;
    ID3D11RenderTargetView* rtv = Rtv(dst, rtvFmt);
    ID3D11ShaderResourceView* b = Srv(black), *wt = Srv(white), *wd = Srv(world);
    if (!rtv || !b || !wt || !wd) return false;
    ID3D11ShaderResourceView* v[5] = {b, nullptr, nullptr, wt, wd};
    Run5(rtv, w, h, g_psCompose, v);
    return true;
}

}} // namespace mea::blit
