#pragma once
#include <string>
#include "../vrkit/vrkit_menu_placement.h"

namespace mea {

// Live settings: loaded from config.txt at start, edited by the in-game menu (vrkit), saved back on menu close.
struct ModCfg {
    int logLevel = 2;
    // flat screen
    float screenDistance = 2.5f;
    float screenWidth = 3.2f;
    float screenHeight = 0.0f;
    bool screenSrgb = true;
    bool screenForceBlit = false;
    int dumpFrame = 600;
    // menu
    vrkit::menu::Placement menu;
    // HUD panel (stereo): the game's UI layer
    bool hudEnabled = true;
    vrkit::menu::Placement hud{1.4f, 1.5f, -0.05f, 0};
    std::string menuChord = "l3r3";   // l3r3 | ltrt | none
    int menuHoldMs = 300;
    int menuKey = 0x2D;               // VK_INSERT
    bool debugMenuOpen = false;       // open the menu at start (automated tests)
    // frame capture (F10)
    // engine hooks (Frostbite RenderView)
    bool engineHooks = true;
    bool engineLog = true;            // log the RenderViews every 10 s
    float engineMainAspect = 16.f / 9.f;
    float debugViewYaw = 0.f;
    int vrMode = 2;                   // 0 screen (flat quad), 1 head (mono head tracking), 2 stereo (two engine views)
    float worldScale = 1.f;           // game units (metres in Frostbite) per real metre of head movement
    bool headPosition = true;         // apply head position (else rotation only)
    bool headLevel = true;
    bool hudLayer = true;              // stereo: the game's UI on its own quad layer (matte from the two eye frames)
    bool stereoEyeHistory = true;
    bool noDof = true;                // depth of field off (GlobalPostProcessSettings forced blur 0), any VR mode
    bool stereoNoTaa = true;          // stereo: temporal SMAA (T2x) -> SMAA 1x (its history would mix the eyes)      // stereo: each eye's previous view is its own (motion vectors)
    bool stereoSplitDt = true;        // stereo: each eye render gets half the frame time (shader time advances once per game frame)
    bool stereoPresentLeft = false;   // stereo: also present the left-eye frame to the monitor (flickers between eyes)            // head tracking starts from the game camera with pitch / roll removed (level horizon)
    int stereoMethod = 0;
    int stereoRender = 0;              // 0: two engine views, 1: the game's main view rendered once per eye
    bool eyeFov = true;                // stereo: render with the headset's field of view and per-eye render size
    float eyeResScale = 0.6f;
    bool stereoRenderSize = false;    // MEA: window to headset shape (replaced by render_resolution in DAI)
    bool eyeAsymmetric = false;        // off: exact frustum broke shadows (user test); config only
    std::string stereoFbOverrides = "WorldRenderSettings.MotionBlurEnable 0; WorldRenderSettings.MotionBlurForceOn 0";   // engine settings while head tracking (head / stereo)   // engine settings while in stereo
    // The views' output is not yet in display encoding: the game's final pass maps it as display = (a * v)^g
    // (fitted on paired eye / back buffer images: a 1.92, g 1.88, rms error 2/255)
    bool eyeTone = false;
    float eyeToneScale = 1.92f, eyeToneGamma = 1.88f;         // stereo: exact (off-centre) per-eye frustum instead of a symmetric one      // stereo: back buffer (= engine render size) set to the eye render size          // stereo: eye render height = recommended height * this
    // NVIDIA variable rate shading (fixed foveation) on the eyes' scene passes; radii in units of the half image height
    bool vrsEnabled = true;
    float vrsInner = 0.55f, vrsOuter = 0.85f;   // full rate inside inner, 2x2 to outer, 4x4 beyond
    // head aim: the game's aim follows the head (HUD on the head: always; HUD stationary: while LT / right mouse held)
    bool headAim = false;
    float headAimDeadzone = 0.f;       // degrees of head motion that stay render-only
    int headAimLatency = 4;
    bool headAimOrbitFix = true;
    int headAimWhen = 0;               // 0 weapon out (reticle shown), 1 while LT / right mouse held, 2 always
    bool headAimHudFollow = true;
    float hudAimDistance = 10.f;
    bool headAimDepth = true;
    bool firstPerson = false;          // prototype: eyes at Ryder's head in gameplay
    float fpUp = 0.05f, fpForward = 0.25f, fpRight = 0.f;   // eye offset from the camera orbit pivot (m)
    float headAimSmooth = 0.5f;        // aim smoothing 0 (off) .. 1 (strong)          // aim at the depth-buffer point straight ahead; reticle on it       // HUD distance while head aim is engaged (same angular size)      // HUD panel follows the game camera while head aim is engaged       // keep the view at the body camera position (third-person orbit)
    int headAimSource = 0;             // 0 head (HMD), 1 right controller aim pose
    bool vrPad = false;                // VR controllers as XInput gamepad 1 (DAI: config only, not in the menu)
    float vrPadDeadzone = 0.12f;
    int vrPadShift = 0;                // 0: left thumbrest / trackpad touch, 1: hold left X + Y
    bool vrPadRumble = true;
    bool vrPadGripClick = true;        // Index: haptic click when a grip turns LB / RB on and off
    int headAimMethod = 0;             // 0 auto (memory if found, else mouse), 1 mouse, 2 memory only
    float headAimMouseYaw = -0.002617f, headAimMousePitch = -0.001965f;   // camera rad per mouse count            // XR frames between a write and the game camera that contains it
    bool vrsGaze = false;               // centre on the eye-tracking gaze when the headset provides it
    bool debugFakeGaze = false;        // test: circling gaze without a tracker
    bool stereoOrigPatch = false;
    bool stereoCbFix = false;
    bool stereoSkipSmallViews = true;
    bool stereoWideCull = false;       // experiment: turn / widen the game's main view for culling
    float stereoCullMargin = 0.15f;  // stereo through frames where the game adds tiny extra views (<= 512 px)           // rebuild the characters' local-origin view constants per eye      // experiment: copy the eye's view entry over the game's during each eye's render
    float debugOrigYaw = 0.f;
    std::string debugSkipVs;
    bool debugSkipMultiDraw = false;   // test: skip NVAPI multi-draw-indirect calls           // test (capture_hooks = 1): comma list of VS hashes whose draws are skipped          // test: yaw (deg) applied to the game's own view in stereo mode             // 0: eye offset in the camera position, 1: in the projection (camera at the head centre)
    bool stereoTest = false;          // test: render the main view twice (left / right half of the screen)
    bool debugFakeHead = false;       // test without a moving headset: sweep yaw +-30 deg, pitch +-10 deg         // test: rotate the main view by this many degrees
    bool captureHooks = true;         // install the capture hooks at start (0: no D3D11 context hooks at all)
    bool captureStacks = false;       // record call stacks (exe RVAs) at render-target changes
    int captureFrame = 0;             // capture automatically at this game Present (0 = only F10)
    bool captureKeepBytecode = true;  // keep shader bytecode for reflection (needed for shaders.txt details)
    bool captureDisasm = true;        // write disassembly of every shader used in the frame
    bool captureAllBuffers = false;   // also store contents of non-constant buffers that are mapped
    int captureCbMax = 1 << 20;       // max bytes stored per buffer upload
    int captureBlobMb = 768;          // max constant data per capture
};
extern ModCfg g_cfg;

void LoadSettings();          // from G().cfg
void ApplyLogLevel();
}
