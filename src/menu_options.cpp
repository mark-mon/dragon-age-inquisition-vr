// Dragon Age: Inquisition VR adapter for the vrkit in-game settings menu.
// Registers the mod's settings (pointers into g_cfg) and wires each one to the code that applies it live.
#include "version.h"
#include <windows.h>
#include <d3d11.h>
#include <string>
#include <cstdio>
#include "../vrkit/vrkit_options.h"
#include "../vrkit/vrkit_menu.h"
#include "../vrkit/vrkit_menu_placement.h"
#include "settings.h"
#include "globals.h"
#include "log.h"
#include "menu_options.h"
#include "xr.h"
#include "capture.h"
#include "present.h"

namespace mea {

static void LogToMod(const char* m) { LOGI("%s", m); }

static void Register() {
    using vrkit::Options;
    Options& o = Options::Get();
    if (!o.All().empty()) return;

    // ---- View
    static std::string modeStr;
    modeStr = g_cfg.vrMode == 2 ? "stereo" : g_cfg.vrMode == 1 ? "head" : "screen";
    auto& mode = o.AddChoice("View", "vr_mode", "VR mode", &modeStr, {"screen", "head", "stereo"},
                             "Flat screen: the game on a virtual screen.\nHead tracked: one image that follows your head.\n"
                             "Stereo: native 3D - the game renders each eye with its own camera.",
                             [] { g_cfg.vrMode = modeStr == "stereo" ? 2 : modeStr == "head" ? 1 : 0; });
    mode.choiceLabels = {"Flat screen", "Head tracked", "Stereo"};
    o.AddBool("View", "head_position", "Head position", &g_cfg.headPosition, "Also move the camera with your head (not only turn).");
    o.AddBool("View", "head_level", "Level horizon", &g_cfg.headLevel,
              "Start from the game camera with its pitch removed, so your head alone decides up / down.");
    o.AddFloat("View", "world_scale", "World scale", &g_cfg.worldScale, 0.25f, 4.f, 0.05f, 2, "Game metres per real metre of head movement.");
    o.AddBool("View", "eye_fov", "Headset field of view", &g_cfg.eyeFov, "Render with the headset's field of view (off: the game's own).");
    o.AddAction("View", "Recenter view", [] { XrRecenter(); }, "Forward = where you look now (also F9).");

    // ---- Display (render resolution; applied once per game start)
    static std::string resStr;
    resStr = G().cfg.GetStr("render_resolution", "auto");
    if (resStr != "auto" && resStr != "game") resStr = "auto";
    auto& rr = o.AddChoice("Display", "render_resolution", "Render resolution (requires game restart)", &resStr, {"auto", "game"},
                           "Auto: the headset's recommended eye size x Render scale.\nGame: the game's own window size.\n"
                           "Takes effect at the next game start.");
    rr.choiceLabels = {"Headset (auto)", "Game window"};
    static std::string aspStr;
    aspStr = G().cfg.GetStr("render_aspect", "headset");
    if (aspStr != "headset" && aspStr != "16:9") aspStr = "headset";
    auto& ra = o.AddChoice("Display", "render_aspect", "Render shape (requires game restart)", &aspStr, {"headset", "16:9"},
                           "Headset: each eye has the headset's own shape (square pixels, full vertical detail).\n"
                           "16:9: the old shape (the eye image is stretched to the headset's field of view).\n"
                           "Takes effect at the next game start.");
    ra.choiceLabels = {"Headset", "16:9"};
    static float renderScale;
    renderScale = G().cfg.GetFloat("render_scale", 1.0f);
    o.AddFloat("Display", "render_scale", "Render scale (requires game restart)", &renderScale, 0.3f, 2.5f, 0.05f, 2,
               "Multiple of the headset's recommended eye size (both eyes are rendered one after the other).\n"
               "Capped at the headset runtime's largest image (SteamVR 8192).\n"
               "Takes effect at the next game start.");

    // ---- Performance
    o.AddBool("Performance", "vrs", "Foveated rendering (VRS)", &g_cfg.vrsEnabled,
              "NVIDIA GPUs: shade the edges of each eye at a lower rate (2x2, then 4x4) in the 3D scene passes.\n"
              "The final image, HUD and menus are not affected.");
    o.AddBool("Performance", "vrs_gaze", "Eye tracking (follow gaze)", &g_cfg.vrsGaze,
              "Centre the full-rate area where you look instead of the middle of each eye.\n"
              "Needs a headset with eye tracking through OpenXR (XR_EXT_eye_gaze_interaction, e.g. Pimax Dream Air).\n"
              "Without tracking data it falls back to the fixed centre.");
    o.AddFloat("Performance", "vrs_inner", "Full-rate radius", &g_cfg.vrsInner, 0.2f, 1.5f, 0.05f, 2,
               "Full shading inside this radius (1.0 = half the image height).");
    o.AddFloat("Performance", "vrs_outer", "Half-rate radius", &g_cfg.vrsOuter, 0.3f, 2.0f, 0.05f, 2, "2x2 shading up to this radius, 4x4 beyond.");


    // ---- Image
    o.AddBool("Image", "dof_off", "Depth of field off", &g_cfg.noDof,
              "Removes the game's depth-of-field blur (gameplay, conversations, cutscenes). Off = the game's own blur.");
    o.AddBool("Image", "stereo_no_taa", "No temporal AA in stereo", &g_cfg.stereoNoTaa,
              "Temporal SMAA (T2x) keeps one history for both eyes and smears them; it is switched to SMAA 1x while in stereo.");
    o.AddBool("Image", "stereo_eye_history", "Per-eye motion history", &g_cfg.stereoEyeHistory,
              "Each eye's motion vectors come from its own previous view (not the other eye's).");

    // ---- HUD
    o.AddBool("HUD", "hud_layer", "HUD on its own panel", &g_cfg.hudLayer,
              "Stereo: the game's HUD, prompts and menus on a floating panel. Off: drawn into both eyes.");
    o.AddBool("HUD", "hud_enabled", "Show HUD panel", &g_cfg.hudEnabled, "Show / hide the panel (also F10).");
    o.AddFloat("HUD", "hud_distance", "Distance (m)", &g_cfg.hud.distance, 0.5f, 5.0f, 0.05f, 2, "How far away the HUD panel is.");
    o.AddFloat("HUD", "hud_width", "Width (m)", &g_cfg.hud.width, 0.3f, 5.0f, 0.05f, 2, "Width of the HUD panel.");
    o.AddFloat("HUD", "hud_offset_y", "Height offset (m)", &g_cfg.hud.offsetY, -1.5f, 1.5f, 0.02f, 2, "Up (+) or down (-) from eye level.");
    static std::string hudAnchor;
    hudAnchor = g_cfg.hud.anchor ? "Head" : "World";
    auto& ha = o.AddChoice("HUD", "hud_anchor", "HUD attached to", &hudAnchor, {"World", "Head"},
                           "World: the HUD stays where it appeared (recentre moves it).\nHead: the HUD follows your head.",
                           [] { g_cfg.hud.anchor = hudAnchor == "Head" ? 1 : 0; });
    ha.choiceLabels = {"World", "Head"};

    // ---- Screen (flat game screen in the headset)
    o.AddFloat("Screen", "screen_distance", "Distance (m)", &g_cfg.screenDistance, 0.5f, 20.f, 0.1f, 1, "How far away the game screen is.");
    o.AddFloat("Screen", "screen_width", "Width (m)", &g_cfg.screenWidth, 0.5f, 30.f, 0.1f, 1,
               "Width of the game screen. Height follows the game's aspect ratio.");
    o.AddFloat("Screen", "screen_height", "Height offset (m)", &g_cfg.screenHeight, -3.f, 3.f, 0.05f, 2,
               "Up (+) or down (-) from eye height at the last recentre.");
    o.AddAction("Screen", "Recenter view", [] { XrRecenter(); }, "Put the screen in front of where you are looking now.");

    // ---- Menu panel
    vrkit::menu::AddPlacementOptions(o, g_cfg.menu, "Menu");

    // ---- Controls: hidden in DAI (VR controllers as a pad were for MEA's aiming work). Config only: vr_pad,
    //      vr_pad_shift, vr_pad_rumble, vr_pad_deadzone, vr_pad_grip_click.

    // ---- Debug
    o.AddInt("Debug", "log_level", "Log level", &g_cfg.logLevel, 0, 4, 1,
             "0 errors, 1 warnings, 2 info, 3 debug, 4 trace (daivr.log).", [] { ApplyLogLevel(); });
    o.AddAction("Debug", "Dump screen / eye images", [] { XrRequestDump(); }, "Writes DAIVR\\screen_NNN.bmp, eyeL / eyeR, hud (same as F11).");
    o.AddAction("Debug", "Dump menu image", [] { XrRequestMenuDump(); }, "Writes DAIVR\\menu_NNN.bmp.");
}

void MenuOptions_Init(ID3D11Device* dev, ID3D11DeviceContext* imm) {
    vrkit::SetLog(LogToMod);
    Register();
    vrkit::menu::Settings s;
    s.title = "Dragon Age: Inquisition VR " DAIVR_VERSION;
    s.configPath = G().gameDir + L"config.txt";
    s.statusLine = [] { return XrStatusText(); };
    s.input.chordHoldMs = g_cfg.menuHoldMs;
    s.input.chordButtons = g_cfg.menuChord == "l3r3" ? (0x0040 | 0x0080) : 0;   // XINPUT_GAMEPAD_LEFT_THUMB | RIGHT_THUMB
    s.input.chordTriggers = g_cfg.menuChord == "ltrt";
    s.input.keyboardToggleVk = g_cfg.menuKey;
    if (!vrkit::menu::Init(dev, imm, s)) { LOGE("menu: init failed (menu unavailable)"); return; }
    LOGI("menu: ready - hold %s %d ms or press key 0x%X", g_cfg.menuChord == "l3r3" ? "L3+R3" : g_cfg.menuChord == "ltrt" ? "LT+RT" : "(no pad chord)",
         g_cfg.menuHoldMs, g_cfg.menuKey);
    if (g_cfg.debugMenuOpen) vrkit::menu::SetOpen(true);
}

}
