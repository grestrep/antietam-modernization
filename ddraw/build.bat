@echo off
rem ---------------------------------------------------------------------------
rem  Builds the smav2 ddraw.dll (32-bit) into ddraw\build\ and copies smav2.ini
rem  next to it.  Requires Visual Studio 2022 or newer with the C++ x86 toolset and
rem  the Windows SDK (fxc.exe shader compiler, put on PATH by vcvars32).
rem    1. fxc compiles shaders.hlsl into byte-code headers build\shader_*.h
rem       (embedded in the DLL: no shader compiler needed at run time)
rem    2. cl compiles smav2_ddraw.cpp + present_d3d11.cpp into build\ddraw.dll
rem         /LD  DLL     /MT  static CRT (no VC++ redist needed)     /O2  optimise
rem         /DEF export table (ddraw.def)   /Zi + /DEBUG  build\ddraw.pdb for debugging
rem  Deploy: ..\deploy.bat (or copy build\ddraw.dll into the game folder).
rem  In VS Code: Ctrl+Shift+B builds, F5 builds + deploys + runs under the debugger.
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
fxc /nologo /T vs_4_0 /E VS_Fullscreen /Vn g_VS_Fullscreen /Fh build\shader_vs.h      shaders.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PS_Nearest    /Vn g_PS_Nearest    /Fh build\shader_nearest.h shaders.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PS_Linear     /Vn g_PS_Linear     /Fh build\shader_linear.h  shaders.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PS_Sharp      /Vn g_PS_Sharp      /Fh build\shader_sharp.h   shaders.hlsl >nul || exit /b 1
fxc /nologo /T ps_4_0 /E PS_Scale2x    /Vn g_PS_Scale2x    /Fh build\shader_scale2x.h shaders.hlsl >nul || exit /b 1
cl /nologo /O2 /Zi /MT /W3 /EHsc /LD /Ibuild smav2_ddraw.cpp present_d3d11.cpp /Fo:build\ /Fd:build\ /Fe:build\ddraw.dll /link /DEBUG /OPT:REF /OPT:ICF /DEF:ddraw.def user32.lib gdi32.lib kernel32.lib d3d11.lib dxgi.lib || exit /b 1
copy /y smav2.ini build\ >nul
