#pragma once
#include <d3d11.h>
#include <cstdint>
namespace mea {
// The game's final full-screen pass (world colour -> scale -> per-channel 1D LUT -> + UI layer -> sRGB encode into the
// back buffer). Recognised at its Draw (3 vertices, back buffer bound, 1D texture in t2); its inputs are kept so the
// eye images can go through the same colour step and the UI layer can be shown on its own panel.
struct FinalPassInfo {
    bool valid = false;
    uint64_t frame = 0;                 // present index when captured
    ID3D11ShaderResourceView* lut = nullptr;   // t2 (1D, per-channel)
    ID3D11ShaderResourceView* ui = nullptr;    // t1 (UI layer, premultiplied-ish)
    float scale = 1.f, uiScale = 1.f, uiAlpha = 1.f;   // cb0[2].x, .y, .z
};
void FinalPassSetBackBuffer(ID3D11Resource* bb, uint64_t presentIndex);
void FinalPassOnDraw(ID3D11DeviceContext* c);
bool FinalPassGet(FinalPassInfo& out);   // AddRef'ed views: caller releases (FinalPassRelease)
void FinalPassRelease(FinalPassInfo& fi);
}
