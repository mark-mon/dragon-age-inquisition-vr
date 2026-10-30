# Dragon Age: Inquisition VR

A native VR mod for **Dragon Age: Inquisition** (PC, DirectX 11). The game renders a full, separate image for each eye
from its own camera. It does not use alternate-eye rendering or reprojection tricks. Your head drives the camera
through OpenXR (SteamVR and other OpenXR runtimes).

Version **1.0.0**.

## Features

- **Native stereo**: every game frame is rendered twice, once per eye, with the headset's per-eye field of view.
- **Head tracking** on the game camera (third person, the camera follows your head; F9 recentres).
- **Render resolution from the headset**: each eye is rendered at the headset's recommended size and shape
  (square pixels), times a render scale you choose. The mod switches the game to Windowed display mode for this.
- **HUD and menus on their own floating panel**: the game's UI is separated from the 3D image, so it isn't
  baked into both eyes.
- **Depth of field removed** (on by default; strong in this game and uncomfortable in VR).
- **Foveated rendering** on NVIDIA GPUs (Variable Rate Shading): the edges of each eye are shaded at a lower rate.
  It can follow **eye tracking** on headsets that support `XR_EXT_eye_gaze_interaction` (off by default).
- **In-headset settings menu** (Insert, or hold L3+R3 on a gamepad), saved to `config.txt`.
- Motion blur and temporal anti-aliasing are switched off in stereo; they would mix the two eyes.

## Requirements

- Dragon Age: Inquisition for PC (tested with the EA app version, build 923305), **DirectX 11** renderer (not Mantle).
- A PC VR headset with an OpenXR runtime (SteamVR tested; Pimax tested with eye tracking).
- Play with a gamepad or keyboard and mouse. VR controllers aren't used.
- A strong GPU: the game renders the scene twice per frame. NVIDIA is needed for foveated rendering.

## Install

1. Copy `version.dll` into the game folder, next to `DragonAgeInquisition.exe`.
2. Start your VR runtime (for example SteamVR), then start the game.
3. On the first start the mod writes a commented `config.txt` and a log, `daivr.log`, in the game folder.

To uninstall, delete `version.dll` (and `config.txt`, `daivr.log` and the `DAIVR` folder if you like).

Notes:
- The mod changes the game's **Display Mode to Windowed** at start, because the game can only change its render
  size in Windowed mode. Fullscreen and Borderless keep the desktop resolution. The game window will be larger
  than your monitor. That is expected: the headset gets the full image. Set `force_windowed = 0` to stop this; then
  `render_scale` has no effect.
- Render resolution, render shape and render scale apply at the next game start.

## Controls

| Key | Action |
| --- | --- |
| Insert (or hold L3+R3) | Open / close the VR menu |
| F8 | Cycle VR mode: flat screen, head-tracked (one image), stereo |
| F9 | Recentre the view |
| F10 | Show / hide the HUD panel |

In the menu, use the d-pad or arrow keys to move, A or Enter to change, and LB/RB or Page Up/Page Down to switch pages.

## Settings

Everything is in the in-headset menu and in `config.txt` (game folder). The most important keys:

| Key | Default | Meaning |
| --- | --- | --- |
| `vr_mode` | `stereo` | `stereo` (native 3D), `head` (head-tracked, one image), `screen` (flat virtual screen) |
| `render_resolution` | `auto` | `auto` = headset size x `render_scale`; `game` = leave the window alone; or `WxH` |
| `render_scale` | `1.0` | Multiple of the headset's recommended eye size. Capped at the runtime's largest image (SteamVR: 8192) |
| `render_aspect` | `headset` | `headset` (each eye has the headset's shape) or `16:9` |
| `force_windowed` | `1` | Switch the game to Windowed at start (needed for `render_resolution`) |
| `dof_off` | `1` | Remove the game's depth-of-field blur |
| `hud_layer` | `1` | HUD and menus on their own panel (`0` = drawn into both eyes) |
| `vrs` | `1` | NVIDIA foveated rendering |
| `vrs_gaze` | `0` | Foveated rendering follows eye tracking |
| `world_scale` | `1.0` | Game metres per real metre of head movement |

Performance tip: `render_scale` has the biggest effect on performance. 1.0 is the headset's own recommended size.
Lower it if you can't hold the headset's frame rate, raise it for sharpness. Foveated rendering gave about +17%
in testing.

## Known issues

- The HUD panel has the same shape as the eye image. The game pins its HUD elements to the window edges, so in a
  near-square window the minimap and quest tracker are further apart top to bottom. You can shrink or move the
  panel on the menu's HUD page.
- The game can log a first-chance access violation while closing (a game destructor). It closes normally.
- Conversations and cutscenes use the same stereo path as gameplay. Report anything that looks wrong, with your
  `daivr.log`.

## Building

The mod is one DLL (`version.dll`, a proxy that forwards the real `version.dll` exports) built from C++17 sources.

- **Windows / Visual Studio**: open an *x64 Native Tools Command Prompt* and run `build_msvc.cmd` → `dist\version.dll`.
- **Linux cross build** (how the releases are built): `TC=<toolchain> ./build.sh`, where `<toolchain>/build.sh` wraps
  `zig clang --driver-mode=cl --target=x86_64-pc-windows-msvc` with an MSVC and Windows SDK copy. See the top of
  `build.sh`.

Layout:

- `src/`: the mod. `engine.cpp` (game hooks: stereo rendering, eye cameras, HUD capture, render resize), `xr.cpp`
  (OpenXR session, layers, eye tracking), `present.cpp` (Present hook, hotkeys, engine setting overrides),
  `window_size.cpp` (render resolution), `vrs.cpp` (NVIDIA VRS), `fbsettings.cpp` + `fb_table.inc` (Frostbite
  settings by reflection), `menu_options.cpp` (menu pages), `config.cpp` / `settings.cpp`.
- `vrkit/`: the in-headset menu library (ImGui-based).
- `third_party/`: MinHook, OpenXR headers and the static loader.
- `tools/`: `dai_sdk.py` (Frostbite 2014 reflection dump from a decrypted in-memory image of the exe) and
  `gen_fbtable.py` (generates `src/fb_table.inc`).
- `NOTES.md`: engineering notes. Covers how the game was reverse-engineered, the engine structures and addresses
  used, and what was tried.

The game exe is packed; addresses in the code refer to the in-memory image (fixed base `0x140000000`) of the EA app
build listed above. Other builds of the game may need new addresses.

## Credits and licenses

- This mod: **GNU General Public License v3.0** (`LICENSE`).
- Built on the same code base as the Mass Effect: Andromeda VR mod (Frostbite engine).
- [MinHook](https://github.com/TsudaKageyu/minhook): BSD 2-Clause (`third_party/minhook/LICENSE.txt`).
- [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK): Apache 2.0 (`third_party/openxr/LICENSE`).
- [Dear ImGui](https://github.com/ocornut/imgui): MIT (`vrkit/third_party/imgui/LICENSE.txt`).

Dragon Age: Inquisition is a trademark of Electronic Arts Inc. This project isn't affiliated with or endorsed by EA or
BioWare. No game files are included.
