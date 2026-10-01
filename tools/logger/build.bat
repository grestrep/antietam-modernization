@echo off
rem ---------------------------------------------------------------------------
rem  Builds the logging proxy ddraw.dll (research tool, 32-bit, MSVC x86)
rem  into tools\logger\build\.  Requires VS 2022+ with C++ x86 tools.
rem  Use: copy build\ddraw.dll next to the game exe, play, then remove it.
rem  It writes ddraw_proxy.log and ddraw_proxy_summary.txt next to the exe.
rem ---------------------------------------------------------------------------
setlocal
rem Find Visual Studio (any edition/version with the C++ x86 tools) via vswhere.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (echo Visual Studio not found - install VS 2022+ with "Desktop development with C++". & exit /b 1)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR (echo No Visual Studio with the C++ x86/x64 tools found. & exit /b 1)
call "%VSDIR%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /W3 /EHsc /LD ddraw_proxy.cpp /Fo:build\ /Fe:build\ddraw.dll /link /DEF:ddraw.def user32.lib kernel32.lib
