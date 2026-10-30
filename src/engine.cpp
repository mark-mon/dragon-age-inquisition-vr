// Engine-level hooks into Frostbite (Dragon Age: Inquisition, Frostbite 2014, exe fixed at 0x140000000).
// Found by static analysis of the decrypted in-memory image (see FINDINGS.md):
//   exe+39DBA81  updateViews(renderer, uint count, const ViewEntry* views)  - copies the game's view entries into
//                renderer+0x68 (count at +0x60), applies GameRenderSettings (renderer+0x50: NearPlane, ViewDistance,
//                ForceFov, ortho), aspect from the output target, then RenderView::update(rv, 0xF)
//   exe+3D2A57F  RenderView::update(rv, mask)  (1 view, 2 projection, 4 fov values, 8 frustum)
//   exe+39D92D3  renderViews(renderer, ...)    - copies each entry's RenderView into a per-frame array and hands it
//                to the world renderer (renderer+0xE0, vtable +0xF0 / +0xF8)
// View entry (0x10B0): +0x000 RenderView (current), +0x510 previous, +0xA20 ?, +0xF38 output target object,
//                      +0xF50 u16 viewport x, y, w, h, +0xF58 screen index, +0xF60.. matrices
// RenderView (0x510): +0x040 LinearTransform (right, up, back, position), +0x090 float2 (projection offset),
//   +0x0C8 int ortho, +0x0CC fovY (radians), +0x0D4 near, +0x0D8 far, +0x0DC aspect, +0x100 dirty flags,
//   +0x110 frustum, +0x270 fovX (deg), +0x290 view, +0x310 camera (copy of +0x40), +0x350 projection,
//   +0x450 view-projection, +0x410 inverse projection, +0x4D0 inverse view-projection
#include "depthprobe.h"
#include "headaim.h"
#include "vrs.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <mutex>
#include <vector>
#include "engine.h"
#include "headpose.h"
#include "settings.h"
#include "globals.h"
#include "log.h"
#include "cbfix.h"
#include "MinHook.h"
#include <d3d11.h>
#include <cstdio>
#include "blit.h"

namespace mea {

static const uintptr_t kExe = 0x140000000;
static const uintptr_t kUpdateViews = 0x39DBA81;
static const uintptr_t kRvUpdate = 0x3D2A57F;
static const uintptr_t kRenderViews = 0x39D92D3;
static const size_t kEntry = 0x10B0;
static const size_t kRv = 0x510;
// RenderView field offsets
static const int RV_XF = 0x40, RV_FOVY = 0xCC, RV_ASPECT = 0xDC, RV_DIRTY = 0x100, RV_PROJ = 0x350;

typedef void(__fastcall* PFN_RvUpdate)(uint8_t* rv, uint32_t mask);
typedef void(__fastcall* PFN_UpdateViews)(uint8_t* r, uint32_t count, uint8_t* views);
typedef void(__fastcall* PFN_RenderViews)(uint8_t* r);
static PFN_RvUpdate o_RvUpdate = nullptr;      // original (trampoline when hooked, else the function itself)
static PFN_UpdateViews o_UpdateViews = nullptr;
static PFN_RenderViews o_RenderViews = nullptr;
// renderer vtable +0x28: render(renderer, float dt, float b, uint count, views, sync*, dispatch*) - game thread, once per
// game frame (updateViews, build jobs, schedule the render-thread dispatch job exe+39D9AFD). Stereo renders the frame
// twice (left then right eye) from the same game state; the render thread executes both, each ending in a Present.
typedef void(__fastcall* PFN_Render)(uint8_t* r, float a, float b, uint32_t count, void* views, void* sync, void* disp);
static PFN_Render o_Render = nullptr;
static const uintptr_t kRender = 0x39D11E5;
static const uintptr_t kWaitJob = 0x301F03F;   // waitJob(handle*, const char* name)
static int g_frameEye = -1;                      // game thread: eye being built (-1: normal single render)
static HeadState g_frameHs;                       // head state shared by both eyes of a frame
static float g_eyeXf[2][16];                      // the eye cameras as written (to recognise them on the render thread)
static volatile LONG g_eyeXfSeq[2] = {};
static RenderedView g_pairView;                   // poses / fov of the pair being built
static volatile int g_rtEye = -1;                 // render thread: eye of the frame being rendered
static volatile LONG g_stereoActiveTick = 0;

static float g_gameCam[16];
static volatile DWORD g_gameCamTick = 0;
static DWORD g_lastGameMultiView = 0;
static int g_eyeRecH = 0, g_eyeRecW = 0;
static uint8_t* g_renderer = nullptr;
static uint8_t* g_grs = nullptr;               // GameRenderSettings instance (renderer+0x50)

void EngineSetEyeRecommended(int w, int h) { if (w > 0) g_eyeRecW = w; if (h > 0) g_eyeRecH = h; }
void EngineGetEyeRecommended(int& w, int& h) { w = g_eyeRecW; h = g_eyeRecH; }
uint8_t* EngineGameRenderSettings() { return g_grs; }

float EngineEyeAspect(const HeadState& hs) {
    float a = 0.f;
    for (int e = 0; e < 2; ++e) {
        float l = std::tan(-hs.fov[e][0]), r = std::tan(hs.fov[e][1]), u = std::tan(hs.fov[e][2]), d = std::tan(-hs.fov[e][3]);
        float tx = std::fmax(l, r), ty = std::fmax(u, d);
        if (ty > 0.f) a = std::fmax(a, tx / ty);
    }
    return a > 0.f ? a : 1.f;
}

static bool Readable(const void* p, size_t n) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery(p, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    return (const uint8_t*)p + n <= (const uint8_t*)mbi.BaseAddress + mbi.RegionSize;
}
static unsigned long long Rva(const void* p) {
    uintptr_t a = (uintptr_t)p;
    return (a >= kExe && a < kExe + 0x6400000) ? (unsigned long long)(a - kExe) : 0ull;
}

// camera' = camera * pose: the pose (OpenXR axes x right, y up, -z forward, relative to the recentre reference,
// which is the game camera) applied in the camera's own frame. Frostbite camera rows: right, up, back, position.
static void ApplyHead(const float* in, const Pose& h, float scale, float* out) {
    const float* R = in; const float* U = in + 4; const float* B = in + 8; const float* P = in + 12;
    auto map = [&](const float* v, float* w) { for (int i = 0; i < 3; ++i) w[i] = v[0] * R[i] + v[1] * U[i] + v[2] * B[i]; };
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    float a[3], w[3];
    memcpy(out, in, 64);
    QuatRotate(h.q, ex, a); map(a, w); out[0] = w[0]; out[1] = w[1]; out[2] = w[2];
    QuatRotate(h.q, ey, a); map(a, w); out[4] = w[0]; out[5] = w[1]; out[6] = w[2];
    QuatRotate(h.q, ez, a); map(a, w); out[8] = w[0]; out[9] = w[1]; out[10] = w[2];
    float hp[3] = {h.p[0] * scale, h.p[1] * scale, h.p[2] * scale};
    map(hp, w);
    out[12] = P[0] + w[0]; out[13] = P[1] + w[1]; out[14] = P[2] + w[2];
}

// remove roll / pitch from the game camera so the headset's own pitch and roll are not added to a tilted camera
static void LevelCamera(const float* in, float* out) {
    memcpy(out, in, 64);
    float bx = in[8], bz = in[10];
    float l = std::sqrt(bx * bx + bz * bz);
    if (l < 1e-4f) return;
    bx /= l; bz /= l;
    // back = (bx, 0, bz), up = (0, 1, 0), right = up x back = (bz, 0, -bx)
    out[0] = bz; out[1] = 0; out[2] = -bx; out[3] = 0;
    out[4] = 0; out[5] = 1; out[6] = 0; out[7] = 0;
    out[8] = bx; out[9] = 0; out[10] = bz; out[11] = 0;
}

static void SetFovAspect(uint8_t* rv, float tanY, float aspect) {
    *(float*)(rv + RV_FOVY) = 2.f * std::atan(tanY);
    *(float*)(rv + RV_ASPECT) = aspect;
    *(uint32_t*)(rv + RV_DIRTY) |= 0xF;
}

static void HeadsetTangents(const HeadState& hs, int eye, float& tx, float& ty) {
    tx = ty = 0.f;
    for (int e = 0; e < 2; ++e) {
        if (eye >= 0 && e != eye) continue;
        tx = std::fmax(tx, std::fmax(std::tan(-hs.fov[e][0]), std::tan(hs.fov[e][1])));
        ty = std::fmax(ty, std::fmax(std::tan(hs.fov[e][2]), std::tan(-hs.fov[e][3])));
    }
}

static bool IsMainEntry(const uint8_t* e) {
    const uint8_t* rv = e;
    float fov = *(const float*)(rv + RV_FOVY), asp = *(const float*)(rv + RV_ASPECT);
    int ortho = *(const int*)(rv + 0xC8);
    // the title / loading screens use a default view: position (0, 1, 0) looking along the axis - not gameplay
    const float* t = (const float*)(rv + RV_XF);
    bool defaultView = std::fabs(t[12]) < 1e-3f && std::fabs(t[14]) < 1e-3f && std::fabs(t[13] - 1.f) < 1e-3f && std::fabs(t[10] - 1.f) < 1e-4f;
    return ortho == 0 && fov > 0.2f && fov < 2.8f && asp > 0.3f && asp < 5.f && !defaultView;
}

static void LogViews(uint8_t* r, uint32_t count, const uint8_t* views, const char* when) {
    char b[512]; int n = 0;
    for (uint32_t i = 0; i < count && i < 4; ++i) {
        const uint8_t* e = views + i * kEntry;
        const float* t = (const float*)(e + RV_XF);
        const uint16_t* vp = (const uint16_t*)(e + 0xF50);
        n += snprintf(b + n, sizeof(b) - n, " [%u] fov %.3f asp %.3f ortho %d pos %.2f %.2f %.2f back %.2f %.2f %.2f vp %u,%u %ux%u scr %u",
                      i, *(const float*)(e + RV_FOVY), *(const float*)(e + RV_ASPECT), *(const int*)(e + 0xC8), t[12], t[13], t[14], t[8], t[9], t[10],
                      vp[0], vp[1], vp[2], vp[3], *(const uint32_t*)(e + 0xF58));
    }
    LOGI("engine: %s count %u%s", when, count, b);
}

static DWORD g_vlogTick = 0;
static void __fastcall H_UpdateViews(uint8_t* r, uint32_t count, uint8_t* views) {
    if (r != g_renderer) {
        g_renderer = r;
        void* vt = Readable(r, 8) ? *(void**)r : nullptr;
        uint8_t* wr = Readable(r + 0xE0, 8) ? *(uint8_t**)(r + 0xE0) : nullptr;
        void* wvt = (wr && Readable(wr, 8)) ? *(void**)wr : nullptr;
        LOGI("engine: renderer %p vtable exe+%llX; +0xE0 %p vtable exe+%llX (vfunc +0xF0 exe+%llX, +0xF8 exe+%llX); +0x50 settings %p",
             (void*)r, Rva(vt), (void*)wr, Rva(wvt), wvt ? Rva(((void**)wvt)[0xF0 / 8]) : 0ull, wvt ? Rva(((void**)wvt)[0xF8 / 8]) : 0ull,
             Readable(r + 0x50, 8) ? *(void**)(r + 0x50) : nullptr);
    }
    if (Readable(r + 0x50, 8)) g_grs = *(uint8_t**)(r + 0x50);
    DWORD now = GetTickCount();
    bool logNow = g_cfg.engineLog && now - g_vlogTick > 10000 && views && count;
    if (logNow) { g_vlogTick = now; LogViews(r, count, views, "game views"); }

    if (count > 1) g_lastGameMultiView = now;
    if (count >= 1 && views) { memcpy(g_gameCam, views + RV_XF, 64); g_gameCamTick = now; }

    HeadState hs = HeadRead();
    bool head = hs.valid && count == 1 && views && IsMainEntry(views);

    // ------------------------------------------------------------------ native stereo: this render is one eye
    if (g_frameEye >= 0 && count == 1 && views) {
        o_UpdateViews(r, count, views);
        uint8_t* rv = *(uint8_t**)(r + 0x68);
        if (!rv || *(uint32_t*)(r + 0x60) != 1) return;
        const HeadState& h = g_frameHs;
        int e = g_frameEye;
        float base[16];
        if (g_cfg.headLevel) LevelCamera((const float*)(views + RV_XF), base); else memcpy(base, views + RV_XF, 64);
        Pose eye = PoseMul(h.head, h.eye[e]);
        if (!g_cfg.headPosition) {
            float t[3]; QuatRotate(h.head.q, h.eye[e].p, t);
            for (int i = 0; i < 3; ++i) eye.p[i] = t[i];
        }
        ApplyHead(base, eye, g_cfg.worldScale, (float*)(rv + RV_XF));
        float tx, ty;
        HeadsetTangents(h, e, tx, ty);
        if (g_cfg.eyeFov && tx > 0.1f && ty > 0.1f) SetFovAspect(rv, ty, tx / ty);
        *(uint32_t*)(rv + RV_DIRTY) |= 0xF;
        o_RvUpdate(rv, 0xF);
        memcpy(g_eyeXf[e], rv + RV_XF, 64);
        InterlockedIncrement(&g_eyeXfSeq[e]);
        // per-eye history: the previous RenderView (entry +0x510, motion vectors / temporal effects) is this eye's
        // own view of the last frame, not the other eye's
        {
            alignas(16) static uint8_t prev[2][kRv];
            static bool prevOk[2] = {};
            if (g_cfg.stereoEyeHistory && prevOk[e]) memcpy(rv + kRv, prev[e], kRv);
            memcpy(prev[e], rv, kRv);
            prevOk[e] = true;
        }
        g_pairView.eye[e] = eye;
        g_pairView.eTanY[e] = std::tan(*(float*)(rv + RV_FOVY) * 0.5f);
        g_pairView.eTanX[e] = g_pairView.eTanY[e] * *(float*)(rv + RV_ASPECT);
        if (e == 1) {
            g_pairView.valid = g_pairView.stereo = true;
            g_pairView.head = h.head;
            g_pairView.tanX = g_pairView.eTanX[0]; g_pairView.tanY = g_pairView.eTanY[0];
            RenderedPublish(g_pairView);
        }
        if (logNow) LogViews(r, 1, rv, e ? "right eye view" : "left eye view");
        return;
    }

    o_UpdateViews(r, count, views);

    // ------------------------------------------------------------------ mono head tracking
    if (g_cfg.vrMode == 1 && head) {
        uint8_t* rv = *(uint8_t**)(r + 0x68);
        if (!rv || *(uint32_t*)(r + 0x60) < 1) return;
        float base[16];
        if (g_cfg.headLevel) LevelCamera((const float*)(views + RV_XF), base); else memcpy(base, views + RV_XF, 64);
        ApplyHead(base, hs.head, g_cfg.worldScale, (float*)(rv + RV_XF));
        float tx, ty;
        HeadsetTangents(hs, -1, tx, ty);
        if (g_cfg.eyeFov && tx > 0.1f && ty > 0.1f) SetFovAspect(rv, ty, tx / ty);
        *(uint32_t*)(rv + RV_DIRTY) |= 0xF;
        o_RvUpdate(rv, 0xF);
        RenderedView ro;
        ro.valid = true;
        ro.head = hs.head;
        ro.tanY = std::tan(*(float*)(rv + RV_FOVY) * 0.5f);
        ro.tanX = ro.tanY * *(float*)(rv + RV_ASPECT);
        RenderedPublish(ro);
    } else if (g_cfg.debugViewYaw != 0.f && count == 1) {
        uint8_t* rv = *(uint8_t**)(r + 0x68);
        if (!rv) return;
        Pose p; QuatFromYaw(g_cfg.debugViewYaw * 3.14159265f / 180.f, p.q);
        float base[16]; memcpy(base, rv + RV_XF, 64);
        ApplyHead(base, p, 1.f, (float*)(rv + RV_XF));
        *(uint32_t*)(rv + RV_DIRTY) |= 0xF;
        o_RvUpdate(rv, 0xF);
    }
}

// ------------------------------------------------------------------ render twice per game frame (stereo)
static DWORD g_slogTick = 0;
static LONG g_pairs = 0, g_singles = 0;
static void __fastcall H_Render(uint8_t* r, float a, float b, uint32_t count, void* views, void* sync, void* disp) {
    HeadState hs = HeadRead();
    bool stereo = g_cfg.vrMode == 2 && count == 1 && views && IsMainEntry((const uint8_t*)views) && hs.valid && sync;
    DWORD now = GetTickCount();
    if (g_cfg.engineLog && now - g_slogTick > 10000) {
        g_slogTick = now;
        LOGI("engine: render dt %.4f b %.4f count %u - last 10 s: %ld stereo pairs, %ld single renders", a, b, count, g_pairs, g_singles);
        g_pairs = g_singles = 0;
    }
    if (!stereo) { ++g_singles; g_frameEye = -1; o_Render(r, a, b, count, views, sync, disp); return; }
    ++g_pairs;
    InterlockedExchange(&g_stereoActiveTick, (LONG)now);
    g_frameHs = hs;
    float a2 = g_cfg.stereoSplitDt ? a * 0.5f : a;
    g_frameEye = 0;
    o_Render(r, a2, b, count, views, sync, disp);
    ((void(__fastcall*)(void*, const char*))(kExe + kWaitJob))(sync, "daivrLeftEye");
    g_frameEye = 1;
    o_Render(r, a2, b, count, views, sync, disp);
    g_frameEye = -1;
}

bool EngineStereoActive() { return g_cfg.vrMode == 2 && GetTickCount() - (DWORD)g_stereoActiveTick < 500; }
int EngineRenderEye() { return g_rtEye; }

// Render thread: the eye images. The world image of an eye frame is taken right after the game's final composite
// (the first non-indexed Draw into the back buffer); with the HUD layer on, the back buffer is then cleared to white
// (left eye) / black (right eye) so the game's UI draws over a known background, and the UI image is taken at
// Present. The pair gives the HUD matte (alpha = 1 - (white - black)); the monitor gets world + UI recomposed.
static ID3D11Texture2D* g_eyeTex[2] = {};
static D3D11_TEXTURE2D_DESC g_eyeDesc[2] = {};
static volatile LONG g_eyeFrame[2] = {};
static ID3D11Texture2D* g_hudTex[2] = {};        // [0] UI over white (left frame), [1] UI over black (right frame)
static D3D11_TEXTURE2D_DESC g_hudDesc[2] = {};
static volatile LONG g_hudFrame = 0;
static ID3D11Resource* g_bb = nullptr;          // current back buffer (not referenced)
static bool g_compositeDone = false, g_hudCleared = false;
static bool g_hudPairOk[2] = {};

static bool EnsureCopyTex(ID3D11Device* dev, ID3D11Texture2D*& t, D3D11_TEXTURE2D_DESC& have, const D3D11_TEXTURE2D_DESC& d, const char* what, int e) {
    if (t && have.Width == d.Width && have.Height == d.Height && have.Format == d.Format) return true;
    if (t) { blit::ForgetTexture(t); t->Release(); }
    t = nullptr;
    D3D11_TEXTURE2D_DESC nd = d;
    nd.MipLevels = 1; nd.ArraySize = 1; nd.SampleDesc = {1, 0}; nd.Usage = D3D11_USAGE_DEFAULT;
    nd.BindFlags = D3D11_BIND_SHADER_RESOURCE; nd.CPUAccessFlags = 0; nd.MiscFlags = 0;
    if (FAILED(dev->CreateTexture2D(&nd, nullptr, &t))) { LOGE("engine: %s %d texture failed", what, e); return false; }
    have = d;
    LOGI("engine: %s %d texture %ux%u fmt %d", what, e, d.Width, d.Height, (int)d.Format);
    return true;
}

bool EngineHudLayer() { return g_cfg.vrMode == 2 && g_cfg.hudLayer; }

void EngineAfterDraw(ID3D11DeviceContext* c, UINT vertexCount) {
    int e = g_rtEye;
    if (e < 0 || g_compositeDone || !g_bb) return;
    ID3D11RenderTargetView* rtv = nullptr;
    c->OMGetRenderTargets(1, &rtv, nullptr);
    if (!rtv) return;
    ID3D11Resource* res = nullptr;
    rtv->GetResource(&res);
    if (res == g_bb) {
        g_compositeDone = true;
        D3D11_TEXTURE2D_DESC d; ((ID3D11Texture2D*)res)->GetDesc(&d);
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        if (EnsureCopyTex(dev, g_eyeTex[e], g_eyeDesc[e], d, "eye", e)) { c->CopyResource(g_eyeTex[e], res); InterlockedIncrement(&g_eyeFrame[e]); }
        if (EngineHudLayer()) {
            const float white[4] = {1, 1, 1, 1}, black[4] = {0, 0, 0, 0};
            c->ClearRenderTargetView(rtv, e == 0 ? white : black);
            g_hudCleared = true;
        }
        static bool logged = false;
        if (!logged) { logged = true; LOGI("engine: final composite found (Draw %u into the back buffer) - eye images taken there", vertexCount); }
        dev->Release();
    }
    res->Release();
    rtv->Release();
}

// Present (render thread). Returns the eye of the frame just presented (0 / 1) or -1.
int EngineOnPresent(IDXGISwapChain* sc) {
    int e = g_rtEye;
    g_rtEye = -1;
    bool composite = g_compositeDone, cleared = g_hudCleared;
    g_compositeDone = g_hudCleared = false;
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return -1;
    g_bb = bb;   // for the next frame's composite detection
    if (e < 0 || !EngineStereoActive()) { bb->Release(); return -1; }
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    ID3D11Device* dev = nullptr; bb->GetDevice(&dev);
    ID3D11DeviceContext* ctx = nullptr; dev->GetImmediateContext(&ctx);
    if (!composite) {   // no composite seen: the whole back buffer (UI baked in) is the eye image
        if (EnsureCopyTex(dev, g_eyeTex[e], g_eyeDesc[e], d, "eye", e)) { ctx->CopyResource(g_eyeTex[e], bb); InterlockedIncrement(&g_eyeFrame[e]); }
        g_hudPairOk[e] = false;
    } else if (cleared) {
        if (EnsureCopyTex(dev, g_hudTex[e], g_hudDesc[e], d, "hud", e)) { ctx->CopyResource(g_hudTex[e], bb); g_hudPairOk[e] = true; }
        if (e == 1 && g_hudPairOk[0] && g_hudPairOk[1]) {
            InterlockedIncrement(&g_hudFrame);
            // the monitor: the right eye's world with the UI over it
            if (blit::Init(dev, ctx)) blit::Compose(g_eyeTex[1], g_hudTex[1], g_hudTex[0], bb, d.Format, d.Width, d.Height);
        }
    } else g_hudPairOk[e] = false;
    ctx->Release(); dev->Release(); bb->Release();
    return e;
}

// the HUD pair of the last stereo frame: UI over black / over white
bool EngineHudTextures(ID3D11Texture2D** black, ID3D11Texture2D** white, D3D11_TEXTURE2D_DESC* desc, LONG* frame) {
    if (!EngineHudLayer() || !g_hudPairOk[0] || !g_hudPairOk[1] || !g_hudTex[0] || !g_hudTex[1]) return false;
    *black = g_hudTex[1]; *white = g_hudTex[0]; *desc = g_hudDesc[1]; *frame = g_hudFrame;
    return true;
}

// ------------------------------------------------------------------ diagnostics: the Dx11 renderer's screens
// renderer (global exe+2A732E8?): +0x30 screen count, +0x38 screens (0x150 each): +0x00 hwnd, +0x1C mode,
// +0x25 resize pending?, +0x28/+0x2C current back buffer size, +0x30/+0x34 client size, +0xD4/+0xD8 wanted size,
// +0x100 swapchain; renderer +0x80 DxDisplaySettings, +0x100 ID3D11Device
void EngineLogScreens() {
    static DWORD t = 0;
    if (GetTickCount() - t < 10000) return;
    t = GetTickCount();
    uint8_t* dx = *(uint8_t**)(kExe + 0x2A732E8);
    if (!dx || !Readable(dx, 0x110)) { LOGI("screens: renderer %p not readable", (void*)dx); return; }
    uint32_t n = *(uint32_t*)(dx + 0x30);
    uint8_t* sc = *(uint8_t**)(dx + 0x38);
    LOGI("screens: renderer %p vtable exe+%llX count %u array %p display settings %p device %p", (void*)dx, Rva(*(void**)dx), n, (void*)sc,
         *(void**)(dx + 0x80), *(void**)(dx + 0x100));
    for (uint32_t i = 0; i < n && i < 4 && sc && Readable(sc + i * 0x150, 0x150); ++i) {
        uint8_t* s = sc + i * 0x150;
        LOGI("screens: [%u] hwnd %p mode %u flags24 %02X %02X cur %ux%u client %ux%u want %ux%u swapchain %p", i, *(void**)s, *(uint32_t*)(s + 0x1C),
             s[0x24], s[0x25], *(uint32_t*)(s + 0x28), *(uint32_t*)(s + 0x2C), *(uint32_t*)(s + 0x30), *(uint32_t*)(s + 0x34),
             *(uint32_t*)(s + 0xD4), *(uint32_t*)(s + 0xD8), *(void**)(s + 0x100));
    }
}

// The engine polls each screen's client rect (wanted size +0xD4/+0xD8) but only resizes its swapchain and targets
// when the resize flag +0x25 is set (normally by its window procedure on a user resize). After the mod has resized
// the window, the flag is set here so the engine runs its own resize path.
bool EngineRequestScreenResize(HWND hwnd) {
    uint8_t* dx = *(uint8_t**)(kExe + 0x2A732E8);
    if (!dx || !Readable(dx, 0x110)) return false;
    uint32_t n = *(uint32_t*)(dx + 0x30);
    uint8_t* sc = *(uint8_t**)(dx + 0x38);
    for (uint32_t i = 0; i < n && i < 4 && sc && Readable(sc + i * 0x150, 0x150); ++i) {
        uint8_t* s = sc + i * 0x150;
        if (*(HWND*)s != hwnd) continue;
        uint32_t cw = *(uint32_t*)(s + 0x28), ch = *(uint32_t*)(s + 0x2C), ww = *(uint32_t*)(s + 0xD4), wh = *(uint32_t*)(s + 0xD8);
        if (cw == ww && ch == wh) return true;
        if (*(uint32_t*)(s + 0x1C) != 0) { static int warned = 0; if (!warned++) LOGW("render resolution: screen mode %u (fullscreen, not windowed) - not resized - set Display Mode to Windowed in the game's video options", *(uint32_t*)(s + 0x1C)); return false; }
        s[0x25] = 1;
        LOGI("render resolution: engine screen %u %ux%u -> %ux%u (resize flag set)", i, cw, ch, ww, wh);
        return true;
    }
    return false;
}

// ------------------------------------------------------------------ per-frame renderViews: who renders the views
static DWORD g_rlogTick = 0;
static void __fastcall H_RenderViews(uint8_t* r) {
    DWORD now = GetTickCount();
    if (g_cfg.engineLog && now - g_rlogTick > 10000) {
        g_rlogTick = now;
        LOGI("engine: renderViews(%p) count %u, +0xE0 %p +0xE8 %p, thread %lu", (void*)r, *(uint32_t*)(r + 0x60), *(void**)(r + 0xE0),
             *(void**)(r + 0xE8), GetCurrentThreadId());
    }
    int eye = -1;
    uint8_t* ent = *(uint8_t**)(r + 0x68);
    if (ent && *(uint32_t*)(r + 0x60) == 1)
        for (int e = 0; e < 2; ++e) if (memcmp(ent + RV_XF, g_eyeXf[e], 64) == 0) eye = e;
    g_rtEye = eye;
    bool vrs = eye >= 0 && g_cfg.vrsEnabled;
    if (vrs) {
        VrsSetExclude(g_bb);
        if (g_bb) { D3D11_TEXTURE2D_DESC bd; ((ID3D11Texture2D*)g_bb)->GetDesc(&bd); VrsSetMainSize(bd.Width, bd.Height); }
        VrsBeginEye(eye);
    }
    o_RenderViews(r);
    if (vrs) VrsEndEye();
}


bool EngineGameCamera(float out[16]) {
    if (!g_gameCamTick || GetTickCount() - g_gameCamTick > 500) return false;
    memcpy(out, g_gameCam, 64);
    return true;
}
void EngineRequestViewScan() {}
bool EngineGameMultiViewRecent() { return g_lastGameMultiView && GetTickCount() - g_lastGameMultiView < 1500; }
LONG EngineEyeFrame(int eye) { return (eye == 0 || eye == 1) ? g_eyeFrame[eye] : 0; }
ID3D11Texture2D* EngineEyeTexture(int eye, D3D11_TEXTURE2D_DESC* desc) {
    if (eye < 0 || eye > 1 || !g_eyeTex[eye]) return nullptr;
    if (desc) *desc = g_eyeDesc[eye];
    return g_eyeTex[eye];
}

static bool Hook(uintptr_t rva, const uint8_t* sig, size_t n, void* det, void** orig, const char* name) {
    const uint8_t* f = (const uint8_t*)(kExe + rva);
    if (!Readable(f, n) || memcmp(f, sig, n) != 0) {
        LOGE("engine: %s signature mismatch at exe+%llX (%02X %02X %02X %02X) - not hooked", name, (unsigned long long)rva, f[0], f[1], f[2], f[3]);
        return false;
    }
    if (MH_CreateHook((void*)f, det, orig) != MH_OK || MH_EnableHook((void*)f) != MH_OK) { LOGE("engine: hook %s failed", name); return false; }
    LOGI("engine: %s hooked (exe+%llX)", name, (unsigned long long)rva);
    return true;
}

void EngineInstallHooks() {
    if (!g_cfg.engineHooks) { LOGI("engine: hooks disabled (engine_hooks = 0)"); return; }
    if ((uintptr_t)GetModuleHandleW(nullptr) != kExe) { LOGE("engine: exe not at 0x140000000 - hooks disabled"); return; }
    static const uint8_t sigRvUpdate[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x89, 0xD0};
    static const uint8_t sigUpdateViews[] = {0x55, 0x56, 0x41, 0x54, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24};
    static const uint8_t sigRenderViews[] = {0x48, 0x89, 0xE0, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    const uint8_t* f = (const uint8_t*)(kExe + kRvUpdate);
    if (Readable(f, sizeof sigRvUpdate) && memcmp(f, sigRvUpdate, sizeof sigRvUpdate) == 0) o_RvUpdate = (PFN_RvUpdate)f;
    else { LOGE("engine: RenderView::update signature mismatch - engine hooks disabled"); return; }
    static const uint8_t sigRender[] = {0x48, 0x89, 0xE0, 0x55, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57};
    Hook(kRender, sigRender, sizeof sigRender, (void*)H_Render, (void**)&o_Render, "render");
    Hook(kUpdateViews, sigUpdateViews, sizeof sigUpdateViews, (void*)H_UpdateViews, (void**)&o_UpdateViews, "updateViews");
    Hook(kRenderViews, sigRenderViews, sizeof sigRenderViews, (void*)H_RenderViews, (void**)&o_RenderViews, "renderViews");
}

} // namespace mea
