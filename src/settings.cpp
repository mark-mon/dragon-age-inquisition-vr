#include "settings.h"
#include <cstdio>
#include "capture.h"
#include "globals.h"
#include "log.h"

namespace mea {

ModCfg g_cfg;

void LoadSettings() {
    const Config& c = G().cfg;
    g_cfg.logLevel = c.GetInt("log_level", g_cfg.logLevel);
    g_cfg.screenDistance = c.GetFloat("screen_distance", g_cfg.screenDistance);
    g_cfg.screenWidth = c.GetFloat("screen_width", g_cfg.screenWidth);
    g_cfg.screenHeight = c.GetFloat("screen_height", g_cfg.screenHeight);
    g_cfg.screenSrgb = c.GetBool("screen_srgb", g_cfg.screenSrgb);
    g_cfg.screenForceBlit = c.GetBool("screen_force_blit", g_cfg.screenForceBlit);
    g_cfg.dumpFrame = c.GetInt("dump_frame", g_cfg.dumpFrame);
    g_cfg.menu.distance = c.GetFloat("MenuDistance", g_cfg.menu.distance);
    g_cfg.menu.width = c.GetFloat("MenuWidth", g_cfg.menu.width);
    g_cfg.menu.offsetY = c.GetFloat("MenuOffsetY", g_cfg.menu.offsetY);
    g_cfg.menu.anchor = c.GetStr("MenuAnchor", "World") == "Head" ? 1 : 0;
    g_cfg.hudEnabled = c.GetBool("hud_enabled", g_cfg.hudEnabled);
    g_cfg.hud.distance = c.GetFloat("hud_distance", g_cfg.hud.distance);
    g_cfg.hud.width = c.GetFloat("hud_width", g_cfg.hud.width);
    g_cfg.hud.offsetY = c.GetFloat("hud_offset_y", g_cfg.hud.offsetY);
    g_cfg.hud.anchor = c.GetStr("hud_anchor", g_cfg.hud.anchor ? "Head" : "World") == "Head" ? 1 : 0;
    g_cfg.menuChord = c.GetStr("menu_chord", g_cfg.menuChord.c_str());
    g_cfg.menuHoldMs = c.GetInt("menu_hold_ms", g_cfg.menuHoldMs);
    g_cfg.menuKey = c.GetInt("menu_key", g_cfg.menuKey);
    g_cfg.debugMenuOpen = c.GetBool("debug_menu_open", g_cfg.debugMenuOpen);
    g_cfg.engineHooks = c.GetBool("engine_hooks", g_cfg.engineHooks);
    g_cfg.engineLog = c.GetBool("engine_log", g_cfg.engineLog);
    g_cfg.engineMainAspect = c.GetFloat("engine_main_aspect", g_cfg.engineMainAspect);
    { std::string m = c.GetStr("vr_mode", g_cfg.vrMode == 2 ? "stereo" : g_cfg.vrMode == 1 ? "head" : "screen"); g_cfg.vrMode = m == "stereo" ? 2 : m == "head" ? 1 : 0; }
    g_cfg.headLevel = c.GetBool("head_level", g_cfg.headLevel);
    g_cfg.hudLayer = c.GetBool("hud_layer", g_cfg.hudLayer);
    g_cfg.noDof = c.GetBool("dof_off", g_cfg.noDof);
    g_cfg.stereoNoTaa = c.GetBool("stereo_no_taa", g_cfg.stereoNoTaa);
    g_cfg.stereoEyeHistory = c.GetBool("stereo_eye_history", g_cfg.stereoEyeHistory);
    g_cfg.stereoSplitDt = c.GetBool("stereo_split_dt", g_cfg.stereoSplitDt);
    g_cfg.stereoPresentLeft = c.GetBool("stereo_present_left", g_cfg.stereoPresentLeft);
    g_cfg.headPosition = c.GetBool("head_position", g_cfg.headPosition);
    g_cfg.debugFakeHead = c.GetBool("debug_fake_head", g_cfg.debugFakeHead);
    g_cfg.debugViewYaw = c.GetFloat("debug_view_yaw", g_cfg.debugViewYaw);
    g_cfg.worldScale = c.GetFloat("world_scale", g_cfg.worldScale);
    g_cfg.stereoTest = c.GetBool("stereo_test", g_cfg.stereoTest);
    g_cfg.stereoMethod = c.GetInt("stereo_method", g_cfg.stereoMethod);
    g_cfg.stereoRender = c.GetStr("stereo_render_experimental", "") == "main" ? 1 : 0;   // two views (main-view-twice: experimental)
    g_cfg.eyeFov = c.GetBool("eye_fov", g_cfg.eyeFov);
    g_cfg.eyeResScale = c.GetFloat("eye_resolution_scale", g_cfg.eyeResScale);
    g_cfg.stereoRenderSize = c.GetBool("stereo_render_size", g_cfg.stereoRenderSize);
    g_cfg.eyeAsymmetric = c.GetBool("eye_asymmetric", g_cfg.eyeAsymmetric);
    g_cfg.stereoFbOverrides = c.GetStr("stereo_fb_overrides", g_cfg.stereoFbOverrides.c_str());
    g_cfg.eyeTone = c.GetBool("eye_tone", g_cfg.eyeTone);
    g_cfg.eyeToneScale = c.GetFloat("eye_tone_scale", g_cfg.eyeToneScale);
    g_cfg.eyeToneGamma = c.GetFloat("eye_tone_gamma", g_cfg.eyeToneGamma);
    g_cfg.vrsEnabled = c.GetBool("vrs", g_cfg.vrsEnabled);
    g_cfg.vrsInner = c.GetFloat("vrs_inner", g_cfg.vrsInner);
    g_cfg.vrsOuter = c.GetFloat("vrs_outer", g_cfg.vrsOuter);
    g_cfg.headAim = c.GetBool("head_aim", g_cfg.headAim);
    g_cfg.headAimDeadzone = c.GetFloat("head_aim_deadzone", g_cfg.headAimDeadzone);
    g_cfg.headAimLatency = c.GetInt("head_aim_latency", g_cfg.headAimLatency);
    g_cfg.headAimSource = c.GetStr("head_aim_source", g_cfg.headAimSource ? "controller" : "head") == "controller" ? 1 : 0;
    g_cfg.vrPad = c.GetBool("vr_pad", g_cfg.vrPad);
    g_cfg.vrPadDeadzone = c.GetFloat("vr_pad_deadzone", g_cfg.vrPadDeadzone);
    g_cfg.vrPadShift = c.GetStr("vr_pad_shift", g_cfg.vrPadShift ? "xy" : "touch") == "xy" ? 1 : 0;
    g_cfg.vrPadRumble = c.GetBool("vr_pad_rumble", g_cfg.vrPadRumble);
    g_cfg.vrPadGripClick = c.GetBool("vr_pad_grip_click", g_cfg.vrPadGripClick);
    g_cfg.headAimHudFollow = c.GetBool("head_aim_hud_follow", g_cfg.headAimHudFollow);
    g_cfg.headAimSmooth = c.GetFloat("head_aim_smooth", g_cfg.headAimSmooth);
    g_cfg.firstPerson = c.GetStr("camera_mode", g_cfg.firstPerson ? "first" : "third") == "first";
    g_cfg.fpUp = c.GetFloat("fp_up", g_cfg.fpUp);
    g_cfg.fpForward = c.GetFloat("fp_forward", g_cfg.fpForward);
    g_cfg.fpRight = c.GetFloat("fp_right", g_cfg.fpRight);
    g_cfg.headAimDepth = c.GetBool("head_aim_depth", g_cfg.headAimDepth);
    g_cfg.hudAimDistance = c.GetFloat("hud_aim_distance", g_cfg.hudAimDistance);
    { std::string w = c.GetStr("head_aim_when", "weapon"); g_cfg.headAimWhen = w == "lt" ? 1 : w == "always" ? 2 : 0; }
    g_cfg.headAimOrbitFix = c.GetBool("head_aim_orbit_fix", g_cfg.headAimOrbitFix);
    g_cfg.headAimMethod = c.GetInt("head_aim_method", g_cfg.headAimMethod);
    g_cfg.headAimMouseYaw = c.GetFloat("head_aim_mouse_yaw", g_cfg.headAimMouseYaw);
    g_cfg.headAimMousePitch = c.GetFloat("head_aim_mouse_pitch", g_cfg.headAimMousePitch);
    g_cfg.vrsGaze = c.GetBool("vrs_gaze", g_cfg.vrsGaze);
    g_cfg.debugFakeGaze = c.GetBool("debug_fake_gaze", g_cfg.debugFakeGaze);
    g_cfg.stereoOrigPatch = c.GetBool("stereo_orig_patch", g_cfg.stereoOrigPatch);
    g_cfg.stereoCbFix = c.GetBool("stereo_cb_fix", g_cfg.stereoCbFix);
    g_cfg.stereoWideCull = c.GetBool("stereo_wide_cull", g_cfg.stereoWideCull);
    g_cfg.stereoCullMargin = c.GetFloat("stereo_cull_margin", g_cfg.stereoCullMargin);
    g_cfg.stereoSkipSmallViews = c.GetBool("stereo_skip_small_views", g_cfg.stereoSkipSmallViews);
    g_cfg.debugOrigYaw = c.GetFloat("debug_orig_yaw", g_cfg.debugOrigYaw);
    g_cfg.debugSkipVs = c.GetStr("debug_skip_vs", "");
    g_cfg.debugSkipMultiDraw = c.GetBool("debug_skip_multidraw", false);
    CaptureSetSkipList(g_cfg.debugSkipVs);
    {
        int a = -1, b = 0, n = 0;
        std::string r = c.GetStr("debug_skip_range", "");
        if (sscanf(r.c_str(), "%d,%d,%d", &a, &b, &n) != 3) a = -1;
        CaptureSetSkipRange(a, b, n);
    }
    g_cfg.debugFakeHead = c.GetBool("debug_fake_head", g_cfg.debugFakeHead);
    g_cfg.headPosition = c.GetBool("head_position", g_cfg.headPosition);
    g_cfg.debugViewYaw = c.GetFloat("debug_view_yaw", g_cfg.debugViewYaw);
    g_cfg.captureHooks = c.GetBool("capture_hooks", g_cfg.captureHooks);
    g_cfg.captureStacks = c.GetBool("capture_stacks", g_cfg.captureStacks);
    g_cfg.captureFrame = c.GetInt("capture_frame", g_cfg.captureFrame);
    g_cfg.captureKeepBytecode = c.GetBool("capture_keep_bytecode", g_cfg.captureKeepBytecode);
    g_cfg.captureDisasm = c.GetBool("capture_disasm", g_cfg.captureDisasm);
    g_cfg.captureAllBuffers = c.GetBool("capture_all_buffers", g_cfg.captureAllBuffers);
    g_cfg.captureCbMax = c.GetInt("capture_cb_max", g_cfg.captureCbMax);
    g_cfg.captureBlobMb = c.GetInt("capture_blob_mb", g_cfg.captureBlobMb);
}

void ApplyLogLevel() { LogSetLevel(g_cfg.logLevel); }

}
