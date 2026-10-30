// DXGI Present / ResizeBuffers hooks (inline detours on the implementation found through a dummy swapchain).
#include "aimscan.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <psapi.h>
#include "present.h"
#include <vector>
#include <string>
#include "engine.h"
#include "window_size.h"
#include "finalpass.h"
#include <cmath>
#include "headpose.h"
#include "xr.h"
#include "globals.h"
#include "log.h"
#include "capture.h"
#include "cbfix.h"
#include "camscan.h"
#include "watch.h"
#include "fbsettings.h"
#include "settings.h"
#include "../third_party/minhook/include/MinHook.h"

namespace mea {

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
static PFN_Present o_Present = nullptr;
static PFN_ResizeBuffers o_ResizeBuffers = nullptr;
static thread_local int t_inPresent = 0;
static LONG g_presents = 0;

static HWND g_gameWnd = nullptr;
static void ApplyRenderSize();
static volatile LONG g_camScanReq = 0;
void PresentRequestCamScan() { InterlockedExchange(&g_camScanReq, 1); }
static void RequestCamScan(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    CamScanRequest(d.BufferDesc.Width, d.BufferDesc.Height);
}

static void LogOnce(IDXGISwapChain* sc) {
    static bool done = false;
    if (done) return;
    done = true;
    DXGI_SWAP_CHAIN_DESC d{};
    sc->GetDesc(&d);
    ID3D11Device* dev = nullptr;
    HRESULT hr = sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev);
    LOGI("first Present: swapchain %p %ux%u fmt %d buffers %u swapeffect %d windowed %d flags 0x%X hwnd %p, D3D11 device %p (hr 0x%08X) FL 0x%X",
         (void*)sc, d.BufferDesc.Width, d.BufferDesc.Height, (int)d.BufferDesc.Format, d.BufferCount, (int)d.SwapEffect,
         d.Windowed, d.Flags, (void*)d.OutputWindow, (void*)dev, (unsigned)hr, dev ? (unsigned)dev->GetFeatureLevel() : 0u);
    if (dev) dev->Release();
}

static HRESULT STDMETHODCALLTYPE Hook_Present(IDXGISwapChain* sc, UINT sync, UINT flags) {
    if (t_inPresent == 0 && !(flags & DXGI_PRESENT_TEST)) {
        ++t_inPresent;
        LogOnce(sc);
        InterlockedIncrement(&g_presents);
        {   // hotkeys (only while the game window is in front): F8 VR mode, F9 recentre, F10 HUD panel
            static bool k8 = false, k9 = false, k10 = false;
            DXGI_SWAP_CHAIN_DESC kd{};
            bool fg = SUCCEEDED(sc->GetDesc(&kd)) && GetForegroundWindow() == kd.OutputWindow;
            bool d8 = fg && (GetAsyncKeyState(VK_F8) & 0x8000), d9 = fg && (GetAsyncKeyState(VK_F9) & 0x8000), d10 = fg && (GetAsyncKeyState(VK_F10) & 0x8000);
            if (d8 && !k8) { g_cfg.vrMode = (g_cfg.vrMode + 1) % 3; LOGI("F8: VR mode %s", g_cfg.vrMode == 2 ? "stereo" : g_cfg.vrMode == 1 ? "head" : "screen"); }
            if (d9 && !k9) { XrRecenter(); LOGI("F9: recentre"); }
            if (d10 && !k10) { g_cfg.hudEnabled = !g_cfg.hudEnabled; LOGI("F10: HUD panel %s", g_cfg.hudEnabled ? "on" : "off"); }
            k8 = d8; k9 = d9; k10 = d10;
        }
        static bool f11 = false;
        bool down = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        if (down && !f11) XrRequestDump();
        f11 = down;
        if ((g_presents % 30) == 0) {                           // automation: MEAVR\dump.trigger requests a screen dump
            std::wstring t = G().outDir + L"dump.trigger";
            if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(t.c_str()); XrRequestDump(); }
            t = G().outDir + L"camscan.trigger";                  // automation: find the camera in memory
            if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(t.c_str()); RequestCamScan(sc); }
            AimScanPoll();
            t = G().outDir + L"viewscan.trigger";                 // debug: CPU copies of the view constants
            if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(t.c_str()); EngineRequestViewScan(); }
            t = G().outDir + L"camwatch.trigger";                 // automation: scan, then watch who writes the camera
            if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) { DeleteFileW(t.c_str()); CamScanSetAutoWatch(3); RequestCamScan(sc); }
        }
        if (InterlockedExchange(&g_camScanReq, 0)) RequestCamScan(sc);
        CamScanOnPresent();
        if ((g_presents % 30) == 3) {                           // automation: re-read config.txt
            std::wstring t = G().outDir + L"reload.trigger";
            if (GetFileAttributesW(t.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileW(t.c_str());
                G().cfg.Load((G().gameDir + L"config.txt").c_str());
                LoadSettings();
                LOGI("config reloaded (debug_view_yaw %.1f)", g_cfg.debugViewYaw);
            }
        }
        { extern void NvOnPresent(uint64_t); NvOnPresent((uint64_t)g_presents); }
        WatchOnPresent((uint64_t)g_presents);
        FbOnPresent((uint64_t)g_presents);
        if (!g_gameWnd) { DXGI_SWAP_CHAIN_DESC d{}; sc->GetDesc(&d); g_gameWnd = d.OutputWindow; }
        {   // the back buffer identity, for recognising the game's final pass in the next frame
            ID3D11Resource* bb = nullptr;
            if (SUCCEEDED(sc->GetBuffer(0, __uuidof(ID3D11Resource), (void**)&bb))) { FinalPassSetBackBuffer(bb, (uint64_t)g_presents); bb->Release(); }
        }
        if ((g_presents % 30) == 7) ApplyRenderSize();
        { static bool cfgd = false; if (!cfgd) { cfgd = true; WindowSizeApplyConfig(); } }
        WindowSizeOnPresent(sc, g_cfg.vrMode == 2);
        if (g_cfg.engineLog) EngineLogScreens();
        int eye = EngineOnPresent(sc);
        if (eye == 0) {
            // stereo: the left-eye frame - kept for the headset; the XR frame and the monitor use the right-eye frame
            --t_inPresent;
            if (!g_cfg.stereoPresentLeft) return S_OK;
            return o_Present(sc, sync, flags);
        }
        XrOnPresent(sc);
        static ID3D11DeviceContext* imm = nullptr;
        if (!imm) {
            ID3D11Device* dev = nullptr;
            if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&dev))) { dev->GetImmediateContext(&imm); dev->Release(); }
        }
        if (imm) CaptureOnPresent(imm, (uint64_t)g_presents);
        CaptureFrameStart();   // after our own XR work, so a capture starts with the game's frame
        --t_inPresent;
    }
    return o_Present(sc, sync, flags);
}

// ------------------------------------------------------------------ stereo render size
// The engine renders every view at the back buffer's size. In stereo mode the back buffer is made the eye render size
// (headset aspect, eye_resolution_scale * recommended height) whatever the window size: ResizeBuffers is called with
// that size, and the window is resized (to the same aspect, fitting the monitor) so the game runs its resize path.
static int g_targetW = 0, g_targetH = 0;      // forced back buffer size (0 = none)
static RECT g_savedRect{}; static bool g_haveSaved = false;
static int g_lastAppliedW = -1, g_lastAppliedH = -1;

static void ComputeTarget(int& w, int& h) {
    w = h = 0;
    if (g_cfg.vrMode != 2 || !g_cfg.stereoRenderSize) return;
    int recW = 0, recH = 0;
    EngineGetEyeRecommended(recW, recH);
    HeadState hs = HeadRead();
    if (recH <= 0 || !hs.valid || !(hs.fov[0][1] > hs.fov[0][0])) {   // no pose this moment: keep the current size
        w = g_targetW; h = g_targetH;
        return;
    }
    float aspect = EngineEyeAspect(hs);
    h = (int)(recH * g_cfg.eyeResScale + 0.5f);
    if (h < 360) h = 360;
    if (h > 8192) h = 8192;
    w = (int)(h * aspect + 0.5f);
    if (w > 8192) { w = 8192; h = (int)(w / aspect); }
    w &= ~1; h &= ~1;
}

// The engine renders at the window's client size times its own resolution scaling (GameRenderSettings.ResolutionScaleMode
// +0x2C: 0 disabled, 1/2/3 auto 720p/900p/1080p, 4 custom with ResolutionScale +0x1C), not at the back buffer size.
// The window is limited by the monitor, so the eye size is reached with mode 4 and scale = eye height / client height.
// The game's values are restored when the render size is no longer forced.
static int g_savedScaleMode = -1; static float g_savedScale = 0.f;
[[maybe_unused]] static void ForceRenderScale(bool on, int targetH) {
    uint8_t* g = FbInstance("GameRenderSettings");
    if (!g) return;
    int32_t* mode = (int32_t*)(g + 0x2C);
    float* scale = (float*)(g + 0x1C);
    if (on) {
        RECT cr{};
        GetClientRect(g_gameWnd, &cr);
        int ch = cr.bottom - cr.top;
        if (ch <= 0) return;
        float sc = (float)targetH / (float)ch;
        if (g_savedScaleMode < 0) { g_savedScaleMode = *mode; g_savedScale = *scale; }
        if (*mode != 4 || std::fabs(*scale - sc) > 1e-3f)
            LOGI("render size: game resolution scale mode %d scale %.2f -> custom %.3f (client height %d, eye height %d)", *mode, *scale, sc, ch, targetH);
        *mode = 4; *scale = sc;
    } else if (g_savedScaleMode >= 0) {
        *mode = g_savedScaleMode; *scale = g_savedScale;
        LOGI("render size: game resolution scale mode %d scale %.2f restored", *mode, *scale);
        g_savedScaleMode = -1;
    }
}

// Effects that keep a single history across views (screen-space reflections read the previous frame's final image,
// which in stereo is the other eye's) are switched off while in stereo: stereo_fb_overrides = "Class.Field value; ..."
// applied every 30 frames, the game's values restored when leaving stereo.
static std::vector<std::pair<std::string, std::string>> g_ovSaved;   // "Class.Field" -> original text
static void StereoOverrides(bool on) {
    if (on) {
        std::string list = g_cfg.stereoFbOverrides;
        size_t i = 0;
        while (i < list.size()) {
            size_t j = list.find(';', i);
            if (j == std::string::npos) j = list.size();
            std::string item = list.substr(i, j - i);
            i = j + 1;
            size_t a = item.find_first_not_of(" \t");
            if (a == std::string::npos) continue;
            item = item.substr(a);
            size_t sp = item.find(' ');
            if (sp == std::string::npos) continue;
            std::string key = item.substr(0, sp), val = item.substr(sp + 1);
            size_t dot = key.find('.');
            if (dot == std::string::npos) continue;
            std::string cur = FbGet(key.substr(0, dot).c_str(), key.substr(dot + 1).c_str());
            if (cur.empty()) continue;
            bool saved = false;
            for (auto& kv : g_ovSaved) if (kv.first == key) saved = true;
            if (!saved) { g_ovSaved.push_back({key, cur}); LOGI("stereo override: %s %s -> %s", key.c_str(), cur.c_str(), val.c_str()); }
            if (cur != val) FbSet((key + " " + val).c_str());
        }
    } else if (!g_ovSaved.empty()) {
        for (auto& kv : g_ovSaved) { FbSet((kv.first + " " + kv.second).c_str()); LOGI("stereo override: %s restored to %s", kv.first.c_str(), kv.second.c_str()); }
        g_ovSaved.clear();
    }
}

// stereo: temporal SMAA (mode 7, T2x) keeps one history for both eyes -> SMAA 1x (6) while in stereo
static std::string g_aaSaved;
static void StereoAa(bool on) {
    std::string cur = FbGet("WorldRenderSettings", "PostProcessAntialiasingMode");
    if (cur.empty()) return;
    if (on && g_cfg.stereoNoTaa) {
        if (cur == "7") { g_aaSaved = cur; FbSet("WorldRenderSettings.PostProcessAntialiasingMode 6"); LOGI("stereo: temporal SMAA T2x -> SMAA 1x"); }
    } else if (!g_aaSaved.empty()) {
        FbSet(("WorldRenderSettings.PostProcessAntialiasingMode " + g_aaSaved).c_str());
        LOGI("stereo: anti-aliasing mode %s restored", g_aaSaved.c_str());
        g_aaSaved.clear();
    }
}

// Depth of field: GlobalPostProcessSettings.Force* override the visual environment's DOF values while they are not -1.
// Forcing the blur factor / add (and both blur maxima) to 0 leaves every DOF pass with zero blur (test 025: an
// extreme forced DOF becomes fully sharp; ForceDofEnable 0 alone does nothing). Game values restored when off.
static std::vector<std::pair<std::string, std::string>> g_dofSaved;
static void DofOverride(bool off) {
    static const char* kF[] = {"ForceDofBlurFactor", "ForceDofBlurAdd", "ForceSimpleDofBlurMax", "ForceSpriteDofBlurMax"};
    if (off) {
        for (const char* f : kF) {
            std::string cur = FbGet("GlobalPostProcessSettings", f);
            if (cur.empty()) return;
            std::string key = std::string("GlobalPostProcessSettings.") + f;
            bool saved = false;
            for (auto& kv : g_dofSaved) if (kv.first == key) saved = true;
            if (!saved) g_dofSaved.push_back({key, cur});
            if (cur != "0") FbSet((key + " 0").c_str());
        }
        static bool logged = false;
        if (!logged) { logged = true; LOGI("depth of field: off (forced blur 0)"); }
    } else if (!g_dofSaved.empty()) {
        for (auto& kv : g_dofSaved) FbSet((kv.first + " " + kv.second).c_str());
        g_dofSaved.clear();
        LOGI("depth of field: game values restored");
    }
}

static void ApplyRenderSize() {
    DofOverride(g_cfg.vrMode != 0 && g_cfg.noDof);
    StereoOverrides(g_cfg.vrMode != 0);
    StereoAa(g_cfg.vrMode == 2);
    if (!g_gameWnd) return;
    int w, h;
    ComputeTarget(w, h);
    // quality comes from the game's own resolution scale (its options menu); the mod only shapes the window
    if (w == g_lastAppliedW && h == g_lastAppliedH) return;
    g_lastAppliedW = w; g_lastAppliedH = h;
    g_targetW = w; g_targetH = h;
    WINDOWPLACEMENT wp{sizeof(wp)};
    GetWindowPlacement(g_gameWnd, &wp);
    if (w > 0) {
        if (!g_haveSaved) { GetWindowRect(g_gameWnd, &g_savedRect); g_haveSaved = true; }
        if (IsZoomed(g_gameWnd)) ShowWindowAsync(g_gameWnd, SW_RESTORE);
        // a window of the same aspect that fits the monitor's work area; its client size differs from the current
        // one, so the game sees WM_SIZE and resizes (our hook substitutes the eye size)
        HMONITOR mon = MonitorFromWindow(g_gameWnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        int maxW = (mi.rcWork.right - mi.rcWork.left) * 9 / 10, maxH = (mi.rcWork.bottom - mi.rcWork.top) * 9 / 10;
        float s = std::fmin((float)maxW / w, (float)maxH / h);
        RECT r{0, 0, (LONG)(w * s), (LONG)(h * s)};
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongW(g_gameWnd, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(g_gameWnd, GWL_EXSTYLE));
        LOGI("render size: window client %ldx%ld (headset aspect %.3f)", (long)(w * s), (long)(h * s), (float)w / h);
        SetWindowPos(g_gameWnd, nullptr, mi.rcWork.left + 20, mi.rcWork.top + 20, r.right - r.left, r.bottom - r.top,
                     SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
    } else if (g_haveSaved) {
        LOGI("render size: back to the window size");
        SetWindowPos(g_gameWnd, nullptr, g_savedRect.left, g_savedRect.top, g_savedRect.right - g_savedRect.left,
                     g_savedRect.bottom - g_savedRect.top, SWP_NOZORDER | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
        g_haveSaved = false;
    }
}

static HRESULT STDMETHODCALLTYPE Hook_ResizeBuffers(IDXGISwapChain* sc, UINT n, UINT w, UINT h, DXGI_FORMAT f, UINT fl) {
    LOGI("ResizeBuffers %ux%u fmt %d count %u flags 0x%X", w, h, (int)f, n, fl);
    XrOnResize();
    return o_ResizeBuffers(sc, n, w, h, f, fl);
}

static void DescribeAddress(const char* what, void* p) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = L"?";
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)p, &m))
        GetModuleFileNameW(m, path, MAX_PATH);
    LOGI("%s at %p in %S+0x%llX", what, p, path, (unsigned long long)((uintptr_t)p - (uintptr_t)m));
}

bool InstallPresentHooks() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DAIVR_dummy";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    auto create = (PFN_D3D11_CREATE_DEVICE_AND_SWAP_CHAIN)GetProcAddress(GetModuleHandleW(L"d3d11.dll"), "D3D11CreateDeviceAndSwapChain");
    if (!create) { LOGE("D3D11CreateDeviceAndSwapChain not found"); DestroyWindow(hwnd); return false; }
    HRESULT hr = create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &fl, 1, D3D11_SDK_VERSION,
                                               &sd, &sc, &dev, nullptr, &ctx);
    if (FAILED(hr)) {
        LOGE("dummy D3D11CreateDeviceAndSwapChain failed 0x%08X", (unsigned)hr);
        DestroyWindow(hwnd);
        return false;
    }
    void** vt = *(void***)sc;
    void* present = vt[8];         // IDXGISwapChain::Present
    void* resize = vt[13];         // IDXGISwapChain::ResizeBuffers
    DescribeAddress("Present", present);
    DescribeAddress("ResizeBuffers", resize);

    bool ok = true;
    if (MH_CreateHook(present, (void*)Hook_Present, (void**)&o_Present) != MH_OK || MH_EnableHook(present) != MH_OK) {
        LOGE("could not hook Present");
        ok = false;
    }
    if (MH_CreateHook(resize, (void*)Hook_ResizeBuffers, (void**)&o_ResizeBuffers) != MH_OK || MH_EnableHook(resize) != MH_OK) {
        LOGE("could not hook ResizeBuffers");
        ok = false;
    }
    if (g_cfg.captureHooks) InstallCaptureHooks(dev, ctx);
    else InstallStereoCbHooks(dev, ctx);
    sc->Release();
    ctx->Release();
    dev->Release();
    DestroyWindow(hwnd);
    if (ok) LOGI("Present/ResizeBuffers hooked");
    return ok;
}

} // namespace mea
