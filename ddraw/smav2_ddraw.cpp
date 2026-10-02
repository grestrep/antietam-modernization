// =====================================================================================
//  smav2 ddraw.dll  -  replacement DirectDraw for Sid Meier's Antietam! (32-bit, x86)
// =====================================================================================
//
// WHAT THE GAME DOES WITH DIRECTDRAW (measured with tools/logger, see docs/ddraw-trace.md)
//   DirectDrawCreate(NULL)                          -> one IDirectDraw (v1) object
//   SetCooperativeLevel(hwnd, FULLSCREEN|EXCLUSIVE)
//   SetDisplayMode(800, 600, 8)                     -> the monitor goes to 800x600, 256 colours
//   SetCooperativeLevel(NULL, NORMAL)
//   CreatePalette(8BIT), CreateSurface(PRIMARY)     -> created but never drawn to
//   ... RestoreDisplayMode on exit.
// Everything the player sees is drawn with GDI (DIB sections + BitBlt/StretchBlt) into the game
// window. DirectDraw is used for nothing but the mode switch.
//
// WHAT THIS DLL DOES
// The game imports ddraw.dll by name, so a ddraw.dll placed next to the exe is loaded instead of
// the system one. We load the real C:\Windows\SysWOW64\ddraw.dll ourselves and forward every
// export to it, but:
//  1. Emulated mode switch (docs/mode-switch.md). A few methods of the real IDirectDraw object
//     are patched:
//       SetCooperativeLevel  EXCLUSIVE|FULLSCREEN is downgraded to NORMAL
//       SetDisplayMode       only records the requested mode; the monitor is never switched
//       RestoreDisplayMode   no-op;   GetDisplayMode   reports the emulated mode (800x600x8)
//     and the game window is taken over: windowed (title bar) or borderless (whole monitor),
//     cursor clipping, focus loss hidden (keep_focus), DWM "Not Responding" ghosting disabled.
//  2. Scaling (docs/ddraw-dll.md section "Scaling"). The game still renders an 800x600 picture,
//     but into an off-screen "virtual screen" instead of its window. We then copy that picture
//     into the window enlarged, centred, with black bars: borderless by a whole factor
//     (scale=auto: the largest that fits), windowed to any 4:3 size (window_size=1024x768 etc.;
//     the player may also resize/maximise). Filters: nearest, linear, or sharp (crisp at
//     non-whole factors). Window-related calls of the game exe are translated between the game's
//     800x600 coordinates and the real, scaled window, so mouse clicks and edge scrolling land
//     in the right place.
//  3. Presentation: GDI StretchBlt (renderer=gdi) or the Direct3D 11 presenter in
//     present_d3d11.cpp (renderer=d3d11: GPU scaling with shader filters, v-sync, own thread).
//  4. Diagnostics: a log file, a hang/stall watchdog and a frame counter.
// mode=fullscreen in smav2.ini turns all of this off: the DLL then only forwards to the system
// ddraw.dll and the game behaves exactly as without it.
//
// FILES
//   smav2_ddraw.cpp   this file          ddraw.def      export table (same names/ordinals as Windows)
//   present_d3d11.*   D3D11 presenter    shaders.hlsl   its pixel shaders (precompiled by build.bat)
//   smav2.ini         settings           build.bat      MSVC x86 build -> build\ddraw.dll
//
// TECHNIQUES USED
//   * Export forwarding: naked jmp stubs through a table filled with GetProcAddress (FWD macro).
//   * Vtable patching: instead of writing a full COM wrapper, the real IDirectDraw object is
//     returned to the game and the handful of interesting entries of its (shared, read-only)
//     vtable are replaced with our functions. The originals are kept in g_orig.
//   * IAT hooking: entries in the game exe's import address table (GetDC, BitBlt,
//     ScreenToClient, ...) are overwritten, so only calls made by the game exe are intercepted;
//     our own calls and Windows' internal calls reach the real functions.
//   * Window subclassing: our WndProc sits in front of the game's window procedure.
// =====================================================================================

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>   // makes <ddraw.h> define the IID_IDirectDraw* GUIDs in this file
#include <ddraw.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <share.h>
#include <mmsystem.h>   // timeGetTime (hooked for present pacing)
#include "present_d3d11.h"

// =====================================================================================
//  Configuration (smav2.ini next to the DLL) and logging (smav2_ddraw.log)
// =====================================================================================

enum Mode { MODE_FULLSCREEN, MODE_WINDOWED, MODE_BORDERLESS, MODE_EXCLUSIVE };

static Mode g_mode       = MODE_WINDOWED;  // [display] mode
static int  g_scaleCfg   = 0;              // [display] scale: 0 = auto, 1, 2, 3, ...
enum Filter { FILTER_AUTO, FILTER_NEAREST, FILTER_LINEAR, FILTER_SHARP, FILTER_SCALE2X };
static Filter g_filterCfg = FILTER_AUTO;   // [display] filter
static int  g_winW, g_winH;                // [display] window_size (windowed client size), 0 = auto
static bool g_wantD3D   = false;          // [display] renderer: gdi (false) | d3d11 (true)
static bool g_vsync     = true;           // [display] vsync (d3d11 only)
static bool g_flipModel = true;           // [display] swap: flip | blt (d3d11 only)
static int  g_gpuPref   = 1;              // [display] gpu: high (1) | low (2) | default (0)
static bool g_clipCursor = true;           // [display] clip_cursor
static bool g_dpiAware   = true;           // [display] dpi_aware
static bool g_keepFocus  = true;           // [display] keep_focus
static int  g_maxFps     = 40;             // [game]    max_fps (0 = unlimited)
static bool g_logOn      = true;           // [debug]   log
static bool g_watchdog   = false;          // [debug]   hang_watchdog

static char g_dir[MAX_PATH];               // folder of this DLL (= game folder)
static FILE* g_log;
static CRITICAL_SECTION g_cs;              // guards the log file and vtable patching

// Number of BitBlt/StretchBlt calls made by the game exe. The game draws every frame with one of
// these, so the watchdog uses this counter to tell "running" from "stopped drawing".
static volatile LONG g_blits;

// Appends one line "[tick] message" to smav2_ddraw.log. Thread-safe; flushes every line so the
// log survives the game being killed.
static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "[%8lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

// Reads smav2.ini. Missing file or keys -> the defaults above.
static void LoadConfig()
{
    char ini[MAX_PATH], v[32];
    sprintf_s(ini, "%s\\smav2.ini", g_dir);
    GetPrivateProfileStringA("display", "mode", "windowed", v, sizeof v, ini);
    g_mode = !_stricmp(v, "fullscreen") ? MODE_FULLSCREEN
           : !_stricmp(v, "borderless") ? MODE_BORDERLESS
           : !_stricmp(v, "exclusive")  ? MODE_EXCLUSIVE
           : MODE_WINDOWED;
    GetPrivateProfileStringA("display", "scale", "auto", v, sizeof v, ini);
    g_scaleCfg = !_stricmp(v, "auto") ? 0 : atoi(v);
    if (g_scaleCfg < 0 || g_scaleCfg > 8) g_scaleCfg = 0;
    GetPrivateProfileStringA("display", "filter", "auto", v, sizeof v, ini);
    g_filterCfg = !_stricmp(v, "nearest") ? FILTER_NEAREST
                : !_stricmp(v, "linear")  ? FILTER_LINEAR
                : !_stricmp(v, "sharp")   ? FILTER_SHARP
                : !_stricmp(v, "scale2x") ? FILTER_SCALE2X
                : FILTER_AUTO;
    GetPrivateProfileStringA("display", "renderer", "gdi", v, sizeof v, ini);
    g_wantD3D = !_stricmp(v, "d3d11") || g_mode == MODE_EXCLUSIVE;   // exclusive needs D3D11
    g_vsync = GetPrivateProfileIntA("display", "vsync", 1, ini) != 0;
    GetPrivateProfileStringA("display", "swap", "flip", v, sizeof v, ini);
    g_flipModel = _stricmp(v, "blt") != 0;
    GetPrivateProfileStringA("display", "gpu", "high", v, sizeof v, ini);
    g_gpuPref = !_stricmp(v, "default") ? 0 : !_stricmp(v, "low") ? 2 : 1;
    GetPrivateProfileStringA("display", "window_size", "", v, sizeof v, ini);
    if (sscanf_s(v, "%dx%d", &g_winW, &g_winH) != 2 || g_winW < 320 || g_winH < 240) g_winW = g_winH = 0;
    g_clipCursor = GetPrivateProfileIntA("display", "clip_cursor", 1, ini) != 0;
    g_dpiAware   = GetPrivateProfileIntA("display", "dpi_aware", 1, ini) != 0;
    g_keepFocus  = GetPrivateProfileIntA("display", "keep_focus", 1, ini) != 0;
    g_maxFps     = GetPrivateProfileIntA("game", "max_fps", 40, ini);
    if (g_maxFps < 0 || g_maxFps > 1000) g_maxFps = 0;
    g_logOn      = GetPrivateProfileIntA("debug", "log", 1, ini) != 0;
    g_watchdog   = GetPrivateProfileIntA("debug", "hang_watchdog", 0, ini) != 0;
}

// =====================================================================================
//  Emulated display mode, window layout
// =====================================================================================

// The display mode the game asked for. 800x600x8 is what Antietam always requests; these
// defaults matter only for calls made before SetDisplayMode.
static DWORD g_w = 800, g_h = 600, g_bpp = 8;
static bool  g_modeSet;          // SetDisplayMode called and not yet "restored"

static HWND    g_hwnd;           // the game's main window (from the first SetCooperativeLevel)
static WNDPROC g_origProc;       // the game's own window procedure (we subclass in front of it)
static bool    g_applying;       // true while we change the style ourselves (WM_STYLECHANGING)
static bool    g_inSizeMove;     // the player is dragging/resizing the window

// Window styles we give the game window.
static const DWORD kWindowedStyle = WS_OVERLAPPEDWINDOW;   // caption, resizable, min/max buttons
static const DWORD kBorderlessStyle = WS_POPUP;

// Layout, computed by ComputeLayout()/LayoutForClient():
//   the window's client area is g_cw x g_ch; inside it the game picture (g_w x g_h) is drawn
//   enlarged to g_pw x g_ph at offset (g_offX, g_offY); the rest is black bars.
//   g_scaling: the virtual screen is in use and coordinates are translated. This is the case in
//   windowed and borderless mode (even at 1:1, so the window can be resized at any time).
static int  g_pw = 800, g_ph = 600, g_offX, g_offY, g_cw = 800, g_ch = 600;
static bool g_scaling;
static bool g_useNearest, g_useSharp;   // effective filter for the current picture size (GDI)
static D3DFilter g_d3dFilter = D3DF_NEAREST;   // effective filter for the D3D11 presenter

// Picks the filter for the current picture size: filter=auto uses plain nearest-neighbour for
// whole-number factors (pixel-perfect) and "sharp" for everything else.
static void ChooseFilter()
{
    bool integer = g_pw % (int)g_w == 0 && g_ph % (int)g_h == 0 && g_pw / (int)g_w == g_ph / (int)g_h;
    Filter f = g_filterCfg;
    if (f == FILTER_AUTO) f = integer ? FILTER_NEAREST : FILTER_SHARP;
    if (f == FILTER_SHARP && integer) f = FILTER_NEAREST;   // sharp == nearest at whole factors
    g_d3dFilter = f == FILTER_NEAREST ? D3DF_NEAREST : f == FILTER_LINEAR ? D3DF_LINEAR
                : f == FILTER_SCALE2X ? D3DF_SCALE2X : D3DF_SHARP;
    if (f == FILTER_SCALE2X) f = FILTER_SHARP;              // GDI has no Scale2x: use sharp there
    g_useNearest = f == FILTER_NEAREST;
    g_useSharp   = f == FILTER_SHARP;
}

// Given the current client size g_cw x g_ch: the largest picture with the game's aspect ratio
// (4:3) that fits, centred. Used for windowed mode, where the player may resize or maximise.
static void LayoutForClient()
{
    if ((long long)g_cw * g_h > (long long)g_ch * g_w) {   // client wider than 4:3: bars left/right
        g_ph = g_ch; g_pw = (int)((long long)g_ch * g_w / g_h);
    } else {                                                // taller: bars top/bottom
        g_pw = g_cw; g_ph = (int)((long long)g_cw * g_h / g_w);
    }
    if (g_pw < 1) g_pw = 1;
    if (g_ph < 1) g_ph = 1;
    g_offX = (g_cw - g_pw) / 2;
    g_offY = (g_ch - g_ph) / 2;
    ChooseFilter();
}

// Size of the window frame (borders + caption) around a client area, for kWindowedStyle.
static SIZE FrameExtra()
{
    RECT fr = { 0, 0, 100, 100 };
    AdjustWindowRectEx(&fr, kWindowedStyle, FALSE, 0);
    SIZE s = { (fr.right - fr.left) - 100, (fr.bottom - fr.top) - 100 };
    return s;
}

// Initial layout for the monitor the window is on.
//   borderless: the client area is the whole monitor; the picture is enlarged by a whole factor
//               (scale=auto: the largest that fits, or scale=N) and centred.
//   windowed:   the client area is window_size (e.g. 1024x768) if set, otherwise the picture at a
//               whole factor (scale=auto: the largest whose window still fits the work area).
//               A requested size larger than the work area is reduced, keeping 4:3.
static void ComputeLayout()
{
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoA(mon, &mi);
    int monW = mi.rcMonitor.right - mi.rcMonitor.left, monH = mi.rcMonitor.bottom - mi.rcMonitor.top;
    int waW = mi.rcWork.right - mi.rcWork.left,        waH = mi.rcWork.bottom - mi.rcWork.top;

    int availW = monW, availH = monH;
    if (g_mode == MODE_WINDOWED) {               // room left for the client area after the frame
        SIZE fe = FrameExtra();
        availW = waW - fe.cx;
        availH = waH - fe.cy;
    }
    int fit = min(availW / (int)g_w, availH / (int)g_h);
    if (fit < 1) fit = 1;
    int s = g_scaleCfg ? min(g_scaleCfg, fit) : fit;   // never larger than what fits

    if (g_mode == MODE_EXCLUSIVE) {             // the monitor itself switches to g_w x g_h: 1:1
        g_cw = g_pw = (int)g_w; g_ch = g_ph = (int)g_h;
        g_offX = g_offY = 0;
        ChooseFilter();
    } else if (g_mode == MODE_BORDERLESS) {
        g_cw = monW; g_ch = monH;
        g_pw = (int)g_w * s; g_ph = (int)g_h * s;
        g_offX = (monW - g_pw) / 2;
        g_offY = (monH - g_ph) / 2;
        ChooseFilter();
    } else {
        if (g_winW) {                            // explicit window_size
            g_cw = g_winW; g_ch = g_winH;
            if (g_cw > availW || g_ch > availH) {      // too big for the screen: shrink, keep 4:3
                double k = min((double)availW / g_cw, (double)availH / g_ch);
                g_cw = (int)(g_cw * k); g_ch = (int)(g_ch * k);
                Log("window_size %dx%d does not fit, reduced to %dx%d", g_winW, g_winH, g_cw, g_ch);
            }
        } else {
            g_cw = (int)g_w * s; g_ch = (int)g_h * s;
        }
        LayoutForClient();
    }
    g_scaling = true;
}

// Outer window rectangle for the initial layout.
static RECT TargetRect()
{
    HMONITOR mon = MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = { sizeof mi };
    GetMonitorInfoA(mon, &mi);
    if (g_mode == MODE_BORDERLESS)               // cover the whole monitor (incl. taskbar)
        return mi.rcMonitor;
    if (g_mode == MODE_EXCLUSIVE) {              // game-sized popup at the monitor's corner; DXGI
        RECT r = { mi.rcMonitor.left, mi.rcMonitor.top,   // then switches the monitor to that size
                   mi.rcMonitor.left + (LONG)g_w, mi.rcMonitor.top + (LONG)g_h };
        return r;
    }

    // Windowed: grow the client rectangle by the frame/caption, centre it in the work area.
    SIZE fe = FrameExtra();
    int w = g_cw + fe.cx, h = g_ch + fe.cy;
    const RECT& wa = mi.rcWork;                  // monitor minus taskbar
    int x = wa.left + ((wa.right - wa.left) - w) / 2;
    int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
    if (y < wa.top) y = wa.top;                  // never put the title bar off-screen
    RECT out = { x, y, x + w, y + h };
    return out;
}

// Screen position of the window's client-area origin (real coordinates).
static POINT ClientOrigin()
{
    POINT o = { 0, 0 };
    ClientToScreen(g_hwnd, &o);                  // our call: not hooked
    return o;
}

// Confines the mouse to the game picture (on=true) or releases it. The game scrolls the map when
// the cursor touches the edge of its 800x600 area; in a window that only works if the cursor
// cannot leave the picture. Controlled by clip_cursor.
static void ClipToClient(bool on)
{
    if (!g_clipCursor) return;
    if (!on || !g_hwnd) { ClipCursor(0); return; }
    POINT o = ClientOrigin();
    RECT c = { o.x + g_offX, o.y + g_offY, o.x + g_offX + g_pw, o.y + g_offY + g_ph };
    ClipCursor(&c);
}

// =====================================================================================
//  Scaling: virtual screen and presentation
// =====================================================================================
// While g_scaling is on, the game draws into g_vdc, an off-screen 32-bit 800x600 bitmap (the
// "virtual screen"), instead of its window:
//   * GetDC/BeginPaint on the game window return g_vdc (and ReleaseDC/EndPaint present it);
//   * GDI calls made on the window's real DC are redirected to g_vdc (RedirectDC), which covers
//     the case of the game keeping its window DC (the window class is CS_OWNDC).
// Present() copies g_vdc into the window, enlarged to g_pw x g_ph at (g_offX, g_offY), and paints
// the black bars. It runs when the game finishes drawing (ReleaseDC/EndPaint, or a blit covering
// at least a quarter of the screen) and from a 16 ms timer if something is left unpresented.
// Filters:
//   nearest  every game pixel becomes a block of screen pixels (perfect at whole factors; at
//            1.28x etc. some columns/rows are doubled and others not, which looks uneven)
//   linear   smooth bilinear-style stretch (GDI HALFTONE), soft at any size
//   sharp    "sharp bilinear": first nearest-neighbour to the next whole factor (e.g. 2x for
//            1.28x) into g_pdc, then a smooth downscale to the target: crisp but even pixels
// The game's 8-bit DIB sections carry their own colour tables, so blitting them into a 32-bit
// virtual screen gives correct colours (same as drawing into a window on a 32-bit desktop).

static HDC     g_vdc;            // memory DC holding the virtual screen
static HBITMAP g_vbmp, g_vbmpOld;
static void*   g_vbits;          // pixels of the virtual screen (top-down BGRX), for D3D11 upload
static HDC     g_realDC;         // the game window's own DC (CS_OWNDC: the same handle every time)
static PAINTSTRUCT g_realPs;     // real BeginPaint result, kept for the matching EndPaint
static bool    g_dirty;          // virtual screen changed since the last Present
static double  g_lastPresent;    // NowMs() of the last Present
static double  g_dirtySince;     // NowMs() when the virtual screen first changed after a Present

// Milliseconds from a high-resolution clock. (GetTickCount only advances in ~15.6 ms steps, too
// coarse for "at most one present every few ms" - it made presents arrive late and in bursts.)
static double NowMs()
{
    static LARGE_INTEGER f; LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// Latency statistics, logged every 10 s while frames are presented (see LogPresentStats).
static LONG   g_statPresents, g_statByEnd, g_statByDraw, g_statByTimer;
static double g_statLagSum, g_statLagMax, g_statLastLog;
static const UINT_PTR kPresentTimer = 0x534D4132;   // 'SMA2'

static BOOL (WINAPI* g_realBitBlt)(HDC, int, int, int, int, HDC, int, int, DWORD);
static BOOL (WINAPI* g_realStretchBlt)(HDC, int, int, int, int, HDC, int, int, int, int, DWORD);

// Creates (or re-creates after a mode change) the virtual screen, cleared to black.
static void CreateVirtualScreen()
{
    if (g_vdc) {
        SelectObject(g_vdc, g_vbmpOld); DeleteObject(g_vbmp); DeleteDC(g_vdc);
        g_vdc = 0;
    }
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = (LONG)g_w;
    bi.bmiHeader.biHeight = -(LONG)g_h;          // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = 0;
    g_vdc = CreateCompatibleDC(0);
    g_vbmp = CreateDIBSection(g_vdc, &bi, DIB_RGB_COLORS, &bits, 0, 0);
    g_vbits = bits;
    g_vbmpOld = (HBITMAP)SelectObject(g_vdc, g_vbmp);
    PatBlt(g_vdc, 0, 0, g_w, g_h, BLACKNESS);
    Log("virtual screen %lux%lu created (dc %p)", g_w, g_h, g_vdc);
}

// Intermediate bitmap for filter=sharp: the virtual screen enlarged by a whole factor g_pk.
static HDC     g_pdc;
static HBITMAP g_pbmp, g_pbmpOld;
static int     g_pk;

// Makes sure g_pdc holds a (g_w*k) x (g_h*k) bitmap.
static void EnsurePrescale(int k)
{
    if (g_pdc && g_pk == k) return;
    if (g_pdc) { SelectObject(g_pdc, g_pbmpOld); DeleteObject(g_pbmp); DeleteDC(g_pdc); g_pdc = 0; }
    HDC screen = GetDC(0);
    g_pdc = CreateCompatibleDC(screen);
    g_pbmp = CreateCompatibleBitmap(screen, (int)g_w * k, (int)g_h * k);
    ReleaseDC(0, screen);
    g_pbmpOld = (HBITMAP)SelectObject(g_pdc, g_pbmp);
    g_pk = k;
}

// Bookkeeping after each present: clears the dirty flag and collects latency statistics
// (time from the first change of the virtual screen to its presentation).
static void FinishPresent()
{
    double now = NowMs();
    if (g_dirty) {
        double lag = now - g_dirtySince;
        g_statLagSum += lag;
        if (lag > g_statLagMax) g_statLagMax = lag;
    }
    ++g_statPresents;
    g_dirty = false;
    g_lastPresent = now;
    if (now - g_statLastLog >= 10000) {
        if (g_statPresents > 1)
            Log("present stats (10 s): %ld presents (frame end %ld, drawing %ld, deferred %ld), "
                "change-to-present avg %.1f ms, max %.1f ms",
                g_statPresents, g_statByEnd, g_statByDraw, g_statByTimer,
                g_statLagSum / g_statPresents, g_statLagMax);
        g_statPresents = g_statByEnd = g_statByDraw = g_statByTimer = 0;
        g_statLagSum = g_statLagMax = 0;
        g_statLastLog = now;
    }
}

// Debug ([debug] snapshot=1): when a file "smav2_snap.req" appears in the game folder, write the
// virtual screen to smav2_snap.bmp and delete the request. Lets test scripts see exactly what the
// game drew, whatever the renderer, even when the screen is static (checked from our timer).
static bool g_snapEnabled;
static void SnapshotIfRequested()
{
    static double lastCheck;                     // also called from hot hooks: look at most every 100 ms
    double now = NowMs();
    if (now - lastCheck < 100) return;
    lastCheck = now;
    char req[MAX_PATH], bmp[MAX_PATH];
    sprintf_s(req, "%s\\smav2_snap.req", g_dir);
    if (!g_vbits || GetFileAttributesA(req) == INVALID_FILE_ATTRIBUTES) return;
    DeleteFileA(req);
    GdiFlush();
    sprintf_s(bmp, "%s\\smav2_snap.bmp", g_dir);
    DWORD size = g_w * g_h * 4;
    BITMAPFILEHEADER fh = { 0x4D42, sizeof(fh) + sizeof(BITMAPINFOHEADER) + size, 0, 0, sizeof(fh) + sizeof(BITMAPINFOHEADER) };
    BITMAPINFOHEADER ih = { sizeof(ih), (LONG)g_w, -(LONG)g_h, 1, 32, BI_RGB };
    HANDLE f = CreateFileA(bmp, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n;
    WriteFile(f, &fh, sizeof(fh), &n, 0); WriteFile(f, &ih, sizeof(ih), &n, 0); WriteFile(f, g_vbits, size, &n, 0);
    CloseHandle(f);
    Log("snapshot written to %s", bmp);
}

// Copies the virtual screen into the game window, scaled, and blacks out the bars.
static void Present()
{
    if (!g_scaling || !g_vdc || !g_hwnd) return;
    if (D3D_Active()) {                          // renderer=d3d11: hand the frame to the GPU thread
        GdiFlush();                              // make sure GDI has finished writing g_vbits
        D3D_SubmitFrame(g_vbits);
        FinishPresent();
        return;
    }
    HDC dc = GetDC(g_hwnd);                      // our call: real GetDC
    int pw = g_pw, ph = g_ph;
    if (g_useNearest) {                          // straight nearest-neighbour
        SetStretchBltMode(dc, COLORONCOLOR);
        g_realStretchBlt(dc, g_offX, g_offY, pw, ph, g_vdc, 0, 0, g_w, g_h, SRCCOPY);
    } else if (g_useSharp) {                     // nearest to a whole factor, then smooth down
        int k = max((pw + (int)g_w - 1) / (int)g_w, (ph + (int)g_h - 1) / (int)g_h);   // ceil
        EnsurePrescale(k);
        SetStretchBltMode(g_pdc, COLORONCOLOR);
        g_realStretchBlt(g_pdc, 0, 0, (int)g_w * k, (int)g_h * k, g_vdc, 0, 0, g_w, g_h, SRCCOPY);
        SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, 0);
        g_realStretchBlt(dc, g_offX, g_offY, pw, ph, g_pdc, 0, 0, (int)g_w * k, (int)g_h * k, SRCCOPY);
    } else {                                     // linear
        SetStretchBltMode(dc, HALFTONE); SetBrushOrgEx(dc, 0, 0, 0);
        g_realStretchBlt(dc, g_offX, g_offY, pw, ph, g_vdc, 0, 0, g_w, g_h, SRCCOPY);
    }
    // black bars (only the ones that exist)
    if (g_offX > 0) {
        PatBlt(dc, 0, 0, g_offX, g_ch, BLACKNESS);
        PatBlt(dc, g_offX + pw, 0, g_cw - g_offX - pw, g_ch, BLACKNESS);
    }
    if (g_offY > 0) {
        PatBlt(dc, g_offX, 0, pw, g_offY, BLACKNESS);
        PatBlt(dc, g_offX, g_offY + ph, pw, g_ch - g_offY - ph, BLACKNESS);
    }
    ReleaseDC(g_hwnd, dc);
    FinishPresent();
}

// Tells the D3D11 presenter about the current layout (no-op with the GDI renderer).
static void PushLayout()
{
    if (D3D_Active()) D3D_SetLayout(g_cw, g_ch, g_offX, g_offY, g_pw, g_ph, g_d3dFilter);
}

// Present pacing. The game changes the virtual screen with many small GDI calls (a hover
// highlight, a drag rectangle) as well as whole-frame blits, and gives no "frame done" signal for
// the small ones. So every change presents right away, unless we presented less than
// kMinPresentMs ago; in that case the change waits for the next drawing call or, at the latest,
// the 16 ms timer. With D3D11 a present is only a 0.2 ms memcpy, so the gap can be short.
static const double kMinPresentMsD3D = 2.0, kMinPresentMsGdi = 6.0;
static void MarkDirty()
{
    double now = NowMs();
    if (!g_dirty) { g_dirty = true; g_dirtySince = now; }
    if (now - g_lastPresent >= (D3D_Active() ? kMinPresentMsD3D : kMinPresentMsGdi)) {
        ++g_statByDraw;
        Present();
    }
}
// A change that had to wait (we had just presented) is presented here as soon as the minimum gap
// has passed. Called from hooks the game calls constantly - timeGetTime (its frame limiter spins
// on it) and GetAsyncKeyState (input polling) - because the WM_TIMER backstop is unreliable: the
// game's message loop picks up WM_TIMER only every ~160 ms, which showed up as mouse lag
// (hover highlights and drag boxes appearing late).
static void FlushIfDue()
{
    if (!g_dirty || !g_scaling) return;
    if (NowMs() - g_lastPresent >= (D3D_Active() ? kMinPresentMsD3D : kMinPresentMsGdi)) {
        ++g_statByTimer;                         // counted as "deferred"
        Present();
    }
}

// Waits about `ms` milliseconds with sub-millisecond precision (a high-resolution waitable timer
// where Windows has one, Sleep otherwise).
static void PreciseWait(double ms)
{
    static HANDLE timer = CreateWaitableTimerExW(0, 0, 0x00000002 /*CREATE_WAITABLE_TIMER_HIGH_RESOLUTION*/,
                                                 TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(ms * 10000.0);    // relative, 100 ns units
        if (SetWaitableTimer(timer, &due, 0, 0, 0, FALSE)) { WaitForSingleObject(timer, INFINITE); return; }
    }
    Sleep((DWORD)(ms + 0.5));
}

// [game] max_fps: caps how many frames the game draws per second. The game runs its main loop
// flat out (over 1000 frames per second while scrolling on a modern PC) and moves the map a step
// per frame - growing while the mouse stays at the edge - so edge scrolling is far too fast.
// Holding each frame end until its time slot comes brings the frame rate - and with it the scroll
// speed - back to what the game was tuned for. Same pacing as DDrawCompat's FpsLimiter.
// We count only whole-screen blits (0,0 800x600) as frames, like DDrawCompat: a battle frame is
// one such blit followed by ReleaseDC (traced 2026-10-01). Drop-down menus and dialogs draw item
// by item with ReleaseDC after each piece; pacing those made them appear line by line.
static void FrameLimit()
{
    if (g_maxFps <= 0) return;
    static double next;
    double interval = 1000.0 / g_maxFps, now = NowMs();
    if (now >= next) { next = now + interval; return; }  // on time or late: no catch-up burst
    PreciseWait(next - now);
    next += interval;
}

// The game finished a frame (ReleaseDC/EndPaint/whole-screen blit): present now if possible.
static void PresentSoon()
{
    double now = NowMs();
    if (!g_dirty) { g_dirty = true; g_dirtySince = now; }
    if (now - g_lastPresent >= (D3D_Active() ? kMinPresentMsD3D : kMinPresentMsGdi)) {
        ++g_statByEnd;
        Present();
    }
}

// Maps a DC the game passes to a GDI call: the game window's real DC -> the virtual screen.
static HDC RedirectDC(HDC dc)
{
    return (g_scaling && dc && dc == g_realDC && g_vdc) ? g_vdc : dc;
}

// Virtual client point (game coordinates) <-> real client point (scaled window). VirtToReal gives
// the top-left screen pixel of game pixel v; RealToVirt the game pixel under real pixel r. Exact
// for any picture size (no rounding drift): x_real = off + x * g_pw / g_w.
static int ToRealX(int x) { return g_offX + (int)((long long)x * g_pw / (int)g_w); }
static int ToRealY(int y) { return g_offY + (int)((long long)y * g_ph / (int)g_h); }
static POINT VirtToReal(POINT v) { POINT r = { ToRealX(v.x), ToRealY(v.y) }; return r; }
static POINT RealToVirt(POINT r)
{
    POINT v = { (LONG)((long long)(r.x - g_offX) * (int)g_w / g_pw),
                (LONG)((long long)(r.y - g_offY) * (int)g_h / g_ph) };
    if (r.x < g_offX) v.x = -1;                  // left of the picture (division rounds to 0)
    if (r.y < g_offY) v.y = -1;
    return v;
}

// Is h a child window of the game window? (Positioned in game coordinates.)
static bool IsGameChild(HWND h)
{
    return g_scaling && h && g_hwnd && GetParent(h) == g_hwnd &&
           (GetWindowLongA(h, GWL_STYLE) & WS_CHILD);
}

// Note on child windows: the intro videos play in an MCI AVI child window that Windows' video
// code (not the game exe) creates and centres using the real client size, so it lands in the
// middle of the screen at its native 640x480 by itself; we leave it alone. Child windows the game
// exe positions itself go through the MoveWindow/SetWindowPos hooks below.

// =====================================================================================
//  The game window: window procedure and takeover
// =====================================================================================

// Name of a focus/activation-related message, or NULL. Used only for logging.
static const char* FocusMsgName(UINT m)
{
    switch (m) {
    case WM_ACTIVATEAPP:     return "WM_ACTIVATEAPP";
    case WM_ACTIVATE:        return "WM_ACTIVATE";
    case WM_SETFOCUS:        return "WM_SETFOCUS";
    case WM_KILLFOCUS:       return "WM_KILLFOCUS";
    case WM_QUERYNEWPALETTE: return "WM_QUERYNEWPALETTE";
    case WM_PALETTECHANGED:  return "WM_PALETTECHANGED";
    case WM_SYSCOMMAND:      return "WM_SYSCOMMAND";
    case WM_SIZE:            return "WM_SIZE";
    case WM_SHOWWINDOW:      return "WM_SHOWWINDOW";
    case WM_CLOSE:           return "WM_CLOSE";
    case WM_DESTROY:         return "WM_DESTROY";
    case WM_KEYDOWN:         return "WM_KEYDOWN";
    case WM_SYSKEYDOWN:      return "WM_SYSKEYDOWN";
    case WM_LBUTTONDOWN:     return "WM_LBUTTONDOWN";
    case WM_LBUTTONUP:       return "WM_LBUTTONUP";
    case WM_RBUTTONDOWN:     return "WM_RBUTTONDOWN";
    }                                                  // (WM_CAPTURECHANGED: too frequent to log)
    return 0;
}

// Our window procedure, installed in front of the game's (subclassing). Everything not handled
// here is passed on to the game's own procedure through CallWindowProcA.
static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    if (const char* n = FocusMsgName(m)) {
        static LONG logged;                              // cap: these can be frequent
        if (InterlockedIncrement(&logged) <= 400)
            Log("msg %s wp=0x%x lp=0x%x", n, (unsigned)wp, (unsigned)lp);
    }

    // keep_focus: the game was written for exclusive fullscreen, where losing focus means being
    // minimised. Windowed, it can lose focus while staying visible; we simply never tell it.
    if (g_keepFocus) {
        if ((m == WM_ACTIVATEAPP && !wp) || m == WM_KILLFOCUS) return 0;
        if (m == WM_ACTIVATE && LOWORD(wp) == WA_INACTIVE) { ClipToClient(false); return 0; }
    }

    // Scaling: mouse messages carry client coordinates of the real (scaled) window; the game
    // expects 800x600 coordinates.
    if (g_scaling && m >= WM_MOUSEFIRST && m <= WM_MOUSELAST && m != WM_MOUSEWHEEL && m != WM_MOUSEHWHEEL) {
        POINT r = { (short)LOWORD(lp), (short)HIWORD(lp) };
        POINT v = RealToVirt(r);
        lp = MAKELPARAM((WORD)(short)v.x, (WORD)(short)v.y);
    }

    switch (m) {
    case WM_TIMER:
        if (wp == kPresentTimer) {                                                          // ours
            if (g_dirty) { ++g_statByTimer; Present(); }
            if (g_snapEnabled) SnapshotIfRequested();
            return 0;
        }
        break;
    case WM_PAINT:                                       // window uncovered/restored: redraw
        if (!g_dirty) { g_dirty = true; g_dirtySince = NowMs(); }   // redraw from the virtual screen
        break;
    case WM_ERASEBKGND:
        if (g_scaling) return 1;                         // Present paints everything; no flicker
        break;
    case WM_WINDOWPOSCHANGING:
        // Borderless: the window must keep covering the monitor, whoever tries to move it.
        // (Windowed: the player may move/resize freely; the game's own attempts to move or size
        // its window are already dropped by the MoveWindow/SetWindowPos hooks.)
        if (g_mode == MODE_BORDERLESS && !IsIconic(h)) {
            WINDOWPOS* p = (WINDOWPOS*)lp;
            RECT r = TargetRect();
            p->x = r.left; p->y = r.top; p->cx = r.right - r.left; p->cy = r.bottom - r.top;
            p->flags &= ~(SWP_NOMOVE | SWP_NOSIZE);
        }
        break;
    case WM_SIZING: {
        // Windowed: while the player drags a border, keep the client area at the game's 4:3.
        // The edge being dragged decides which dimension follows the other.
        RECT* r = (RECT*)lp;
        SIZE fe = FrameExtra();
        int cw = (r->right - r->left) - fe.cx, ch = (r->bottom - r->top) - fe.cy;
        if (wp == WMSZ_TOP || wp == WMSZ_BOTTOM) cw = (int)((long long)ch * g_w / g_h);  // height leads
        else                                     ch = (int)((long long)cw * g_h / g_w);  // width leads
        if (wp == WMSZ_LEFT || wp == WMSZ_TOPLEFT || wp == WMSZ_BOTTOMLEFT) r->left = r->right - (cw + fe.cx);
        else                                                                  r->right = r->left + cw + fe.cx;
        if (wp == WMSZ_TOP || wp == WMSZ_TOPLEFT || wp == WMSZ_TOPRIGHT)     r->top = r->bottom - (ch + fe.cy);
        else                                                                  r->bottom = r->top + ch + fe.cy;
        return TRUE;
    }
    case WM_GETMINMAXINFO: {                             // windowed: not smaller than half size
        LRESULT res = CallWindowProcA(g_origProc, h, m, wp, lp);
        if (g_mode == MODE_WINDOWED) {
            SIZE fe = FrameExtra();
            MINMAXINFO* mm = (MINMAXINFO*)lp;
            mm->ptMinTrackSize.x = (LONG)g_w / 2 + fe.cx;
            mm->ptMinTrackSize.y = (LONG)g_h / 2 + fe.cy;
        }
        return res;
    }
    case WM_STYLECHANGING:
        // If the game (not us) changes its window style, e.g. back to WS_POPUP for "fullscreen",
        // rewrite the new style to ours. WS_VISIBLE/WS_CLIP*/WS_MAXIMIZE/WS_MINIMIZE are kept.
        if (wp == GWL_STYLE && !g_applying) {
            STYLESTRUCT* s = (STYLESTRUCT*)lp;
            DWORD keep = s->styleNew & (WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_MAXIMIZE | WS_MINIMIZE);
            s->styleNew = (g_mode == MODE_WINDOWED ? kWindowedStyle : kBorderlessStyle) | keep;
        }
        break;
    case WM_ENTERSIZEMOVE:                               // player starts dragging/resizing
        g_inSizeMove = true;
        ClipCursor(0);                                   // the cursor must be free to do that
        break;
    case WM_EXITSIZEMOVE:
        g_inSizeMove = false;
        if (GetForegroundWindow() == h) ClipToClient(true);
        break;
    case WM_ACTIVATE:                                    // clip while active and not minimised
        ClipToClient(LOWORD(wp) != WA_INACTIVE && !HIWORD(wp));
        break;
    case WM_SIZE:
        // Windowed: the player resized or maximised the window -> fit the picture to the new
        // client area (letterboxed if it is not 4:3). Mouse mapping follows automatically.
        // Exclusive: the same, so the picture follows the display mode DXGI picked.
        if ((g_mode == MODE_WINDOWED || g_mode == MODE_EXCLUSIVE) && wp != SIZE_MINIMIZED && LOWORD(lp) && HIWORD(lp)) {
            g_cw = LOWORD(lp); g_ch = HIWORD(lp);
            LayoutForClient();
            Log("resized: client %dx%d, picture %dx%d at (%d,%d), filter %s", g_cw, g_ch, g_pw, g_ph,
                g_offX, g_offY, g_useNearest ? "nearest" : g_useSharp ? "sharp" : "linear");
            PushLayout();
            Present();
        }
        if (!g_inSizeMove && GetForegroundWindow() == h) ClipToClient(true);
        break;
    case WM_MOVE:                                        // keep the clip rectangle up to date
        if (!g_inSizeMove && GetForegroundWindow() == h) ClipToClient(true);
        break;
    case WM_DESTROY:
        ClipToClient(false);
        KillTimer(h, kPresentTimer);
        D3D_Stop();                                      // render thread off before the window goes
        break;
    }
    LRESULT res = CallWindowProcA(g_origProc, h, m, wp, lp);
    if (m == WM_PAINT && g_scaling) Present();           // after the game's own WM_PAINT handling
    return res;
}

// Turns the game's fullscreen popup into our window: computes the layout, sets the style,
// subclasses the window procedure (once), moves/sizes the window and sets up the virtual screen.
// Called when we first learn the window handle (SetCooperativeLevel) and after SetDisplayMode.
static void ApplyWindow(const char* why)
{
    if (g_mode == MODE_FULLSCREEN || !g_hwnd || !IsWindow(g_hwnd)) return;
    RECT before; GetWindowRect(g_hwnd, &before);
    DWORD styleBefore = (DWORD)GetWindowLongA(g_hwnd, GWL_STYLE);

    g_applying = true;                                   // let WM_STYLECHANGING pass unchanged
    DWORD style = styleBefore & (WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS);
    style |= g_mode == MODE_WINDOWED ? kWindowedStyle : kBorderlessStyle;
    SetWindowLongA(g_hwnd, GWL_STYLE, (LONG)style);
    SetWindowLongA(g_hwnd, GWL_EXSTYLE, GetWindowLongA(g_hwnd, GWL_EXSTYLE) & ~WS_EX_TOPMOST);
    g_applying = false;

    if (!g_origProc)
        g_origProc = (WNDPROC)SetWindowLongA(g_hwnd, GWL_WNDPROC, (LONG)(LONG_PTR)WndProc);

    ComputeLayout();
    RECT r = TargetRect();
    SetWindowPos(g_hwnd, HWND_NOTOPMOST, r.left, r.top, r.right - r.left, r.bottom - r.top,
                 SWP_FRAMECHANGED | SWP_SHOWWINDOW);
    RECT rc; GetClientRect(g_hwnd, &rc);
    Log("window %p (%s): style 0x%08lx rect (%ld,%ld)-(%ld,%ld)  ->  style 0x%08lx rect (%ld,%ld)-(%ld,%ld) client %ldx%ld",
        g_hwnd, why, styleBefore, before.left, before.top, before.right, before.bottom,
        style, r.left, r.top, r.right, r.bottom, rc.right, rc.bottom);
    Log("layout: picture %dx%d (%.2fx, filter %s) at (%d,%d) in client %dx%d",
        g_pw, g_ph, (double)g_pw / g_w, g_wantD3D ? (g_d3dFilter == D3DF_NEAREST ? "nearest" : g_d3dFilter == D3DF_LINEAR ? "linear" : g_d3dFilter == D3DF_SCALE2X ? "scale2x" : "sharp")
                                               : g_useNearest ? "nearest" : g_useSharp ? "sharp" : "linear",
        g_offX, g_offY, g_cw, g_ch);

    if (g_scaling) {
        g_realDC = GetDC(g_hwnd);                        // CS_OWNDC: this is the window's DC
        ReleaseDC(g_hwnd, g_realDC);                     // (no-op for an own DC; handle stays valid)
        CreateVirtualScreen();
        SetTimer(g_hwnd, kPresentTimer, 16, 0);
        {   char ini[MAX_PATH]; sprintf_s(ini, "%s\\smav2.ini", g_dir);
            g_snapEnabled = GetPrivateProfileIntA("debug", "snapshot", 0, ini) != 0; }
        // renderer=d3d11: start the GPU presenter (once); on any failure stay with GDI.
        static bool triedD3D;
        if (g_wantD3D && !triedD3D) {
            triedD3D = true;
            char ini[MAX_PATH]; sprintf_s(ini, "%s\\smav2.ini", g_dir);
            if (int n = GetPrivateProfileIntA("debug", "d3d_dump", 0, ini)) {   // debug screenshot
                char bmp[MAX_PATH]; sprintf_s(bmp, "%s\\smav2_d3d_dump.bmp", g_dir);
                D3D_DebugDump(n, bmp);
            }
            bool ok = D3D_Start(g_hwnd, g_w, g_h, g_vsync, g_flipModel, g_mode == MODE_EXCLUSIVE, g_gpuPref, Log);
            if (!ok && g_mode == MODE_EXCLUSIVE) {
                // Exclusive fullscreen did not work: borderless at 1:1 (same picture size, monitor
                // unchanged), then try the D3D11 presenter again without the mode switch.
                Log("mode=exclusive failed -> borderless, scale 1");
                g_mode = MODE_BORDERLESS; g_scaleCfg = 1;
                ComputeLayout();
                RECT br = TargetRect();
                SetWindowPos(g_hwnd, HWND_NOTOPMOST, br.left, br.top, br.right - br.left, br.bottom - br.top,
                             SWP_FRAMECHANGED | SWP_SHOWWINDOW);
                ok = D3D_Start(g_hwnd, g_w, g_h, g_vsync, g_flipModel, false, g_gpuPref, Log);
            }
            Log(ok ? "renderer: Direct3D 11%s" : "renderer: Direct3D 11 not available -> GDI%s",
                g_mode == MODE_EXCLUSIVE ? " (exclusive fullscreen)" : "");
        }
        PushLayout();
        Present();
    }
}

// =====================================================================================
//  Diagnostics: hang / stall watchdog
// =====================================================================================
// A background thread checks once per second:
//   HANG   the window does not answer a WM_NULL within 2 s (its thread is not processing
//          messages at all)
//   STALL  the window answers, but the game made no BitBlt/StretchBlt call for 5 s (it is alive
//          but not drawing; this also fires harmlessly on menus, which only redraw on change)
//   IsHungAppWindow changes: Windows' own "Not Responding" verdict (input not read for 5 s)
// On HANG/STALL it takes 5 samples of the game thread: registers plus every value on its stack
// that lies inside the game exe's code (0x401000-0x4B0000), i.e. likely return addresses, so the
// log shows where in the game code it is stuck. Disable with hang_watchdog=0.

// Suspends thread `tid` briefly and logs its registers and the game-code addresses on its stack.
static void SampleThread(DWORD tid, int n)
{
    HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
    if (!t) { Log("HANG: cannot open thread %lu", tid); return; }
    if (SuspendThread(t) == (DWORD)-1) { CloseHandle(t); return; }
    CONTEXT c = {}; c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (GetThreadContext(t, &c)) {
        char buf[1024];
        int len = sprintf_s(buf, "sample %d: eip=%08lx esp=%08lx ebp=%08lx eax=%08lx ecx=%08lx edx=%08lx | game addrs on stack:",
                            n, c.Eip, c.Esp, c.Ebp, c.Eax, c.Ecx, c.Edx);
        DWORD* sp = (DWORD*)c.Esp; int found = 0;
        for (int i = 0; i < 2048 && found < 24 && len < 980; ++i) {
            DWORD v;   // ReadProcessMemory instead of *sp: stops cleanly at the end of the stack
            if (!ReadProcessMemory(GetCurrentProcess(), sp + i, &v, 4, 0)) break;
            if (v > 0x401000 && v < 0x4b0000) { len += sprintf_s(buf + len, sizeof buf - len, " %08lx", v); ++found; }
        }
        Log("%s", buf);
    }
    ResumeThread(t);
    CloseHandle(t);
}

static DWORD WINAPI Watchdog(void*)
{
    bool hung = false, stalled = false, hungApp = false;
    LONG lastBlits = 0;
    int  still = 0;                                      // seconds without a blit
    for (;;) {
        Sleep(1000);
        if (!g_hwnd || !IsWindow(g_hwnd)) continue;

        DWORD_PTR r;
        if (!SendMessageTimeoutA(g_hwnd, WM_NULL, 0, 0, SMTO_BLOCK, 2000, &r)) {
            // HANG: the window thread did not process our message for 2 s.
            if (hung) continue;                          // already reported this hang
            hung = true;
            DWORD tid = GetWindowThreadProcessId(g_hwnd, 0);
            Log("HANG: window %p (thread %lu) not responding for 2 s", g_hwnd, tid);
            for (int i = 0; i < 5; ++i) { SampleThread(tid, i); Sleep(150); }
            continue;
        }
        if (hung) Log("HANG: window responding again");
        hung = false;

        // Windows' own verdict. TRUE here although WM_NULL was answered means: the thread runs,
        // but nobody reads the input queue (see the ghosting comment in DllMain).
        bool h = IsHungAppWindow(g_hwnd) != 0;
        if (h != hungApp) {
            hungApp = h;
            Log("IsHungAppWindow=%d (window answers sent messages, but input has not been read for 5 s)", h);
        }

        // STALL: alive, but is the game still drawing? (Normal on idle menus.)
        LONG b = g_blits;
        if (b != lastBlits) {
            if (stalled) Log("drawing resumed after %d s (%ld blits)", still, b - lastBlits);
            stalled = false; still = 0; lastBlits = b;
        } else if (++still == 5 && !stalled) {
            stalled = true;
            DWORD tid = GetWindowThreadProcessId(g_hwnd, 0);
            Log("STALL (harmless on idle menus): window responds but the game drew nothing for 5 s (foreground=%d, iconic=%d)",
                GetForegroundWindow() == g_hwnd, IsIconic(g_hwnd));
            for (int i = 0; i < 5; ++i) { SampleThread(tid, i); Sleep(150); }
        }
    }
}

// =====================================================================================
//  IAT hooks in the game exe
// =====================================================================================
// Only calls made by the game exe are affected, not calls from Windows, MCI or this DLL.
// Drawing hooks redirect the window DC to the virtual screen; window/mouse hooks translate
// between game coordinates (800x600) and the real, scaled window. With scaling off every hook
// simply calls the real function (apart from GetSystemMetrics).
//
// Coordinate model while scaling: the game's "client coordinates" are virtual (0..799, 0..599);
// "screen coordinates" are real. ScreenToClient/ClientToScreen on the game window convert between
// the two, so code like GetCursorPos + ScreenToClient, or ClientToScreen + SetCursorPos, works
// unchanged, and popup windows the game places with ClientToScreen land on the right spot.

#define REAL(fn) static decltype(&fn) g_real_##fn
REAL(GetSystemMetrics); REAL(GetDC); REAL(ReleaseDC); REAL(BeginPaint); REAL(EndPaint);
REAL(ScreenToClient); REAL(ClientToScreen); REAL(GetWindowRect); REAL(MoveWindow); REAL(SetWindowPos);
REAL(SetWindowRgn); REAL(FillRect); REAL(TextOutA); REAL(SelectClipRgn); REAL(SelectPalette);
REAL(RealizePalette); REAL(GetSystemPaletteEntries); REAL(timeGetTime); REAL(GetAsyncKeyState);

// GetSystemMetrics: the game sizes things from the "screen" size, which after a real mode switch
// would be 800x600. Report the emulated mode instead of the real desktop size.
static int WINAPI Hook_GetSystemMetrics(int i)
{
    if (i == SM_CXSCREEN) return (int)g_w;
    if (i == SM_CYSCREEN) return (int)g_h;
    return g_real_GetSystemMetrics(i);
}

// --- present pacing: hooks the game calls constantly (see FlushIfDue) -------------------------
static DWORD WINAPI Hook_timeGetTime()
{
    if (g_dirty) FlushIfDue();
    if (g_snapEnabled) SnapshotIfRequested();    // in battle the WM_TIMER backstop is starved
    return g_real_timeGetTime();
}
static SHORT WINAPI Hook_GetAsyncKeyState(int k) { if (g_dirty) FlushIfDue(); return g_real_GetAsyncKeyState(k); }

// --- drawing -------------------------------------------------------------------------------
static HDC WINAPI Hook_GetDC(HWND h)
{
    if (g_scaling && h && h == g_hwnd && g_vdc) return g_vdc;
    return g_real_GetDC(h);
}
static int WINAPI Hook_ReleaseDC(HWND h, HDC dc)
{
    if (dc && dc == g_vdc) { PresentSoon(); return 1; }  // the game finished a frame
    return g_real_ReleaseDC(h, dc);
}
static HDC WINAPI Hook_BeginPaint(HWND h, LPPAINTSTRUCT ps)
{
    if (!(g_scaling && h == g_hwnd && g_vdc)) return g_real_BeginPaint(h, ps);
    g_real_BeginPaint(h, &g_realPs);                     // validates the update region
    *ps = g_realPs;
    ps->hdc = g_vdc;                                     // ... but the game paints the virtual screen
    SetRect(&ps->rcPaint, 0, 0, g_w, g_h);
    return g_vdc;
}
static BOOL WINAPI Hook_EndPaint(HWND h, const PAINTSTRUCT* ps)
{
    if (!(g_scaling && h == g_hwnd && ps && ps->hdc == g_vdc)) return g_real_EndPaint(h, ps);
    g_real_EndPaint(h, &g_realPs);
    Present();
    return TRUE;
}
static BOOL WINAPI Hook_BitBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, DWORD rop)
{
    InterlockedIncrement(&g_blits);
    d = RedirectDC(d); s = RedirectDC(s);
    BOOL ok = g_realBitBlt(d, x, y, w, h, s, sx, sy, rop);
    if (d == g_vdc && g_vdc && x == 0 && y == 0 && w == (int)g_w && h == (int)g_h) FrameLimit();   // whole screen = frame
    if (d == g_vdc && g_vdc && w * h * 4 >= (int)(g_w * g_h)) PresentSoon();   // big blit: present now
    else if (d == g_vdc && g_vdc) MarkDirty();
    return ok;
}
static BOOL WINAPI Hook_StretchBlt(HDC d, int x, int y, int w, int h, HDC s, int sx, int sy, int sw, int sh, DWORD rop)
{
    InterlockedIncrement(&g_blits);
    d = RedirectDC(d); s = RedirectDC(s);
    BOOL ok = g_realStretchBlt(d, x, y, w, h, s, sx, sy, sw, sh, rop);
    if (d == g_vdc && g_vdc && x == 0 && y == 0 && w == (int)g_w && h == (int)g_h) FrameLimit();
    if (d == g_vdc && g_vdc && w * h * 4 >= (int)(g_w * g_h)) PresentSoon();
    else if (d == g_vdc && g_vdc) MarkDirty();
    return ok;
}
static int WINAPI Hook_FillRect(HDC dc, const RECT* r, HBRUSH b)
{
    HDC d = RedirectDC(dc); if (d == g_vdc && g_vdc) MarkDirty();
    return g_real_FillRect(d, r, b);
}
static BOOL WINAPI Hook_TextOutA(HDC dc, int x, int y, LPCSTR s, int n)
{
    HDC d = RedirectDC(dc); if (d == g_vdc && g_vdc) MarkDirty();
    return g_real_TextOutA(d, x, y, s, n);
}
static int WINAPI Hook_SelectClipRgn(HDC dc, HRGN r)            { return g_real_SelectClipRgn(RedirectDC(dc), r); }
static HPALETTE WINAPI Hook_SelectPalette(HDC dc, HPALETTE p, BOOL bg) { return g_real_SelectPalette(RedirectDC(dc), p, bg); }
static UINT WINAPI Hook_RealizePalette(HDC dc)                  { return g_real_RealizePalette(RedirectDC(dc)); }
static UINT WINAPI Hook_GetSystemPaletteEntries(HDC dc, UINT a, UINT n, LPPALETTEENTRY e)
{
    return g_real_GetSystemPaletteEntries(RedirectDC(dc), a, n, e);
}

// --- coordinates and windows -----------------------------------------------------------------
static BOOL WINAPI Hook_ScreenToClient(HWND h, LPPOINT p)
{
    if (!(g_scaling && h == g_hwnd && p)) return g_real_ScreenToClient(h, p);
    POINT o = ClientOrigin();
    POINT r = { p->x - o.x, p->y - o.y };
    *p = RealToVirt(r);
    return TRUE;
}
static BOOL WINAPI Hook_ClientToScreen(HWND h, LPPOINT p)
{
    if (!(g_scaling && h == g_hwnd && p)) return g_real_ClientToScreen(h, p);
    POINT o = ClientOrigin();
    POINT r = VirtToReal(*p);                            // top-left of the scaled pixel
    p->x = o.x + r.x; p->y = o.y + r.y;
    return TRUE;
}
// The game window's rectangle as the game imagines it: 800x600 at the picture's screen position.
static BOOL WINAPI Hook_GetWindowRect(HWND h, LPRECT r)
{
    if (!(g_scaling && h == g_hwnd && r)) return g_real_GetWindowRect(h, r);
    POINT o = ClientOrigin();
    SetRect(r, o.x + g_offX, o.y + g_offY, o.x + g_offX + g_w, o.y + g_offY + g_h);
    return TRUE;
}
// The game moving/sizing windows:
//   its main window: the game still tries to put it at (0,0) 800x600 as in fullscreen; we keep
//   the position and size we (or the player) chose and only let z-order/show changes through;
//   child windows of the main window: positioned in game coordinates, so they are scaled.
static BOOL WINAPI Hook_MoveWindow(HWND h, int x, int y, int w, int hh, BOOL rp)
{
    if (g_scaling && h == g_hwnd) return TRUE;
    if (!IsGameChild(h)) return g_real_MoveWindow(h, x, y, w, hh, rp);
    POINT v = { x, y }, r = VirtToReal(v);
    return g_real_MoveWindow(h, r.x, r.y, ToRealX(x + w) - r.x, ToRealY(y + hh) - r.y, rp);
}
static BOOL WINAPI Hook_SetWindowPos(HWND h, HWND after, int x, int y, int w, int hh, UINT f)
{
    if (g_scaling && h == g_hwnd) return g_real_SetWindowPos(h, after, x, y, w, hh, f | SWP_NOMOVE | SWP_NOSIZE);
    if (!IsGameChild(h) || (f & SWP_NOMOVE && f & SWP_NOSIZE)) return g_real_SetWindowPos(h, after, x, y, w, hh, f);
    POINT v = { x, y }, r = VirtToReal(v);
    return g_real_SetWindowPos(h, after, r.x, r.y, ToRealX(x + w) - r.x, ToRealY(y + hh) - r.y, f);
}
// A window region on the (now monitor-sized) game window would cut the picture: ignore it.
static int WINAPI Hook_SetWindowRgn(HWND h, HRGN rgn, BOOL redraw)
{
    if (!(g_scaling && h == g_hwnd)) return g_real_SetWindowRgn(h, rgn, redraw);
    Log("SetWindowRgn on the game window ignored while scaling");
    if (rgn) DeleteObject(rgn);                          // SetWindowRgn would have owned it
    return 1;
}

// Replaces the import-address-table entry of `mod` that currently points at `target` (an export
// of `dll`) with `hook`. Windows has already resolved the exe's imports when our DllMain runs
// (ddraw.dll is one of the exe's static imports), so the IAT holds the real function addresses.
static void HookIat(HMODULE mod, const char* dll, FARPROC target, void* hook)
{
    BYTE* base = (BYTE*)mod;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR* d = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp((char*)(base + d->Name), dll)) continue;
        for (IMAGE_THUNK_DATA* t = (IMAGE_THUNK_DATA*)(base + d->FirstThunk); t->u1.Function; ++t)
            if ((FARPROC)t->u1.Function == target) {
                DWORD old;
                VirtualProtect(&t->u1.Function, sizeof(void*), PAGE_READWRITE, &old);
                t->u1.Function = (ULONG_PTR)hook;
                VirtualProtect(&t->u1.Function, sizeof(void*), old, &old);
                return;
            }
    }
    Log("NOTE: %s import %p not found in game exe", dll, target);
}

// Installs all IAT hooks. HOOK(dll, fn) stores the real address in g_real_fn and points the
// exe's import at Hook_fn.
static void InstallGameHooks()
{
    HMODULE exe = GetModuleHandleA(0);
    HMODULE u32 = GetModuleHandleA("user32.dll"), gdi = GetModuleHandleA("gdi32.dll");
    HMODULE wmm = GetModuleHandleA("winmm.dll");
#define HOOK(mod, dllname, fn) \
    g_real_##fn = (decltype(g_real_##fn))GetProcAddress(mod, #fn); \
    HookIat(exe, dllname, (FARPROC)g_real_##fn, (void*)Hook_##fn)
    HOOK(u32, "USER32.dll", GetSystemMetrics);
    HOOK(u32, "USER32.dll", GetDC);
    HOOK(u32, "USER32.dll", ReleaseDC);
    HOOK(u32, "USER32.dll", BeginPaint);
    HOOK(u32, "USER32.dll", EndPaint);
    HOOK(u32, "USER32.dll", ScreenToClient);
    HOOK(u32, "USER32.dll", ClientToScreen);
    HOOK(u32, "USER32.dll", GetWindowRect);
    HOOK(u32, "USER32.dll", MoveWindow);
    HOOK(u32, "USER32.dll", SetWindowPos);
    HOOK(u32, "USER32.dll", SetWindowRgn);
    HOOK(u32, "USER32.dll", FillRect);
    HOOK(gdi, "GDI32.dll", TextOutA);
    HOOK(gdi, "GDI32.dll", SelectClipRgn);
    HOOK(gdi, "GDI32.dll", SelectPalette);
    HOOK(gdi, "GDI32.dll", RealizePalette);
    HOOK(gdi, "GDI32.dll", GetSystemPaletteEntries);
    HOOK(u32, "USER32.dll", GetAsyncKeyState);
    HOOK(wmm, "WINMM.dll", timeGetTime);
#undef HOOK
    g_realBitBlt     = (decltype(g_realBitBlt))GetProcAddress(gdi, "BitBlt");
    g_realStretchBlt = (decltype(g_realStretchBlt))GetProcAddress(gdi, "StretchBlt");
    HookIat(exe, "GDI32.dll", (FARPROC)g_realBitBlt, (void*)Hook_BitBlt);
    HookIat(exe, "GDI32.dll", (FARPROC)g_realStretchBlt, (void*)Hook_StretchBlt);
    Log("game exe hooks installed");
}

// =====================================================================================
//  IDirectDraw vtable hooks
// =====================================================================================
// The game uses IDirectDraw (v1) only. IDirectDraw2 is handled as well in case the game (or a
// different build) asks for it via QueryInterface; its vtable has the same layout for the
// methods we patch, except SetDisplayMode, which takes two extra arguments.
//
// IDirectDraw vtable slots used here (see ddraw.h):
//    0 QueryInterface   12 GetDisplayMode   19 RestoreDisplayMode
//   20 SetCooperativeLevel                  21 SetDisplayMode
// The hooks are templates on the interface kind K only so that each kind keeps its own originals
// in g_orig[K]; the method code is identical.

enum { K_DD1, K_DD2, K_COUNT };
static void* g_orig[K_COUNT][24];   // original vtable entries, per interface kind
static void* g_patched[K_COUNT];    // vtable already patched for that kind

static void PatchDD(void* obj, int k);

// QueryInterface: pass through; make sure any IDirectDraw/IDirectDraw2 handed out is patched too.
template<int K> HRESULT WINAPI DD_QueryInterface(void* self, REFIID riid, void** out)
{
    HRESULT hr = ((HRESULT(WINAPI*)(void*, REFIID, void**))g_orig[K][0])(self, riid, out);
    if (SUCCEEDED(hr) && out && *out) {
        if (IsEqualGUID(riid, IID_IDirectDraw))       PatchDD(*out, K_DD1);
        else if (IsEqualGUID(riid, IID_IDirectDraw2)) PatchDD(*out, K_DD2);
        else if (IsEqualGUID(riid, IID_IDirectDraw4) || IsEqualGUID(riid, IID_IDirectDraw7))
            Log("WARNING: game asked for IDirectDraw4/7 - not emulated");
    }
    return hr;
}

// GetDisplayMode: real desktop mode from Windows, then overwritten with the emulated mode so the
// game sees what it asked for (800x600, 8-bit palettized).
template<int K> HRESULT WINAPI DD_GetDisplayMode(void* self, LPDDSURFACEDESC d)
{
    HRESULT hr = ((HRESULT(WINAPI*)(void*, LPDDSURFACEDESC))g_orig[K][12])(self, d);
    if (SUCCEEDED(hr) && d && g_modeSet) {
        d->dwWidth = g_w; d->dwHeight = g_h; d->lPitch = (LONG)(g_w * g_bpp / 8);
        d->ddpfPixelFormat.dwRGBBitCount = g_bpp;
        if (g_bpp == 8) {
            d->ddpfPixelFormat.dwFlags = DDPF_RGB | DDPF_PALETTEINDEXED8;
            d->ddpfPixelFormat.dwRBitMask = d->ddpfPixelFormat.dwGBitMask = d->ddpfPixelFormat.dwBBitMask = 0;
        }
    }
    return hr;
}

// RestoreDisplayMode: the monitor was never switched, so there is nothing to restore.
template<int K> HRESULT WINAPI DD_RestoreDisplayMode(void*)
{
    Log("RestoreDisplayMode -> ignored (monitor mode never changed)");
    g_modeSet = false;
    return DD_OK;
}

// SetCooperativeLevel: exclusive fullscreen becomes normal (windowed) cooperation. The first call
// with a window handle tells us which window is the game's; that is where we take it over.
template<int K> HRESULT WINAPI DD_SetCooperativeLevel(void* self, HWND w, DWORD f)
{
    DWORD nf = f;
    if (f & (DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN))
        nf = (f & ~(DDSCL_EXCLUSIVE | DDSCL_FULLSCREEN | DDSCL_ALLOWMODEX | DDSCL_ALLOWREBOOT)) | DDSCL_NORMAL;
    HRESULT hr = ((HRESULT(WINAPI*)(void*, HWND, DWORD))g_orig[K][20])(self, w, nf);
    Log("SetCooperativeLevel(hwnd=%p, 0x%lx) -> sent 0x%lx -> 0x%08lx", w, f, nf, hr);
    if (w && !g_hwnd) {
        g_hwnd = w;
        ApplyWindow("SetCooperativeLevel");
        if (g_watchdog) CloseHandle(CreateThread(0, 0, Watchdog, 0, 0, 0));
    }
    return hr;
}

// SetDisplayMode: remember the mode, lay the window out for it, report success. The real
// SetDisplayMode is never called, so the monitor keeps its resolution.
static HRESULT SetMode(DWORD w, DWORD h, DWORD bpp)
{
    g_w = w; g_h = h; g_bpp = bpp; g_modeSet = true;
    Log("SetDisplayMode(%lux%lux%lu) -> emulated, monitor unchanged", w, h, bpp);
    ApplyWindow("SetDisplayMode");
    return DD_OK;
}
// IDirectDraw::SetDisplayMode(w, h, bpp) and IDirectDraw2::SetDisplayMode(w, h, bpp, refresh, flags)
template<int K> HRESULT WINAPI DD_SetDisplayMode1(void*, DWORD w, DWORD h, DWORD bpp) { return SetMode(w, h, bpp); }
template<int K> HRESULT WINAPI DD_SetDisplayMode2(void*, DWORD w, DWORD h, DWORD bpp, DWORD, DWORD) { return SetMode(w, h, bpp); }

// Our replacement for vtable slot i of interface kind k, or NULL to keep the original.
static void* Hook(int k, int i)
{
    switch (i) {
    case 0:  return k == K_DD1 ? (void*)&DD_QueryInterface<K_DD1>      : (void*)&DD_QueryInterface<K_DD2>;
    case 12: return k == K_DD1 ? (void*)&DD_GetDisplayMode<K_DD1>      : (void*)&DD_GetDisplayMode<K_DD2>;
    case 19: return k == K_DD1 ? (void*)&DD_RestoreDisplayMode<K_DD1>  : (void*)&DD_RestoreDisplayMode<K_DD2>;
    case 20: return k == K_DD1 ? (void*)&DD_SetCooperativeLevel<K_DD1> : (void*)&DD_SetCooperativeLevel<K_DD2>;
    case 21: return k == K_DD1 ? (void*)&DD_SetDisplayMode1<K_DD1>     : (void*)&DD_SetDisplayMode2<K_DD2>;
    }
    return 0;
}

// Patches the vtable of DirectDraw object `obj` (kind k) once: saves all 24 original entries and
// replaces the hooked ones. The vtable lives in the system ddraw.dll's read-only data, hence
// VirtualProtect. Patching affects every object of that interface kind.
static void PatchDD(void* obj, int k)
{
    void** vt = *(void***)obj;
    EnterCriticalSection(&g_cs);
    if (!g_patched[k]) {
        g_patched[k] = vt;
        DWORD old;
        VirtualProtect(vt, 24 * sizeof(void*), PAGE_READWRITE, &old);
        for (int i = 0; i < 24; ++i) {
            g_orig[k][i] = vt[i];
            if (void* h = Hook(k, i)) vt[i] = h;
        }
        VirtualProtect(vt, 24 * sizeof(void*), old, &old);
        Log("patched IDirectDraw%s vtable %p", k == K_DD2 ? "2" : "", vt);
    } else if (g_patched[k] != vt) {
        Log("WARNING: second IDirectDraw%s vtable %p not patched", k == K_DD2 ? "2" : "", vt);
    }
    LeaveCriticalSection(&g_cs);
}

// =====================================================================================
//  Exports
// =====================================================================================
// Everything except DirectDrawCreate is forwarded unchanged to the system ddraw.dll. A forwarder
// is a naked function that jumps through g_fwd[i]: the caller's stack and registers reach the
// real function untouched, so no prototypes are needed. ddraw.def maps the export names to these
// Fwd_* functions (same names and ordinals as the Windows ddraw.dll).

static HMODULE g_real;   // C:\Windows\SysWOW64\ddraw.dll

// Order must match the FWD(i, ...) indices below.
static const char* kFwdNames[] = {
    "AcquireDDThreadLock", "CompleteCreateSysmemSurface", "D3DParseUnknownCommand",
    "DDGetAttachedSurfaceLcl", "DDInternalLock", "DDInternalUnlock", "DSoundHelp",
    "DirectDrawCreateClipper", "DirectDrawEnumerateA", "DirectDrawEnumerateExA",
    "DirectDrawEnumerateExW", "DirectDrawEnumerateW", "DllCanUnloadNow", "DllGetClassObject",
    "GetDDSurfaceLocal", "GetOLEThunkData", "GetSurfaceFromDC", "RegisterSpecialCase",
    "ReleaseDDThreadLock", "SetAppCompatData", "DirectDrawCreateEx" };
extern "C" FARPROC g_fwd[21] = {};   // filled by LoadReal()

#define FWD(i, name) extern "C" __declspec(naked) void Fwd_##name() { __asm { jmp dword ptr [g_fwd + i * 4] } }
FWD(0, AcquireDDThreadLock)      FWD(1, CompleteCreateSysmemSurface)  FWD(2, D3DParseUnknownCommand)
FWD(3, DDGetAttachedSurfaceLcl)  FWD(4, DDInternalLock)               FWD(5, DDInternalUnlock)
FWD(6, DSoundHelp)               FWD(7, DirectDrawCreateClipper)      FWD(8, DirectDrawEnumerateA)
FWD(9, DirectDrawEnumerateExA)   FWD(10, DirectDrawEnumerateExW)      FWD(11, DirectDrawEnumerateW)
FWD(12, DllCanUnloadNow)         FWD(13, DllGetClassObject)           FWD(14, GetDDSurfaceLocal)
FWD(15, GetOLEThunkData)         FWD(16, GetSurfaceFromDC)            FWD(17, RegisterSpecialCase)
FWD(18, ReleaseDDThreadLock)     FWD(19, SetAppCompatData)            FWD(20, DirectDrawCreateEx)

// Loads the system ddraw.dll by full path (a plain LoadLibrary("ddraw.dll") would find us again)
// and resolves all forwarded exports. The game is 32-bit, so on 64-bit Windows the right copy is
// in SysWOW64.
static bool LoadReal()
{
    if (g_real) return true;
    char sys[MAX_PATH] = {};
    if (!GetSystemWow64DirectoryA(sys, MAX_PATH)) GetSystemDirectoryA(sys, MAX_PATH);  // 32-bit Windows: System32
    strcat_s(sys, "\\ddraw.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) { Log("FATAL: cannot load %s (error %lu)", sys, GetLastError()); return false; }
    for (int i = 0; i < 21; ++i) g_fwd[i] = GetProcAddress(g_real, kFwdNames[i]);
    Log("real ddraw: %s", sys);
    return true;
}

// The one export the game actually uses. Creates the real DirectDraw object and patches its
// vtable (unless mode=fullscreen, in which case the game gets it untouched).
extern "C" HRESULT WINAPI Proxy_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* u)
{
    if (!LoadReal()) return DDERR_GENERIC;
    auto real = (HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*))GetProcAddress(g_real, "DirectDrawCreate");
    HRESULT hr = real(guid, out, u);
    Log("DirectDrawCreate -> 0x%08lx", hr);
    if (SUCCEEDED(hr) && out && *out && g_mode != MODE_FULLSCREEN) PatchDD(*out, K_DD1);
    return hr;
}

// =====================================================================================
//  Game fixes (all modes, including mode=fullscreen)
// =====================================================================================
// These make the *unmodified* Antietam.exe work on Windows 10/11, so nobody has to patch or
// distribute Firaxis' exe (docs/WINDOWS11_FIX.md, docs/exe-patch.md):
//
// 1. GlobalFree misuse. The game frees memory it got from its own C runtime (malloc/new) with
//    the Win32 GlobalFree(). Windows 9x tolerated it; Windows 10/11 corrupts the heap and the
//    game crashes ~30 s in. The exe never imports GlobalAlloc, so every GlobalFree call is such a
//    mismatch. All 65 (v9.84) / 68 (v12.10) call sites go through the GlobalFree import, so we
//    point that import at Hook_GlobalFree, which calls the game's own CRT free() instead - the
//    same effect as the old exe patch, done at load time. free() is found by its MSVC6 byte
//    signature in the exe's code (exactly one match in every build checked).
// 2. The EmulateHeap shim (AcGenral.dll) that Windows applies to programs named "Antietam.exe"
//    crashes while sounds load. It is matched by file name, so the game must run under another
//    name; install.bat creates a renamed copy. If we see the shim loaded we log a warning.

static void (__cdecl* g_crtFree)(void*);     // the game's statically linked CRT free()

static HGLOBAL WINAPI Hook_GlobalFree(HGLOBAL p)
{
    if (p) g_crtFree(p);
    return 0;                                    // GlobalFree returns NULL on success
}

// Finds the game's CRT free() by its signature inside the exe's .text section:
//   56 8B 74 24 08 85 F6 74 ?? 6A 09 E8   push esi; mov esi,[esp+8]; test esi,esi; je ..; push 9; call _lock
static void* FindCrtFree(HMODULE exe)
{
    BYTE* base = (BYTE*)exe;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    BYTE* code = base + sec->VirtualAddress;     // first section = .text
    DWORD size = sec->Misc.VirtualSize;
    static const BYTE sig[] = { 0x56, 0x8B, 0x74, 0x24, 0x08, 0x85, 0xF6, 0x74, 0x00, 0x6A, 0x09, 0xE8 };
    void* found = 0; int hits = 0;
    for (DWORD i = 0; i + sizeof sig <= size; ++i) {
        bool ok = true;
        for (DWORD j = 0; j < sizeof sig && ok; ++j)
            if (j != 8 && code[i + j] != sig[j]) ok = false;     // byte 8 = jump distance, varies
        if (ok) { found = code + i; ++hits; }
    }
    return hits == 1 ? found : 0;
}

// Logs which game build this is (PE timestamp + file size) - useful in bug reports.
static void IdentifyGame(HMODULE exe)
{
    BYTE* base = (BYTE*)exe;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    DWORD ts = nt->FileHeader.TimeDateStamp;
    char path[MAX_PATH]; GetModuleFileNameA(exe, path, MAX_PATH);
    LARGE_INTEGER size = {};
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, 0, OPEN_EXISTING, 0, 0);
    if (f != INVALID_HANDLE_VALUE) { GetFileSizeEx(f, &size); CloseHandle(f); }
    const char* build = ts == 0x38A45BF6 ? "v9.84 / v2.0 Beta Patch (Feb 2000)"
                      : ts == 0x398EF5B0 ? "v12.10 / v3.0 Final Patch w/ South Mountain, Civil War Collection (Aug 2000)"
                      : "unknown build - please report";
    Log("game: %s, %lld bytes, timestamp 0x%08lX = %s", path, size.QuadPart, ts, build);
}

// Campaigns (map sets): 1 = Antietam, 3 = South Mountain, 20-99 = player-made battle packs.
// We deliberately leave them alone. v12.10 takes "-campaign N" on its command line (it only knows
// 1 and 3 and opens South Mountain for anything else). v9.84 has no switch and reads "Campaign=N"
// from antietam.ini, which is what battle packs set when JSGME installs them. An earlier version
// of this DLL copied -campaign into antietam.ini; that overwrote the pack's own setting, so it
// was removed (tested 2026-10-01 with the Austerlitz pack).

static void ApplyGameFixes()
{
    HMODULE exe = GetModuleHandleA(0);
    IdentifyGame(exe);

    if (GetModuleHandleA("AcGenral.dll"))
        Log("WARNING: Windows' compatibility shim (AcGenral.dll) is active - it crashes this game. "
            "Start the renamed copy made by install.bat instead of Antietam.exe.");

    g_crtFree = (void(__cdecl*)(void*))FindCrtFree(exe);
    if (!g_crtFree) {
        Log("WARNING: game's free() not found - GlobalFree fix NOT applied (unknown build?)");
        return;
    }
    FARPROC gf = GetProcAddress(GetModuleHandleA("kernel32.dll"), "GlobalFree");
    HookIat(exe, "KERNEL32.dll", gf, (void*)Hook_GlobalFree);
    Log("GlobalFree fix applied: GlobalFree -> game's free() at %p", g_crtFree);
}

// Makes the process per-monitor DPI aware. The game declares nothing, so on a display set to
// 125-200% Windows would stretch our window once more (blurring the "sharp 2x" picture) and
// report a scaled-down screen size. Must happen before the game creates its window - DllMain of
// ddraw.dll runs before the exe's own code, so it does. Off in mode=fullscreen (original path).
static void MakeDpiAware()
{
    typedef BOOL (WINAPI* SetCtxFn)(HANDLE);
    SetCtxFn setCtx = (SetCtxFn)GetProcAddress(GetModuleHandleA("user32.dll"), "SetProcessDpiAwarenessContext");
    bool ok = setCtx && setCtx((HANDLE)-4);      // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2
    if (!ok) ok = SetProcessDPIAware() != 0;     // older Windows 10: system-DPI aware
    Log("DPI awareness: %s", ok ? "per-monitor" : "not set");
}

// =====================================================================================
//  DLL entry point
// =====================================================================================

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);

        // Settings and log live next to this DLL (the game folder).
        GetModuleFileNameA(h, g_dir, MAX_PATH);
        char* s = strrchr(g_dir, '\\'); if (s) *s = 0;
        LoadConfig();
        if (g_logOn) {
            char p[MAX_PATH]; sprintf_s(p, "%s\\smav2_ddraw.log", g_dir);
            g_log = _fsopen(p, "w", _SH_DENYWR);         // others may read it while the game runs
        }
        Log("smav2 ddraw loaded, mode=%s scale=%d (0=auto) window_size=%dx%d filter=%s clip_cursor=%d keep_focus=%d",
            g_mode == MODE_FULLSCREEN ? "fullscreen" : g_mode == MODE_BORDERLESS ? "borderless" : g_mode == MODE_EXCLUSIVE ? "exclusive" : "windowed",
            g_scaleCfg, g_winW, g_winH, g_filterCfg == FILTER_NEAREST ? "nearest" : g_filterCfg == FILTER_LINEAR ? "linear" : g_filterCfg == FILTER_SHARP ? "sharp" : g_filterCfg == FILTER_SCALE2X ? "scale2x" : "auto", g_clipCursor, g_keepFocus);
        Log("renderer requested: %s, vsync=%d, swap=%s", g_wantD3D ? "d3d11" : "gdi", g_vsync, g_flipModel ? "flip" : "blt");
        Log("max_fps=%d (0 = unlimited)", g_maxFps);

        LoadReal();
        ApplyGameFixes();                                // crash fixes, in every mode

        if (g_mode != MODE_FULLSCREEN) {
            if (g_dpiAware) MakeDpiAware();
            // No "Not Responding" ghost window. The game reads mouse and keyboard directly
            // (GetAsyncKeyState/GetCursorPos) and its PeekMessage loops skip the input-message
            // ranges, so input messages pile up unread. After 5 s Windows declares the window hung
            // and DWM covers it with a frozen "ghost" copy, although the game keeps running
            // underneath. Exclusive fullscreen never gets ghosted, so this only shows windowed.
            // (Diagnosed 2026-10-01, see docs/mode-switch.md.)
            DisableProcessWindowsGhosting();
            InstallGameHooks();
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        ClipCursor(0);                                   // never leave the mouse trapped
        Log("unloaded");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
