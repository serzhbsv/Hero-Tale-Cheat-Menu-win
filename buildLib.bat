@echo off
call "D:\Install\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64

if not exist build mkdir build
if not exist lib mkdir lib

echo Building static library (ImGui + MinHook)...

cl.exe /c /utf-8 /MT /EHsc /O2 /std:c++17 ^
    /Fo.\build\ ^
    /Iimgui /IMinHook\include ^
    imgui\imgui.cpp imgui\imgui_demo.cpp imgui\imgui_draw.cpp ^
    imgui\imgui_tables.cpp imgui\imgui_widgets.cpp ^
    imgui\imgui_impl_dx11.cpp imgui\imgui_impl_win32.cpp ^
    MinHook\src\hook.c MinHook\src\buffer.c ^
    MinHook\src\trampoline.c MinHook\src\hde\hde64.c

if errorlevel 1 (
    echo [ERROR] Library compile failed
    pause
    exit /b 1
)

lib /OUT:.\lib\ImGuiMinHook.lib .\build\*.obj
if errorlevel 1 (
    echo [ERROR] lib failed
    pause
    exit /b 1
)

del .\build\*.obj
echo [LIB] Done: lib\ImGuiMinHook.lib
pause
