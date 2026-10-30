// Render resolution at headset size (game-independent, for windowed / borderless D3D games).
//
// The game renders at its window's client size (the swapchain follows WM_SIZE). To render each eye at the headset's
// resolution the game window's client area is set to the target size - larger than the monitor if needed: the window
// procedure is subclassed so WM_GETMINMAXINFO allows it and (render_resolution_hold = 1) WM_WINDOWPOSCHANGING keeps it.
// The resize is done once, on the window's own thread (posted message), after the game has shown frames for a while
// (resizing during startup loading crashed other engines).
//
// Config: render_resolution = game | auto | WxH ; render_scale (auto only, default 1.0) ; render_resolution_hold.
// auto = 16:9 at the headset's recommended eye height * render_scale (game UIs often only work at 16:9).
#include <windows.h>
#include <dxgi.h>
#include <atomic>
#include <string>
#include <cstdio>
#include "window_size.h"
#include "engine.h"
#include "settings.h"
#include <shlobj.h>
#include <fstream>
#include <sstream>
#include "xr.h"
#include <cmath>
#include "globals.h"
#include "log.h"

namespace mea {

static const UINT WM_DAIVR_RESIZE = WM_APP + 0x3A7;
static HWND g_hwnd = nullptr;
static WNDPROC g_orig = nullptr;
static std::atomic<int> g_tw{0}, g_th{0};          // target client size (0 = leave the game alone)
static std::atomic<bool> g_hold{true};
static std::atomic<int> g_state{0};                // 0 waiting, 1 posted, 2 done
static uint64_t g_presents = 0;
static std::atomic<bool> g_dirty{true};
static std::string g_cfgMode;

static bool ClientSize(HWND h, int& w, int& ht) { RECT r{}; if (!GetClientRect(h, &r)) return false; w = r.right; ht = r.bottom; return true; }

static void OuterSize(HWND h, int cw, int ch, int& ow, int& oh) {
    RECT r{0, 0, cw, ch};
    DWORD st = (DWORD)GetWindowLongPtrW(h, GWL_STYLE), ex = (DWORD)GetWindowLongPtrW(h, GWL_EXSTYLE);
    AdjustWindowRectEx(&r, st, FALSE, ex);
    ow = r.right - r.left; oh = r.bottom - r.top;
}

static LRESULT CALLBACK SubProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    int tw = g_tw.load(), th = g_th.load();
    if (m == WM_DAIVR_RESIZE && tw > 0) {
        int ow, oh; OuterSize(h, tw, th, ow, oh);
        RECT wr{}; GetWindowRect(h, &wr);
        BOOL ok = SetWindowPos(h, nullptr, wr.left, wr.top, ow, oh, SWP_NOZORDER | SWP_NOACTIVATE);
        int cw = 0, ch = 0; ClientSize(h, cw, ch);
        LOGI("render resolution: window resized to client %dx%d (wanted %dx%d, outer %dx%d) %s", cw, ch, tw, th, ow, oh, ok ? "" : "FAILED");
        g_state = 2;
        return 0;
    }
    if (tw > 0 && g_state.load() == 2) {
        if (m == WM_GETMINMAXINFO) {
            LRESULT r = CallWindowProcW(g_orig, h, m, wp, lp);
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMaxTrackSize.x = mm->ptMaxSize.x = 16384; mm->ptMaxTrackSize.y = mm->ptMaxSize.y = 16384;
            return r;
        }
        if (m == WM_WINDOWPOSCHANGING && g_hold.load()) {
            WINDOWPOS* p = (WINDOWPOS*)lp;
            if (!(p->flags & SWP_NOSIZE)) {
                int ow, oh; OuterSize(h, tw, th, ow, oh);
                if (p->cx != ow || p->cy != oh) {
                    static int logged = 0;
                    if (logged++ < 5) LOGI("render resolution: game resized the window to %dx%d, kept at %dx%d", p->cx, p->cy, ow, oh);
                    p->cx = ow; p->cy = oh;
                }
            }
        }
    } else if (m == WM_GETMINMAXINFO && g_state.load() == 1 && tw > 0) {
        LRESULT r = CallWindowProcW(g_orig, h, m, wp, lp);
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMaxTrackSize.x = mm->ptMaxSize.x = 16384; mm->ptMaxTrackSize.y = mm->ptMaxSize.y = 16384;
        return r;
    }
    return CallWindowProcW(g_orig, h, m, wp, lp);
}

static std::string g_status = "render size: game";
std::string WindowSizeStatus() {
    char b[96];
    int w = 0, h = 0;
    { D3D11_TEXTURE2D_DESC d; if (EngineEyeTexture(0, &d)) { w = d.Width; h = d.Height; } }
    if (w) snprintf(b, sizeof b, "eye %dx%d (%s)", w, h, g_status.c_str()); else snprintf(b, sizeof b, "%s", g_status.c_str());
    return b;
}

void WindowSizeApplyConfig() {
    std::string m = G().cfg.GetStr("render_resolution", "auto");
    g_cfgMode = m;
    g_hold = G().cfg.GetBool("render_resolution_hold", true);
    int w = 0, h = 0;
    if (m == "game" || m == "off" || m == "0") { w = h = 0; }
    else if (m != "auto" && sscanf(m.c_str(), "%dx%d", &w, &h) != 2) { LOGW("render_resolution '%s' not understood (game | auto | WxH)", m.c_str()); w = h = 0; }
    if (m == "auto") w = -1;   // resolved once the headset is known
    g_tw = w; g_th = h;
    g_dirty = true;
    LOGI("render resolution config: %s, scale %.2f, hold %d", m.c_str(), G().cfg.GetFloat("render_scale", 1.0f), (int)g_hold.load());
}

// Render thread, every Present.
void WindowSizeOnPresent(IDXGISwapChain* sc, bool vrActive) {
    ++g_presents;
    // DAI: once the window has the target size, let the engine follow (its resize flag), checked every 60 presents
    if (g_state.load() == 2 && g_hwnd && g_tw.load() > 0 && (g_presents % 60) == 0) {
        DXGI_SWAP_CHAIN_DESC cd{};
        if (SUCCEEDED(sc->GetDesc(&cd)) && ((int)cd.BufferDesc.Width != g_tw.load() || (int)cd.BufferDesc.Height != g_th.load())) {
            static int tries = 0;
            if (tries++ < 20) EngineRequestScreenResize(g_hwnd);
        }
    }
    if (!vrActive) return;
    if (!g_dirty.load() && g_state.load() != 0) return;
    if (g_tw.load() == 0) { g_dirty = false; return; }
    if (g_presents < 600) return;   // let the game come up first
    if (g_tw.load() < 0) {   // auto
        int rw = 0, rh = 0;
        EngineGetEyeRecommended(rw, rh);
        if (rh <= 0) return;   // headset not known yet
        float sc = G().cfg.GetFloat("render_scale", 1.0f);
        int h = int(rh * sc + 0.5f) & ~1;
        if (h < 360) h = 360;
        // render_aspect = headset (default: the eye image has the headset's own shape, square pixels) | 16:9
        bool hsAspect = G().cfg.GetStr("render_aspect", "headset") != "16:9";
        int w = hsAspect ? (int)((int64_t)h * rw / rh) & ~1 : (h * 16 / 9) & ~1;
        // limits: the runtime's largest swapchain image (each eye image goes into one) and D3D11's 16384
        uint32_t mw = 0, mh = 0;
        XrMaxSwapchain(mw, mh);
        int lw = (mw ? (int)mw : 16384), lh = (mh ? (int)mh : 16384);
        if (lw > 16384) lw = 16384;
        if (lh > 16384) lh = 16384;
        if (w > lw || h > lh) {
            float k = std::fmin((float)lw / w, (float)lh / h);
            int w2 = int(w * k) & ~1, h2 = int(h * k) & ~1;
            LOGW("render resolution: %dx%d is larger than the headset runtime allows (%dx%d) - using %dx%d (render_scale %.2f is the most that helps)",
                 w, h, lw, lh, w2, h2, sc * k);
            w = w2; h = h2;
        }
        g_tw = w; g_th = h;
        LOGI("render resolution auto: headset eye %dx%d x %.2f -> %dx%d (%s)", rw, rh, sc, w, h, hsAspect ? "headset aspect" : "16:9");
    }
    DXGI_SWAP_CHAIN_DESC d{};
    if (FAILED(sc->GetDesc(&d)) || !d.OutputWindow) return;
    HWND h = d.OutputWindow;
    int cw = 0, ch = 0; ClientSize(h, cw, ch);
    g_dirty = false;
    if (cw == g_tw.load() && ch == g_th.load()) { g_state = 2; return; }
    static bool resizedOnce = false;
    if (resizedOnce) {   // test 051: a second resize in game left the left eye black - only once per launch
        LOGW("render resolution: %dx%d takes effect after restarting the game (changing it in game breaks the left eye)", g_tw.load(), g_th.load());
        g_tw = cw; g_th = ch; g_state = 2;
        return;
    }
    resizedOnce = true;
    if (!d.Windowed) { g_status = "fullscreen - set the game to windowed"; LOGW("render resolution: exclusive fullscreen - set the game to windowed / borderless to change the render size"); g_state = 2; return; }
    if (!g_orig) {
        g_hwnd = h;
        g_orig = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)SubProc);
        if (!g_orig) { LOGE("render resolution: could not subclass the game window (%lu)", GetLastError()); g_tw = 0; return; }
    }
    LOGI("render resolution: client %dx%d -> %dx%d", cw, ch, g_tw.load(), g_th.load());
    { char b[64]; snprintf(b, sizeof b, "target %dx%d", g_tw.load(), g_th.load()); g_status = b; }
    g_state = 1;
    PostMessageW(h, WM_DAIVR_RESIZE, 0, 0);
}

} // namespace mea

// The engine only resizes its back buffer in Windowed display mode (fullscreen and borderless are both "fullscreen"
// screen modes). Before the game reads its options, switch GstRender.FullscreenEnabled to 0 in the profile
// (Documents\BioWare\Dragon Age Inquisition\Save\ProfileOptions_profile). Config: force_windowed (default 1,
// only when vr_mode is head/stereo and render_resolution is not "game").
namespace mea {
void WindowSizeForceWindowedProfile() {
    if (g_cfg.vrMode == 0) return;
    if (G().cfg.GetStr("render_resolution", "auto") == "game") return;
    if (!G().cfg.GetBool("force_windowed", true)) { LOGI("force_windowed = 0: game display mode left as it is"); return; }
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) || !docs) { LOGW("force_windowed: Documents folder not found"); return; }
    std::wstring path = std::wstring(docs) + L"\\BioWare\\Dragon Age Inquisition\\Save\\ProfileOptions_profile";
    CoTaskMemFree(docs);
    std::string t;
    { std::ifstream f(path, std::ios::binary); if (!f) { LOGW("force_windowed: %S not found", path.c_str()); return; } std::stringstream ss; ss << f.rdbuf(); t = ss.str(); }
    const char* key = "GstRender.FullscreenEnabled 1";
    size_t at = t.find(key);
    if (at == std::string::npos) { LOGI("force_windowed: game already set to Windowed"); return; }
    t[at + strlen(key) - 1] = '0';
    std::ofstream o(path, std::ios::binary | std::ios::trunc);
    if (!o) { LOGW("force_windowed: cannot write %S", path.c_str()); return; }
    o.write(t.data(), t.size());
    LOGI("force_windowed: game Display Mode switched from fullscreen/borderless to Windowed (needed for render_resolution; force_windowed = 0 to keep it)");
}
}  // namespace mea
