#pragma once
#include <d3d11_1.h>
#include <cstdint>
namespace mea {
void CbFixNoteMap(ID3D11Resource* r, void* p);
bool CbFixRead(ID3D11Buffer* b, UINT offsetBytes, void* out, UINT bytes);   // CPU copy of a dynamic constant buffer
void CbFixBeginEye(int eye, const uint8_t* renderView, const uint8_t* matchView);   // matchView: view the blocks were built from (nullptr: renderView)
void CbFixEndEye();
bool CbFixSetCB1(ID3D11DeviceContext* c, UINT slot, UINT n, ID3D11Buffer* const* bufs, const UINT* first, const UINT* num,
                 void (STDMETHODCALLTYPE* orig)(ID3D11DeviceContext1*, UINT, UINT, ID3D11Buffer* const*, const UINT*, const UINT*));
void InstallStereoCbHooks(ID3D11Device* dev, ID3D11DeviceContext* imm);
}
