#include "vrkit_input.h"
#include "vrkit_options.h"
#include <windows.h>
#include <xinput.h>
#include "MinHook.h"

namespace vrkit { namespace input {

namespace {
Settings g_set;
using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
struct Hooked { const wchar_t* dll; void* target; GetStateFn orig; GetStateFn origEx; void* targetEx; };
Hooked g_mods[3] = { { L"xinput1_4.dll" }, { L"xinput1_3.dll" }, { L"xinput9_1_0.dll" } };
GetStateFn g_read = nullptr;                // an original XInputGetState for our own polling
volatile bool g_block = false, g_chordHeld = false;
DWORD g_fakePacket[4] = {};

// Virtual pad (e.g. VR controllers mapped to a gamepad), merged into user 0.
CRITICAL_SECTION g_vcs; bool g_vcsInit = false;
bool g_vActive = false; XINPUT_GAMEPAD g_vPad = {}; DWORD g_vSeq = 0;
WORD g_rumble[2] = {0, 0};   // last XInputSetState for user 0 (left / low, right / high motor)
void VInit() { if (!g_vcsInit) { InitializeCriticalSection(&g_vcs); g_vcsInit = true; } }
SHORT MergeAxis(SHORT a, SHORT b) { return (abs((int)b) > abs((int)a)) ? b : a; }
DWORD MergeVirtual(DWORD r, DWORD user, XINPUT_STATE* st) {
    if (!st || user != 0 || !g_vcsInit) return r;
    EnterCriticalSection(&g_vcs);
    bool on = g_vActive; XINPUT_GAMEPAD v = g_vPad; DWORD seq = g_vSeq;
    LeaveCriticalSection(&g_vcs);
    if (!on) return r;
    if (r != ERROR_SUCCESS) { ZeroMemory(st, sizeof(*st)); r = ERROR_SUCCESS; }
    XINPUT_GAMEPAD& p = st->Gamepad;
    p.wButtons |= v.wButtons;
    p.bLeftTrigger = p.bLeftTrigger > v.bLeftTrigger ? p.bLeftTrigger : v.bLeftTrigger;
    p.bRightTrigger = p.bRightTrigger > v.bRightTrigger ? p.bRightTrigger : v.bRightTrigger;
    p.sThumbLX = MergeAxis(p.sThumbLX, v.sThumbLX); p.sThumbLY = MergeAxis(p.sThumbLY, v.sThumbLY);
    p.sThumbRX = MergeAxis(p.sThumbRX, v.sThumbRX); p.sThumbRY = MergeAxis(p.sThumbRY, v.sThumbRY);
    st->dwPacketNumber += seq << 20;
    return r;
}

volatile bool g_untilRelease = false;   // after closing: keep blocking until the pad is released
DWORD Filter(DWORD r, DWORD user, XINPUT_STATE* st) {
    r = MergeVirtual(r, user, st);
    if (r == ERROR_SUCCESS && st && g_untilRelease && !g_block) {
        const XINPUT_GAMEPAD& p = st->Gamepad;
        if (!p.wButtons && p.bLeftTrigger < 30 && p.bRightTrigger < 30) g_untilRelease = false;
    }
    // while all chord buttons are down together, the game doesn't see them (no stray L3 / R3 actions)
    if (r == ERROR_SUCCESS && st && g_set.chordButtons && (st->Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons)
        st->Gamepad.wButtons &= ~g_set.chordButtons;
    if (r == ERROR_SUCCESS && st && (g_block || g_chordHeld || g_untilRelease)) {
        DWORD pkt = ++g_fakePacket[user & 3];
        ZeroMemory(st, sizeof(*st));
        st->dwPacketNumber = pkt;
    }
    return r;
}
// One hook function per DLL (each needs its own trampoline).
template <int N> DWORD WINAPI HkGet(DWORD u, XINPUT_STATE* s) { return Filter(g_mods[N].orig(u, s), u, s); }
template <int N> DWORD WINAPI HkGetEx(DWORD u, XINPUT_STATE* s) { return Filter(g_mods[N].origEx(u, s), u, s); }
void* const kHk[3] = { (void*)HkGet<0>, (void*)HkGet<1>, (void*)HkGet<2> };
void* const kHkEx[3] = { (void*)HkGetEx<0>, (void*)HkGetEx<1>, (void*)HkGetEx<2> };
// capabilities: user 0 reports a connected gamepad while the virtual pad is active
using GetCapsFn = DWORD(WINAPI*)(DWORD, DWORD, XINPUT_CAPABILITIES*);
using SetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
GetCapsFn g_origCaps[3] = {}; SetStateFn g_origSet[3] = {};
DWORD CapsFilter(DWORD r, DWORD user, XINPUT_CAPABILITIES* c) {
    if (user != 0 || !c || !g_vcsInit || !g_vActive || r == ERROR_SUCCESS) return r;
    ZeroMemory(c, sizeof(*c));
    c->Type = XINPUT_DEVTYPE_GAMEPAD; c->SubType = XINPUT_DEVSUBTYPE_GAMEPAD;
    c->Gamepad.wButtons = 0xF3FF; c->Gamepad.bLeftTrigger = c->Gamepad.bRightTrigger = 255;
    c->Gamepad.sThumbLX = c->Gamepad.sThumbLY = c->Gamepad.sThumbRX = c->Gamepad.sThumbRY = (SHORT)0xFFC0;
    c->Vibration.wLeftMotorSpeed = c->Vibration.wRightMotorSpeed = 0xFF;
    return ERROR_SUCCESS;
}
template <int N> DWORD WINAPI HkCaps(DWORD u, DWORD f, XINPUT_CAPABILITIES* c) { return CapsFilter(g_origCaps[N](u, f, c), u, c); }
template <int N> DWORD WINAPI HkSet(DWORD u, XINPUT_VIBRATION* v) {
    if (u == 0 && v) { g_rumble[0] = v->wLeftMotorSpeed; g_rumble[1] = v->wRightMotorSpeed; }
    DWORD r = g_origSet[N](u, v);
    if (u == 0 && g_vActive) r = ERROR_SUCCESS;
    return r;
}
void* const kHkCaps[3] = { (void*)HkCaps<0>, (void*)HkCaps<1>, (void*)HkCaps<2> };
void* const kHkSet[3] = { (void*)HkSet<0>, (void*)HkSet<1>, (void*)HkSet<2> };
void* g_capsTarget[3] = {}; void* g_setTarget[3] = {};

// polling state
WORD g_prevButtons = 0; bool g_prevKeys[256] = {};
DWORD g_chordStart = 0; bool g_chordFired = false;
DWORD g_repeatAt[4] = {}; uint32_t g_repeatDir = 0;
} // namespace

void Configure(const Settings& s) { g_set = s; }

bool InstallHooks() {
    VInit();
    bool any = false;
    for (int k = 0; k < 3; ++k) {
        Hooked& h = g_mods[k];
        HMODULE m = GetModuleHandleW(h.dll);
        if (!m) continue;
        if (!h.target) {
            void* p = (void*)GetProcAddress(m, "XInputGetState");
            if (p && MH_CreateHook(p, kHk[k], (void**)&h.orig) == MH_OK && MH_EnableHook(p) == MH_OK) { h.target = p; Log("vrkit: hooked XInputGetState in %ls", h.dll); }
            void* px = (void*)GetProcAddress(m, (LPCSTR)100);   // XInputGetStateEx (ordinal 100, includes the guide button)
            if (px && px != p && MH_CreateHook(px, kHkEx[k], (void**)&h.origEx) == MH_OK && MH_EnableHook(px) == MH_OK) h.targetEx = px;
        }
        if (!g_capsTarget[k]) {
            void* pc = (void*)GetProcAddress(m, "XInputGetCapabilities");
            if (pc && MH_CreateHook(pc, kHkCaps[k], (void**)&g_origCaps[k]) == MH_OK && MH_EnableHook(pc) == MH_OK) g_capsTarget[k] = pc;
        }
        if (!g_setTarget[k]) {
            void* ps = (void*)GetProcAddress(m, "XInputSetState");
            if (ps && MH_CreateHook(ps, kHkSet[k], (void**)&g_origSet[k]) == MH_OK && MH_EnableHook(ps) == MH_OK) g_setTarget[k] = ps;
        }
        if (h.orig && !g_read) g_read = h.orig;
        any = any || h.target;
    }
    if (!g_read) {   // the game hasn't loaded XInput (yet): read the pad ourselves
        HMODULE m = LoadLibraryW(L"xinput1_4.dll");
        if (!m) m = LoadLibraryW(L"xinput1_3.dll");
        if (m) g_read = (GetStateFn)GetProcAddress(m, "XInputGetState");
    }
    return any;
}

static volatile float g_lt = 0.f, g_rt = 0.f, g_rsx = 0.f, g_rsy = 0.f;
void RightStick(float& x, float& y) { x = g_rsx; y = g_rsy; }

void SetVirtualPad(bool active, const XINPUT_GAMEPAD& pad) {
    VInit();
    EnterCriticalSection(&g_vcs);
    if (active != g_vActive || memcmp(&pad, &g_vPad, sizeof(pad)) != 0) ++g_vSeq;
    g_vActive = active; g_vPad = active ? pad : XINPUT_GAMEPAD{};
    LeaveCriticalSection(&g_vcs);
}
void GetRumble(float& low, float& high) { low = g_rumble[0] / 65535.f; high = g_rumble[1] / 65535.f; }
float LeftTrigger() { return g_lt; }
float RightTrigger() { return g_rt; }

void SetBlocking(bool b) { if (g_block && !b) g_untilRelease = true; g_block = b; }

uint32_t Poll() {
    uint32_t out = 0;
    // --- gamepad (first connected)
    XINPUT_STATE st = {}; bool pad = false;
    DWORD padUser = 0;
    if (g_read) for (DWORD u = 0; u < 4 && !pad; ++u) { ZeroMemory(&st, sizeof(st)); pad = g_read(u, &st) == ERROR_SUCCESS; padUser = u; }
    if (!pad) ZeroMemory(&st, sizeof(st));
    if (!pad || padUser == 0) pad = MergeVirtual(pad ? ERROR_SUCCESS : ERROR_DEVICE_NOT_CONNECTED, 0, &st) == ERROR_SUCCESS;
    WORD b = pad ? st.Gamepad.wButtons : 0;
    float lt = pad ? st.Gamepad.bLeftTrigger / 255.f : 0.f, rt = pad ? st.Gamepad.bRightTrigger / 255.f : 0.f;
    g_lt = lt; g_rt = rt;
    g_rsx = pad ? st.Gamepad.sThumbRX / 32767.f : 0.f; g_rsy = pad ? st.Gamepad.sThumbRY / 32767.f : 0.f;
    // stick as d-pad
    if (pad) {
        const SHORT dz = 16000;
        if (st.Gamepad.sThumbLY > dz) b |= XINPUT_GAMEPAD_DPAD_UP;
        if (st.Gamepad.sThumbLY < -dz) b |= XINPUT_GAMEPAD_DPAD_DOWN;
        if (st.Gamepad.sThumbLX > dz) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
        if (st.Gamepad.sThumbLX < -dz) b |= XINPUT_GAMEPAD_DPAD_LEFT;
    }
    WORD pressed = b & ~g_prevButtons;
    g_prevButtons = b;
    // --- open / close chord (default L3 + R3)
    DWORD now = GetTickCount();
    bool chord = pad && (g_set.chordButtons || g_set.chordTriggers);
    if (g_set.chordButtons) chord = chord && (st.Gamepad.wButtons & g_set.chordButtons) == g_set.chordButtons;
    if (g_set.chordTriggers) chord = chord && lt > g_set.triggerThreshold && rt > g_set.triggerThreshold;
    if (chord) {
        if (!g_chordStart) g_chordStart = now;
        if (!g_chordFired && now - g_chordStart >= (DWORD)g_set.chordHoldMs) { g_chordFired = true; out |= Toggle; }
        g_chordHeld = g_chordFired;   // after it fired, keep the game from seeing the triggers until released
    } else { g_chordStart = 0; g_chordFired = false; g_chordHeld = false; }
    // --- keyboard (every key is queried each frame so its edge state stays right). Esc is not used: the
    // game would see it too (keyboard input is not blocked) and most games open their own menu on it.
    auto key = [&](int vk) { bool d = (GetAsyncKeyState(vk) & 0x8000) != 0; bool p = d && !g_prevKeys[vk]; g_prevKeys[vk] = d; return p; };
    auto held = [&](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    bool kTog = g_set.keyboardToggleVk ? key(g_set.keyboardToggleVk) : false;
    bool kRet = key(VK_RETURN), kBack = key(VK_BACK), kPgUp = key(VK_PRIOR), kPgDn = key(VK_NEXT), kDel = key(VK_DELETE);
    if (kTog) out |= Toggle;
    if ((pressed & XINPUT_GAMEPAD_A) || kRet) out |= Accept;
    if ((pressed & XINPUT_GAMEPAD_B) || kBack) out |= Back;
    if ((pressed & XINPUT_GAMEPAD_LEFT_SHOULDER) || kPgUp) out |= PagePrev;
    if ((pressed & XINPUT_GAMEPAD_RIGHT_SHOULDER) || kPgDn) out |= PageNext;
    if ((pressed & XINPUT_GAMEPAD_Y) || kDel) out |= Reset;
    // directions with auto-repeat (first repeat after 400 ms, then every 90 ms)
    uint32_t dirs = 0;
    if (b & XINPUT_GAMEPAD_DPAD_UP || held(VK_UP)) dirs |= Up;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN || held(VK_DOWN)) dirs |= Down;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT || held(VK_LEFT)) dirs |= Left;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT || held(VK_RIGHT)) dirs |= Right;
    const uint32_t bits[4] = { Up, Down, Left, Right };
    for (int k = 0; k < 4; ++k) {
        if (!(dirs & bits[k])) { g_repeatAt[k] = 0; continue; }
        if (!g_repeatAt[k]) { out |= bits[k]; g_repeatAt[k] = now + 400; }
        else if ((int)(now - g_repeatAt[k]) >= 0) { out |= bits[k]; g_repeatAt[k] = now + 90; }
    }
    return out;
}

} } // namespace vrkit::input
