#include <windows.h>
#include <d3d11_1.h>
#include <mutex>
#include "finalpass.h"
#include "cbfix.h"
#include "log.h"

namespace mea {

static ID3D11Resource* g_bb = nullptr;   // not owned
static uint64_t g_present = 0;
static std::mutex g_mx;
static FinalPassInfo g_info;
static bool g_logged = false;

void FinalPassSetBackBuffer(ID3D11Resource* bb, uint64_t presentIndex) { g_bb = bb; g_present = presentIndex; }

void FinalPassOnDraw(ID3D11DeviceContext* c) {
    if (!g_bb) return;
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* r = nullptr;
    rtv->GetResource(&r);
    rtv->Release();
    bool isBb = r == g_bb;
    if (r) r->Release();
    if (!isBb) return;
    ID3D11ShaderResourceView* srv[3] = {};
    c->PSGetShaderResources(0, 3, srv);
    bool match = false;
    if (srv[2]) {
        D3D11_SHADER_RESOURCE_VIEW_DESC d;
        srv[2]->GetDesc(&d);
        match = d.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE1D;
    }
    if (match) {
        float row[4] = {1, 1, 1, 0};
        ID3D11DeviceContext1* c1 = nullptr;
        ID3D11Buffer* cb = nullptr; UINT first = 0, num = 0;
        if (SUCCEEDED(c->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&c1))) {
            c1->PSGetConstantBuffers1(0, 1, &cb, &first, &num);
            c1->Release();
        } else c->PSGetConstantBuffers(0, 1, &cb);
        bool cbOk = cb && CbFixRead(cb, first * 16 + 32, row, 16);
        if (cb) cb->Release();
        std::lock_guard<std::mutex> l(g_mx);
        if (g_info.lut) g_info.lut->Release();
        if (g_info.ui) g_info.ui->Release();
        g_info.lut = srv[2]; srv[2] = nullptr;
        g_info.ui = srv[1]; srv[1] = nullptr;
        if (cbOk) { g_info.scale = row[0]; g_info.uiScale = row[1]; g_info.uiAlpha = row[2]; }
        g_info.valid = true;
        g_info.frame = g_present;
        if (!g_logged) {
            g_logged = true;
            LOGI("finalpass: found (scale %.3f, ui %.3f / %.3f, constants %s, ui layer %p)", row[0], row[1], row[2], cbOk ? "read" : "unknown",
                 (void*)g_info.ui);
        }
    }
    for (auto* v : srv) if (v) v->Release();
}

bool FinalPassGet(FinalPassInfo& out) {
    std::lock_guard<std::mutex> l(g_mx);
    if (!g_info.valid) return false;
    out = g_info;
    if (out.lut) out.lut->AddRef();
    if (out.ui) out.ui->AddRef();
    return true;
}

void FinalPassRelease(FinalPassInfo& fi) {
    if (fi.lut) fi.lut->Release();
    if (fi.ui) fi.ui->Release();
    fi.lut = fi.ui = nullptr;
}

}
