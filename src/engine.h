#pragma once
#include <d3d11.h>
#include <dxgi.h>
namespace mea {
void EngineInstallHooks();
bool EngineStereoActive();          // stereo double rendering is running
int EngineOnPresent(IDXGISwapChain* sc);
void EngineAfterDraw(ID3D11DeviceContext* c, UINT vertexCount);   // non-indexed draws (final composite detection)
bool EngineHudLayer();
void EngineLogScreens();
bool EngineRequestScreenResize(HWND hwnd);   // after a window resize: make the engine resize its swapchain
bool EngineHudTextures(ID3D11Texture2D** black, ID3D11Texture2D** white, D3D11_TEXTURE2D_DESC* desc, LONG* frame);   // render thread, at Present: keeps the eye image; returns its eye or -1   // Frostbite RenderView / WorldRenderer hooks
void EngineRequestViewScan();
bool EngineGameCamera(float out[16]);   // the game's main camera rows (right, up, back, pos) from the last frame
void EngineSetEyeRecommended(int w, int h);
void EngineGetEyeRecommended(int& w, int& h);
struct HeadState;
float EngineEyeAspect(const HeadState& hs);   // headset recommended per-eye size   // debug: log CPU copies of the view constants (viewscan.trigger)
long EngineEyeFrame(int eye);
bool EngineGameMultiViewRecent();   // the game drew its own multi-view scene (e.g. skills menu) in the last 1.5 s   // incremented each time the eye view's output is captured
ID3D11Texture2D* EngineEyeTexture(int eye, D3D11_TEXTURE2D_DESC* desc);   // stereo: last output of view 0 / 1
}
