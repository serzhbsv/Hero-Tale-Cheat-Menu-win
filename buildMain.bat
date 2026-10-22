@echo off
call "D:\Install\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat" x64

if not exist build mkdir build

if not exist .\lib\ImGuiMinHook.lib (
    echo [ERROR] ImGuiMinHook.lib not found! Run build_lib.bat first.
    pause
    exit /b 1
)

echo Compiling Main.cpp...

cl.exe /c /utf-8 /MT /EHsc /O2 /std:c++17 ^
    /Fo.\build\Main.obj ^
    /Iimgui /IMinHook\include ^
    Main.cpp

if errorlevel 1 (
    echo [ERROR] Compile failed
    pause
    exit /b 1
)

echo Linking ExpHook.dll...

link /OUT:.\build\ExpHook.dll /DLL ^
    .\build\Main.obj ^
    .\lib\ImGuiMinHook.lib ^
    d3d11.lib dxgi.lib user32.lib gdi32.lib

if errorlevel 1 (
    echo [ERROR] Link failed
    pause
    exit /b 1
)

del .\build\Main.obj

echo Copying to game folder...
copy /Y .\build\ExpHook.dll "D:\Games\HeroTale\Hero.Tale.v1.0\ExpHook.dll"
copy /Y .\build\ExpHook.dll "D:\Games\HeroTale\Hero.Tale.v1.4.12\ExpHook.dll"
copy /Y .\build\ExpHook.dll "D:\Games\HeroTale\Hero.Tale.v1.4.18\ExpHook.dll"

echo Done.
pause
