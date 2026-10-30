// vrkit - controller / keyboard input for the in-game menu.
//  - Reads the gamepad through XInput (through the original functions, so it still works while the game's
//    view of the pad is blocked) and the keyboard through GetAsyncKeyState.
//  - Hooks the game's XInputGetState / XInputGetStateEx (xinput1_4, xinput1_3, xinput9_1_0) so that while
//    the menu is open (and until the open/close chord is released; the chord buttons are also hidden from
//    the game while they are held together) the game sees a neutral pad.
// Requires MinHook to be initialised by the host (MH_Initialize) before InstallHooks().
#pragma once
#include <cstdint>

namespace vrkit { namespace input {

enum Button : uint32_t {
    Up = 1, Down = 2, Left = 4, Right = 8, Accept = 16, Back = 32, PagePrev = 64, PageNext = 128, Reset = 256, Toggle = 512
};

struct Settings {
    // Open / close chord: hold every button in chordButtons (XINPUT_GAMEPAD_* mask) for chordHoldMs.
    // Default L3 + R3 (both stick clicks). Set chordTriggers to use LT + RT instead (chordButtons = 0)
    // or in addition.
    unsigned short chordButtons = 0x0040 | 0x0080;   // XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB
    bool chordTriggers = false;
    int chordHoldMs = 300;
    int keyboardToggleVk = 0x2D;// VK_INSERT
    float triggerThreshold = 0.6f;
};

void Configure(const Settings& s);
bool InstallHooks();            // hook the game's XInput (call once after the game loaded its xinput DLL; safe to repeat)
void SetBlocking(bool block);   // game sees a neutral pad while true (plus until the chord is released)
// Poll once per frame. Returns newly pressed buttons (with auto-repeat for Up/Down/Left/Right when held);
// Toggle is set when the chord was held long enough or the keyboard toggle key was pressed.
uint32_t Poll();
// Last polled pad triggers (0..1; 0 without a pad), as the game sees them.
float LeftTrigger();
// Virtual pad merged into XInput user 0 for the game (and for the menu): buttons OR-ed, larger stick deflection and
// trigger win. While active, user 0 also reports connected (XInputGetCapabilities) without a physical pad.
// XINPUT_GAMEPAD layout: wButtons, bLeftTrigger, bRightTrigger, sThumbLX, sThumbLY, sThumbRX, sThumbRY.
void SetVirtualPad(bool active, const struct _XINPUT_GAMEPAD& pad);
void GetRumble(float& low, float& high);   // last vibration the game requested for user 0 (0..1)
float RightTrigger();
void RightStick(float& x, float& y);   // last polled right stick (pad + virtual pad), -1..1

} } // namespace vrkit::input
