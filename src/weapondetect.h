#pragma once
#include <d3d11.h>
namespace mea {
// "Weapon out" from the game's UI layer: the reticle sits at the exact screen centre whenever a weapon is drawn
// (hip "( . )", aiming crosshair) and is absent when holstered. A small centre box is copied to staging textures
// each frame and read back a few frames later (no stall).
void WeaponDetectSample(ID3D11DeviceContext* ctx, ID3D11Resource* uiTex);
bool WeaponOut();
bool HudGameplayVisible();   // the gameplay HUD (compass, top centre) is showing: not a conversation / cutscene / menu
}
