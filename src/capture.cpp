// One-frame D3D11 capture (F10 or capture_frame = N): every context call that matters for finding the camera and the
// render passes, written to MEAVR\cap_NNN\ as trace.txt (call sequence), shaders.txt (hash + reflection of every
// shader used), cb.bin (constant buffer contents, offsets in the trace) and resources.txt.
// Outside a capture each hook costs one flag test.
#include "depthprobe.h"
#include "vrs.h"
#include <windows.h>
#include <d3d11_1.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <vector>
#include <string>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <cstdio>
#include "capture.h"
#include "engine.h"
#include "cbfix.h"
#include "finalpass.h"
#include "camscan.h"
#include "vtbl.h"
#include "globals.h"
#include "settings.h"
#include "log.h"
#include "MinHook.h"

namespace mea {

// ------------------------------------------------------------------ shader registry (always on, cheap)
struct ShaderInfo { uint64_t hash; char kind; std::vector<uint8_t> code; };
static std::mutex g_shMx;
static std::unordered_map<void*, ShaderInfo> g_shaders;
static volatile LONG g_shaderCount = 0;

static uint64_t Fnv(const void* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) { h ^= ((const uint8_t*)p)[i]; h *= 1099511628211ull; }
    return h;
}
static void RegisterShader(void* obj, char kind, const void* code, SIZE_T n) {
    if (!obj || !code) return;
    ShaderInfo si{Fnv(code, n), kind, {}};
    if (g_cfg.captureKeepBytecode) si.code.assign((const uint8_t*)code, (const uint8_t*)code + n);
    std::lock_guard<std::mutex> l(g_shMx);
    g_shaders[obj] = std::move(si);
    InterlockedIncrement(&g_shaderCount);
}
// debug draw skipping by vertex shader hash
static std::mutex g_skMx;
static std::vector<uint64_t> g_skip;
static volatile LONG g_skipOn = 0;
static std::unordered_map<void*, uint64_t> g_curVs;   // context -> bound VS hash
void CaptureSetSkipList(const std::string& list) {
    std::lock_guard<std::mutex> l(g_skMx);
    g_skip.clear();
    size_t i = 0;
    while (i < list.size()) {
        size_t j = list.find_first_of(", ", i);
        if (j == std::string::npos) j = list.size();
        if (j > i) g_skip.push_back(strtoull(list.substr(i, j - i).c_str(), nullptr, 16));
        i = j + 1;
    }
    InterlockedExchange(&g_skipOn, g_skip.empty() ? 0 : 1);
}
// debug: skip a range of draws inside the n-th 4-target pass of the frame (debug_skip_range = pass,first,count)
static volatile LONG g_mrtPass = -1, g_passDraw = 0;
static int g_skipPass = -1, g_skipFirst = 0, g_skipCount = 0;
void CaptureSetSkipRange(int pass, int first, int count) { g_skipPass = pass; g_skipFirst = first; g_skipCount = count; }
void CaptureFrameStart() { g_mrtPass = -1; g_passDraw = 0; }
static void NoteRT(UINT n, ID3D11DepthStencilView* ds) {
    if (g_skipPass < 0) return;
    if (n == 4 && ds) { ++g_mrtPass; g_passDraw = 0; }
    else if (n != 4) g_passDraw = -1000000;   // left the pass
}
static bool SkipDraw(ID3D11DeviceContext* c) {
    if (g_skipPass >= 0 && g_mrtPass == g_skipPass && g_passDraw >= 0) {
        LONG i = g_passDraw++;
        if (i >= g_skipFirst && i < g_skipFirst + g_skipCount) return true;
    }
    if (!g_skipOn) return false;
    std::lock_guard<std::mutex> l(g_skMx);
    auto it = g_curVs.find(c);
    if (it == g_curVs.end()) return false;
    for (uint64_t h : g_skip) if (h == it->second) return true;
    return false;
}
static const ShaderInfo* FindShader(void* obj);
static void NoteVs(ID3D11DeviceContext* c, void* s) {
    if (!g_skipOn) return;
    const ShaderInfo* si = s ? FindShader(s) : nullptr;
    std::lock_guard<std::mutex> l(g_skMx);
    g_curVs[c] = si ? si->hash : 0;
}
static const ShaderInfo* FindShader(void* obj) {
    std::lock_guard<std::mutex> l(g_shMx);
    auto it = g_shaders.find(obj);
    return it == g_shaders.end() ? nullptr : &it->second;
}

// ------------------------------------------------------------------ capture state
static volatile LONG g_active = 0;
static volatile LONG g_request = 0;
static std::mutex g_mx;
static std::string g_trace;
static std::vector<uint8_t> g_blob;
static std::unordered_map<void*, int> g_ids;          // resource / view / context / command list -> small id
static std::string g_resText;
static std::unordered_set<void*> g_usedShaders;
static std::unordered_map<std::string, void*> g_mapped;   // "ctx:res" -> mapped pointer
static ID3D11DeviceContext* g_imm = nullptr;
static int g_capIndex = 0;
static uint64_t g_events = 0;
static size_t g_blobLimit = 0;

static bool On() { return g_active != 0; }

static void T(const char* fmt, ...) {
    char b[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    g_trace += b;
    ++g_events;
}

static const char* Fmt(DXGI_FORMAT f) {
    static char b[16];
    snprintf(b, sizeof(b), "fmt%d", (int)f);
    return b;
}

static void DescribeResource(ID3D11Resource* r, int id) {
    D3D11_RESOURCE_DIMENSION dim;
    r->GetType(&dim);
    char b[256];
    if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)r)->GetDesc(&d);
        snprintf(b, sizeof(b), "r%d tex2d %ux%u %s mips%u arr%u ms%u bind0x%X usage%d misc0x%X\n", id, d.Width, d.Height, Fmt(d.Format),
                 d.MipLevels, d.ArraySize, d.SampleDesc.Count, d.BindFlags, (int)d.Usage, d.MiscFlags);
    } else if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
        snprintf(b, sizeof(b), "r%d buffer %u bytes bind0x%X usage%d cpu0x%X misc0x%X stride%u\n", id, d.ByteWidth, d.BindFlags, (int)d.Usage,
                 d.CPUAccessFlags, d.MiscFlags, d.StructureByteStride);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE3D) {
        D3D11_TEXTURE3D_DESC d; ((ID3D11Texture3D*)r)->GetDesc(&d);
        snprintf(b, sizeof(b), "r%d tex3d %ux%ux%u %s bind0x%X\n", id, d.Width, d.Height, d.Depth, Fmt(d.Format), d.BindFlags);
    } else snprintf(b, sizeof(b), "r%d dim%d\n", id, (int)dim);
    g_resText += b;
}

// ids: resources get "r" ids described once; views map to their resource id
static int Rid(ID3D11Resource* r) {
    if (!r) return 0;
    auto it = g_ids.find(r);
    if (it != g_ids.end()) return it->second;
    int id = (int)g_ids.size() + 1;
    g_ids[r] = id;
    DescribeResource(r, id);
    return id;
}
static int ViewRid(ID3D11View* v) {
    if (!v) return 0;
    ID3D11Resource* r = nullptr;
    v->GetResource(&r);
    int id = Rid(r);
    if (r) r->Release();
    return id;
}
static int Oid(void* p) {   // contexts, command lists
    if (!p) return 0;
    auto it = g_ids.find(p);
    if (it != g_ids.end()) return it->second;
    int id = (int)g_ids.size() + 1;
    g_ids[p] = id;
    return id;
}
static const char* Ctx(ID3D11DeviceContext* c) {
    static thread_local char b[24];
    if (c == g_imm) return "imm";
    snprintf(b, sizeof(b), "d%d", Oid(c));
    return b;
}
static void UseShader(void* s, char kind) {
    const ShaderInfo* si = s ? FindShader(s) : nullptr;
    if (s) g_usedShaders.insert(s);
    T(" %c=%016llx", kind, si ? (unsigned long long)si->hash : 0ull);
}
static void AddBlob(const void* p, size_t n, size_t& off) {
    off = g_blob.size();
    if (g_blob.size() + n > g_blobLimit) { off = (size_t)-1; return; }
    g_blob.insert(g_blob.end(), (const uint8_t*)p, (const uint8_t*)p + n);
}

// ------------------------------------------------------------------ hooks
#define DECL(name, ret, ...) typedef ret(STDMETHODCALLTYPE* PFN_##name)(__VA_ARGS__); static PFN_##name o_##name[2] = {};
DECL(CreateVertexShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**)
DECL(CreatePixelShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**)
DECL(CreateComputeShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11ComputeShader**)
DECL(CreateGeometryShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11GeometryShader**)
DECL(CreateHullShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11HullShader**)
DECL(CreateDomainShader, HRESULT, ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11DomainShader**)
DECL(SetCB, void, ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*)
DECL(SetCB1, void, ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*)
DECL(SetShader, void, ID3D11DeviceContext*, ID3D11DeviceChild*, ID3D11ClassInstance* const*, UINT)
DECL(Map, HRESULT, ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*)
DECL(Unmap, void, ID3D11DeviceContext*, ID3D11Resource*, UINT)
DECL(UpdateSubresource, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT)
DECL(UpdateSubresource1, void, ID3D11DeviceContext1*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT, UINT)
DECL(OMSetRenderTargets, void, ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*)
DECL(OMSetRTUAV, void, ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*, UINT, UINT, ID3D11UnorderedAccessView* const*, const UINT*)
DECL(RSSetViewports, void, ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*)
DECL(ClearRTV, void, ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT*)
DECL(ClearDSV, void, ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8)
DECL(Draw, void, ID3D11DeviceContext*, UINT, UINT)
DECL(DrawIndexed, void, ID3D11DeviceContext*, UINT, UINT, INT)
DECL(DrawInstanced, void, ID3D11DeviceContext*, UINT, UINT, UINT, UINT)
DECL(DrawIndexedInstanced, void, ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT)
DECL(DrawIndexedInstancedIndirect, void, ID3D11DeviceContext*, ID3D11Buffer*, UINT)
DECL(DrawInstancedIndirect, void, ID3D11DeviceContext*, ID3D11Buffer*, UINT)
DECL(Dispatch, void, ID3D11DeviceContext*, UINT, UINT, UINT)
DECL(DispatchIndirect, void, ID3D11DeviceContext*, ID3D11Buffer*, UINT)
DECL(ExecuteCommandList, void, ID3D11DeviceContext*, ID3D11CommandList*, BOOL)
DECL(FinishCommandList, HRESULT, ID3D11DeviceContext*, BOOL, ID3D11CommandList**)
DECL(CopyResource, void, ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*)
DECL(CopySubresourceRegion, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*)
DECL(ResolveSubresource, void, ID3D11DeviceContext*, ID3D11Resource*, UINT, ID3D11Resource*, UINT, DXGI_FORMAT)
#undef DECL

// one trampoline per shader stage for SetCB / SetCB1 / SetShader
// [context kind][stage]. Immediate and deferred contexts have different implementations, so each needs its own
// trampoline (one shared pointer would send immediate-context calls into the deferred implementation).
static PFN_SetCB o_SetCB_[2][6];
static PFN_SetCB1 o_SetCB1_[2][3];
static PFN_SetShader o_SetShader_[2][4];
static const char kStage[] = "VPCGHD";

#define CREATE_HOOK(Name, kind, Iface)                                                                         \
    static HRESULT STDMETHODCALLTYPE H_##Name(ID3D11Device* d, const void* c, SIZE_T n, ID3D11ClassLinkage* l, Iface** out) { \
        HRESULT hr = o_##Name[0](d, c, n, l, out);                                                                \
        if (SUCCEEDED(hr) && out) RegisterShader(*out, kind, c, n);                                            \
        return hr;                                                                                             \
    }
CREATE_HOOK(CreateVertexShader, 'V', ID3D11VertexShader)
CREATE_HOOK(CreatePixelShader, 'P', ID3D11PixelShader)
CREATE_HOOK(CreateComputeShader, 'C', ID3D11ComputeShader)
CREATE_HOOK(CreateGeometryShader, 'G', ID3D11GeometryShader)
CREATE_HOOK(CreateHullShader, 'H', ID3D11HullShader)
CREATE_HOOK(CreateDomainShader, 'D', ID3D11DomainShader)

template <int K, int S>
static void STDMETHODCALLTYPE H_SetCB(ID3D11DeviceContext* c, UINT slot, UINT n, ID3D11Buffer* const* b) {
    if (On() && b) {
        std::lock_guard<std::mutex> l(g_mx);
        T("%s %cSCB %u:", Ctx(c), kStage[S], slot);
        for (UINT i = 0; i < n; ++i) T(" r%d", Rid(b[i]));
        T("\n");
    }
    o_SetCB_[K][S](c, slot, n, b);
}
template <int K, int S>
static void STDMETHODCALLTYPE H_SetCB1(ID3D11DeviceContext1* c, UINT slot, UINT n, ID3D11Buffer* const* b, const UINT* first, const UINT* num) {
    if (On() && b) {
        std::lock_guard<std::mutex> l(g_mx);
        T("%s %cSCB1 %u:", Ctx(c), kStage[S], slot);
        for (UINT i = 0; i < n; ++i) T(" r%d@%u+%u", Rid(b[i]), first ? first[i] * 16 : 0, num ? num[i] * 16 : 0);
        T("\n");
    }
    if (K == 0 && S < 2 && g_cfg.vrMode == 2 && CbFixSetCB1(c, slot, n, b, first, num, o_SetCB1_[K][S])) return;
    o_SetCB1_[K][S](c, slot, n, b, first, num);
}
template <int K, int S>
static void STDMETHODCALLTYPE H_SetShader(ID3D11DeviceContext* c, ID3D11DeviceChild* s, ID3D11ClassInstance* const* ci, UINT n) {
    if (On()) {
        std::lock_guard<std::mutex> l(g_mx);
        T("%s %cSSET", Ctx(c), kStage[S]);
        UseShader(s, kStage[S]);
        T("\n");
    }
    if (S == 0) NoteVs(c, s);
    o_SetShader_[K][S](c, s, ci, n);
}

template <int K> static HRESULT STDMETHODCALLTYPE H_Map(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m) {
    HRESULT hr = o_Map[K](c, r, sub, t, f, m);
    if (K == 0 && SUCCEEDED(hr) && m && (t == D3D11_MAP_WRITE_DISCARD || t == D3D11_MAP_WRITE_NO_OVERWRITE)) CbFixNoteMap(r, m->pData);
    if ((On() || CamScanArmed()) && SUCCEEDED(hr) && m) {
        std::lock_guard<std::mutex> l(g_mx);
        char k[48]; snprintf(k, sizeof(k), "%p:%p", (void*)c, (void*)r);
        g_mapped[k] = m->pData;
        T("%s MAP r%d type%d\n", Ctx(c), Rid(r), (int)t);
    }
    return hr;
}
template <int K> static void STDMETHODCALLTYPE H_Unmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub) {
    if (CamScanArmed() && !On()) {
        std::lock_guard<std::mutex> l(g_mx);
        char k[48]; snprintf(k, sizeof(k), "%p:%p", (void*)c, (void*)r);
        auto it = g_mapped.find(k);
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (it != g_mapped.end() && dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
            if (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) CamScanInspect(it->second, d.ByteWidth);
        }
        if (it != g_mapped.end()) g_mapped.erase(it);
    }
    if (On()) {
        std::lock_guard<std::mutex> l(g_mx);
        char k[48]; snprintf(k, sizeof(k), "%p:%p", (void*)c, (void*)r);
        auto it = g_mapped.find(k);
        if (CamScanArmed() && it != g_mapped.end()) {
            D3D11_RESOURCE_DIMENSION dm; r->GetType(&dm);
            if (dm == D3D11_RESOURCE_DIMENSION_BUFFER) { D3D11_BUFFER_DESC bd; ((ID3D11Buffer*)r)->GetDesc(&bd); if (bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER) CamScanInspect(it->second, bd.ByteWidth); }
        }
        D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
        if (it != g_mapped.end() && dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
            size_t n = d.ByteWidth;
            if (n > (size_t)g_cfg.captureCbMax) n = g_cfg.captureCbMax;
            size_t off;
            if ((d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) || g_cfg.captureAllBuffers) {
                AddBlob(it->second, n, off);
                T("%s UNMAP r%d data@%lld+%zu\n", Ctx(c), Rid(r), (long long)off, n);
            } else T("%s UNMAP r%d\n", Ctx(c), Rid(r));
            g_mapped.erase(it);
        } else T("%s UNMAP r%d\n", Ctx(c), Rid(r));
    }
    o_Unmap[K](c, r, sub);
}
static void LogUpdate(ID3D11DeviceContext* c, ID3D11Resource* r, const D3D11_BOX* box, const void* data, UINT rowPitch) {
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER && data) {
        D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
        size_t a = box ? box->left : 0, b = box ? box->right : d.ByteWidth;
        size_t n = b > a ? b - a : 0;
        if (n > (size_t)g_cfg.captureCbMax) n = g_cfg.captureCbMax;
        size_t off;
        AddBlob(data, n, off);
        T("%s UPDATE r%d [%zu..%zu) data@%lld+%zu\n", Ctx(c), Rid(r), a, b, (long long)off, n);
    } else T("%s UPDATE r%d (texture)\n", Ctx(c), Rid(r));
}
template <int K> static void STDMETHODCALLTYPE H_UpdateSubresource(ID3D11DeviceContext* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp) {
    if (On()) { std::lock_guard<std::mutex> l(g_mx); LogUpdate(c, r, box, data, rp); }
    o_UpdateSubresource[K](c, r, sub, box, data, rp, dp);
}
template <int K> static void STDMETHODCALLTYPE H_UpdateSubresource1(ID3D11DeviceContext1* c, ID3D11Resource* r, UINT sub, const D3D11_BOX* box, const void* data, UINT rp, UINT dp, UINT fl) {
    if (On()) { std::lock_guard<std::mutex> l(g_mx); LogUpdate(c, r, box, data, rp); }
    o_UpdateSubresource1[K](c, r, sub, box, data, rp, dp, fl);
}
// call stack as exe RVAs (frames outside the exe are skipped), for finding the engine's render structure
static void TStack() {
    void* fr[40];
    USHORT n = RtlCaptureStackBackTrace(2, 40, fr, nullptr);
    uintptr_t base = (uintptr_t)GetModuleHandleW(nullptr);
    T("   @");
    int shown = 0;
    for (USHORT i = 0; i < n && shown < 24; ++i) {
        uintptr_t a = (uintptr_t)fr[i];
        if (a >= base + 0x2ECE000 && a < base + 0x4F30000) { T(" %X", (unsigned)(a - base)); ++shown; }
    }
    T("\n");
}

static void LogRT(ID3D11DeviceContext* c, const char* what, UINT n, ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* ds) {
    T("%s %s", Ctx(c), what);
    for (UINT i = 0; i < n; ++i) T(" rt%u=r%d", i, rt ? ViewRid(rt ? rt[i] : nullptr) : 0);
    T(" ds=r%d\n", ViewRid(ds));
    if (g_cfg.captureStacks) TStack();
}
template <int K> static void STDMETHODCALLTYPE H_OMSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* ds) {
    if (On()) { std::lock_guard<std::mutex> l(g_mx); LogRT(c, "OMRT", n, rt, ds); }
    if (K == 0) NoteRT(n, ds);
    o_OMSetRenderTargets[K](c, n, rt, ds);
    if (K == 0) VrsOnSetRenderTargets(c, n, rt, ds);
    if (K == 0 && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) DepthProbeNoteDSV(c, ds);
}
template <int K> static void STDMETHODCALLTYPE H_OMSetRTUAV(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rt, ID3D11DepthStencilView* ds, UINT us, UINT un,
                                           ID3D11UnorderedAccessView* const* u, const UINT* ic) {
    if (On()) {
        std::lock_guard<std::mutex> l(g_mx);
        if (n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) LogRT(c, "OMRTUAV", n, rt, ds);
        if (u && un != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) { T("%s OMUAV %u:", Ctx(c), us); for (UINT i = 0; i < un; ++i) T(" r%d", ViewRid(u[i])); T("\n"); }
    }
    o_OMSetRTUAV[K](c, n, rt, ds, us, un, u, ic);
    if (K == 0) VrsOnSetRenderTargets(c, n, rt, ds);
    if (K == 0 && n != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) DepthProbeNoteDSV(c, ds);
}
template <int K> static void STDMETHODCALLTYPE H_RSSetViewports(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    if (On() && n && v) { std::lock_guard<std::mutex> l(g_mx); T("%s VP %.0f,%.0f %.0fx%.0f z%.2f-%.2f\n", Ctx(c), v[0].TopLeftX, v[0].TopLeftY, v[0].Width, v[0].Height, v[0].MinDepth, v[0].MaxDepth); }
    o_RSSetViewports[K](c, n, v);
}
template <int K> static void STDMETHODCALLTYPE H_ClearRTV(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT* col) {
    if (On()) { std::lock_guard<std::mutex> l(g_mx); T("%s CLEARRT r%d %.2f %.2f %.2f %.2f\n", Ctx(c), ViewRid(v), col[0], col[1], col[2], col[3]); }
    o_ClearRTV[K](c, v, col);
}
template <int K> static void STDMETHODCALLTYPE H_ClearDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, UINT f, FLOAT d, UINT8 s) {
    if (On()) { std::lock_guard<std::mutex> l(g_mx); T("%s CLEARDS r%d flags%u depth%.3f st%u\n", Ctx(c), ViewRid(v), f, d, s); if (g_cfg.captureStacks) TStack(); }
    o_ClearDSV[K](c, v, f, d, s);
}
#define DRAWLOG(fmt, ...) if (On()) { std::lock_guard<std::mutex> l(g_mx); T("%s " fmt "\n", Ctx(c), __VA_ARGS__); }
template <int K> static void STDMETHODCALLTYPE H_Draw(ID3D11DeviceContext* c, UINT v, UINT s) { if (K == 0 && v == 3) FinalPassOnDraw(c); if (SkipDraw(c)) return; DRAWLOG("DRAW v%u", v); o_Draw[K](c, v, s); if (K == 0) EngineAfterDraw(c, v); }
template <int K> static void STDMETHODCALLTYPE H_DrawIndexed(ID3D11DeviceContext* c, UINT i, UINT s, INT b) { if (SkipDraw(c)) return; DRAWLOG("DRAWIX i%u", i); o_DrawIndexed[K](c, i, s, b); }
template <int K> static void STDMETHODCALLTYPE H_DrawInstanced(ID3D11DeviceContext* c, UINT v, UINT n, UINT s, UINT si) { if (SkipDraw(c)) return; DRAWLOG("DRAWINST v%u n%u", v, n); o_DrawInstanced[K](c, v, n, s, si); }
template <int K> static void STDMETHODCALLTYPE H_DrawIndexedInstanced(ID3D11DeviceContext* c, UINT i, UINT n, UINT s, INT b, UINT si) { if (SkipDraw(c)) return; DRAWLOG("DRAWIXINST i%u n%u", i, n); o_DrawIndexedInstanced[K](c, i, n, s, b, si); }
template <int K> static void STDMETHODCALLTYPE H_DrawIndexedInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) { if (SkipDraw(c)) return; DRAWLOG("DRAWIXINDIRECT r%d", Rid(b)); o_DrawIndexedInstancedIndirect[K](c, b, o); }
template <int K> static void STDMETHODCALLTYPE H_DrawInstancedIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) { if (SkipDraw(c)) return; DRAWLOG("DRAWINDIRECT r%d", Rid(b)); o_DrawInstancedIndirect[K](c, b, o); }
template <int K> static void STDMETHODCALLTYPE H_Dispatch(ID3D11DeviceContext* c, UINT x, UINT y, UINT z) { DRAWLOG("DISPATCH %u %u %u", x, y, z); o_Dispatch[K](c, x, y, z); }
template <int K> static void STDMETHODCALLTYPE H_DispatchIndirect(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT o) { DRAWLOG("DISPATCHINDIRECT r%d", Rid(b)); o_DispatchIndirect[K](c, b, o); }
template <int K> static void STDMETHODCALLTYPE H_ExecuteCommandList(ID3D11DeviceContext* c, ID3D11CommandList* cl, BOOL r) { DRAWLOG("EXEC cl%d restore%d", Oid(cl), (int)r); o_ExecuteCommandList[K](c, cl, r); }
template <int K> static HRESULT STDMETHODCALLTYPE H_FinishCommandList(ID3D11DeviceContext* c, BOOL r, ID3D11CommandList** cl) {
    HRESULT hr = o_FinishCommandList[K](c, r, cl);
    if (On() && SUCCEEDED(hr) && cl) { std::lock_guard<std::mutex> l(g_mx); T("%s FINISH -> cl%d\n", Ctx(c), Oid(*cl)); }
    return hr;
}
template <int K> static void STDMETHODCALLTYPE H_CopyResource(ID3D11DeviceContext* c, ID3D11Resource* d, ID3D11Resource* s) { DRAWLOG("COPY r%d <- r%d", Rid(d), Rid(s)); o_CopyResource[K](c, d, s); }
template <int K> static void STDMETHODCALLTYPE H_CopySubresourceRegion(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* s, UINT ss, const D3D11_BOX* b) {
    DRAWLOG("COPYSUB r%d.%u <- r%d.%u", Rid(d), ds, Rid(s), ss); o_CopySubresourceRegion[K](c, d, ds, x, y, z, s, ss, b);
}
template <int K> static void STDMETHODCALLTYPE H_ResolveSubresource(ID3D11DeviceContext* c, ID3D11Resource* d, UINT ds, ID3D11Resource* s, UINT ss, DXGI_FORMAT f) {
    DRAWLOG("RESOLVE r%d <- r%d", Rid(d), Rid(s)); o_ResolveSubresource[K](c, d, ds, s, ss, f);
}

// ------------------------------------------------------------------ install
static bool Hook(void** vt, int slot, void* fn, void** orig, const char* name) {
    void* target = vt[slot];
    MH_STATUS s = MH_CreateHook(target, fn, orig);
    if (s == MH_ERROR_ALREADY_CREATED) {
        // the same function is behind another slot or the other context kind: that hook already handles every call to it
        LOGD("capture: %s at %p already hooked (shared implementation)", name, target);
        return true;
    }
    if (s != MH_OK || MH_EnableHook(target) != MH_OK) { LOGE("capture: hook %s failed (%d)", name, (int)s); return false; }
    return true;
}

template <int K>
static int InstallCtx(void** v) {
    int n = 0;
#define HC(m, fn, o) n += Hook(v, VT_CTX_##m, (void*)fn, (void**)&o, #m)
    HC(VSSetConstantBuffers, (H_SetCB<K, 0>), o_SetCB_[K][0]); HC(PSSetConstantBuffers, (H_SetCB<K, 1>), o_SetCB_[K][1]);
    HC(CSSetConstantBuffers, (H_SetCB<K, 2>), o_SetCB_[K][2]); HC(GSSetConstantBuffers, (H_SetCB<K, 3>), o_SetCB_[K][3]);
    HC(HSSetConstantBuffers, (H_SetCB<K, 4>), o_SetCB_[K][4]); HC(DSSetConstantBuffers, (H_SetCB<K, 5>), o_SetCB_[K][5]);
    HC(VSSetConstantBuffers1, (H_SetCB1<K, 0>), o_SetCB1_[K][0]); HC(PSSetConstantBuffers1, (H_SetCB1<K, 1>), o_SetCB1_[K][1]);
    HC(CSSetConstantBuffers1, (H_SetCB1<K, 2>), o_SetCB1_[K][2]);
    HC(VSSetShader, (H_SetShader<K, 0>), o_SetShader_[K][0]); HC(PSSetShader, (H_SetShader<K, 1>), o_SetShader_[K][1]);
    HC(CSSetShader, (H_SetShader<K, 2>), o_SetShader_[K][2]); HC(GSSetShader, (H_SetShader<K, 3>), o_SetShader_[K][3]);
    HC(Map, H_Map<K>, o_Map[K]); HC(Unmap, H_Unmap<K>, o_Unmap[K]);
    HC(UpdateSubresource, H_UpdateSubresource<K>, o_UpdateSubresource[K]); HC(UpdateSubresource1, H_UpdateSubresource1<K>, o_UpdateSubresource1[K]);
    HC(OMSetRenderTargets, H_OMSetRenderTargets<K>, o_OMSetRenderTargets[K]);
    HC(OMSetRenderTargetsAndUnorderedAccessViews, H_OMSetRTUAV<K>, o_OMSetRTUAV[K]);
    HC(RSSetViewports, H_RSSetViewports<K>, o_RSSetViewports[K]);
    HC(ClearRenderTargetView, H_ClearRTV<K>, o_ClearRTV[K]); HC(ClearDepthStencilView, H_ClearDSV<K>, o_ClearDSV[K]);
    HC(Draw, H_Draw<K>, o_Draw[K]); HC(DrawIndexed, H_DrawIndexed<K>, o_DrawIndexed[K]); HC(DrawInstanced, H_DrawInstanced<K>, o_DrawInstanced[K]);
    HC(DrawIndexedInstanced, H_DrawIndexedInstanced<K>, o_DrawIndexedInstanced[K]);
    HC(DrawIndexedInstancedIndirect, H_DrawIndexedInstancedIndirect<K>, o_DrawIndexedInstancedIndirect[K]);
    HC(DrawInstancedIndirect, H_DrawInstancedIndirect<K>, o_DrawInstancedIndirect[K]);
    HC(Dispatch, H_Dispatch<K>, o_Dispatch[K]); HC(DispatchIndirect, H_DispatchIndirect<K>, o_DispatchIndirect[K]);
    HC(ExecuteCommandList, H_ExecuteCommandList<K>, o_ExecuteCommandList[K]); HC(FinishCommandList, H_FinishCommandList<K>, o_FinishCommandList[K]);
    HC(CopyResource, H_CopyResource<K>, o_CopyResource[K]); HC(CopySubresourceRegion, H_CopySubresourceRegion<K>, o_CopySubresourceRegion[K]);
    HC(ResolveSubresource, H_ResolveSubresource<K>, o_ResolveSubresource[K]);
#undef HC
    return n;
}

// Only the hooks the stereo constant fix needs (when the full capture hooks are not installed).
void InstallStereoCbHooks(ID3D11Device* dev, ID3D11DeviceContext* imm) {
    void** v = *(void***)imm;
    int n = 0;
#define HC(m, fn, o) n += Hook(v, VT_CTX_##m, (void*)fn, (void**)&o, #m)
    HC(VSSetConstantBuffers1, (H_SetCB1<0, 0>), o_SetCB1_[0][0]); HC(PSSetConstantBuffers1, (H_SetCB1<0, 1>), o_SetCB1_[0][1]);
    HC(Map, H_Map<0>, o_Map[0]);
    HC(Draw, H_Draw<0>, o_Draw[0]);
    HC(OMSetRenderTargets, H_OMSetRenderTargets<0>, o_OMSetRenderTargets[0]);
    HC(OMSetRenderTargetsAndUnorderedAccessViews, H_OMSetRTUAV<0>, o_OMSetRTUAV[0]);
#undef HC
    VrsInit(dev);
    LOGI("cbfix: hooks installed (%d)", n);
}

void InstallCaptureHooks(ID3D11Device* dev, ID3D11DeviceContext* imm) {
    VrsInit(dev);
    void** dv = *(void***)dev;
#define HD(m) Hook(dv, VT_DEV_##m, (void*)H_##m, (void**)&o_##m[0], #m)
    HD(CreateVertexShader); HD(CreatePixelShader); HD(CreateComputeShader);
    HD(CreateGeometryShader); HD(CreateHullShader); HD(CreateDomainShader);
#undef HD
    ID3D11DeviceContext* def = nullptr;
    dev->CreateDeferredContext(0, &def);
    int n = InstallCtx<0>(*(void***)imm);
    if (def) n += InstallCtx<1>(*(void***)def);
    if (def) def->Release();
    LOGI("capture: hooks installed (%d context hooks ok)", n);
}

// ------------------------------------------------------------------ frame control + output
void CaptureRequest() { InterlockedExchange(&g_request, 1); }
bool CaptureActive() { return g_active != 0; }

typedef HRESULT(WINAPI* PFN_Reflect)(LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT(WINAPI* PFN_Disasm)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

static void WriteShaders(const std::wstring& dir) {
    HMODULE dc = LoadLibraryW(L"d3dcompiler_47.dll");
    auto reflect = dc ? (PFN_Reflect)GetProcAddress(dc, "D3DReflect") : nullptr;
    auto disasm = dc ? (PFN_Disasm)GetProcAddress(dc, "D3DDisassemble") : nullptr;
    FILE* f = _wfopen((dir + L"shaders.txt").c_str(), L"w");
    if (!f) return;
    CreateDirectoryW((dir + L"asm").c_str(), nullptr);
    for (void* s : g_usedShaders) {
        const ShaderInfo* si = FindShader(s);
        if (!si) continue;
        fprintf(f, "== %c %016llx %zu bytes\n", si->kind, (unsigned long long)si->hash, si->code.size());
        if (si->code.empty() || !reflect) continue;
        ID3D11ShaderReflection* r = nullptr;
        if (FAILED(reflect(si->code.data(), si->code.size(), IID_ID3D11ShaderReflection, (void**)&r))) { fprintf(f, "  (no reflection)\n"); continue; }
        D3D11_SHADER_DESC sd; r->GetDesc(&sd);
        for (UINT i = 0; i < sd.BoundResources; ++i) {
            D3D11_SHADER_INPUT_BIND_DESC b; r->GetResourceBindingDesc(i, &b);
            fprintf(f, "  bind %s type%d slot%u\n", b.Name, (int)b.Type, b.BindPoint);
        }
        for (UINT i = 0; i < sd.ConstantBuffers; ++i) {
            auto cb = r->GetConstantBufferByIndex(i);
            D3D11_SHADER_BUFFER_DESC bd; cb->GetDesc(&bd);
            fprintf(f, "  cbuffer %s size %u vars %u\n", bd.Name, bd.Size, bd.Variables);
            for (UINT v = 0; v < bd.Variables; ++v) {
                auto var = cb->GetVariableByIndex(v);
                D3D11_SHADER_VARIABLE_DESC vd; var->GetDesc(&vd);
                D3D11_SHADER_TYPE_DESC td; var->GetType()->GetDesc(&td);
                fprintf(f, "    %4u %4u %s %s%ux%u%s\n", vd.StartOffset, vd.Size, vd.Name, td.Class == D3D_SVC_MATRIX_COLUMNS ? "colmat" : td.Class == D3D_SVC_MATRIX_ROWS ? "rowmat" : "",
                        td.Rows, td.Columns, td.Elements ? ("[" + std::to_string(td.Elements) + "]").c_str() : "");
            }
        }
        r->Release();
        if (disasm && g_cfg.captureDisasm) {
            ID3DBlob* b = nullptr;
            if (SUCCEEDED(disasm(si->code.data(), si->code.size(), 0, nullptr, &b)) && b) {
                wchar_t p[MAX_PATH];
                swprintf_s(p, L"%sasm\\%c_%016llx.txt", dir.c_str(), si->kind, (unsigned long long)si->hash);
                if (FILE* a = _wfopen(p, L"wb")) { fwrite(b->GetBufferPointer(), 1, b->GetBufferSize() - 1, a); fclose(a); }
                b->Release();
            }
        }
    }
    fclose(f);
}

static void Finish() {
    InterlockedExchange(&g_active, 0);
    std::lock_guard<std::mutex> l(g_mx);
    wchar_t d[MAX_PATH];
    swprintf_s(d, L"%scap_%03d\\", G().outDir.c_str(), g_capIndex++);
    std::wstring dir = d;
    CreateDirectoryW(dir.c_str(), nullptr);
    auto write = [&](const wchar_t* name, const void* p, size_t n) {
        if (FILE* f = _wfopen((dir + name).c_str(), L"wb")) { fwrite(p, 1, n, f); fclose(f); }
    };
    write(L"trace.txt", g_trace.data(), g_trace.size());
    write(L"resources.txt", g_resText.data(), g_resText.size());
    write(L"cb.bin", g_blob.data(), g_blob.size());
    WriteShaders(dir);
    LOGI("capture: %S written: %llu events, %zu KB trace, %zu KB constants, %zu shaders (%ld registered)", dir.c_str(), g_events,
         g_trace.size() / 1024, g_blob.size() / 1024, g_usedShaders.size(), g_shaderCount);
    g_trace.clear(); g_trace.shrink_to_fit();
    g_blob.clear(); g_blob.shrink_to_fit();
    g_resText.clear(); g_ids.clear(); g_usedShaders.clear(); g_mapped.clear();
}

// Called at the top of every Present (render thread). A capture covers Present..Present.
void CaptureOnPresent(ID3D11DeviceContext* imm, uint64_t presentIndex) {
    g_imm = imm;
    if (g_active) Finish();
    static bool f10 = false;
    bool down = false;   // DAI: F10 is the HUD toggle; captures via capture.trigger only. edge detect (the "pressed since last call" bit is unreliable)
    bool pressed = down && !f10;
    f10 = down;
    if ((presentIndex % 30) == 0) {                            // automation: MEAVR\capture.trigger requests a capture
        std::wstring t = G().outDir + L"capture.trigger";
        if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(t.c_str()); pressed = true; }
    }
    if (pressed || (g_cfg.captureFrame > 0 && presentIndex == (uint64_t)g_cfg.captureFrame))
        InterlockedExchange(&g_request, 1);
    if (InterlockedExchange(&g_request, 0)) {
        std::lock_guard<std::mutex> l(g_mx);
        g_events = 0;
        g_blobLimit = (size_t)g_cfg.captureBlobMb << 20;
        g_trace.reserve(8 << 20);
        T("# capture at present %llu\n", (unsigned long long)presentIndex);
        InterlockedExchange(&g_active, 1);
        LOGI("capture: started at present %llu", (unsigned long long)presentIndex);
    }
}

} // namespace mea
