// NVAPI multi-draw-indirect hooks (debug): Frostbite issues some draws through NvAPI_D3D11_MultiDraw*Indirect, which
// bypass the D3D11 context vtable. Counts them per present and can skip them (debug_skip_multidraw = 1).
#include <windows.h>
#include <d3d11.h>
#include "MinHook.h"
#include "settings.h"
#include "log.h"

namespace mea {

typedef void* (__cdecl* PFN_QI)(unsigned int id);
typedef int(__cdecl* PFN_MDII)(ID3D11DeviceContext*, unsigned int, ID3D11Buffer*, unsigned int, unsigned int);
static PFN_MDII o_MDII = nullptr, o_MDI = nullptr;
static volatile LONG g_nMDII = 0, g_nMDI = 0, g_nDrawsMDII = 0;

static int __cdecl H_MDII(ID3D11DeviceContext* c, unsigned int n, ID3D11Buffer* b, unsigned int off, unsigned int stride) {
    InterlockedIncrement(&g_nMDII); InterlockedAdd(&g_nDrawsMDII, (LONG)n);
    if (g_cfg.debugSkipMultiDraw) return 0;
    return o_MDII(c, n, b, off, stride);
}
static int __cdecl H_MDI(ID3D11DeviceContext* c, unsigned int n, ID3D11Buffer* b, unsigned int off, unsigned int stride) {
    InterlockedIncrement(&g_nMDI);
    if (g_cfg.debugSkipMultiDraw) return 0;
    return o_MDI(c, n, b, off, stride);
}

void NvInstallHooks() {
    HMODULE m = LoadLibraryW(L"nvapi64.dll");
    if (!m) { LOGI("nvapi: not available"); return; }
    auto qi = (PFN_QI)GetProcAddress(m, "nvapi_QueryInterface");
    if (!qi) return;
    void* f1 = qi(0x59E890F9u);   // NvAPI_D3D11_MultiDrawIndexedInstancedIndirect
    void* f2 = qi(0xD4E26BBFu);   // NvAPI_D3D11_MultiDrawInstancedIndirect
    if (f1 && MH_CreateHook(f1, (void*)H_MDII, (void**)&o_MDII) == MH_OK) MH_EnableHook(f1);
    if (f2 && MH_CreateHook(f2, (void*)H_MDI, (void**)&o_MDI) == MH_OK) MH_EnableHook(f2);
    LOGI("nvapi: multidraw hooks %p %p", f1, f2);
}

void NvOnPresent(uint64_t presents) {
    static DWORD last = 0;
    DWORD now = GetTickCount();
    if (now - last > 10000) {
        last = now;
        LOGI("nvapi: multidraw calls in last 10 s: indexed %ld (%ld draws), non-indexed %ld", InterlockedExchange(&g_nMDII, 0),
             InterlockedExchange(&g_nDrawsMDII, 0), InterlockedExchange(&g_nMDI, 0));
    }
}
}
