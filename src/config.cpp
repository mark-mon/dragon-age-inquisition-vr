#include "config.h"
#include "log.h"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <windows.h>

namespace mea {

static std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)tolower(c); });
    return s;
}

bool Config::Load(const wchar_t* path) {
    FILE* f = _wfopen(path, L"r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line;
        size_t c = l.find_first_of("#;");
        if (c != std::string::npos) l = l.substr(0, c);
        size_t eq = l.find('=');
        if (eq == std::string::npos) continue;
        std::string k = Lower(Trim(l.substr(0, eq)));
        std::string v = Trim(l.substr(eq + 1));
        if (!k.empty()) m_[k] = v;
    }
    fclose(f);
    return true;
}

std::string Config::GetStr(const char* key, const char* def) const {
    auto it = m_.find(Lower(key));
    return it == m_.end() ? std::string(def) : it->second;
}
int Config::GetInt(const char* key, int def) const {
    auto it = m_.find(Lower(key));
    return it == m_.end() ? def : (int)strtol(it->second.c_str(), nullptr, 0);
}
float Config::GetFloat(const char* key, float def) const {
    auto it = m_.find(Lower(key));
    return it == m_.end() ? def : (float)atof(it->second.c_str());
}
bool Config::GetBool(const char* key, bool def) const {
    auto it = m_.find(Lower(key));
    if (it == m_.end()) return def;
    std::string v = Lower(it->second);
    return v == "1" || v == "true" || v == "yes" || v == "on";
}
void Config::Dump() const {
    for (auto& kv : m_) LOGI("config: %s = %s", kv.first.c_str(), kv.second.c_str());
}

static const char* kDefaultConfig =
"# Dragon Age: Inquisition VR - config.txt (next to DragonAgeInquisition.exe). Dumps / triggers go to the DAIVR folder.\n"
"# vr_mode: screen (flat virtual screen) | head (head-tracked, one image) | stereo (two engine views)\n"
"vr_mode = stereo\n"
"# world_scale: game metres per real metre of head movement\n"
"world_scale = 1.0\n"
"# Render resolution in VR (requires game restart): auto = the headset's recommended eye size x render_scale\n"
"#   (capped at the runtime's largest image, SteamVR 8192) | game = leave the window alone | WxH.\n"
"#   The game is switched to Windowed display mode at start (force_windowed = 0 to keep fullscreen; then no resize).\n"
"render_resolution = auto\n"
"render_scale = 1.0\n"
"# render_aspect: headset (each eye has the headset's shape) | 16:9\n"
"render_aspect = headset\n"
"# Foveated rendering (NVIDIA): vrs = 1 on; vrs_gaze = 1 follows eye tracking (headsets with XR_EXT_eye_gaze_interaction)\n"
"vrs = 1\n"
"vrs_gaze = 0\n"
"# HUD / UI on its own panel in stereo (1) or drawn into both eyes (0); F10 toggles the panel\n"
"hud_layer = 1\n"
"# dof_off = 1 removes the game's depth-of-field blur in VR (0 = the game's own blur)\n"
"dof_off = 1\n"
"# log_level: 0 error, 1 warn, 2 info, 3 debug, 4 trace\n"
"log_level = 2\n"
"# vr_enabled: 0 = only log, do not start OpenXR\n"
"vr_enabled = 1\n"
"# Flat screen in the headset: distance and width in metres, height of its centre above the start eye height\n"
"screen_distance = 2.5\n"
"screen_width = 3.2\n"
"screen_height = 0.0\n"
"# Dump the image sent to the headset as BMP after this many VR frames (0 = off). F11 dumps on demand.\n"
"dump_frame = 600\n"
"# 1: treat the back buffer as gamma-encoded (sRGB swapchain). Set 0 if the screen looks too dark.\n"
"screen_srgb = 1\n"
"# 1: always convert through a shader instead of a plain copy (automatic for 10-bit / float back buffers)\n"
"screen_force_blit = 0\n"
"# In-game settings menu: hold L3+R3 (menu_chord = l3r3 | ltrt | none) or press menu_key (0x2D = Insert)\n"
"menu_chord = l3r3\n"
"menu_hold_ms = 300\n"
"menu_key = 0x2D\n"
"# Menu panel placement (also editable on the menu's Menu page). MenuAnchor = World | Head\n"
"MenuDistance = 1.00\n"
"MenuWidth = 0.75\n"
"MenuOffsetY = -0.05\n"
"MenuAnchor = World\n"
;

void WriteDefaultConfig(const wchar_t* path) {
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    FILE* f = _wfopen(path, L"w");
    if (!f) return;
    fputs(kDefaultConfig, f);
    fclose(f);
}

} // namespace mea
