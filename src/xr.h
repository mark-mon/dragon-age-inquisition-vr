#pragma once
#include <dxgi.h>
#include <string>
#include <cstdint>
namespace mea {
void XrInitInstanceLoop();                 // worker thread: create OpenXR instance + system (retries)
void XrOnPresent(IDXGISwapChain* sc);      // render thread, before the game's Present
void XrOnResize();
void XrRequestDump();
void XrRequestMenuDump();
void XrRecenter();
bool XrGazeActive();      // eye gaze tracked right now
bool XrGazeSupported();   // the headset offers eye tracking
void XrScreenFormatChanged();
std::string XrStatusText();
void XrMaxSwapchain(uint32_t& w, uint32_t& h);   // runtime limit (0 until known)
}
