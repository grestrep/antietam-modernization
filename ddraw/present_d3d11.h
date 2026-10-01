// =====================================================================================
//  smav2 ddraw.dll - Direct3D 11 presenter (renderer=d3d11)
// =====================================================================================
// Replaces the GDI StretchBlt presentation of the virtual screen with the GPU:
//   game thread:   D3D_SubmitFrame() copies the finished 800x600 32-bit frame into a shared
//                  buffer (memcpy, ~0.2 ms) and wakes the render thread - it never waits for
//                  the GPU or v-sync, so the game's own timing is unaffected.
//   render thread: owns the D3D11 device and a flip-model swap chain on the game window;
//                  uploads the newest frame to a texture and draws it into the picture
//                  rectangle (black letterbox bars around it) with the selected shader filter,
//                  then Present(vsync).
// If anything fails (no D3D11, swap chain creation, device lost twice), the presenter reports
// failure and smav2_ddraw.cpp falls back to the GDI path.
// =====================================================================================
#pragma once
#include <windows.h>

enum D3DFilter { D3DF_NEAREST, D3DF_LINEAR, D3DF_SHARP, D3DF_SCALE2X };

typedef void (*D3DLogFn)(const char* fmt, ...);

// Starts the render thread for `hwnd` and waits until the device and swap chain exist.
// exclusive = mode=exclusive: switch the monitor to texW x texH and go exclusive fullscreen
// (re-entered automatically after Alt-Tab, left on D3D_Stop and by DXGI on exit).
// gpuPref: 0 = Windows' default adapter, 1 = high-performance GPU, 2 = power-saving GPU.
// Returns false (and stops) if Direct3D 11 - or the exclusive switch - cannot be used.
bool D3D_Start(HWND hwnd, int texW, int texH, bool vsync, bool flipModel, bool exclusive, int gpuPref, D3DLogFn log);

// Stops the render thread and releases everything. Safe to call when not started.
void D3D_Stop();

// True while the presenter is running (false after a fatal error -> use GDI).
bool D3D_Active();

// Layout: client size of the window, picture rectangle inside it, and the filter.
void D3D_SetLayout(int clientW, int clientH, int picX, int picY, int picW, int picH, D3DFilter f);

// Hands over a new frame: `bits` = top-down 32-bit BGRX, texW*4 bytes per row.
void D3D_SubmitFrame(const void* bits);

// Asks for a re-present of the last frame (e.g. after WM_PAINT).
void D3D_Refresh();

// Number of frames presented so far (diagnostics).
LONG D3D_PresentCount();

// Debug: write the back buffer of present number N to bmpPath (call before D3D_Start).
void D3D_DebugDump(LONG presentNumber, const char* bmpPath);
