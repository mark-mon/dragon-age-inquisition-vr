#pragma once
#include <d3d11.h>
namespace mea {
// Distance to what is straight ahead of the left eye (its image centre = head forward, symmetric frustum), read
// from the eye's main depth buffer a few frames late (no stall). Only runs while requested (head aim engaged).
void DepthProbeBeginEye(int eye);                                    // engine, around each eye RenderView
void DepthProbeNoteDSV(ID3D11DeviceContext* c, ID3D11DepthStencilView* dsv);   // OMSetRenderTargets hook
void DepthProbeEndEye(const float* proj);   // proj = the eye's projection (row-vector 4x4)
void DepthProbeRequest(bool on);
bool DepthProbeDistance(float& gameMetres);                         // false: none recent / sky
// The aim ray in the left eye's frame (real metres; x right, y up, z back), set every XR frame; the render thread
// marches it through the eye's depth buffer. DepthProbeHit: distance along the ray (real m) to the first surface.
void DepthProbeSetRay(const float origin[3], const float dir[3]);
bool DepthProbeHit(float& t);
}
