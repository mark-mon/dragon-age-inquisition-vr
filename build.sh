#!/bin/bash
# Cross-builds DAI VR (64-bit) on Linux with clang-cl + lld-link against an MSVC/Windows SDK copy.
# $TC/build.sh is a small wrapper: build.sh <x64|x86> <out.dll> <cl args...> /link <link args...>
# (zig clang --driver-mode=cl --target=x86_64-pc-windows-msvc with the MSVC + Windows SDK include and lib paths).
# Output: dist/dinput8.dll and dist/version.dll (identical code; the file name selects what it forwards to).
set -e
cd "$(dirname "$0")"
B=${TC:?set TC to the toolchain folder (see README)}/build.sh
mkdir -p build dist
CFLAGS="/O2 /Zi /W3 -D_CRT_SECURE_NO_WARNINGS -Wno-unused-function -Wno-missing-field-initializers -Wno-unknown-pragmas -Wno-sign-compare -Wno-unused-variable /Ithird_party/openxr/include /Ithird_party/minhook/include /Ivrkit /Ivrkit/third_party/imgui"
SRC="src/main.cpp src/crash.cpp src/proxy.cpp src/present.cpp src/xr.cpp src/blit.cpp src/settings.cpp src/menu_options.cpp src/capture.cpp src/camscan.cpp src/watch.cpp src/fbsettings.cpp src/engine.cpp src/window_size.cpp src/headpose.cpp src/nvhook.cpp src/cbfix.cpp src/finalpass.cpp src/vrs.cpp src/aimscan.cpp src/headaim.cpp src/vrpad.cpp src/weapondetect.cpp src/depthprobe.cpp src/vtbl.c \
 vrkit/vrkit_input.cpp vrkit/vrkit_menu.cpp vrkit/vrkit_menu_core.cpp vrkit/vrkit_options.cpp \
 vrkit/third_party/imgui/imgui.cpp vrkit/third_party/imgui/imgui_draw.cpp vrkit/third_party/imgui/imgui_tables.cpp vrkit/third_party/imgui/imgui_widgets.cpp vrkit/third_party/imgui/imgui_impl_dx11.cpp src/log.cpp src/config.cpp \
 third_party/minhook/src/buffer.c third_party/minhook/src/hook.c third_party/minhook/src/trampoline.c third_party/minhook/src/hde/hde64.c"
$B x64 dist/daivr.dll $CFLAGS /LD /Fobuild/ /Fdbuild/daivr.pdb $SRC \
   /link /DEF:src/exports.def /DEBUG /PDB:dist/daivr.pdb third_party/openxr/lib/openxr_loader_static.lib \
   user32.lib dxgi.lib d3dcompiler.lib dxguid.lib advapi32.lib ole32.lib shell32.lib pathcch.lib shlwapi.lib

cp dist/daivr.dll dist/version.dll; cp dist/daivr.pdb dist/version.pdb
ls -la dist
