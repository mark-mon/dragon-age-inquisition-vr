@echo off
rem Builds version.dll with Visual Studio 2019/2022 (run from an "x64 Native Tools Command Prompt").
rem Output: dist\version.dll (+ pdb). Copy version.dll next to DragonAgeInquisition.exe.
setlocal
cd /d "%~dp0"
if not exist build mkdir build
if not exist dist mkdir dist
set SRC=src\main.cpp src\crash.cpp src\proxy.cpp src\present.cpp src\xr.cpp src\blit.cpp src\settings.cpp src\menu_options.cpp ^
 src\capture.cpp src\camscan.cpp src\watch.cpp src\fbsettings.cpp src\engine.cpp src\window_size.cpp src\headpose.cpp src\nvhook.cpp ^
 src\cbfix.cpp src\finalpass.cpp src\vrs.cpp src\aimscan.cpp src\headaim.cpp src\vrpad.cpp src\weapondetect.cpp src\depthprobe.cpp src\vtbl.c ^
 vrkit\vrkit_input.cpp vrkit\vrkit_menu.cpp vrkit\vrkit_menu_core.cpp vrkit\vrkit_options.cpp ^
 vrkit\third_party\imgui\imgui.cpp vrkit\third_party\imgui\imgui_draw.cpp vrkit\third_party\imgui\imgui_tables.cpp ^
 vrkit\third_party\imgui\imgui_widgets.cpp vrkit\third_party\imgui\imgui_impl_dx11.cpp src\log.cpp src\config.cpp ^
 third_party\minhook\src\buffer.c third_party\minhook\src\hook.c third_party\minhook\src\trampoline.c third_party\minhook\src\hde\hde64.c
cl /nologo /O2 /Zi /W3 /std:c++17 /EHsc /MT /DWIN32 /D_WINDOWS /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS ^
 /Ithird_party\openxr\include /Ithird_party\minhook\include /Ivrkit /Ivrkit\third_party\imgui ^
 /LD /Fobuild\ /Fdbuild\daivr.pdb %SRC% /Fedist\version.dll ^
 /link /DEF:src\exports.def /DEBUG /PDB:dist\version.pdb third_party\openxr\lib\openxr_loader_static.lib ^
 user32.lib dxgi.lib d3dcompiler.lib dxguid.lib advapi32.lib ole32.lib shell32.lib pathcch.lib shlwapi.lib
if errorlevel 1 exit /b 1
echo Built dist\version.dll
