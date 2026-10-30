/* COM vtable slot indices computed from the SDK's C declarations (never hand-maintained). */
#define CINTERFACE
#define COBJMACROS
#include <stddef.h>
#include <d3d11_1.h>
#include "vtbl.h"

#define CTX(m)  const int VT_CTX_##m  = (int)(offsetof(ID3D11DeviceContext1Vtbl, m) / sizeof(void*));
#define DEV(m)  const int VT_DEV_##m  = (int)(offsetof(ID3D11DeviceVtbl, m) / sizeof(void*));
VT_CTX_LIST(CTX)
VT_DEV_LIST(DEV)
