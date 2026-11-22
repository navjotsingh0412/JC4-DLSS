@echo off
setlocal
rem Builds src\jc4_dlss.cpp into build\out\jc4_dlss.addon64 with MSVC x64.
rem Run scripts\get_deps.ps1 first so that .\deps exists.
where cl >nul 2>nul
if errorlevel 1 (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" (
        call "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" (
        call "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
    ) else (
        echo cl.exe not found. Run this from an "x64 Native Tools Command Prompt for VS 2022".
        exit /b 1
    )
)
cd /d "%~dp0\.."
if not exist deps\nvidia-dlss-sdk\lib\nvsdk_ngx_d.lib (
    echo deps not found. Run: powershell -ExecutionPolicy Bypass -File scripts\get_deps.ps1 -AcceptNvidiaLicense
    exit /b 1
)
if not exist build\out mkdir build\out
cl /nologo /std:c++17 /O2 /EHsc /MD /LD ^
   /I deps\imgui /I deps\reshade\include /I deps\nvidia-dlss-sdk\include ^
   src\jc4_dlss.cpp /Fo:build\out\ /Fe:build\out\jc4_dlss.addon64 ^
   /link /LIBPATH:deps\nvidia-dlss-sdk\lib nvsdk_ngx_d.lib user32.lib d3d11.lib dxguid.lib d3dcompiler.lib advapi32.lib
if errorlevel 1 exit /b 1
echo BUILD OK: build\out\jc4_dlss.addon64
