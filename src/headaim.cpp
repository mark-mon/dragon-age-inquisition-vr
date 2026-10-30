// Head aim (mouse method). See headaim.h.
//
// The game's camera control values could not be found in memory, so head aim turns the game camera by sending
// relative mouse moves (SendInput) while the game window has focus. The camera turns linearly with mouse counts, but
// by a different amount when aiming down sights (LT zooms in and lowers the sensitivity), so nothing is assumed:
//  - the render thread measures how far the game camera has actually turned because of us ("achieved"), from the
//    game's own camera each frame (while the right stick is idle, every camera change during head aim is ours), and
//    the eye cameras get only the head rotation the game camera does not contain yet - in sync with the frame being
//    rendered, whatever the game's sensitivity, smoothing or latency;
//  - the XR thread drives "achieved" towards the head (or controller) direction with a delay-compensated controller
//    (Smith predictor: target - achieved - moves still in flight) and learns the counts-to-camera gain per mode
//    (hip / aiming) from the same measurements.
// On release the head offset is driven back to zero (the aim returns to the body direction).
#include <windows.h>
#include <cmath>
#include <cstring>
#include "headaim.h"
#include "headpose.h"
#include "settings.h"
#include "engine.h"
#include "log.h"
#include "../vrkit/vrkit_input.h"
#include "weapondetect.h"
#include "depthprobe.h"

namespace mea {

static const float kTwoPi = 6.2831853f;
static float Wrap(float a) { return a - kTwoPi * std::round(a / kTwoPi); }

// --- shared between the XR thread (writer of the command side) and the render thread (writer of the measurements)
static volatile bool g_active = false;          // head aim wants an offset (XR thread)
static volatile bool g_engaged = false;         // an offset is applied or being removed (XR thread sets, render clears)
static volatile float g_sent[2] = {0, 0};       // commanded camera change so far, in measured units x expected gain
static volatile float g_achieved[2] = {0, 0};   // camera change the game actually made because of us (render thread)
static volatile float g_gain[2][2] = {{1, 1}, {1, 1}};   // [mode hip / aim][yaw / pitch] actual / expected
static volatile int g_mode = 0;
static int g_signYaw = 0, g_signPitch = 0;      // measured camera yaw / pitch change per head yaw-left / pitch-up
static volatile DWORD g_lastMeasure = 0;   // last frame the render thread measured the game camera
static volatile float g_camOff[3] = {0, 0, 0};   // game camera position relative to the rendered body camera (body frame, game m)

// Third-person orbit: the game camera position = pivot + M * o (o = camera offset from the pivot in the camera
// frame: x right, y up, z back). Turning the game camera therefore moves it around the pivot; the eye cameras must
// use the body camera's position, pivot + Mbody * o. o is learned per mode (hip / aim) from frame-to-frame camera
// changes: dP = dM * o (the pivot is nearly still between frames).
struct Orbit { double A[3][3] = {}, b[3] = {}; float o[3] = {0, 0, 0}; int n = 0; bool ok = false; };
static Orbit g_orbit[2];

void HeadAimSetTarget(int kind, uintptr_t addr, int sign, bool deg) {
    LOGI("headaim: memory control %s at %p found (sign %+d, %s) - not used, the mouse method is", kind ? "pitch" : "yaw", (void*)addr, sign,
         deg ? "deg" : "rad");
}
bool HeadAimHasTarget() { return true; }
bool HeadAimActive() { return g_active; }

static HWND g_gameWnd = nullptr;
static BOOL CALLBACK FindWnd(HWND h, LPARAM) {
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId() && IsWindowVisible(h) && !GetWindow(h, GW_OWNER)) { g_gameWnd = h; return FALSE; }
    return TRUE;
}
static bool GameFocused() {
    if (!g_gameWnd || !IsWindow(g_gameWnd)) { g_gameWnd = nullptr; EnumWindows(FindWnd, 0); }
    return g_gameWnd && GetForegroundWindow() == g_gameWnd;
}
static void MouseMove(int dx, int dy) {
    if (!dx && !dy) return;
    INPUT in{}; in.type = INPUT_MOUSE; in.mi.dx = dx; in.mi.dy = dy; in.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &in, sizeof(in));
}

static void MeasureSigns() {
    if (g_signYaw && g_signPitch) return;
    float c[16];
    if (!EngineGameCamera(c)) return;
    // forward = -back. A yaw-left turns forward towards -right; a pitch-up towards +up.
    float f[3] = {-c[8], -c[9], -c[10]}, R[3] = {c[0], c[1], c[2]}, U[3] = {c[4], c[5], c[6]};
    const float e = 0.05f;
    float fy[3], fp[3];
    for (int i = 0; i < 3; ++i) { fy[i] = f[i] - e * R[i]; fp[i] = f[i] + e * U[i]; }
    auto yawOf = [](const float* v) { return std::atan2(v[0], v[2]); };
    auto pitchOf = [](const float* v) { float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); return std::asin(v[1] / n); };
    g_signYaw = Wrap(yawOf(fy) - yawOf(f)) > 0 ? 1 : -1;
    g_signPitch = pitchOf(fp) > pitchOf(f) ? 1 : -1;
    LOGI("headaim: camera yaw sign %+d, pitch sign %+d", g_signYaw, g_signPitch);
}

// While the right stick is used (and 0.4 s after, for the camera's own smoothing) head aim pauses: no mouse moves,
// and every camera change belongs to the stick (the body turns). Afterwards the aim catches up from the new body.
static volatile DWORD g_stickHoldUntil = 0;
static bool RightStickIdle() {
    float x, y;
    vrkit::input::RightStick(x, y);
    DWORD now = GetTickCount();
    if (std::fabs(x) >= 0.15f || std::fabs(y) >= 0.15f) { g_stickHoldUntil = now + 400; return false; }
    return (int)(g_stickHoldUntil - now) <= 0;
}

// ------------------------------------------------------------------ XR thread
static float g_rem[2] = {0, 0};          // fractional mouse counts carried over
static float g_inflight[8][2] = {};      // expected camera change of the last commands (Smith predictor)
static int g_inPos = 0;

// One-euro filter (Casiez et al.): heavy smoothing while the aim is nearly still (hand / head tremor), little lag
// when it moves fast. strength 0..1 -> minimum cutoff 8 Hz .. 0.25 Hz.
struct OneEuro {
    bool init = false; float x = 0, dx = 0;
    float Step(float v, float dt, float minCut, float beta) {
        if (!init || dt <= 0) { init = true; x = v; dx = 0; return v; }
        auto alpha = [dt](float cut) { float tau = 1.f / (6.2831853f * cut); return 1.f / (1.f + tau / dt); };
        float d = (v - x) / dt;
        dx += alpha(1.f) * (d - dx);
        float cut = minCut + beta * std::fabs(dx);
        x += alpha(cut) * (v - x);
        return x;
    }
};
static OneEuro g_fYaw, g_fPitch;
static LARGE_INTEGER g_fLast{};

static volatile float g_hudDist = 10.f;
float HeadAimHudDistance() { return g_hudDist; }

void HeadAimUpdate(const float ro[3], const float rd[3], bool aimHeld) {
    bool when = g_cfg.headAimWhen == 0 ? WeaponOut() : g_cfg.headAimWhen == 1 ? aimHeld : true;
    bool want = g_cfg.headAim && g_cfg.vrMode == 2 && when && GameFocused();
    if (want) MeasureSigns();
    if (want && !(g_signYaw && g_signPitch)) want = false;
    if (want != g_active) LOGI("headaim: %s", want ? "on" : "off");
    g_mode = aimHeld ? 1 : 0;
    float target[2] = {0, 0};   // measured units
    bool useDepth = want && g_cfg.headAimDepth;
    DepthProbeRequest(useDepth);
    float hudDist = g_cfg.hudAimDistance;
    if (want) {
        const float* f = rd;
        float yaw = std::atan2(-f[0], -f[2]);
        float pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, f[1])));
        // aim at what the aim ray (eyes or controller) hits: the first surface along it in the eye's depth buffer, seen
        // from the game camera (its aim ray starts beside us - third-person orbit), so shot and reticle land there
        float t;
        float scale = g_cfg.worldScale > 0.01f ? g_cfg.worldScale : 1.f;
        static float sm = 0;
        if (useDepth && DepthProbeHit(t)) {
            sm = sm > 0 ? sm * std::pow(t / sm, 0.35f) : t;   // smoothed (log space)
            float H[3], O[3] = {g_camOff[0] / scale, g_camOff[1] / scale, g_camOff[2] / scale};
            for (int i = 0; i < 3; ++i) H[i] = ro[i] + f[i] * sm;
            float dir[3] = {H[0] - O[0], H[1] - O[1], H[2] - O[2]};
            float hz = std::sqrt(dir[0] * dir[0] + dir[2] * dir[2]);
            yaw = std::atan2(-dir[0], -dir[2]);
            pitch = std::atan2(dir[1], hz);
            hudDist = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
        } else sm = 0;
        // smoothing (aim and therefore the reticle / HUD; the view itself is never smoothed)
        LARGE_INTEGER now, fq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&fq);
        float dt = g_fLast.QuadPart ? (float)((now.QuadPart - g_fLast.QuadPart) / (double)fq.QuadPart) : 0.011f;
        g_fLast = now;
        if (dt > 0.1f) { g_fYaw.init = g_fPitch.init = false; }
        float st = std::fmax(0.f, std::fmin(1.f, g_cfg.headAimSmooth));
        if (st > 0.f) {
            float minCut = 8.f * std::pow(0.25f / 8.f, st);   // 8 Hz (light) .. 0.25 Hz (strong)
            float beta = 0.6f;                                // fast moves pass through
            if (g_fYaw.init) yaw = g_fYaw.x + Wrap(yaw - g_fYaw.x);   // unwrap around the filtered value
            yaw = Wrap(g_fYaw.Step(yaw, dt, minCut, beta));
            pitch = g_fPitch.Step(pitch, dt, minCut, beta);
        } else g_fYaw.init = g_fPitch.init = false;
        float dz = g_cfg.headAimDeadzone * 0.0174533f;   // small head motion stays a render-only look
        auto dead = [dz](float a) { return std::fabs(a) <= dz ? 0.f : a - std::copysign(dz, a); };
        target[0] = g_signYaw * dead(yaw);
        target[1] = g_signPitch * dead(pitch);
        g_engaged = true;
    }
    if (!want) g_fYaw.init = g_fPitch.init = false;
    g_active = want;
    g_hudDist = hudDist;
    if (!g_engaged) { memset(g_inflight, 0, sizeof(g_inflight)); g_rem[0] = g_rem[1] = 0; return; }
    // Safety: head aim only steers while it can see the result. Without fresh measurements (flat-screen fallback,
    // menus, loading, frames with extra views) it sends nothing; and it gives up - accepting the current camera as
    // the body direction - when returning takes too long, measurements stay away, or it loses sync.
    {
        static DWORD engagedAt = 0, releasedAt = 0, staleSince = 0, offSyncSince = 0;
        DWORD tnow = GetTickCount();
        if (!engagedAt) engagedAt = tnow;
        if (want) releasedAt = 0; else if (!releasedAt) releasedAt = tnow;
        bool fresh = tnow - g_lastMeasure < 60;
        if (fresh) staleSince = 0; else if (!staleSince) staleSince = tnow;
        float e0 = Wrap(target[0] - g_achieved[0]), e1 = target[1] - g_achieved[1];
        bool off = want && (std::fabs(e0) > 0.6f || std::fabs(e1) > 0.5f);
        if (off) { if (!offSyncSince) offSyncSince = tnow; } else offSyncSince = 0;
        const char* why = nullptr;
        if (!want && releasedAt && tnow - releasedAt > 2000) why = "return took too long";
        else if (staleSince && tnow - staleSince > (want ? 1500u : 300u)) why = "no camera measurements";
        else if (offSyncSince && tnow - offSyncSince > 1500) why = "lost sync with the game camera";
        if (why) {
            LOGW("headaim: reset (%s) - the current camera becomes the body direction", why);
            g_engaged = false;
            g_achieved[0] = g_achieved[1] = 0;
            memset(g_inflight, 0, sizeof(g_inflight)); g_rem[0] = g_rem[1] = 0;
            engagedAt = releasedAt = staleSince = offSyncSince = 0;
            if (want) g_active = false;   // re-engages next frame from the new body direction
            return;
        }
        if (!fresh) { memset(g_inflight, 0, sizeof(g_inflight)); return; }   // wait for the camera to be visible again
    }
    if (!GameFocused()) return;   // never move the desktop mouse
    if (!RightStickIdle()) { memset(g_inflight, 0, sizeof(g_inflight)); return; }   // the stick turns the body: wait

    // in flight = commands from the last `lat` frames that the measured camera cannot show yet
    int lat = g_cfg.headAimLatency < 0 ? 0 : g_cfg.headAimLatency > 7 ? 7 : g_cfg.headAimLatency;
    float inflight[2] = {0, 0};
    for (int i = 0; i < lat; ++i)
        for (int k = 0; k < 2; ++k) inflight[k] += g_inflight[(g_inPos - i) & 7][k];
    float rate[2] = {g_cfg.headAimMouseYaw, g_cfg.headAimMousePitch};   // measured camera rad per count (default sensitivity)
    int counts[2] = {0, 0};
    float cmd[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        float err = target[k] - g_achieved[k] - inflight[k];
        if (k == 0) err = Wrap(err);
        float g = g_gain[g_mode][k];
        float c = rate[k] != 0.f ? 0.8f * err / (rate[k] * g) + g_rem[k] : 0.f;
        c = std::fmax(-600.f, std::fmin(600.f, c));
        counts[k] = (int)std::lround(c);
        g_rem[k] = c - counts[k];
        cmd[k] = counts[k] * rate[k] * g;    // expected camera change
        g_sent[k] = g_sent[k] + counts[k] * rate[k];   // at default gain (the gain estimator compares against this)
    }
    g_inPos = (g_inPos + 1) & 7;
    g_inflight[g_inPos][0] = cmd[0]; g_inflight[g_inPos][1] = cmd[1];
    MouseMove(counts[0], counts[1]);

    static DWORD tick = 0;
    DWORD now = GetTickCount();
    if (now - tick > (g_cfg.debugFakeHead ? 500u : 2000u)) {
        tick = now;
        LOGI("headaim: %s target yaw %+.3f pitch %+.3f, achieved %+.3f %+.3f, gain hip %.2f/%.2f aim %.2f/%.2f, orbit hip %s%.2f %.2f %.2f aim %s%.2f %.2f %.2f",
             want ? "on" : "returning", target[0], target[1], g_achieved[0], g_achieved[1], g_gain[0][0], g_gain[0][1], g_gain[1][0], g_gain[1][1],
             g_orbit[0].ok ? "" : "?", g_orbit[0].o[0], g_orbit[0].o[1], g_orbit[0].o[2], g_orbit[1].ok ? "" : "?", g_orbit[1].o[0], g_orbit[1].o[1], g_orbit[1].o[2]);
    }
}

// ------------------------------------------------------------------ render thread
static float g_prevRows[12] = {}, g_prevPos[3] = {};
static bool g_havePrevRows = false;
static void OrbitLearn(int mode, const float* c) {
    if (g_havePrevRows) {
        float J[3][3], dP[3];   // columns dR, dU, dB
        float rot = 0, mv = 0;
        for (int i = 0; i < 3; ++i) {
            J[i][0] = c[i] - g_prevRows[i]; J[i][1] = c[4 + i] - g_prevRows[4 + i]; J[i][2] = c[8 + i] - g_prevRows[8 + i];
            dP[i] = c[12 + i] - g_prevPos[i];
            rot += J[i][2] * J[i][2]; mv += dP[i] * dP[i];
        }
        if (rot > 0.005f * 0.005f && mv < 0.25f) {
            Orbit& o = g_orbit[mode];
            for (int r = 0; r < 3; ++r) { o.b[r] *= 0.97; for (int k = 0; k < 3; ++k) o.A[r][k] *= 0.97; }
            for (int r = 0; r < 3; ++r) {
                for (int k = 0; k < 3; ++k) o.A[r][k] += (double)J[0][r] * J[0][k] + (double)J[1][r] * J[1][k] + (double)J[2][r] * J[2][k];
                o.b[r] += (double)J[0][r] * dP[0] + (double)J[1][r] * dP[1] + (double)J[2][r] * dP[2];
            }
            ++o.n;
            const double (*A)[3] = o.A;
            double det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                         A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
            double tr = A[0][0] + A[1][1] + A[2][2];
            if (o.n > 20 && det > 1e-4 * tr * tr * tr) {
                double x[3];
                for (int col = 0; col < 3; ++col) {   // Cramer
                    double M[3][3];
                    for (int r = 0; r < 3; ++r) for (int k = 0; k < 3; ++k) M[r][k] = k == col ? o.b[r] : A[r][k];
                    x[col] = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                              M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0])) / det;
                }
                if (std::fabs(x[0]) < 3 && std::fabs(x[1]) < 3 && std::fabs(x[2]) < 8) {
                    for (int k = 0; k < 3; ++k) o.o[k] = (float)x[k];
                    if (!o.ok) LOGI("headaim: %s camera orbit offset learned: right %.2f up %.2f back %.2f m", mode ? "aim" : "hip", x[0], x[1], x[2]);
                    o.ok = true;
                }
            }
        }
    }
    memcpy(g_prevRows, c, 48); memcpy(g_prevPos, c + 12, 12); g_havePrevRows = true;
}

static float g_pending[2] = {0, 0};
static bool g_havePrev = false;
static float g_prev[2] = {0, 0};
static float g_winDG[2] = {0, 0}, g_winDS[2] = {0, 0}, g_lastSent[2] = {0, 0};
static DWORD g_winStart = 0;
static int g_winMode = 0;
static DWORD g_quietSince = 0;

// per rendered frame, with the game's own main camera (right, up, back, pos)
static void OnGameCamera(const float* c, float& pitchOut) {
    g_lastMeasure = GetTickCount();
    float f[3] = {-c[8], -c[9], -c[10]};
    float yaw = std::atan2(f[0], f[2]);
    float pitch = std::asin(std::fmax(-1.f, std::fmin(1.f, f[1])));
    pitchOut = pitch;
    float d[2] = {0, 0};
    if (g_havePrev) { d[0] = Wrap(yaw - g_prev[0]); d[1] = pitch - g_prev[1]; }
    g_prev[0] = yaw; g_prev[1] = pitch; g_havePrev = true;
    OrbitLearn(g_mode, c);
    float sent[2] = {g_sent[0], g_sent[1]};
    float ds[2] = {sent[0] - g_lastSent[0], sent[1] - g_lastSent[1]};
    g_lastSent[0] = sent[0]; g_lastSent[1] = sent[1];
    if (!g_engaged) { g_achieved[0] = g_achieved[1] = 0; g_pending[0] = g_pending[1] = 0; g_winStart = 0; return; }
    bool idle = RightStickIdle();
    int mode = g_mode;
    // Only the camera turn we asked for counts as ours: what we sent becomes "pending" (expected, at the learned gain)
    // and measured turns use it up as they arrive (whatever the game's latency / smoothing), with 25 % slack. Turns
    // the game makes by itself - aim assist snapping and pulling to targets, cover, scripted moves - and the stick
    // belong to the body: the view follows them and head aim does not fight them. Unfulfilled expectation fades.
    float attrib[2] = {0, 0};
    for (int k = 0; k < 2; ++k) {
        g_pending[k] = g_pending[k] + ds[k] * g_gain[mode][k];
        float a = 0;
        if (idle && std::fabs(d[k]) <= 0.25f && g_pending[k] * d[k] > 0) {
            a = std::copysign(std::fmin(std::fabs(d[k]), std::fabs(g_pending[k]) * 1.25f + 0.003f), d[k]);
            g_pending[k] = g_pending[k] - a;
            if (g_pending[k] * (g_pending[k] + a) < 0) g_pending[k] = 0;   // overshot: nothing left
        }
        g_pending[k] = g_pending[k] * 0.97f;
        attrib[k] = a;
        g_achieved[k] = g_achieved[k] + a;
    }
    // gain learning (stick idle, same mode for the whole window)
    DWORD now = GetTickCount();
    if (!g_winStart || mode != g_winMode || !idle) { g_winStart = now; g_winMode = mode; memset(g_winDG, 0, sizeof(g_winDG)); memset(g_winDS, 0, sizeof(g_winDS)); }
    else {
        for (int k = 0; k < 2; ++k) { g_winDG[k] += attrib[k]; g_winDS[k] += ds[k]; }
        if (now - g_winStart > 600) {
            for (int k = 0; k < 2; ++k)
                if (std::fabs(g_winDS[k]) > 0.08f) {
                    float est = g_winDG[k] / g_winDS[k];
                    if (est < 0.15f) continue;   // camera barely (or not) following: the game moved it itself
                    est = std::fmin(2.5f, est);
                    g_gain[mode][k] = g_gain[mode][k] + 0.3f * (est - g_gain[mode][k]);
                }
            g_winStart = now; memset(g_winDG, 0, sizeof(g_winDG)); memset(g_winDS, 0, sizeof(g_winDS));
        }
    }
    // released and back at the body direction: done
    if (!g_active && std::fabs(g_achieved[0]) < 0.01f && std::fabs(g_achieved[1]) < 0.01f) {
        if (!g_quietSince) g_quietSince = now;
        else if (now - g_quietSince > 300) { g_engaged = false; g_achieved[0] = g_achieved[1] = 0; g_quietSince = 0; }
    } else g_quietSince = 0;
}

static volatile float g_cinv[4] = {0, 0, 0, 1};
bool HeadAimHudRotation(float q[4], float origin[3]) {
    if (!g_engaged || !g_cfg.headAimHudFollow) return false;
    q[0] = g_cinv[0]; q[1] = g_cinv[1]; q[2] = g_cinv[2]; q[3] = g_cinv[3];
    float s = g_cfg.worldScale > 0.01f ? g_cfg.worldScale : 1.f;
    for (int i = 0; i < 3; ++i) origin[i] = g_camOff[i] / s;
    return true;
}

void HeadAimCullPose(Pose& head) {
    if (!g_engaged) return;
    float c[4] = {-g_cinv[0], -g_cinv[1], -g_cinv[2], g_cinv[3]}, r[4];   // C = inverse of Cinv
    QuatMul(c, head.q, r);
    memcpy(head.q, r, sizeof(r));
}

static void RotX(float a, float* q) { q[0] = std::sin(a * 0.5f); q[1] = 0; q[2] = 0; q[3] = std::cos(a * 0.5f); }
static void RotY(float a, float* q) { q[0] = 0; q[1] = std::sin(a * 0.5f); q[2] = 0; q[3] = std::cos(a * 0.5f); }

// First person (prototype): the eyes go to Ryder's head, estimated from the camera's orbit pivot (pivot = game camera
// position - M * o) plus offsets (up / forward / right, real m, in a level frame facing the body camera). Active only in
// gameplay: the HUD compass is showing and the game camera has not just cut (conversations / cutscenes hide the HUD
// and cut the camera) - otherwise the normal third-person view.
static volatile bool g_fpActive = false;
static DWORD g_lastCut = 0;
static float g_prevPivot[3] = {0, 0, 0}, g_prevBack[3] = {0, 0, 0};
static bool g_havePivot = false;
bool HeadAimFirstPerson() { return g_fpActive; }

bool HeadAimCompensate(const float* gameCam, Pose& head, float dPos[3], bool measure) {
    float pg = 0;
    dPos[0] = dPos[1] = dPos[2] = 0;
    // frames where the game also renders extra views do not carry the normal frame-to-frame camera: compensate with
    // what is known, but do not learn from them (they made the achieved offset run away)
    if (measure) OnGameCamera(gameCam, pg);
    else pg = std::asin(std::fmax(-1.f, std::fmin(1.f, -gameCam[9])));
    bool engaged = g_engaged && g_signYaw && g_signPitch;
    // C: camera-local rotation from the game camera back to the body camera (identity when head aim is not engaged)
    float c[4] = {0, 0, 0, 1};
    if (engaged) {
        // achieved offset in the OpenXR sense (yaw left +, pitch up +)
        float Ay = g_signYaw * g_achieved[0], Ap = g_signPitch * g_achieved[1];
        float pgx = pg * g_signPitch;
        // game camera = Ry(yaw_b + Ay) Rx(pitch_b + Ap), body camera = Ry(yaw_b) Rx(pitch_b), in the camera's frame:
        // C = Rx(-pg) Ry(-Ay) Rx(pg - Ap)
        float q1[4], q2[4], q3[4], t[4];
        RotX(-pgx, q1); RotY(-Ay, q2); RotX(pgx - Ap, q3);
        QuatMul(q1, q2, t); QuatMul(t, q3, c);
        // the game camera relative to the body camera (for the HUD panel)
        g_cinv[0] = -c[0]; g_cinv[1] = -c[1]; g_cinv[2] = -c[2]; g_cinv[3] = c[3];
        float r[4]; QuatMul(c, head.q, r);
        memcpy(head.q, r, sizeof(r));
        float pp[3]; QuatRotate(c, head.p, pp);
        memcpy(head.p, pp, sizeof(pp));
    }
    const Orbit& ob = g_orbit[g_mode];
    auto mapG = [gameCam](const float* v, float* w) { for (int i = 0; i < 3; ++i) w[i] = v[0] * gameCam[i] + v[1] * gameCam[4 + i] + v[2] * gameCam[8 + i]; };
    float sc = g_cfg.worldScale > 0.01f ? g_cfg.worldScale : 1.f;
    DWORD now = GetTickCount();

    // first person: pivot, camera-cut detection, gameplay check
    bool fp = false;
    float eye[3] = {0, 0, 0};
    if (g_cfg.firstPerson && ob.ok) {
        float mo[3]; mapG(ob.o, mo);
        float pivot[3] = {gameCam[12] - mo[0], gameCam[13] - mo[1], gameCam[14] - mo[2]};
        if (measure) {
            if (g_havePivot) {
                float dp = std::sqrt((pivot[0] - g_prevPivot[0]) * (pivot[0] - g_prevPivot[0]) + (pivot[1] - g_prevPivot[1]) * (pivot[1] - g_prevPivot[1]) +
                                     (pivot[2] - g_prevPivot[2]) * (pivot[2] - g_prevPivot[2]));
                float dotB = gameCam[8] * g_prevBack[0] + gameCam[9] * g_prevBack[1] + gameCam[10] * g_prevBack[2];
                if (dp > 1.0f * sc || dotB < 0.9f) g_lastCut = now;   // > 1 m or > 25 deg in one frame: a camera cut
            }
            memcpy(g_prevPivot, pivot, 12); memcpy(g_prevBack, gameCam + 8, 12); g_havePivot = true;
        }
        bool gameplay = HudGameplayVisible() && now - g_lastCut > 600;
        if (gameplay) {
            // the body camera's axes (game camera turned by C), levelled: up = world up, forward / right horizontal
            float bz[3], bx[3], ez[3] = {0, 0, 1}, ex[3] = {1, 0, 0}, cz[3], cx[3];
            QuatRotate(c, ez, cz); QuatRotate(c, ex, cx);
            mapG(cz, bz); mapG(cx, bx);
            float fwd[3] = {-bz[0], 0, -bz[2]}, rgt[3] = {bx[0], 0, bx[2]};
            float nf = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]), nr = std::sqrt(rgt[0] * rgt[0] + rgt[2] * rgt[2]);
            if (nf > 0.1f && nr > 0.1f) {
                for (int i = 0; i < 3; ++i) { fwd[i] /= nf; rgt[i] /= nr; }
                for (int i = 0; i < 3; ++i)
                    eye[i] = pivot[i] + (fwd[i] * g_cfg.fpForward + rgt[i] * g_cfg.fpRight) * sc + (i == 1 ? g_cfg.fpUp * sc : 0.f);
                fp = true;
            }
        }
    }
    if (fp != g_fpActive) LOGI("firstperson: %s", fp ? "on" : g_cfg.firstPerson ? "off - cutscene / conversation / no HUD (third person)" : "off");
    g_fpActive = fp;

    if (fp) {
        for (int i = 0; i < 3; ++i) dPos[i] = eye[i] - gameCam[12 + i];
        // the game camera seen from the rendered eye base, in the body frame (real m), for aim and reticle
        float rel[3] = {gameCam[12] - eye[0], gameCam[13] - eye[1], gameCam[14] - eye[2]};
        float axes[3][3], e3[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (int a = 0; a < 3; ++a) { float t[3]; QuatRotate(c, e3[a], t); mapG(t, axes[a]); }
        for (int a = 0; a < 3; ++a) g_camOff[a] = (rel[0] * axes[a][0] + rel[1] * axes[a][1] + rel[2] * axes[a][2]) / sc;
        return true;
    }
    if (!engaged) { g_camOff[0] = g_camOff[1] = g_camOff[2] = 0; return false; }
    // third person: the body camera's position - undo the orbit move
    if (g_cfg.headAimOrbitFix && ob.ok) {
        float co[3], d[3];
        QuatRotate(c, ob.o, co);
        for (int i = 0; i < 3; ++i) d[i] = co[i] - ob.o[i];
        mapG(d, dPos);
        // where the game camera (the origin of its aim ray) is, seen from the body camera: Cinv*o - o
        float ci[4] = {-c[0], -c[1], -c[2], c[3]}, cio[3];
        QuatRotate(ci, ob.o, cio);
        for (int i = 0; i < 3; ++i) g_camOff[i] = cio[i] - ob.o[i];
    } else g_camOff[0] = g_camOff[1] = g_camOff[2] = 0;
    return true;
}

}
