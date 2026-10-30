// VR controllers as an XInput gamepad (merged into user 0 by vrkit::input::SetVirtualPad).
// Mapping: sticks, stick clicks and triggers as on a pad; grips = LB / RB; right A / B = A / B, left X / Y = X / Y
// (Index: left A / B = X / Y); left menu button (Quest, WMR, Vive) = Start.
// Shift (held with the LEFT thumb): touching the left thumbrest (Quest) / trackpad (Index, WMR) - or, with
// vr_pad_shift = xy, holding left X + Y together - makes the right stick the d-pad, right A = Start, right B = Back.
// An input that starts shifted stays shifted until it is released (no stray stick / A / B when shift ends first).
// Game rumble (XInputSetState) goes to the controllers: low-frequency motor left, high-frequency motor right.
#include <windows.h>
#include <xinput.h>
#include <cmath>
#include <cstring>
#include <openxr/openxr.h>
#include "vrpad.h"
#include "settings.h"
#include "log.h"
#include "../vrkit/vrkit_input.h"

namespace mea {

static XrInstance g_inst = XR_NULL_HANDLE;
static XrActionSet g_set = XR_NULL_HANDLE;
static XrPath g_hand[2] = {};   // left, right
enum { A_STICK, A_CLICK, A_TRIG, A_GRIP, A_A, A_B, A_X, A_Y, A_SHIFT, A_MENU, A_AIM, A_HAPTIC, A_COUNT };
static XrAction g_act[A_COUNT] = {};
static XrSpace g_aimSpace[2] = {};
static bool g_ready = false;

static XrPath P(const char* s) { XrPath p = XR_NULL_PATH; xrStringToPath(g_inst, s, &p); return p; }

static bool MakeAction(int id, XrActionType type, const char* name, const char* label, bool perHand) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    ci.actionType = type;
    strcpy_s(ci.actionName, name);
    strcpy_s(ci.localizedActionName, label);
    if (perHand) { ci.countSubactionPaths = 2; ci.subactionPaths = g_hand; }
    XrResult r = xrCreateAction(g_set, &ci, &g_act[id]);
    if (XR_FAILED(r)) { LOGW("vrpad: action %s failed (%d)", name, (int)r); return false; }
    return true;
}

struct B { int act; const char* path; };
static void Suggest(const char* profile, const B* b, int n) {
    std::vector<XrActionSuggestedBinding> v;
    for (int i = 0; i < n; ++i) {
        XrPath p = P(b[i].path);
        if (p != XR_NULL_PATH) v.push_back({g_act[b[i].act], p});
    }
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = P(profile);
    sb.countSuggestedBindings = (uint32_t)v.size();
    sb.suggestedBindings = v.data();
    XrResult r = xrSuggestInteractionProfileBindings(g_inst, &sb);
    LOGI("vrpad: bindings for %s: %s", profile, XR_SUCCEEDED(r) ? "ok" : "rejected");
}

XrActionSet VrPadCreate(XrInstance inst) {
    g_inst = inst;
    XrActionSetCreateInfo asi{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy_s(asi.actionSetName, "meavr_pad");
    strcpy_s(asi.localizedActionSetName, "DAIVR controllers");
    asi.priority = 0;
    if (XR_FAILED(xrCreateActionSet(inst, &asi, &g_set))) { LOGW("vrpad: action set failed"); return XR_NULL_HANDLE; }
    g_hand[0] = P("/user/hand/left"); g_hand[1] = P("/user/hand/right");
    bool ok = MakeAction(A_STICK, XR_ACTION_TYPE_VECTOR2F_INPUT, "stick", "Thumbstick", true) &&
              MakeAction(A_CLICK, XR_ACTION_TYPE_BOOLEAN_INPUT, "stick_click", "Thumbstick click", true) &&
              MakeAction(A_TRIG, XR_ACTION_TYPE_FLOAT_INPUT, "trigger", "Trigger", true) &&
              MakeAction(A_GRIP, XR_ACTION_TYPE_FLOAT_INPUT, "grip", "Grip", true) &&
              MakeAction(A_A, XR_ACTION_TYPE_BOOLEAN_INPUT, "pad_a", "A", false) &&
              MakeAction(A_B, XR_ACTION_TYPE_BOOLEAN_INPUT, "pad_b", "B", false) &&
              MakeAction(A_X, XR_ACTION_TYPE_BOOLEAN_INPUT, "pad_x", "X", false) &&
              MakeAction(A_Y, XR_ACTION_TYPE_BOOLEAN_INPUT, "pad_y", "Y", false) &&
              MakeAction(A_SHIFT, XR_ACTION_TYPE_BOOLEAN_INPUT, "shift", "Shift (thumbrest / trackpad touch)", false) &&
              MakeAction(A_MENU, XR_ACTION_TYPE_BOOLEAN_INPUT, "start", "Start", false) &&
              MakeAction(A_AIM, XR_ACTION_TYPE_POSE_INPUT, "aim", "Aim pose", true) &&
              MakeAction(A_HAPTIC, XR_ACTION_TYPE_VIBRATION_OUTPUT, "rumble", "Rumble", true);
    if (!ok) return XR_NULL_HANDLE;

    const B touch[] = {
        {A_STICK, "/user/hand/left/input/thumbstick"}, {A_STICK, "/user/hand/right/input/thumbstick"},
        {A_CLICK, "/user/hand/left/input/thumbstick/click"}, {A_CLICK, "/user/hand/right/input/thumbstick/click"},
        {A_TRIG, "/user/hand/left/input/trigger/value"}, {A_TRIG, "/user/hand/right/input/trigger/value"},
        {A_GRIP, "/user/hand/left/input/squeeze/value"}, {A_GRIP, "/user/hand/right/input/squeeze/value"},
        {A_A, "/user/hand/right/input/a/click"}, {A_B, "/user/hand/right/input/b/click"},
        {A_X, "/user/hand/left/input/x/click"}, {A_Y, "/user/hand/left/input/y/click"},
        {A_SHIFT, "/user/hand/left/input/thumbrest/touch"}, {A_MENU, "/user/hand/left/input/menu/click"},
        {A_AIM, "/user/hand/left/input/aim/pose"}, {A_AIM, "/user/hand/right/input/aim/pose"},
        {A_HAPTIC, "/user/hand/left/output/haptic"}, {A_HAPTIC, "/user/hand/right/output/haptic"}};
    Suggest("/interaction_profiles/oculus/touch_controller", touch, (int)(sizeof(touch) / sizeof(touch[0])));
    const B index[] = {
        {A_STICK, "/user/hand/left/input/thumbstick"}, {A_STICK, "/user/hand/right/input/thumbstick"},
        {A_CLICK, "/user/hand/left/input/thumbstick/click"}, {A_CLICK, "/user/hand/right/input/thumbstick/click"},
        {A_TRIG, "/user/hand/left/input/trigger/value"}, {A_TRIG, "/user/hand/right/input/trigger/value"},
        {A_GRIP, "/user/hand/left/input/squeeze/force"}, {A_GRIP, "/user/hand/right/input/squeeze/force"},
        {A_A, "/user/hand/right/input/a/click"}, {A_B, "/user/hand/right/input/b/click"},
        {A_X, "/user/hand/left/input/a/click"}, {A_Y, "/user/hand/left/input/b/click"},
        {A_SHIFT, "/user/hand/left/input/trackpad/touch"},
        {A_AIM, "/user/hand/left/input/aim/pose"}, {A_AIM, "/user/hand/right/input/aim/pose"},
        {A_HAPTIC, "/user/hand/left/output/haptic"}, {A_HAPTIC, "/user/hand/right/output/haptic"}};
    Suggest("/interaction_profiles/valve/index_controller", index, (int)(sizeof(index) / sizeof(index[0])));
    const B wmr[] = {
        {A_STICK, "/user/hand/left/input/thumbstick"}, {A_STICK, "/user/hand/right/input/thumbstick"},
        {A_CLICK, "/user/hand/left/input/thumbstick/click"}, {A_CLICK, "/user/hand/right/input/thumbstick/click"},
        {A_TRIG, "/user/hand/left/input/trigger/value"}, {A_TRIG, "/user/hand/right/input/trigger/value"},
        {A_GRIP, "/user/hand/left/input/squeeze/click"}, {A_GRIP, "/user/hand/right/input/squeeze/click"},
        {A_A, "/user/hand/right/input/trackpad/click"}, {A_X, "/user/hand/left/input/trackpad/click"},
        {A_SHIFT, "/user/hand/left/input/trackpad/touch"}, {A_MENU, "/user/hand/left/input/menu/click"},
        {A_AIM, "/user/hand/left/input/aim/pose"}, {A_AIM, "/user/hand/right/input/aim/pose"},
        {A_HAPTIC, "/user/hand/left/output/haptic"}, {A_HAPTIC, "/user/hand/right/output/haptic"}};
    Suggest("/interaction_profiles/microsoft/motion_controller", wmr, (int)(sizeof(wmr) / sizeof(wmr[0])));
    const B vive[] = {
        {A_STICK, "/user/hand/left/input/trackpad"}, {A_STICK, "/user/hand/right/input/trackpad"},
        {A_CLICK, "/user/hand/left/input/trackpad/click"}, {A_CLICK, "/user/hand/right/input/trackpad/click"},
        {A_TRIG, "/user/hand/left/input/trigger/value"}, {A_TRIG, "/user/hand/right/input/trigger/value"},
        {A_GRIP, "/user/hand/left/input/squeeze/click"}, {A_GRIP, "/user/hand/right/input/squeeze/click"},
        {A_MENU, "/user/hand/left/input/menu/click"}, {A_B, "/user/hand/right/input/menu/click"},
        {A_AIM, "/user/hand/left/input/aim/pose"}, {A_AIM, "/user/hand/right/input/aim/pose"},
        {A_HAPTIC, "/user/hand/left/output/haptic"}, {A_HAPTIC, "/user/hand/right/output/haptic"}};
    Suggest("/interaction_profiles/htc/vive_controller", vive, (int)(sizeof(vive) / sizeof(vive[0])));
    return g_set;
}

void VrPadSessionReady(XrSession s) {
    for (int h = 0; h < 2; ++h) {
        XrActionSpaceCreateInfo ci{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        ci.action = g_act[A_AIM];
        ci.subactionPath = g_hand[h];
        ci.poseInActionSpace.orientation.w = 1;
        xrCreateActionSpace(s, &ci, &g_aimSpace[h]);
    }
    g_ready = true;
    LOGI("vrpad: controllers ready");
}

XrActionSet VrPadSet() { return g_set; }

static bool GetBool(XrSession s, int a, int hand, bool* active = nullptr) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_act[a];
    gi.subactionPath = hand >= 0 ? g_hand[hand] : XR_NULL_PATH;
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    if (XR_FAILED(xrGetActionStateBoolean(s, &gi, &st))) return false;
    if (active) *active = st.isActive != 0;
    return st.isActive && st.currentState;
}
static float GetFloat(XrSession s, int a, int hand, bool* active = nullptr) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_act[a];
    gi.subactionPath = g_hand[hand];
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    if (XR_FAILED(xrGetActionStateFloat(s, &gi, &st))) return 0;
    if (active) *active = st.isActive != 0;
    return st.isActive ? st.currentState : 0.f;
}
static XrVector2f GetVec(XrSession s, int hand, bool* active) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = g_act[A_STICK];
    gi.subactionPath = g_hand[hand];
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    *active = false;
    if (XR_FAILED(xrGetActionStateVector2f(s, &gi, &st))) return {0, 0};
    *active = st.isActive != 0;
    return st.isActive ? st.currentState : XrVector2f{0, 0};
}
static SHORT Axis(float v) {
    float dz = g_cfg.vrPadDeadzone;
    float a = std::fabs(v);
    if (a < dz) return 0;
    float o = (a - dz) / (1.f - dz);   // rescale outside the dead zone
    if (o > 1.f) o = 1.f;
    return (SHORT)(std::copysign(o, v) * 32767.f);
}

static bool g_anyActive = false;
static bool g_isIndex = false;   // Index controllers: grips are force sensors - give a haptic "click" on LB / RB

static void Pulse(XrSession s, int hand, float amp, int64_t ns) {
    XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
    v.amplitude = amp; v.duration = ns; v.frequency = XR_FREQUENCY_UNSPECIFIED;
    XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
    hi.action = g_act[A_HAPTIC]; hi.subactionPath = g_hand[hand];
    xrApplyHapticFeedback(s, &hi, (XrHapticBaseHeader*)&v);
}
static void CheckProfile(XrSession s) {
    static DWORD t = 0;
    DWORD now = GetTickCount();
    if (now - t < 2000) return;
    t = now;
    XrInteractionProfileState ps{XR_TYPE_INTERACTION_PROFILE_STATE};
    bool idx = false;
    if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(s, g_hand[1], &ps)) && ps.interactionProfile != XR_NULL_PATH)
        idx = ps.interactionProfile == P("/interaction_profiles/valve/index_controller");
    if (idx != g_isIndex) LOGI("vrpad: Index controllers %s", idx ? "detected - grips use squeeze force, haptic click on LB / RB" : "no longer active");
    g_isIndex = idx;
}
bool VrPadActive() { return g_anyActive; }

void VrPadUpdate(XrSession s, XrSpace ref, XrTime t, bool focused) {
    static bool lastOn = false;
    if (!g_ready || !g_cfg.vrPad || !focused) {
        if (lastOn) { vrkit::input::SetVirtualPad(false, XINPUT_GAMEPAD{}); lastOn = false; g_anyActive = false; LOGI("vrpad: off"); }
        return;
    }
    bool actL = false, actR = false;
    XrVector2f ls = GetVec(s, 0, &actL), rs = GetVec(s, 1, &actR);
    bool active = actL || actR;
    XINPUT_GAMEPAD p{};
    if (active) CheckProfile(s);
    if (active) {
        // shift (armed by the touch, held through the hand-over to the stick, latched per input while in use)
        static bool latchStick = false, latchA = false, latchB = false, xyChord = false;
        bool x = GetBool(s, A_X, -1), y = GetBool(s, A_Y, -1);
        bool armed;
        if (g_cfg.vrPadShift == 1) {
            // X + Y together = shift; X / Y are not sent while the chord is held (and until both are released)
            if (x && y) xyChord = true; else if (!x && !y) xyChord = false;
            armed = x && y;
            if (xyChord) x = y = false;
        } else armed = GetBool(s, A_SHIFT, -1);
        bool a = GetBool(s, A_A, -1), b = GetBool(s, A_B, -1);
        bool rsUsed = std::fabs(rs.x) > 0.5f || std::fabs(rs.y) > 0.5f;
        if (rsUsed) { if (armed) latchStick = true; } else latchStick = false;
        if (a) { if (armed) latchA = true; } else latchA = false;
        if (b) { if (armed) latchB = true; } else latchB = false;

        p.sThumbLX = Axis(ls.x); p.sThumbLY = Axis(ls.y);
        if (latchStick) {
            if (rs.y > 0.5f) p.wButtons |= XINPUT_GAMEPAD_DPAD_UP;
            if (rs.y < -0.5f) p.wButtons |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (rs.x > 0.5f) p.wButtons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (rs.x < -0.5f) p.wButtons |= XINPUT_GAMEPAD_DPAD_LEFT;
        } else { p.sThumbRX = Axis(rs.x); p.sThumbRY = Axis(rs.y); }
        if (a) p.wButtons |= latchA ? XINPUT_GAMEPAD_START : XINPUT_GAMEPAD_A;
        if (b) p.wButtons |= latchB ? XINPUT_GAMEPAD_BACK : XINPUT_GAMEPAD_B;
        if (x) p.wButtons |= XINPUT_GAMEPAD_X;
        if (y) p.wButtons |= XINPUT_GAMEPAD_Y;
        if (GetBool(s, A_MENU, -1)) p.wButtons |= XINPUT_GAMEPAD_START;
        if (GetBool(s, A_CLICK, 0)) p.wButtons |= XINPUT_GAMEPAD_LEFT_THUMB;
        if (GetBool(s, A_CLICK, 1)) p.wButtons |= XINPUT_GAMEPAD_RIGHT_THUMB;
        static bool grip[2] = {false, false};
        for (int h = 0; h < 2; ++h) {
            float g = GetFloat(s, A_GRIP, h);
            bool was = grip[h];
            grip[h] = grip[h] ? g > 0.35f : g > 0.6f;   // hysteresis
            if (g_isIndex && g_cfg.vrPadGripClick && grip[h] != was)   // no physical button: a light click to feel it
                Pulse(s, h, grip[h] ? 0.35f : 0.2f, grip[h] ? 20000000 : 12000000);
        }
        if (grip[0]) p.wButtons |= XINPUT_GAMEPAD_LEFT_SHOULDER;
        if (grip[1]) p.wButtons |= XINPUT_GAMEPAD_RIGHT_SHOULDER;
        p.bLeftTrigger = (BYTE)std::lround(std::fmin(1.f, GetFloat(s, A_TRIG, 0)) * 255.f);
        p.bRightTrigger = (BYTE)std::lround(std::fmin(1.f, GetFloat(s, A_TRIG, 1)) * 255.f);
    }
    vrkit::input::SetVirtualPad(active, p);
    if (active != lastOn) LOGI("vrpad: controllers %s", active ? "active - mapped to gamepad 1" : "not active");
    lastOn = active;
    g_anyActive = active;

    // rumble -> haptics (re-sent every ~50 ms while on)
    if (active && g_cfg.vrPadRumble) {
        float rum[2];
        vrkit::input::GetRumble(rum[0], rum[1]);
        static DWORD sent[2] = {0, 0};
        static float lastAmp[2] = {0, 0};
        DWORD now = GetTickCount();
        for (int h = 0; h < 2; ++h) {
            float amp = std::fmin(1.f, rum[h]);
            if (amp > 0.02f && (now - sent[h] > 50 || std::fabs(amp - lastAmp[h]) > 0.1f)) {
                XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
                v.amplitude = amp; v.duration = 80000000;   // 80 ms (refreshed)
                v.frequency = XR_FREQUENCY_UNSPECIFIED;
                XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
                hi.action = g_act[A_HAPTIC]; hi.subactionPath = g_hand[h];
                xrApplyHapticFeedback(s, &hi, (XrHapticBaseHeader*)&v);
                sent[h] = now; lastAmp[h] = amp;
            } else if (amp <= 0.02f && lastAmp[h] > 0.02f) {
                XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
                hi.action = g_act[A_HAPTIC]; hi.subactionPath = g_hand[h];
                xrStopHapticFeedback(s, &hi);
                lastAmp[h] = 0;
            }
        }
    }
    (void)ref; (void)t;
}

bool VrPadAimPose(XrSpace ref, XrTime t, int hand, XrPosef& pose) {
    if (!g_ready || hand < 0 || hand > 1 || !g_aimSpace[hand]) return false;
    XrSpaceLocation loc{XR_TYPE_SPACE_LOCATION};
    if (XR_FAILED(xrLocateSpace(g_aimSpace[hand], ref, t, &loc))) return false;
    if (!(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    pose = loc.pose;
    return true;
}

}
