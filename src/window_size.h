#pragma once
#include <dxgi.h>
#include <string>
namespace mea {
void WindowSizeApplyConfig();
std::string WindowSizeStatus();
void WindowSizeForceWindowedProfile();   // at load, before the game reads its options   // for the menu status line
// Render thread, every Present. vrActive: a VR mode is on and the headset is running.
void WindowSizeOnPresent(IDXGISwapChain* sc, bool vrActive);
}
