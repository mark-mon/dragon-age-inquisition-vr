# Dragon Age: Inquisition VR (DAIVR) - findings

Engineering notes for the mod (addresses, engine structures, what was tried and what worked). Game: EA app version,
build 923305. "test NNN" refers to numbered test runs during development (RTX 5070 with the SteamVR null driver;
final tuning on an RTX 5090 + Pimax). Closest earlier project: Mass Effect Andromeda (Frostbite 2015, D3D11) - most code is from MEAVR.

## Executable (2026-10-29)
- `DragonAgeInquisition.exe`, 68 MB on disk, link date 2015-09-22, Frostbite 2014 (DA3 = Dragon Age 3 in type names).
- Packed (EA activation + Denuvo-style layer): sections `.text1 .text .xcode .xtext ...` with mangled names; the only
  import on disk is `Core/Activation64.dll`. Fixed base **0x140000000** (no relocation in practice).
- The decrypted image is dumped from the running process (ReadProcessMemory page by page at the title screen) into a
  flat file (0x628E000 bytes, offset == RVA). Real code: **.xcode
  0x2ECE000 .. 0x4F2F000** (149,676 .pdata entries; function bodies are split into many chained chunks). Above
  0x5000000 is still encrypted (entropy 8). Some calls go through `gs:[0x30]`-relative obfuscated thunks.
- Runtime modules: VERSION.dll, DINPUT8.dll, WINMM.dll, XInput1_4 + XInput9_1_0, d3d11/dxgi, EA IGO64.dll overlay,
  MSVCR110. **Proxy = version.dll** (same DLL design as MEAVR). No Mantle DLL is loaded on NVIDIA: D3D11 always.
- Swapchain: 1280x720 windowed, format 28 (R8G8B8A8_UNORM), 3 buffers, DISCARD, feature level 11_1.

## Tools
- Disassembly was done on the dumped image with capstone-based scripts (function by .pdata and linear disassembly -
  function bodies are split into chained chunks, so linear disassembly of a chunk is the useful view).
- `tools/dai_sdk.py` - Frostbite 2014 reflection dump (3054 types -> dai_sdk.txt). TypeInfoData layout in DAI:
  +0 name, +8 u16 flags (kind = (flags>>4)&31), +0xA u16 size, +0x10 runtime TypeInfo*, +0x18 u8 alignment,
  +0x19 u8 field count, +0x20 super TypeInfo*, +0x30 FieldInfoData* (24 bytes: name, u16 flags, u16 offset, TypeInfo*).
- `tools/gen_fbtable.py` -> `src/fb_table.inc` (DxDisplaySettings, GameRenderSettings, DA3WorldRenderSettings,
  GlobalPostProcessSettings).

## View pipeline (Frostbite 2014) - src/engine.cpp
- Renderer object (vtable exe+1F6C530; vfunc +0x28 = frame render exe+39D11E5):
  +0x50 GameRenderSettings*, +0x60 view count, +0x68 view entries (0x10B0 each), +0x70 previous count,
  +0x78 previous RenderViews, +0x134 output aspect, +0xE0 (null in this build).
- **exe+39DBA81 updateViews(renderer, count, views)** (game thread): (re)allocates entries when the count changes,
  for each: copy the game's entry (exe+39CC70D), apply GameRenderSettings (NearPlane +0x40 -> setNear exe+3D28B31,
  ViewDistance +0x44 -> setFar exe+3D28AAF, ForceFov +0x48 (deg, if != -1) -> setFovY exe+3D28ACF, ortho flags ->
  exe+3D28BC6 / 3D28BA6 / 3D28B81), aspect from the output target / renderer+0x134 -> setAspect exe+3D28A2A, then
  RenderView::update(rv, 0xF). Previous RenderView (+0x510) from renderer+0x78 for motion vectors.
- **exe+3D2A57F RenderView::update(rv, mask)**: 1 -> exe+3D2B122 (view, VP), 2 -> exe+3D2ABBC (projection),
  4 -> exe+3D2AA5C (fov values), 8 -> frustum exe+3080222(rv+0x110, rv+0x40, rv+0x350).
- **exe+39D92D3 renderViews(renderer)** (render thread = the Present thread): copies each entry's RenderView
  (exe+3D1F33D, RenderView operator=) into a per-frame array ("renderViews", 0x510 each) + "viewSizes", and hands them
  on (exe+3C4CCD8 on the global at exe+2A6D800, exe+3045755, ...).
- View entry (0x10B0): +0x000 RenderView, +0x510 previous RenderView, +0xA20 third, +0xF30 u8, +0xF38 output target
  object (vfunc +0x20 / +0x28 = width / height), +0xF50 u16 viewport x, y, w, h, +0xF58 screen index,
  +0xF60..0x1070 matrices.
- RenderView (0x510): +0x40 LinearTransform (rows right, up, back, position), +0x90 float2 projection offset,
  +0xC8 ortho, **+0xCC fovY (radians)**, +0xD0 ?, +0xD4 near, +0xD8 far, **+0xDC aspect**, +0xE0/+0xE4 ortho size,
  **+0x100 dirty flags**, +0x110 frustum, +0x270 fovX (deg), +0x290 view, +0x310 camera copy, +0x350 projection,
  +0x3D0 view^T?, +0x410 inverse projection, +0x450 view-projection, +0x4D0 inverse view-projection.
- Log (test 004): title screen view fov 0.785 at the origin; gameplay fov 0.960 rad (55 deg), aspect before
  updateViews 1.333, position e.g. 418 / 188 / -150 (metres).

## Phase 1 (test 004): proxy + flat screen
- version.dll loads, Present hook (dxgi+0x19000), D3D11 capture hooks, vrkit menu, OpenXR session on the game's
  device, flat screen quad: ~140 fps at the title / in game on the null headset.

## Head tracking (test 005) - vr_mode = head
- updateViews hook (after the original): entry 0's RenderView gets camera = level(game camera) * head pose, fovY /
  aspect from the headset (symmetric, union of both eyes), then RenderView::update(rv, 0xF). Published as a mono
  projection layer. Fake head sweep (debug_fake_head) turns the view as expected.
- Heavy blur while turning: camera motion blur (and possibly temporal AA). Motion blur is
  WorldRenderSettingsBase.MotionBlurEnable (+0x126) / MotionBlurForceOn (+0x127) in DA3WorldRenderSettings ->
  switched off while head tracking (stereo_fb_overrides, applied every 30 frames, restored in flat mode).
- Settings instances: exe+305249F getSettings(*(exe+2916C80), &runtimeTypeInfo); runtime TypeInfo objects:
  GameRenderSettings exe+2DC5F30 (game caches it at exe+26BF008), DA3WorldRenderSettings exe+2E21340,
  GlobalPostProcessSettings exe+2E10B10, DxDisplaySettings exe+2E0DB00.

## Two engine views do NOT work in DAI (tests 006-008)
- updateViews with 2 entries crashes ~1 s later on the game thread: AV reading 0x10C0 at exe+3ED6FEA
  (stack exe+3EC65B2 <- 39D1DAA <- 39D154B, the world module's render-build vfunc +0x10).
- The world renderer (vtable exe+205D180, ctor exe+3EB9F74) has ONE per-view slot: slot i = renderer+0x280+i*0x36300,
  next members start at +0x36700; view count at renderer+0x214 is written from the submitted count (exe+3ED6ECA).
  Init exe+3ED1327 creates the slot's "WorldView" (+0x1AB80, 0x10E0 bytes, ctor exe+3DB8DD9) and RT sets
  (+0x1AC00, +0x1F180 via exe+3E7EB6C) for slot 0 only. (MEA's renderer had ~9 slots - that is why its two-view
  stereo worked.) The AA jitter (+-0.25 px, frame parity renderer+0x200 & 1) is applied there when AA mode == 7.

## Frame / thread structure (test 009, capture with stacks)
- Game loop (exe+39A801E): renderer->render (vtable +0x28 = exe+39D11E5)(renderer, dt [frame+8], b [frame+0x10],
  count, views, sync*, dispatch*) then waitJob(sync, "waitRenderSync") (exe+301F03F waitJob(handle*, name)).
- render (game thread): updateViews, build jobs, schedules the render-thread job exe+39D9AFD
  (= device->begin, **renderViews(renderer)**, device->end).
- Everything drawn in a frame (G-buffer, shadows, lighting, post, final composite, UI) happens inside renderViews
  on the render thread: modules renderer+0x108..+0x128 vfunc +0x18 (world module render exe+39D1DCF ->
  exe+3ECAAF9 ...), debug (exe+3045755), UI. Present follows on the same thread.
- D3D11 frame (1280x720): G-buffer 5 RTs + depth (~580 draws), 4 sun cascades, lighting to an HDR target,
  transparents, bloom pyramid, **final composite = Draw(6) PS 2559b6fcf8e732a2 into the back buffer**, then
  Scaleform UI (DrawIndexed, VS 8824d2fce2337887 / a7beab782c3d347e) into the back buffer. Constants: 1 MB dynamic
  ring, VSCB1/PSCB1 (SetConstantBuffers1 offsets).

## Native stereo by rendering the frame twice (src/engine.cpp, vr_mode = stereo)
- H_Render: if the game submits its one main view, render() is called twice per game frame from the same game state:
  left eye, waitJob(sync), right eye. Each call's updateViews gets the eye camera (head pose * eye, headset fov) after
  the original. dt is halved per call (stereo_split_dt) so shader time advances once per game frame.
- Render thread: renderViews recognises the eye by the entry's camera (memcmp with the written eye transforms);
  at Present the back buffer is copied to that eye's texture. The left-eye Present is swallowed (stereo_present_left
  = 0); the right-eye Present runs the XR frame (projection layer, both eyes) and goes to the monitor.
- Tests 010-014: works. 140 stereo pairs/s at 1280x720, ~98 pairs/s at 2560x1440 per eye (RTX 5070, null HMD).
  Eye dumps show correct parallax (near characters shift clearly). The title / loading screens use a default view
  (position 0,1,0 facing +Z, fov 0.785): stereo is skipped for it (stereo during loading stalled the load - test 010).
- Motion blur / smearing (011-012): fixed by (a) WorldRenderSettings.MotionBlurEnable = 0 while head tracking
  (the game resets it on load; re-applied every 30 frames) and (b) **per-eye history**: after updateViews the entry's
  previous RenderView (+0x510) is replaced by this eye's own view of the last frame (RenderView is plain data, copied
  with memcpy). Test 013: sharp eyes.
- Temporal AA: WorldRenderSettings.PostProcessAntialiasingMode +0x28C (enum: 0 none, 1-3 FXAA, 4-5 FXAA compute,
  6 SMAA 1x, 7 SMAA T2x). 7 (temporal, jittered by frame parity) is switched to 6 while in stereo (stereo_no_taa).
  The test PC uses 0.

## HUD layer (src/engine.cpp EngineAfterDraw / EngineOnPresent, blit psMatte / psCompose)
- The eye's world image is copied right after the final composite (first non-indexed Draw into the back buffer of
  an eye frame; logged "final composite found (Draw 6 ...)"). The back buffer is then cleared to WHITE (left frame) or
  BLACK (right frame); the game's Scaleform UI draws over it; at Present that image is kept (hud 0 = over white,
  hud 1 = over black). alpha = 1 - mean(white - black), colour = black / alpha (decoded) -> premultiplied linear HUD
  swapchain (quad layer, distance / width / height / World|Head anchor, F10). The monitor gets
  world(right eye) * (1 - alpha) + black via psCompose into the back buffer before the right-eye Present.
- hud_layer = 0: no clearing; the UI stays in both eye images.

## Render resolution (src/window_size.cpp + EngineRequestScreenResize)
- Dx11 renderer object at *(exe+2A732E8) (vtable exe+2033090): +0x30 screen count, +0x38 screens (0x150 bytes each):
  +0x00 hwnd, +0x1C mode (0 windowed), +0x25 resize-pending flag, +0x28/+0x2C current back buffer size,
  +0x30/+0x34 client size (polled each frame with GetClientRect), +0xD4/+0xD8 wanted size, +0x100 IDXGISwapChain;
  renderer +0x80 DxDisplaySettings, +0x100 ID3D11Device. Per-frame screen update exe+3D96E8C -> screen recreate
  exe+3DAC68C -> exe+3DB37A5 schedules exe+3DAD262 (ResizeBuffers + targets) on the render thread.
- The engine does NOT resize when the window is resized programmatically (flag +0x25 only set by its own window
  handling). The mod resizes the window (subclassed: may exceed the monitor), then sets +0x25 -> the engine runs its
  own resize (test 014: ResizeBuffers 2560x1440, eyes / HUD / XR swapchains follow).
- render_resolution = auto (16:9 at headset eye height x render_scale, default 0.5) | game | WxH; once per launch,
  ~600 presents after start (title screen).
- ONLY Windowed display mode resizes. Exclusive fullscreen (screen mode 1) and Borderless (profile FullscreenMode 2;
  engine screen mode 2, later 1) are fullscreen modes: the engine ignores the resize flag (test 019). The user's first
  headset run was in exclusive fullscreen 3840x2160 -> render_scale had no effect. Fix (0.3.0): at DLL load, when
  vr_mode != screen and render_resolution != game, the mod sets "GstRender.FullscreenEnabled 0" in
  Documents\BioWare\Dragon Age Inquisition\Save\ProfileOptions_profile (config force_windowed = 0 disables).
- The auto size is clamped to the OpenXR runtime's max swapchain size (SteamVR: 8192x8192) and 16384; a WARN gives
  the largest useful render_scale. Pimax (SteamVR recommended 3980x3424 per eye): 1.0 -> 6086x3424, 1.3 -> 7910x4450,
  >= ~1.35 -> 8192x4608. The menu status line shows the actual eye render size.

## Hotkeys / menu
- F8 VR mode cycle (screen -> head -> stereo), F9 recentre, F10 HUD panel, F11 dump images; Insert or L3+R3 menu.
- Menu pages: View, Display, Performance, Image, HUD, Screen, Menu, Controls, Debug (MEA's head aim / first person /
  colour curve options removed - not wired for DAI).

## Foveated rendering (NVIDIA VRS, src/vrs.cpp from MEAVR)
- VrsBeginEye / VrsEndEye around each eye frame's renderViews; the rate image is bound for passes whose RT0 and
  depth buffer both have the eye render size (VrsSetMainSize = back buffer size - MEA's "largest depth buffer"
  rule picked DAI's 2048x2048 shadow map and bound almost nothing). Passes into the back buffer (final composite,
  UI) are excluded (VrsSetExclude). ~25,000 scene-pass binds per 10 s at ~115 stereo pairs/s.
- Test 016 (2560x1440 per eye, RTX 5070, standing in Haven): VRS on ~115 stereo pairs/s, off ~98 (+17%).
  Eye-tracked centre (XR_EXT_eye_gaze_interaction) as in MEAVR; untested here (null HMD has no eye tracking).

## Render shape / HUD layout (tests 027-028, 0.3.2)
- render_aspect = headset (default) | 16:9: auto size w = h * recW / recH. World stereo is correct at any aspect (the
  eye FOV / aspect come from the headset). 16:9 before = vertically stretched pixels.
- The game's UI is laid out for the window: elements anchored to the window edges / corners at fixed pixel size (not
  stretched, not letterboxed). In a near-square window the minimap / quest tracker spread apart; a centre 16:9 crop
  would cut them. GstRender.ScreenSafeAreaHeight 0.51 in ProfileOptions_profile: no effect (test 028).
  HUD panel currently has the back buffer's shape. A 16:9 HUD needs the UI's own screen size (not yet found).

## Depth of field (tests 023-026, src/present.cpp DofOverride)
- GlobalPostProcessSettings (registered, instance found) has Force* DOF overrides, all -1 (= use the visual
  environment's DofComponentData) by default: ForceDofEnable (Int32), ForceDofFocusDistance, ForceDofBlurFactor,
  ForceDofBlurAdd, ForceSimple/SpriteDof Near/Far Start/End, ForceSimple/SpriteDofBlurMax. DofMethod = 1.
- Forcing ForceDofEnable 1 with extreme ranges blurs everything (proves the overrides are live, read every frame).
  ForceDofEnable 0 does NOT remove blur; DofMethod 0 neither; BlurMax 0 alone leaves some blur.
  ForceDofBlurFactor 0 + ForceDofBlurAdd 0 (+ both BlurMax 0) -> fully sharp even with the extreme ranges.
- dof_off = 1 (default, menu Image page) forces those four to 0 every 30 presents in head/stereo; restored when off.
  Not yet seen on a natural strong-DOF scene (conversation / cutscene) - the Haven start has almost none.

## Release 1.0.0 (2026-10-30)
- Native stereo (frame rendered twice per game frame), head tracking, HUD on its own panel, render size at the
  headset's shape (auto Windowed), depth of field off, NVIDIA VRS foveation (+ optional eye tracking), vrkit menu.
- Known: HUD panel has the back-buffer shape; first-chance access violation in a game destructor at exit
  (exe+304E1E1, harmless). VR controllers as gamepad: hidden config option (vr_pad), not supported.

## Status update (2026-10-30 04:40) - release candidate 0.4.0
- vrs_gaze default off (menu Performance "Eye tracking (follow gaze)", live). Display options labelled
  "(requires game restart)". Fresh config: vr_mode stereo, render_scale 1.0, render_aspect headset, vrs 1, vrs_gaze 0,
  dof_off 1 (test 029; menu pages checked in the SteamVR mirror).
- VR controllers as pad (vrpad.cpp, from MEA's aiming work): hidden - Controls page removed, vr_pad default 0, no
  controller action set is created when off (test 031). Config only: vr_pad, vr_pad_shift, vr_pad_rumble,
  vr_pad_deadzone, vr_pad_grip_click.
- Open: HUD panel has the back-buffer shape (UI anchored to window edges); conversations / cutscenes in stereo not
  yet reviewed by Claude; game-exit first-chance AV (exe+304E1E1).

## Status update (2026-10-30 04:20) - stage 10 / 0.3.2: headset-shaped eyes, .

## Status update (2026-10-30 03:50) - stage 9 / 0.3.1: dof_off.

## Status update (2026-10-30 03:30) - stage 8 / 0.3.0
- User headset report (5090 + Pimax): stereo works very well; render_scale had no effect -> game was in exclusive
  fullscreen. Fixed with force_windowed + clamp (see Render resolution). Tests 019-022 on the 5070: borderless does
  not resize; with force_windowed the main menu renders in stereo at 5120x2880 from a fullscreen profile.

## Status (2026-10-29 22:15, end of the first session)
- Done: proxy, flat screen, head tracking (mono), NATIVE STEREO (render twice per game frame), HUD on its own quad
  layer (white/black matte from the two eye frames), render resolution at headset size (engine resize flag),
  motion blur off / per-eye history / temporal SMAA off in stereo, VRS foveation, vrkit menu (DAI pages), F8/F9/F10.
- Next: headset test by the user (scale, HUD placement, comfort, performance at 0.8-1.0 render scale on the 5090);
  cutscene / conversation / inventory behaviour in stereo; game-exit access violation (exe+304E1E1, an atexit
  destructor unregistering from a freed manager; test 017: same with and without the window resize, the game still
  closes normally - first-chance only; check once without the mod); render aspect matching
  the headset instead of 16:9 (pixels are anisotropic now); VR controllers as pad (code from MEA, untested here).
