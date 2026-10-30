#pragma once
#include <d3d11.h>
#include <cstdint>
#include <string>
namespace mea {
void CaptureSetSkipRange(int pass, int first, int count);   // debug: skip draws of a 4-target pass
void CaptureFrameStart();
void CaptureSetSkipList(const std::string& list);   // debug: skip draws of these vertex shaders (hex hashes)
void InstallCaptureHooks(ID3D11Device* dev, ID3D11DeviceContext* imm);   // with the dummy device (hooks the implementation)
void CaptureOnPresent(ID3D11DeviceContext* imm, uint64_t presentIndex);  // top of every game Present
void CaptureRequest();
bool CaptureActive();
}
