// Mass Effect Andromeda VR - entry point.
// Loaded by MassEffectAndromeda.exe as dinput8.dll or version.dll (see proxy.cpp).
#include <windows.h>
#include <string>
#include "globals.h"
#include "proxy.h"
#include "present.h"
#include "xr.h"
#include "log.h"
#include "window_size.h"
#include "version.h"
#include "settings.h"
#include "engine.h"
#include "crash.h"
#include "../third_party/minhook/include/MinHook.h"

namespace mea {
void NvInstallHooks();
Globals& G() { static Globals g; return g; }
}
using namespace mea;

static DWORD WINAPI InitThread(void*) {
    // Wait until the game itself has loaded d3d11.dll, so our dummy device does not load it earlier than the game would.
    DWORD t0 = GetTickCount();
    while (!GetModuleHandleW(L"d3d11.dll")) {
        Sleep(20);
        if (GetTickCount() - t0 > 120000) { LOGE("d3d11.dll was not loaded within 120 s - giving up"); return 0; }
    }
    LOGI("d3d11.dll loaded after %lu ms", GetTickCount() - t0);
    MH_STATUS s = MH_Initialize();
    if (s != MH_OK && s != MH_ERROR_ALREADY_INITIALIZED) { LOGE("MH_Initialize failed: %d", (int)s); return 0; }
    if (!InstallPresentHooks()) return 0;
    EngineInstallHooks();
    { NvInstallHooks(); }
    if (G().cfg.GetBool("vr_enabled", true)) XrInitInstanceLoop();
    return 0;
}

static void Init(HMODULE self) {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir = dir.substr(0, dir.find_last_of(L"\\/") + 1);
    G().gameDir = dir;
    G().outDir = dir + L"DAIVR\\";
    CreateDirectoryW(G().outDir.c_str(), nullptr);

    std::wstring cfgPath = G().gameDir + L"config.txt";
    WriteDefaultConfig(cfgPath.c_str());
    G().cfg.Load(cfgPath.c_str());
    LogInit((G().gameDir + L"daivr.log").c_str(), G().cfg.GetInt("log_level", LOG_INFO));
    LOGI("DAI VR " DAIVR_VERSION " (" __DATE__ " " __TIME__ ") loaded as %S into %S (pid %lu)", ProxySelfName(), exe, GetCurrentProcessId());
    if (ProxyGetKind() == ProxyKind::Unknown) LOGW("loaded under an unknown file name: exports are not forwarded");
    G().cfg.Dump();
    LoadSettings();
    WindowSizeForceWindowedProfile();
    CrashInstall();
    CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        ProxyDetect(h);
        // Only one copy initialises per process (e.g. both dinput8.dll and version.dll installed).
        wchar_t name[64];
        swprintf_s(name, L"Local\\DAIVR_init_%lu", GetCurrentProcessId());
        HANDLE e = CreateEventW(nullptr, TRUE, FALSE, name);
        bool first = e && GetLastError() != ERROR_ALREADY_EXISTS;
        if (first) { G().initEvent = e; Init(h); }
        else {
            if (e) CloseHandle(e);
            // Second copy: forwards its exports only. The first copy's log notes it.
            LOGW("second copy loaded as %S - it only forwards exports", ProxySelfName());
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        LOGI("unloading");
        LogFlush();
    }
    return TRUE;
}
