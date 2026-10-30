#pragma once
#include <d3d11.h>
namespace mea { namespace blit {
bool Init(ID3D11Device* dev, ID3D11DeviceContext* ctx);
// Draws src into dst (through an RTV of rtvFmt). UNORM sources are treated as gamma-encoded and written so that an
// sRGB target stores the same encoded values; float sources are treated as linear.
bool Draw(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& srcDesc, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h,
          const float* tone = nullptr);   // tone = {a, g}: display value = saturate((a * v)^g) before the decode
// the game's final colour step (see finalpass.h) for an eye image; output linear for an sRGB target
bool DrawLut(ID3D11Texture2D* src, const D3D11_TEXTURE2D_DESC& srcDesc, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h,
             ID3D11ShaderResourceView* lut, float scale);
// the game's UI layer as premultiplied RGBA (linear colour) for an sRGB target
bool DrawHud(ID3D11ShaderResourceView* ui, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h, float uiScale, float uiAlpha);
// HUD from the UI drawn over black and over white (gamma-encoded 8-bit): premultiplied linear RGBA for an sRGB target
bool DrawMatte(ID3D11Texture2D* black, ID3D11Texture2D* white, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h);
// world with that UI blended over it (gamma-encoded output, e.g. the back buffer for the monitor)
bool Compose(ID3D11Texture2D* world, ID3D11Texture2D* black, ID3D11Texture2D* white, ID3D11Texture2D* dst, DXGI_FORMAT rtvFmt, UINT w, UINT h);
void ForgetTexture(ID3D11Texture2D* t);   // drop cached views of a texture we are about to release
void Reset();          // back buffer size/format changed
void ForgetTargets();  // swapchain images destroyed
}}
