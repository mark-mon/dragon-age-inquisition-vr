// OpenXR in the game process (D3D11). Step 1: the game's back buffer on a world-locked quad ("flat screen").
#include "headaim.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <vector>
#include <string>
#include <cstdio>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include "xr.h"
#include "window_size.h"
#include "globals.h"
#include "log.h"
#include "blit.h"
#include "settings.h"
#include "headpose.h"
#include "engine.h"
#include "finalpass.h"
#include "vrs.h"
#include "vrpad.h"
#include "weapondetect.h"
#include "depthprobe.h"
#include "menu_options.h"
#include "../vrkit/vrkit_menu.h"
#include "../vrkit/vrkit_input.h"
#include "../vrkit/vrkit_menu_placement.h"
#include <cmath>

namespace mea {

static XrInstance g_inst = XR_NULL_HANDLE;
static XrSystemId g_sys = XR_NULL_SYSTEM_ID;
static volatile LONG g_instReady = 0;
static bool g_failed = false;

static XrSession g_sess = XR_NULL_HANDLE;
static XrSpace g_local = XR_NULL_HANDLE;
static XrSpace g_view = XR_NULL_HANDLE;
// eye gaze (XR_EXT_eye_gaze_interaction)
static bool g_hasGazeExt = false, g_gazeSupported = false;
static XrActionSet g_gazeSet = XR_NULL_HANDLE;
static XrAction g_gazeAct = XR_NULL_HANDLE;
static XrSpace g_gazeSpace = XR_NULL_HANDLE;
static bool g_gazeValid = false;
static float g_gazeTan[2] = {0, 0};
static DWORD g_gazeLastOk = 0;
// recentre: the screen is placed relative to this heading / position (LOCAL space)
static float g_rcYaw = 0;
static XrVector3f g_rcPos{0, 0, 0};
static volatile LONG g_recenterReq = 0;
static volatile LONG g_formatChanged = 0;
// menu layer
static XrSwapchain g_msc = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageD3D11KHR> g_mimgs;
static bool g_menuInit = false, g_menuScFailed = false;
static vrkit::menu::QuadPlacer g_placer;
// HUD: the game's UI layer on its own quad (stereo mode)
static XrSwapchain g_hsc = XR_NULL_HANDLE;
static std::vector<XrSwapchainImageD3D11KHR> g_himgs;
static uint32_t g_hscW = 0, g_hscH = 0;
static vrkit::menu::QuadPlacer g_hudPlacer;
static LONG g_lastEyeFrame[2] = {-1, -1};
static int g_staleFrames = 0;
static volatile LONG g_menuDumpReq = 0;
static int g_menuDumpCount = 0;
static double g_fpsGame = 0, g_fpsXr = 0, g_waitAvg = 0;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_running = false;

static XrSwapchain g_sc = XR_NULL_HANDLE;
static UINT g_scW = 0, g_scH = 0;
static DXGI_FORMAT g_scSrcFmt = DXGI_FORMAT_UNKNOWN;
static int64_t g_scFmt = 0;
static std::vector<XrSwapchainImageD3D11KHR> g_imgs;
static bool g_scUnsupported = false;
static bool g_blit = false;          // true: shader blit instead of CopyResource

static ID3D11Device* g_dev = nullptr;
static ID3D11DeviceContext* g_ctx = nullptr;
static ID3D11Texture2D* g_resolve = nullptr;   // for MSAA back buffers

static volatile LONG g_dumpReq = 0;
static int g_dumpCount = 0;
static bool g_stereoDumpReq = false;
static bool g_stereoDumpReqHud = false;
static uint64_t g_xrFrames = 0, g_presents = 0, g_rendered = 0;
static double g_waitMsSum = 0;
static DWORD g_statTick = 0;

static const char* XrRes(XrResult r) {
    static char b[64];
    if (g_inst && xrResultToString(g_inst, r, b) == XR_SUCCESS) return b;
    switch (r) {
    case XR_ERROR_RUNTIME_FAILURE: return "XR_ERROR_RUNTIME_FAILURE";
    case XR_ERROR_RUNTIME_UNAVAILABLE: return "XR_ERROR_RUNTIME_UNAVAILABLE";
    case XR_ERROR_INSTANCE_LOST: return "XR_ERROR_INSTANCE_LOST";
    case XR_ERROR_EXTENSION_NOT_PRESENT: return "XR_ERROR_EXTENSION_NOT_PRESENT";
    case XR_ERROR_API_VERSION_UNSUPPORTED: return "XR_ERROR_API_VERSION_UNSUPPORTED";
    default: break;
    }
    snprintf(b, sizeof(b), "XrResult %d", (int)r);
    return b;
}
#define XRCHECK(call)                                                       \
    do {                                                                    \
        XrResult r_ = (call);                                               \
        if (XR_FAILED(r_)) { LOGE("%s failed: %s", #call, XrRes(r_)); return false; } \
    } while (0)

ID3D11Device* XrGameDevice() { return g_dev; }

// stereo: one swapchain per eye, filled from the engine's per-view output
static XrSwapchain g_esc[2] = {};
static std::vector<XrSwapchainImageD3D11KHR> g_eimgs[2];
static UINT g_escW[2] = {}, g_escH[2] = {};

static bool EnsureEyeSwapchain(int e, UINT w, UINT h) {
    if (g_esc[e] && g_escW[e] == w && g_escH[e] == h) return true;
    if (g_esc[e]) { xrDestroySwapchain(g_esc[e]); g_esc[e] = XR_NULL_HANDLE; blit::ForgetTargets(); }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ci.sampleCount = 1; ci.width = w; ci.height = h; ci.faceCount = ci.arraySize = ci.mipCount = 1;
    if (XR_FAILED(xrCreateSwapchain(g_sess, &ci, &g_esc[e]))) { LOGE("xr: eye swapchain %d failed", e); return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_esc[e], 0, &n, nullptr);
    g_eimgs[e].assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(g_esc[e], n, &n, (XrSwapchainImageBaseHeader*)g_eimgs[e].data());
    g_escW[e] = w; g_escH[e] = h;
    LOGI("xr: eye %d swapchain %ux%u", e, w, h);
    return true;
}

void XrRequestDump() { InterlockedExchange(&g_dumpReq, 1); }
void XrRequestMenuDump() { InterlockedExchange(&g_menuDumpReq, 1); }
void XrRecenter() { InterlockedExchange(&g_recenterReq, 1); }
void XrScreenFormatChanged() { InterlockedExchange(&g_formatChanged, 1); }
static uint32_t g_maxSwW = 0, g_maxSwH = 0;
void XrMaxSwapchain(uint32_t& w, uint32_t& h) { w = g_maxSwW; h = g_maxSwH; }
std::string XrStatusText() {
    char b[256];
    if (!g_running) snprintf(b, sizeof(b), "game %.0f fps | VR not running (state %d)", g_fpsGame, (int)g_state);
    else snprintf(b, sizeof(b), "game %.0f fps | headset %.0f fps | xrWaitFrame %.1f ms | %s", g_fpsGame, g_fpsXr, g_waitAvg,
                  WindowSizeStatus().c_str());
    return b;
}

// ------------------------------------------------------------------ instance (worker thread)
static bool CreateInstance() {
    uint32_t n = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr))) { LOGW("xr: no OpenXR runtime / loader failed"); return false; }
    std::vector<XrExtensionProperties> ext(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, ext.data());
    bool hasD3D11 = false;
    std::string list;
    for (auto& e : ext) {
        if (!strcmp(e.extensionName, XR_KHR_D3D11_ENABLE_EXTENSION_NAME)) hasD3D11 = true;
        if (!strcmp(e.extensionName, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)) g_hasGazeExt = true;
        list += e.extensionName; list += " ";
    }
    LOGD("xr: runtime extensions: %s", list.c_str());
    if (!hasD3D11) { LOGE("xr: runtime has no %s", XR_KHR_D3D11_ENABLE_EXTENSION_NAME); g_failed = true; return false; }

    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME, XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
    LOGI("xr: eye gaze extension %s", g_hasGazeExt ? "available" : "not offered by the runtime");
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy_s(ci.applicationInfo.applicationName, "MassEffectAndromedaVR");
    ci.applicationInfo.applicationVersion = 1;
    strcpy_s(ci.applicationInfo.engineName, "MEAVR");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = g_hasGazeExt ? 2 : 1;
    ci.enabledExtensionNames = exts;
    XrResult r = xrCreateInstance(&ci, &g_inst);
    if (XR_FAILED(r)) { LOGW("xr: xrCreateInstance failed: %s (is SteamVR running?)", XrRes(r)); g_inst = XR_NULL_HANDLE; return false; }
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_inst, &ip);
    LOGI("xr: runtime %s %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion),
         XR_VERSION_PATCH(ip.runtimeVersion));
    return true;
}

static bool GetSystem() {
    XrSystemGetInfo gi{XR_TYPE_SYSTEM_GET_INFO};
    gi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r = xrGetSystem(g_inst, &gi, &g_sys);
    if (XR_FAILED(r)) { LOGW("xr: xrGetSystem: %s (headset not available yet?)", XrRes(r)); return false; }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    XrSystemEyeGazeInteractionPropertiesEXT gp{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT};
    if (g_hasGazeExt) sp.next = &gp;
    xrGetSystemProperties(g_inst, g_sys, &sp);
    g_gazeSupported = g_hasGazeExt && gp.supportsEyeGazeInteraction;
    if (g_hasGazeExt) LOGI("xr: eye tracking %s", g_gazeSupported ? "supported by this headset" : "not supported by this headset");
    g_maxSwW = sp.graphicsProperties.maxSwapchainImageWidth; g_maxSwH = sp.graphicsProperties.maxSwapchainImageHeight;
    LOGI("xr: system '%s' vendor %u, max layers %u, max swapchain %ux%u", sp.systemName, sp.vendorId,
         sp.graphicsProperties.maxLayerCount, sp.graphicsProperties.maxSwapchainImageWidth, sp.graphicsProperties.maxSwapchainImageHeight);
    uint32_t n = 0;
    xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    std::vector<XrViewConfigurationView> v(n, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, v.data());
    for (uint32_t i = 0; i < n; ++i) LOGI("xr: view %u recommended %ux%u", i, v[i].recommendedImageRectWidth, v[i].recommendedImageRectHeight);
    if (n) EngineSetEyeRecommended((int)v[0].recommendedImageRectWidth, (int)v[0].recommendedImageRectHeight);
    return true;
}

void XrInitInstanceLoop() {
    for (int i = 0; i < 60 && !g_inst; ++i) { if (CreateInstance()) break; if (g_failed) return; Sleep(5000); }
    if (!g_inst) { LOGE("xr: giving up on xrCreateInstance"); return; }
    for (int i = 0; ; ++i) {
        if (GetSystem()) break;
        if (i == 100) { LOGE("xr: giving up on xrGetSystem"); return; }
        Sleep(3000);
    }
    InterlockedExchange(&g_instReady, 1);
}

// ------------------------------------------------------------------ eye gaze
static void CreateGazeActions() {
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asi.actionSetName, "meavr_gaze");
    strcpy_s(asi.localizedActionSetName, "DAIVR eye gaze");
    if (XR_FAILED(xrCreateActionSet(g_inst, &asi, &g_gazeSet))) { LOGW("xr: gaze action set failed"); return; }
    XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
    aci.actionType = XR_ACTION_TYPE_POSE_INPUT;
    strcpy_s(aci.actionName, "gaze");
    strcpy_s(aci.localizedActionName, "Eye gaze");
    if (XR_FAILED(xrCreateAction(g_gazeSet, &aci, &g_gazeAct))) { LOGW("xr: gaze action failed"); return; }
    XrPath prof, pose;
    xrStringToPath(g_inst, "/interaction_profiles/ext/eye_gaze_interaction", &prof);
    xrStringToPath(g_inst, "/user/eyes_ext/input/gaze_ext/pose", &pose);
    XrActionSuggestedBinding b{g_gazeAct, pose};
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = prof;
    sb.countSuggestedBindings = 1;
    sb.suggestedBindings = &b;
    XrResult r = xrSuggestInteractionProfileBindings(g_inst, &sb);
    if (XR_FAILED(r)) { LOGW("xr: gaze binding failed: %s", XrRes(r)); return; }
    XrActionSpaceCreateInfo spi{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spi.action = g_gazeAct;
    spi.poseInActionSpace.orientation.w = 1;
    if (XR_FAILED(xrCreateActionSpace(g_sess, &spi, &g_gazeSpace))) { LOGW("xr: gaze space failed"); g_gazeSpace = XR_NULL_HANDLE; return; }
    LOGI("xr: eye gaze action ready");
}

// all action sets (gaze, controllers) are attached together - a session allows only one attach
static XrActionSet g_padSet = XR_NULL_HANDLE;
static bool g_actionsAttached = false;
static void CreateActions() {
    if (g_gazeSupported) CreateGazeActions();
    g_padSet = g_cfg.vrPad ? VrPadCreate(g_inst) : XR_NULL_HANDLE;   // DAI: vr_pad is config only, default off
    if (!g_cfg.vrPad) LOGI("vrpad: off (vr_pad = 0) - no controller actions");
    XrActionSet sets[2]; uint32_t n = 0;
    if (g_gazeSet && g_gazeSpace) sets[n++] = g_gazeSet;
    if (g_padSet) sets[n++] = g_padSet;
    if (!n) return;
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = n;
    at.actionSets = sets;
    XrResult r = xrAttachSessionActionSets(g_sess, &at);
    if (XR_FAILED(r)) { LOGW("xr: attaching the action sets failed: %s", XrRes(r)); g_gazeSpace = XR_NULL_HANDLE; g_padSet = XR_NULL_HANDLE; return; }
    g_actionsAttached = true;
    if (g_padSet) VrPadSessionReady(g_sess);
}
static void SyncActions() {
    if (!g_actionsAttached) return;
    XrActiveActionSet aas[2]; uint32_t n = 0;
    if (g_gazeSet && g_gazeSpace) aas[n++] = {g_gazeSet, XR_NULL_PATH};
    if (g_padSet) aas[n++] = {g_padSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = n;
    si.activeActionSets = aas;
    xrSyncActions(g_sess, &si);
}

// gaze direction as tangents in the head (VIEW) space; held 0.2 s through blinks, then invalid
static void UpdateGaze(XrTime t) {
    bool want = g_cfg.vrsGaze && g_cfg.vrsEnabled && g_cfg.vrMode == 2;
    bool ok = false;
    float tx = 0, ty = 0;
    if (want && g_cfg.debugFakeGaze) {
        float a = GetTickCount() / 1000.f * 1.2566f;
        tx = 0.45f * std::cos(a); ty = 0.3f * std::sin(a);
        ok = true;
    } else if (want && g_gazeSpace) {
        XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(g_gazeSpace, g_view, t, &loc)) &&
            (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) {
            float q[4] = {loc.pose.orientation.x, loc.pose.orientation.y, loc.pose.orientation.z, loc.pose.orientation.w};
            float f[3] = {0, 0, -1}, d[3];
            QuatRotate(q, f, d);
            if (d[2] < -0.1f) { tx = d[0] / -d[2]; ty = d[1] / -d[2]; ok = true; }
        }
    }
    DWORD now = GetTickCount();
    if (ok) {
        bool fresh = !g_gazeValid || now - g_gazeLastOk > 200;
        float k = fresh ? 1.f : 0.5f;   // light smoothing
        g_gazeTan[0] += k * (tx - g_gazeTan[0]);
        g_gazeTan[1] += k * (ty - g_gazeTan[1]);
        g_gazeLastOk = now;
        g_gazeValid = true;
    } else if (!want || now - g_gazeLastOk > 200) g_gazeValid = false;
    static int logged = -1;
    int st = !want ? 0 : g_gazeValid ? 2 : 1;
    if (st != logged) { logged = st; LOGI("xr: eye gaze %s", st == 0 ? "off" : st == 2 ? "tracking" : "not available / not tracking - fixed foveation"); }
}

bool XrGazeActive() { return g_gazeValid; }
bool XrGazeSupported() { return g_gazeSupported || g_cfg.debugFakeGaze; }

// ------------------------------------------------------------------ session (render thread)
static bool CreateSession(IDXGISwapChain* sc) {

    PFN_xrGetD3D11GraphicsRequirementsKHR getReq = nullptr;
    XRCHECK(xrGetInstanceProcAddr(g_inst, "xrGetD3D11GraphicsRequirementsKHR", (PFN_xrVoidFunction*)&getReq));
    XrGraphicsRequirementsD3D11KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    XRCHECK(getReq(g_inst, g_sys, &req));
    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* ad = nullptr;
    DXGI_ADAPTER_DESC ad_desc{};
    if (SUCCEEDED(g_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && SUCCEEDED(dxgiDev->GetAdapter(&ad))) ad->GetDesc(&ad_desc);
    if (ad) ad->Release();
    if (dxgiDev) dxgiDev->Release();
    bool same = memcmp(&ad_desc.AdapterLuid, &req.adapterLuid, sizeof(LUID)) == 0;
    LOGI("xr: game adapter '%S'%s headset adapter; min FL 0x%X, device FL 0x%X", ad_desc.Description, same ? " is the" : " is NOT the",
         (unsigned)req.minFeatureLevel, (unsigned)g_dev->GetFeatureLevel());

    XrGraphicsBindingD3D11KHR gb{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_dev;
    XrSessionCreateInfo si{XR_TYPE_SESSION_CREATE_INFO};
    si.next = &gb;
    si.systemId = g_sys;
    XrResult r = xrCreateSession(g_inst, &si, &g_sess);
    if (XR_FAILED(r)) { LOGE("xr: xrCreateSession: %s", XrRes(r)); g_failed = true; return false; }

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1;
    XRCHECK(xrCreateReferenceSpace(g_sess, &rs, &g_local));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    XRCHECK(xrCreateReferenceSpace(g_sess, &rs, &g_view));
    CreateActions();
    LOGI("xr: session created");
    return true;
}

static void PollEvents() {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED) {
            // which controllers SteamVR reports (Touch, Index "knuckles", ...)
            const char* hands[2] = {"/user/hand/left", "/user/hand/right"};
            for (int h = 0; h < 2; ++h) {
                XrPath hp = XR_NULL_PATH; xrStringToPath(g_inst, hands[h], &hp);
                XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
                char name[256] = "none"; uint32_t n = 0;
                if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(g_sess, hp, &ps)) && ps.interactionProfile != XR_NULL_PATH)
                    xrPathToString(g_inst, ps.interactionProfile, sizeof(name), &n, name);
                LOGI("xr: %s controller profile: %s", h ? "right" : "left", name);
            }
        }
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto& s = *(XrEventDataSessionStateChanged*)&ev;
            g_state = s.state;
            LOGI("xr: session state %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(g_sess, &bi);
                g_running = XR_SUCCEEDED(r);
                LOGI("xr: xrBeginSession: %s", XrRes(r));
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_sess);
                g_running = false;
                LOGI("xr: session ended");
            } else if (g_state == XR_SESSION_STATE_EXITING || g_state == XR_SESSION_STATE_LOSS_PENDING) {
                g_running = false;
                g_failed = true;
                LOGW("xr: session exiting / lost - VR stopped for this run");
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g_running = false;
            g_failed = true;
            LOGW("xr: instance loss pending");
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// Swapchain formats that can receive a CopyResource from the back buffer, best first.
static std::vector<DXGI_FORMAT> Candidates(DXGI_FORMAT bb) {
    bool srgb = g_cfg.screenSrgb;   // back buffer holds gamma-encoded values: show them as sRGB
    switch (bb) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        if (srgb || bb == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM};
        return {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        if (srgb || bb == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) return {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM};
        return {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB};
    default: return {};   // other formats (10-bit, float) go through the shader blit
    }
}

static void DestroySwapchain() {
    if (g_sc) xrDestroySwapchain(g_sc);
    g_sc = XR_NULL_HANDLE;
    g_imgs.clear();
    blit::ForgetTargets();
    if (g_resolve) { g_resolve->Release(); g_resolve = nullptr; }
    g_scW = g_scH = 0;
}

void XrOnResize() { /* swapchain is recreated lazily when the back buffer size changes */ }

static bool EnsureSwapchain(const D3D11_TEXTURE2D_DESC& bb) {
    if (InterlockedExchange(&g_formatChanged, 0)) { DestroySwapchain(); g_scUnsupported = false; }
    if (g_sc && bb.Width == g_scW && bb.Height == g_scH && bb.Format == g_scSrcFmt) return true;
    if (g_scUnsupported && bb.Format == g_scSrcFmt) return false;
    DestroySwapchain();
    g_scSrcFmt = bb.Format;
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(g_sess, 0, &n, nullptr);
    std::vector<int64_t> fmts(n);
    xrEnumerateSwapchainFormats(g_sess, n, &n, fmts.data());
    std::string list;
    for (auto f : fmts) list += std::to_string(f) + " ";
    int64_t pick = 0;
    g_blit = g_cfg.screenForceBlit;
    if (!g_blit)
        for (DXGI_FORMAT c : Candidates(bb.Format)) {
            for (auto f : fmts) if (f == c) { pick = f; break; }
            if (pick) break;
        }
    if (!pick && blit::Init(g_dev, g_ctx)) {
        for (DXGI_FORMAT c : {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}) {
            for (auto f : fmts) if (f == c) { pick = f; break; }
            if (pick) break;
        }
        g_blit = pick != 0;
    }
    LOGI("xr: runtime swapchain formats: %s", list.c_str());
    if (!pick) {
        LOGE("xr: no runtime swapchain format can take a copy of back buffer format %d (runtime: %s)", (int)bb.Format, list.c_str());
        g_scUnsupported = true;
        return false;
    }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = pick;
    ci.sampleCount = 1;
    ci.width = bb.Width;
    ci.height = bb.Height;
    ci.faceCount = 1;
    ci.arraySize = 1;
    ci.mipCount = 1;
    XRCHECK(xrCreateSwapchain(g_sess, &ci, &g_sc));
    xrEnumerateSwapchainImages(g_sc, 0, &n, nullptr);
    g_imgs.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    XRCHECK(xrEnumerateSwapchainImages(g_sc, n, &n, (XrSwapchainImageBaseHeader*)g_imgs.data()));
    if (bb.SampleDesc.Count > 1) {
        D3D11_TEXTURE2D_DESC d = bb;
        d.SampleDesc = {1, 0};
        d.BindFlags = 0;
        d.MiscFlags = 0;
        g_dev->CreateTexture2D(&d, nullptr, &g_resolve);
    }
    g_scW = bb.Width;
    g_scH = bb.Height;
    g_scFmt = pick;
    blit::Reset();
    LOGI("xr: screen swapchain %ux%u format %lld (back buffer format %d, msaa %u), %u images, %s", g_scW, g_scH, (long long)pick,
         (int)bb.Format, bb.SampleDesc.Count, n, g_blit ? "shader blit" : "copy");
    return true;
}

// ------------------------------------------------------------------ dumps
static float Half(uint16_t h) {
    uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? m / 1024.f / 16384.f : (e == 31 ? 65504.f : (1.f + m / 1024.f) * ldexpf(1.f, (int)e - 15));
    return s ? -v : v;
}

static void DumpTexture(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& sd, const wchar_t* name, int index) {
    D3D11_TEXTURE2D_DESC d = sd;
    d.SampleDesc = {1, 0};
    d.Usage = D3D11_USAGE_STAGING;
    d.BindFlags = 0;
    d.MiscFlags = 0;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    d.MipLevels = 1;
    d.ArraySize = 1;
    ID3D11Texture2D* st = nullptr;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &st))) { LOGE("dump: staging texture failed"); return; }
    g_ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); LOGE("dump: map failed"); return; }
    UINT w = d.Width, h = d.Height;
    std::vector<uint8_t> bgr((size_t)w * h * 3);
    double sum = 0;
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
        uint8_t* out = &bgr[(size_t)(h - 1 - y) * w * 3];
        for (UINT x = 0; x < w; ++x) {
            float r, g, b;
            switch (d.Format) {
            case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
                b = row[x * 4] / 255.f; g = row[x * 4 + 1] / 255.f; r = row[x * 4 + 2] / 255.f; break;
            case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
                uint32_t v = ((const uint32_t*)row)[x];
                r = (v & 1023) / 1023.f; g = ((v >> 10) & 1023) / 1023.f; b = ((v >> 20) & 1023) / 1023.f; break; }
            case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
                const uint16_t* p = (const uint16_t*)row + x * 4;
                r = Half(p[0]); g = Half(p[1]); b = Half(p[2]); break; }
            default:
                r = row[x * 4] / 255.f; g = row[x * 4 + 1] / 255.f; b = row[x * 4 + 2] / 255.f; break;
            }
            auto c8 = [](float v) { return (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.f + 0.5f)); };
            out[x * 3] = c8(b); out[x * 3 + 1] = c8(g); out[x * 3 + 2] = c8(r);
            sum += out[x * 3] + out[x * 3 + 1] + out[x * 3 + 2];
        }
    }
    g_ctx->Unmap(st, 0);
    st->Release();

    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s%s_%03d.bmp", G().outDir.c_str(), name, index);
    FILE* f = _wfopen(path, L"wb");
    if (!f) { LOGE("dump: cannot write %S", path); return; }
    uint32_t rowBytes = (w * 3 + 3) & ~3u, img = rowBytes * h;
    BITMAPFILEHEADER fh{0x4D42, (DWORD)(54 + img), 0, 0, 54};
    BITMAPINFOHEADER ih{sizeof(ih), (LONG)w, (LONG)h, 1, 24, BI_RGB, img, 2835, 2835, 0, 0};
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    std::vector<uint8_t> pad(rowBytes, 0);
    for (UINT y = 0; y < h; ++y) { memcpy(pad.data(), &bgr[(size_t)y * w * 3], w * 3); fwrite(pad.data(), rowBytes, 1, f); }
    fclose(f);
    LOGI("dump: %S %ux%u fmt %d, mean level %.1f / 255", path, w, h, (int)d.Format, sum / ((double)w * h * 3));
}

// ------------------------------------------------------------------ menu layer
static bool EnsureMenuSwapchain() {
    if (g_msc) return true;
    if (g_menuScFailed) return false;
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;   // menu colours are authored as sRGB values
    ci.sampleCount = 1;
    ci.width = vrkit::menu::Width();
    ci.height = vrkit::menu::Height();
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(g_sess, &ci, &g_msc);
    if (XR_FAILED(r)) { LOGE("xr: menu swapchain: %s", XrRes(r)); g_menuScFailed = true; return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_msc, 0, &n, nullptr);
    g_mimgs.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(g_msc, n, &n, (XrSwapchainImageBaseHeader*)g_mimgs.data());
    LOGI("xr: menu swapchain %ux%u, %u images", ci.width, ci.height, n);
    return true;
}

static bool EnsureHudSwapchain(uint32_t w, uint32_t h) {
    if (g_hsc && g_hscW == w && g_hscH == h) return true;
    if (g_hsc) { xrDestroySwapchain(g_hsc); g_hsc = XR_NULL_HANDLE; blit::ForgetTargets(); }
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    ci.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    ci.sampleCount = 1;
    ci.width = w; ci.height = h;
    ci.faceCount = ci.arraySize = ci.mipCount = 1;
    XrResult r = xrCreateSwapchain(g_sess, &ci, &g_hsc);
    if (XR_FAILED(r)) { LOGE("xr: hud swapchain: %s", XrRes(r)); g_hsc = XR_NULL_HANDLE; return false; }
    uint32_t n = 0;
    xrEnumerateSwapchainImages(g_hsc, 0, &n, nullptr);
    g_himgs.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    xrEnumerateSwapchainImages(g_hsc, n, &n, (XrSwapchainImageBaseHeader*)g_himgs.data());
    g_hscW = w; g_hscH = h;
    LOGI("xr: hud swapchain %ux%u, %u images", w, h, n);
    return true;
}

static bool LocateHead(XrTime t, XrPosef& out) {
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_view, g_local, t, &loc))) return false;
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if ((loc.locationFlags & need) != need) return false;
    out = loc.pose;
    return true;
}

static float Yaw(const XrQuaternionf& q) {
    float fx = -2.0f * (q.x * q.z + q.w * q.y);
    float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    return std::atan2(-fx, -fz);
}

// ------------------------------------------------------------------ per frame
void XrOnPresent(IDXGISwapChain* sc) {
    ++g_presents;
    DWORD now = GetTickCount();
    if (now - g_statTick > 10000) {
        if (g_statTick) {
            double sec = (now - g_statTick) / 1000.0;
            g_fpsGame = g_presents / sec;
            g_fpsXr = g_xrFrames / sec;
            g_waitAvg = g_xrFrames ? g_waitMsSum / g_xrFrames : 0.0;
            LOGI("stats: %llu presents, %llu xr frames (%llu rendered), avg xrWaitFrame %.2f ms, state %d, menu %s", g_presents, g_xrFrames,
                 g_rendered, g_waitAvg, (int)g_state, vrkit::menu::IsOpen() ? "open" : "closed");
        }
        g_presents = g_xrFrames = g_rendered = 0;
        g_waitMsSum = 0;
        g_statTick = now;
    }
    if (!g_dev) {
        if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&g_dev))) { LOGE("xr: swapchain has no D3D11 device"); g_failed = true; return; }
        g_dev->GetImmediateContext(&g_ctx);
    }
    if (!g_menuInit) { g_menuInit = true; MenuOptions_Init(g_dev, g_ctx); }
    bool menuOpen = vrkit::menu::Update();   // input + redraw (also while VR is not running)

    if (g_failed || !g_instReady) return;
    if (!g_sess && !CreateSession(sc)) return;
    PollEvents();
    if (!g_running) return;

    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC bd;
    bb->GetDesc(&bd);
    bool haveSc = EnsureSwapchain(bd);

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    XrResult r = xrWaitFrame(g_sess, nullptr, &fs);
    QueryPerformanceCounter(&t1);
    if (XR_FAILED(r)) { LOGE("xrWaitFrame: %s", XrRes(r)); bb->Release(); return; }
    g_waitMsSum += (t1.QuadPart - t0.QuadPart) * 1000.0 / f.QuadPart;
    ++g_xrFrames;
    xrBeginFrame(g_sess, nullptr);
    SyncActions();
    VrPadUpdate(g_sess, g_local, fs.predictedDisplayTime, g_state == XR_SESSION_STATE_FOCUSED);

    XrPosef head{{0, 0, 0, 1}, {0, 0, 0}};
    bool headOk = LocateHead(fs.predictedDisplayTime, head);
    static bool autoRecentered = false;
    if (headOk && !autoRecentered) { autoRecentered = true; InterlockedExchange(&g_recenterReq, 1); }
    if (headOk && InterlockedExchange(&g_recenterReq, 0)) {
        g_rcYaw = Yaw(head.orientation);
        g_rcPos = head.position;
        g_placer.Reset();
        g_hudPlacer.Reset();
        LOGI("recentre: yaw %.1f deg, pos %.2f %.2f %.2f", g_rcYaw * 57.2958f, g_rcPos.x, g_rcPos.y, g_rcPos.z);
    }
    // head pose relative to the recentre reference -> engine hook (applied to the game camera)
    float qRefInv[4], qRef[4];
    QuatFromYaw(-g_rcYaw, qRefInv);
    QuatFromYaw(g_rcYaw, qRef);
    if (headOk) {
        HeadState hs;
        hs.valid = true;
        float hq[4] = {head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w};
        QuatMul(qRefInv, hq, hs.head.q);
        float d[3] = {head.position.x - g_rcPos.x, head.position.y - g_rcPos.y, head.position.z - g_rcPos.z};
        if (g_cfg.headPosition) QuatRotate(qRefInv, d, hs.head.p);
        if (g_cfg.debugFakeHead) {
            float t = GetTickCount() / 1000.f;
            float yaw = 0.5236f * std::sin(t * 1.2566f), pitch = 0.1745f * std::sin(t * 0.8f);   // 30 deg @ 0.2 Hz, 10 deg
            float qy[4], qp[4] = {std::sin(pitch * 0.5f), 0, 0, std::cos(pitch * 0.5f)};
            QuatFromYaw(yaw, qy);
            float qf[4]; QuatMul(qy, qp, qf);
            float qh[4]; QuatMul(qf, hs.head.q, qh);
            memcpy(hs.head.q, qh, sizeof(qh));
        }
        // eyes relative to the head
        XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
        li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
        li.displayTime = fs.predictedDisplayTime;
        li.space = g_view;
        XrViewState vs{XR_TYPE_VIEW_STATE};
        XrView xv[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
        uint32_t nv = 0;
        if (XR_SUCCEEDED(xrLocateViews(g_sess, &li, &vs, 2, &nv, xv)) && nv == 2) {
            for (int e = 0; e < 2; ++e) {
                hs.eye[e].q[0] = xv[e].pose.orientation.x; hs.eye[e].q[1] = xv[e].pose.orientation.y;
                hs.eye[e].q[2] = xv[e].pose.orientation.z; hs.eye[e].q[3] = xv[e].pose.orientation.w;
                hs.eye[e].p[0] = xv[e].pose.position.x; hs.eye[e].p[1] = xv[e].pose.position.y; hs.eye[e].p[2] = xv[e].pose.position.z;
                hs.fov[e][0] = xv[e].fov.angleLeft; hs.fov[e][1] = xv[e].fov.angleRight;
                hs.fov[e][2] = xv[e].fov.angleUp; hs.fov[e][3] = xv[e].fov.angleDown;
            }
            UpdateGaze(fs.predictedDisplayTime);
            for (int e = 0; e < 2; ++e) {
                if (!g_gazeValid) { VrsSetGaze(e, false, 0.5f, 0.5f); continue; }
                // gaze into the eye's frame (eyes may be canted), then into the eye image (frustum as rendered)
                float d[3] = {g_gazeTan[0], g_gazeTan[1], -1.f}, de[3];
                float qi[4] = {-hs.eye[e].q[0], -hs.eye[e].q[1], -hs.eye[e].q[2], hs.eye[e].q[3]};
                QuatRotate(qi, d, de);
                if (de[2] > -0.1f) { VrsSetGaze(e, false, 0.5f, 0.5f); continue; }
                float gx = de[0] / -de[2], gy = de[1] / -de[2];
                float l = std::tan(-hs.fov[e][0]), r = std::tan(hs.fov[e][1]), up = std::tan(hs.fov[e][2]), dn = std::tan(-hs.fov[e][3]);
                float x0, x1, y0, y1;   // tangent extents of the rendered image
                if (!g_cfg.eyeFov) { x0 = -l; x1 = r; y0 = -dn; y1 = up; }
                else if (g_cfg.eyeAsymmetric) { x0 = -l; x1 = r; y0 = -dn; y1 = up; }
                else { float tx = std::fmax(l, r), ty = std::fmax(up, dn); x0 = -tx; x1 = tx; y0 = -ty; y1 = ty; }
                float u = (gx - x0) / (x1 - x0), v = (y1 - gy) / (y1 - y0);
                VrsSetGaze(e, true, std::fmin(std::fmax(u, 0.f), 1.f), std::fmin(std::fmax(v, 0.f), 1.f));
            }
            static bool fovLogged = false;
            if (!fovLogged) {
                fovLogged = true;
                for (int e = 0; e < 2; ++e)
                    LOGI("xr: eye %d fov L %.1f R %.1f U %.1f D %.1f deg, pos %.4f %.4f %.4f", e, hs.fov[e][0] * 57.2958f, hs.fov[e][1] * 57.2958f,
                         hs.fov[e][2] * 57.2958f, hs.fov[e][3] * 57.2958f, hs.eye[e].p[0], hs.eye[e].p[1], hs.eye[e].p[2]);
            }
        }
        hs.frame = g_xrFrames;
        HeadPublish(hs);
        bool aimHeld = vrkit::input::LeftTrigger() > 0.35f || (GetAsyncKeyState(VK_RBUTTON) & 0x8000);
        if (vrkit::menu::IsOpen()) aimHeld = false;
        // aim ray in the recentre reference frame (real m): the head, or the right controller's aim pose
        float rayO[3] = {hs.head.p[0], hs.head.p[1], hs.head.p[2]}, rayD[3];
        const float fz[3] = {0, 0, -1};
        QuatRotate(hs.head.q, fz, rayD);
        XrPosef cp;
        if (g_cfg.headAimSource == 1 && VrPadAimPose(g_local, fs.predictedDisplayTime, 1, cp)) {
            float cq[4] = {cp.orientation.x, cp.orientation.y, cp.orientation.z, cp.orientation.w}, q[4];
            QuatMul(qRefInv, cq, q);
            QuatRotate(q, fz, rayD);
            XrVector3f base = g_cfg.headPosition ? g_rcPos : head.position;   // where the body camera is
            float dp[3] = {cp.position.x - base.x, cp.position.y - base.y, cp.position.z - base.z};
            QuatRotate(qRefInv, dp, rayO);
        }
        // the same ray in the left eye's frame, for the depth-buffer march
        {
            Pose e0 = PoseMul(hs.head, hs.eye[0]);
            float qi[4] = {-e0.q[0], -e0.q[1], -e0.q[2], e0.q[3]};
            float rel[3] = {rayO[0] - e0.p[0], rayO[1] - e0.p[1], rayO[2] - e0.p[2]}, oe[3], de[3];
            if (!g_cfg.headPosition) { float t[3]; QuatRotate(hs.head.q, hs.eye[0].p, t); for (int i = 0; i < 3; ++i) rel[i] = rayO[i] - t[i]; }
            QuatRotate(qi, rel, oe);
            QuatRotate(qi, rayD, de);
            DepthProbeSetRay(oe, de);
        }
        HeadAimUpdate(rayO, rayD, aimHeld);
    }

    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView pv[2] = {{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
    XrCompositionLayerQuad mquad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    const XrCompositionLayerBaseHeader* layers[4];
    XrCompositionLayerQuad hquad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    bool hudShown = false;
    FinalPassInfo fi;
    bool haveFi = FinalPassGet(fi);
    uint32_t nLayers = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = 100000000;  // 100 ms
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    if (fs.shouldRender && haveSc) {
        uint32_t idx = 0;
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_sc, &ai, &idx)) && XR_SUCCEEDED(xrWaitSwapchainImage(g_sc, &wi))) {
            ID3D11Texture2D* src = bb;
            if (g_resolve) { g_ctx->ResolveSubresource(g_resolve, 0, bb, 0, bd.Format); src = g_resolve; }
            if (g_blit) {
                D3D11_TEXTURE2D_DESC sdsc = bd;
                sdsc.SampleDesc = {1, 0};
                blit::Draw(src, sdsc, g_imgs[idx].texture, (DXGI_FORMAT)g_scFmt, g_scW, g_scH);
            } else
                g_ctx->CopyResource(g_imgs[idx].texture, src);
            xrReleaseSwapchainImage(g_sc, &ri);

            if (InterlockedExchange(&g_dumpReq, 0) || (g_cfg.dumpFrame > 0 && g_rendered + 1 == (uint64_t)g_cfg.dumpFrame && g_dumpCount == 0)) {
                DumpTexture(src, bd, L"screen", g_dumpCount++);
                if (g_cfg.vrMode == 2) g_stereoDumpReq = true;
                if (vrkit::menu::IsOpen()) XrRequestMenuDump();   // menu image with every screen dump while open
            }

            RenderedView rv = RenderedRead();
            D3D11_TEXTURE2D_DESC ed[2];
            ID3D11Texture2D* et[2] = {EngineEyeTexture(0, &ed[0]), EngineEyeTexture(1, &ed[1])};
            // the eye views are fresh when the engine rendered them since the last frame; menus, loading screens and
            // other states without the world views fall back to the flat screen
            bool fresh = false;
            for (int e = 0; e < 2; ++e) {
                LONG f = EngineEyeFrame(e);
                if (f != g_lastEyeFrame[e]) fresh = true;
                g_lastEyeFrame[e] = f;
            }
            g_staleFrames = fresh ? 0 : g_staleFrames + 1;
            bool eyesOk = g_staleFrames < 3 && !EngineGameMultiViewRecent();
            static bool lastEyesOk = true;
            if (g_cfg.vrMode == 2 && eyesOk != lastEyesOk)
                LOGI("stereo: %s%s", eyesOk ? "world views back - stereo" : "no world views - flat screen",
                     eyesOk ? "" : EngineGameMultiViewRecent() ? " (the game rendered several views)" : " (no eye frames)");
            lastEyesOk = eyesOk;
            if (g_cfg.vrMode == 2 && rv.valid && rv.stereo && et[0] && et[1] && eyesOk) {
                // native stereo: each eye's engine view into its own swapchain, posed as rendered
                bool ok = true;
                for (int e = 0; e < 2 && ok; ++e) {
                    ok = EnsureEyeSwapchain(e, ed[e].Width, ed[e].Height);
                    uint32_t ix = 0;
                    if (ok && XR_SUCCEEDED(xrAcquireSwapchainImage(g_esc[e], &ai, &ix)) && XR_SUCCEEDED(xrWaitSwapchainImage(g_esc[e], &wi))) {
                        if (!blit::Init(g_dev, g_ctx)) ok = false;
                        else {
                            float tone[2] = {g_cfg.eyeToneScale, g_cfg.eyeToneGamma};
                            bool done = false;
                            if (g_cfg.eyeTone && haveFi && fi.lut)   // exact: the game's own final colour step
                                done = blit::DrawLut(et[e], ed[e], g_eimgs[e][ix].texture, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ed[e].Width,
                                                     ed[e].Height, fi.lut, fi.scale);
                            if (!done)
                                blit::Draw(et[e], ed[e], g_eimgs[e][ix].texture, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ed[e].Width, ed[e].Height,
                                           g_cfg.eyeTone ? tone : nullptr);
                        }
                        xrReleaseSwapchainImage(g_esc[e], &ri);
                    } else ok = false;
                    float q[4]; QuatMul(qRef, rv.eye[e].q, q);
                    float pr[3]; QuatRotate(qRef, rv.eye[e].p, pr);
                    pv[e].pose.orientation = {q[0], q[1], q[2], q[3]};
                    pv[e].pose.position = {g_rcPos.x + pr[0], g_rcPos.y + pr[1], g_rcPos.z + pr[2]};
                    pv[e].fov = {-std::atan(rv.eTanX[e]), std::atan(rv.eTanX[e]), std::atan(rv.eTanY[e]), -std::atan(rv.eTanY[e])};
                    if (rv.eAsym) pv[e].fov = {rv.eFov[e][0], rv.eFov[e][1], rv.eFov[e][2], rv.eFov[e][3]};
                    pv[e].subImage.swapchain = g_esc[e];
                    pv[e].subImage.imageRect = {{0, 0}, {(int32_t)ed[e].Width, (int32_t)ed[e].Height}};
                    pv[e].subImage.imageArrayIndex = 0;
                }
                if (g_stereoDumpReq) {
                    g_stereoDumpReq = false;
                    g_stereoDumpReqHud = true;
                    DumpTexture(et[0], ed[0], L"eyeL", g_dumpCount);
                    DumpTexture(et[1], ed[1], L"eyeR", g_dumpCount);
                    if (haveFi && fi.lut) {   // what the headset gets: the eyes through the game's colour step
                        for (int e = 0; e < 2; ++e) {
                            D3D11_TEXTURE2D_DESC td{ed[e].Width, ed[e].Height, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {1, 0},
                                                    D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET, 0, 0};
                            ID3D11Texture2D* t = nullptr;
                            if (SUCCEEDED(g_dev->CreateTexture2D(&td, nullptr, &t))) {
                                if (blit::DrawLut(et[e], ed[e], t, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, td.Width, td.Height, fi.lut, fi.scale))
                                    DumpTexture(t, td, e ? L"eyeRfinal" : L"eyeLfinal", g_dumpCount);
                                blit::ForgetTargets();
                                t->Release();
                            }
                        }
                    }
                    if (haveFi && fi.ui) {
                        ID3D11Resource* ur = nullptr;
                        fi.ui->GetResource(&ur);
                        if (ur) { D3D11_TEXTURE2D_DESC ud; ((ID3D11Texture2D*)ur)->GetDesc(&ud); DumpTexture((ID3D11Texture2D*)ur, ud, L"ui", g_dumpCount); ur->Release(); }
                    }
                }
                if (ok) {
                    proj.space = g_local;
                    proj.viewCount = 2;
                    proj.views = pv;
                    layers[nLayers++] = (XrCompositionLayerBaseHeader*)&proj;
                    ++g_rendered;
                    // HUD: the game's UI layer (drawn before its final pass) on a quad
                    if (haveFi && fi.ui) {   // weapon-out detection from the reticle (also with the HUD panel off)
                        ID3D11Resource* wr = nullptr;
                        fi.ui->GetResource(&wr);
                        if (wr) { WeaponDetectSample(g_ctx, wr); wr->Release(); }
                    }
                    ID3D11Texture2D *hb = nullptr, *hw = nullptr; D3D11_TEXTURE2D_DESC hd{}; LONG hf = 0;
                    if (g_cfg.hudEnabled && EngineHudTextures(&hb, &hw, &hd, &hf)) {
                        // DAI: the UI drawn over white (left eye frame) and black (right eye frame) -> premultiplied matte
                        uint32_t hix = 0;
                        if (hd.Width >= 64 && EnsureHudSwapchain(hd.Width, hd.Height) &&
                            XR_SUCCEEDED(xrAcquireSwapchainImage(g_hsc, &ai, &hix)) && XR_SUCCEEDED(xrWaitSwapchainImage(g_hsc, &wi))) {
                            hudShown = blit::DrawMatte(hb, hw, g_himgs[hix].texture, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, hd.Width, hd.Height);
                            xrReleaseSwapchainImage(g_hsc, &ri);
                        }
                        if (g_stereoDumpReqHud) { g_stereoDumpReqHud = false; DumpTexture(hb, hd, L"hudBlack", g_dumpCount); DumpTexture(hw, hd, L"hudWhite", g_dumpCount); }
                    } else if (g_cfg.hudEnabled && haveFi && fi.ui) {
                        ID3D11Resource* ur = nullptr;
                        fi.ui->GetResource(&ur);
                        D3D11_TEXTURE2D_DESC ud{};
                        if (ur) { ((ID3D11Texture2D*)ur)->GetDesc(&ud); ur->Release(); }
                        uint32_t hix = 0;
                        if (ud.Width >= 64 && EnsureHudSwapchain(ud.Width, ud.Height) &&
                            XR_SUCCEEDED(xrAcquireSwapchainImage(g_hsc, &ai, &hix)) && XR_SUCCEEDED(xrWaitSwapchainImage(g_hsc, &wi))) {
                            hudShown = blit::DrawHud(fi.ui, g_himgs[hix].texture, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, ud.Width, ud.Height,
                                                     fi.uiScale, fi.uiAlpha);
                            xrReleaseSwapchainImage(g_hsc, &ri);
                        }
                    }
                }
            } else if (g_cfg.vrMode == 1 && rv.valid) {
                // head mode: the game image as a projection layer, posed and sized as it was rendered (mono: both eyes)
                XrPosef pose;
                float q[4];
                QuatMul(qRef, rv.head.q, q);
                float pr[3];
                QuatRotate(qRef, rv.head.p, pr);
                pose.orientation = {q[0], q[1], q[2], q[3]};
                pose.position = {g_rcPos.x + pr[0], g_rcPos.y + pr[1], g_rcPos.z + pr[2]};
                for (int e = 0; e < 2; ++e) {
                    pv[e].pose = pose;
                    pv[e].fov = {-std::atan(rv.tanX), std::atan(rv.tanX), std::atan(rv.tanY), -std::atan(rv.tanY)};
                    pv[e].subImage.swapchain = g_sc;
                    pv[e].subImage.imageRect = {{0, 0}, {(int32_t)g_scW, (int32_t)g_scH}};
                    pv[e].subImage.imageArrayIndex = 0;
                }
                proj.space = g_local;
                proj.viewCount = 2;
                proj.views = pv;
                layers[nLayers++] = (XrCompositionLayerBaseHeader*)&proj;
                ++g_rendered;
            } else {
            float d = g_cfg.screenDistance, w = g_cfg.screenWidth;
            quad.layerFlags = 0;  // opaque: back buffer alpha is ignored
            quad.space = g_local;
            quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_sc;
            quad.subImage.imageRect = {{0, 0}, {(int32_t)g_scW, (int32_t)g_scH}};
            quad.pose.orientation = {0, std::sin(g_rcYaw * 0.5f), 0, std::cos(g_rcYaw * 0.5f)};
            quad.pose.position = {g_rcPos.x - std::sin(g_rcYaw) * d, g_rcPos.y + g_cfg.screenHeight, g_rcPos.z - std::cos(g_rcYaw) * d};
            quad.size = {w, w * (float)g_scH / (float)g_scW};
            layers[nLayers++] = (XrCompositionLayerBaseHeader*)&quad;
            ++g_rendered;
            }
        }
    }
    bb->Release();
    if (haveFi) FinalPassRelease(fi);

    g_hudPlacer.Update(hudShown, headOk, head);
    if (hudShown) {
        hquad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied
        hquad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        hquad.subImage.swapchain = g_hsc;
        hquad.subImage.imageRect = {{0, 0}, {(int32_t)g_hscW, (int32_t)g_hscH}};
        g_hudPlacer.Apply(g_cfg.hud, g_local, g_view, g_hscW, g_hscH, hquad);
        float ci[4], org[3];
        if (headOk && HeadAimHudRotation(ci, org)) {
            // head aim: the panel sits on the game camera's aim ray - it starts at the game camera (which the
            // third-person orbit puts beside the rendered body camera) and points where the game camera points -
            // further out (hud_aim_distance) at the same angular size, so the reticle is on the aim point and does
            // not double against far targets
            float qr[4], q[4];
            QuatFromYaw(g_rcYaw, qr);
            QuatMul(qr, ci, q);
            float D = HeadAimHudDistance();
            if (D < g_cfg.hud.distance) D = g_cfg.hud.distance;
            float fwd[3] = {0, 0, -D}, w[3], o[3];
            QuatRotate(q, fwd, w);
            QuatRotate(qr, org, o);
            // the body camera sits at the recentre point (head position tracked) or at the head (not tracked)
            XrVector3f base = g_cfg.headPosition ? g_rcPos : head.position;
            hquad.space = g_local;
            hquad.pose.orientation = {q[0], q[1], q[2], q[3]};
            hquad.pose.position = {base.x + o[0] + w[0], base.y + o[1] + w[1], base.z + o[2] + w[2]};
            float k = D / (g_cfg.hud.distance > 0.1f ? g_cfg.hud.distance : 0.1f);
            hquad.size.width *= k; hquad.size.height *= k;
        }
        layers[nLayers++] = (XrCompositionLayerBaseHeader*)&hquad;
    }

    bool menuShown = false;
    if (fs.shouldRender && menuOpen && vrkit::menu::Texture() && EnsureMenuSwapchain()) {
        uint32_t idx = 0;
        if (XR_SUCCEEDED(xrAcquireSwapchainImage(g_msc, &ai, &idx)) && XR_SUCCEEDED(xrWaitSwapchainImage(g_msc, &wi))) {
            g_ctx->CopyResource(g_mimgs[idx].texture, vrkit::menu::Texture());
            xrReleaseSwapchainImage(g_msc, &ri);
            menuShown = true;
        }
    }
    if (menuOpen && InterlockedExchange(&g_menuDumpReq, 0)) {
        D3D11_TEXTURE2D_DESC md;
        vrkit::menu::Texture()->GetDesc(&md);
        DumpTexture(vrkit::menu::Texture(), md, L"menu", g_menuDumpCount++);
    }
    g_placer.Update(menuShown, headOk, head);
    if (menuShown) {
        mquad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // premultiplied
        mquad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
        mquad.subImage.swapchain = g_msc;
        mquad.subImage.imageRect = {{0, 0}, {(int32_t)vrkit::menu::Width(), (int32_t)vrkit::menu::Height()}};
        g_placer.Apply(g_cfg.menu, g_local, g_view, vrkit::menu::Width(), vrkit::menu::Height(), mquad);
        layers[nLayers++] = (XrCompositionLayerBaseHeader*)&mquad;
    }

    XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
    ei.displayTime = fs.predictedDisplayTime;
    ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    ei.layerCount = nLayers;
    ei.layers = layers;
    r = xrEndFrame(g_sess, &ei);
    if (XR_FAILED(r)) {
        static int n = 0;
        if (n++ < 10) LOGE("xrEndFrame: %s", XrRes(r));
    }
}

} // namespace mea
