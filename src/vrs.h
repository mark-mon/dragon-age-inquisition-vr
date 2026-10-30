#pragma once
#include <d3d11.h>
namespace mea {
// NVIDIA variable rate shading (fixed foveation) for the eye views. NVAPI without the SDK lib.
void VrsInit(ID3D11Device* dev);                                  // once, render thread
void VrsBeginEye(int eye);                                         // before an eye view renders
void VrsEndEye();
void VrsOnSetRenderTargets(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);
void VrsSetGaze(int eye, bool on, float u, float v);   // foveation centre in the eye image (0..1), from eye tracking
const char* VrsStatus();
void VrsSetMainSize(UINT w, UINT h);     // size of the eye's scene passes (0 = largest depth buffer seen)
void VrsSetExclude(ID3D11Resource* r);   // passes into this render target are never foveated (e.g. the back buffer)
}
