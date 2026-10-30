// Stereo fix for Frostbite's "local origin" view constants.
// Skinned characters (the player, NPCs) are drawn with bone matrices relative to a per-object origin and a per-object
// copy of the view constants (cb2: camera position - origin, view / view-projection with the origin folded in). That
// copy is built once per frame from the game's own camera, so both eyes would draw the character from the same
// (centre) camera: zero disparity. While an eye view renders, every such block bound to VS/PS slot 2 is rebuilt from
// the eye's RenderView and bound from our own buffer instead.
//
// View-constant block layout (float4 rows): 1 (W, H, 1/W, 1/H), 2-5 view^T, 10-13 view-projection^T,
// 14-17 camera-relative view-projection^T, 37 camera position, 38 origin (w = 0; zero in the normal per-view block).
#include <windows.h>
#include <d3d11_1.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <unordered_map>
#include "cbfix.h"
#include "settings.h"
#include "log.h"

namespace mea {

static std::unordered_map<ID3D11Resource*, void*> g_cbPtr;   // constant buffer -> latest mapped pointer (immediate ctx)
static SRWLOCK g_lk = SRWLOCK_INIT;
static ID3D11Buffer* g_ring = nullptr;
static const UINT kRingSize = 512 * 1024;
static UINT g_ringPos = kRingSize;
static int g_eye = -1;
static const uint8_t* g_eyeRv = nullptr;
static const uint8_t* g_matchRv = nullptr;   // RenderView the game built the blocks from (its projection identifies them)
struct Cached { UINT first; };
static std::unordered_map<uint64_t, Cached> g_cache;   // (buffer, first constant) -> our block, per eye view
static uint32_t g_fixed = 0, g_fixedLog = 0;

void CbFixNoteMap(ID3D11Resource* r, void* p) {
    D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim);
    if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    D3D11_BUFFER_DESC d; ((ID3D11Buffer*)r)->GetDesc(&d);
    if (!(d.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return;
    AcquireSRWLockExclusive(&g_lk);
    g_cbPtr[r] = p;
    ReleaseSRWLockExclusive(&g_lk);
}

void CbFixBeginEye(int eye, const uint8_t* renderView, const uint8_t* matchView) {
    g_eye = eye; g_eyeRv = renderView; g_matchRv = matchView ? matchView : renderView; g_cache.clear();
}
void CbFixEndEye() {
    g_eye = -1; g_eyeRv = nullptr;
    DWORD now = GetTickCount();
    if (g_fixed && now - g_fixedLog > 10000) { g_fixedLog = now; LOGI("cbfix: %u local-origin view blocks rebuilt per eye in the last 10 s", g_fixed); g_fixed = 0; }
}

static bool ReadBlock(const void* src, float* out, size_t bytes) {
    __try { memcpy(out, src, bytes); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool CbFixRead(ID3D11Buffer* b, UINT offsetBytes, void* out, UINT bytes) {
    AcquireSRWLockShared(&g_lk);
    auto it = g_cbPtr.find(b);
    void* p = it != g_cbPtr.end() ? it->second : nullptr;
    ReleaseSRWLockShared(&g_lk);
    return p && ReadBlock((const uint8_t*)p + offsetBytes, (float*)out, bytes);
}

static bool IsLocalBlock(const float* b) {
    const float* r1 = b + 4;
    if (!(r1[0] > 16.f && r1[1] > 16.f && std::fabs(r1[2] * r1[0] - 1.f) < 1e-3f && std::fabs(r1[3] * r1[1] - 1.f) < 1e-3f)) return false;
    const float* o = b + 38 * 4;
    if (o[3] != 0.f || (o[0] == 0.f && o[1] == 0.f && o[2] == 0.f)) return false;
    for (int i = 0; i < 3; ++i) if (o[i] != std::floor(o[i])) return false;   // origins are whole units
    return true;
}

// Only blocks of the eye's own camera (not shadow cascades, reflections or other helper views): same projection
// scale (row 36 = P00, P11, ?, near).
static bool MatchesProj(const float* b, const uint8_t* rv) {
    const float* P = (const float*)(rv + 0x360);
    const float* r = b + 36 * 4;
    return std::fabs(r[0] - P[0]) < 1e-3f && std::fabs(r[1] - P[5]) < 1e-3f && std::fabs(r[3] - P[14]) < 1e-4f;
}
static bool MatchesEye(const float* b) { return MatchesProj(b, g_eyeRv) || (g_matchRv != g_eyeRv && MatchesProj(b, g_matchRv)); }

// rows r0..r0+3 = transpose(M) with the origin folded into the translation column
static void PutT(float* b, int r0, const float* M, const float* o) {
    for (int r = 0; r < 4; ++r) {
        float* d = b + (r0 + r) * 4;
        d[0] = M[0 * 4 + r]; d[1] = M[1 * 4 + r]; d[2] = M[2 * 4 + r];
        d[3] = M[3 * 4 + r] + (o ? o[0] * d[0] + o[1] * d[1] + o[2] * d[2] : 0.f);
    }
}

static void Rebuild(float* b) {
    const float* V = (const float*)(g_eyeRv + 0x2A0);
    const float* Vr = (const float*)(g_eyeRv + 0x3A0);
    const float* P = (const float*)(g_eyeRv + 0x360);
    const float* VP = (const float*)(g_eyeRv + 0x460);
    const float* cam = (const float*)(g_eyeRv + 0x350);
    float o[3] = {b[38 * 4], b[38 * 4 + 1], b[38 * 4 + 2]};
    PutT(b, 2, V, o);
    PutT(b, 10, VP, o);
    float M[16];
    for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c)
        M[r * 4 + c] = Vr[r * 4] * P[c] + Vr[r * 4 + 1] * P[4 + c] + Vr[r * 4 + 2] * P[8 + c] + Vr[r * 4 + 3] * P[12 + c];
    PutT(b, 14, M, nullptr);
    for (int i = 0; i < 3; ++i) b[37 * 4 + i] = cam[i] - o[i];
}

// Called for VS/PS SetConstantBuffers1 on the immediate context. Returns true when it bound a replacement.
bool CbFixSetCB1(ID3D11DeviceContext* c, UINT slot, UINT n, ID3D11Buffer* const* bufs, const UINT* first, const UINT* num,
                 void (STDMETHODCALLTYPE* orig)(ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*)) {
    if (g_eye < 0 || !g_eyeRv || !bufs || !first || !num || slot > 2 || slot + n <= 2) return false;
    UINT i = 2 - slot;
    ID3D11Buffer* src = bufs[i];
    if (!src || num[i] * 16 < 39 * 16 || num[i] * 16 > 4096) return false;
    uint64_t key = ((uint64_t)(uintptr_t)src << 20) ^ first[i];
    UINT ourFirst;
    auto ci = g_cache.find(key);
    if (ci != g_cache.end()) ourFirst = ci->second.first;
    else {
        AcquireSRWLockShared(&g_lk);
        auto it = g_cbPtr.find(src);
        void* p = it != g_cbPtr.end() ? it->second : nullptr;
        ReleaseSRWLockShared(&g_lk);
        if (!p) return false;
        UINT bytes = num[i] * 16;
        alignas(16) float blk[1024];
        if (!ReadBlock((const uint8_t*)p + first[i] * 16, blk, bytes)) return false;
        if (!IsLocalBlock(blk) || !MatchesEye(blk)) { g_cache[key] = {UINT_MAX}; return false; }
        Rebuild(blk);
        if (!g_ring) {
            ID3D11Device* dev = nullptr; c->GetDevice(&dev);
            D3D11_BUFFER_DESC d{kRingSize, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
            if (dev) { dev->CreateBuffer(&d, nullptr, &g_ring); dev->Release(); }
            if (!g_ring) return false;
        }
        UINT need = (bytes + 255) & ~255u;
        D3D11_MAP mt = D3D11_MAP_WRITE_NO_OVERWRITE;
        if (g_ringPos + need > kRingSize) { g_ringPos = 0; mt = D3D11_MAP_WRITE_DISCARD; }
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(c->Map(g_ring, 0, mt, 0, &m))) return false;
        memcpy((uint8_t*)m.pData + g_ringPos, blk, bytes);
        c->Unmap(g_ring, 0);
        ourFirst = g_ringPos / 16;
        g_ringPos += need;
        g_cache[key] = {ourFirst};
        ++g_fixed;
    }
    if (ourFirst == UINT_MAX) return false;
    ID3D11Buffer* nb[8]; UINT nf[8], nn[8];
    if (n > 8) return false;
    for (UINT k = 0; k < n; ++k) { nb[k] = bufs[k]; nf[k] = first[k]; nn[k] = num[k]; }
    nb[i] = g_ring; nf[i] = ourFirst;
    orig((ID3D11DeviceContext1*)c, slot, n, nb, nf, nn);
    return true;
}

}
