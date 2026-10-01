// =====================================================================================
//  Logging proxy ddraw.dll for Sid Meier's Antietam! (32-bit)  -  research tool
// =====================================================================================
// Purpose: find out exactly which DirectDraw calls the game makes, before writing our own
// ddraw.dll. Result: docs/ddraw-trace.md (the game only uses DirectDraw for the mode switch).
//
// How it works
//   * The game loads ddraw.dll by name, so this DLL (copied next to the exe) is loaded instead
//     of the system one. It loads the real C:\Windows\SysWOW64\ddraw.dll and forwards all
//     exports (naked jmp stubs, FWD macro). DirectDrawCreate/DirectDrawCreateEx are wrapped.
//   * Every DirectDraw object handed to the game (IDirectDraw 1/2/4/7, IDirectDrawSurface
//     1/2/3/4/7, IDirectDrawPalette, IDirectDrawClipper) is instrumented by patching its
//     vtable (PatchObject):
//       - interesting methods get a typed hook that logs the arguments of the first
//         kDetailCalls calls (display mode, cooperative level, CreateSurface/Palette/Clipper,
//         QueryInterface, Blt, BltFast, Flip, Lock, GetDC, SetColorKey, SetPalette,
//         GetAttachedSurface, Palette::SetEntries, Clipper::SetHWnd)
//       - every other method gets a 13-byte machine-code thunk that only counts the call and
//         jumps to the original (MakeThunk), so no prototypes are needed for those
//     Objects returned by QueryInterface/CreateSurface/... are instrumented as they appear.
//   * A background thread rewrites a per-method call-count summary every 2 s.
// Output (next to the exe):
//   ddraw_proxy.log           detailed log
//   ddraw_proxy_summary.txt   call counts per interface/method, most frequent first
// Usage: build.bat, copy build\ddraw.dll next to the game exe, play, then remove it.
// =====================================================================================
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <ddraw.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <share.h>

// ---------------------------------------------------------------- logging
static CRITICAL_SECTION g_cs;
static FILE* g_log;
static char g_dir[MAX_PATH];
static const LONG kDetailCalls = 40;   // per method, log arguments for the first N calls

static void Log(const char* fmt, ...)
{
    if (!g_log) return;
    EnterCriticalSection(&g_cs);
    fprintf(g_log, "[%8lu] ", GetTickCount());
    va_list ap; va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap);
    fputc('\n', g_log); fflush(g_log);
    LeaveCriticalSection(&g_cs);
}

// ---------------------------------------------------------------- interface kinds
enum Kind { K_DD1, K_DD2, K_DD4, K_DD7, K_S1, K_S2, K_S3, K_S4, K_S7, K_PAL, K_CLIP, K_COUNT };
static const int   kSlots[K_COUNT] = { 23, 24, 28, 30, 36, 39, 40, 45, 49, 7, 9 };
static const char* kKindName[K_COUNT] = { "IDirectDraw", "IDirectDraw2", "IDirectDraw4", "IDirectDraw7",
    "IDirectDrawSurface", "IDirectDrawSurface2", "IDirectDrawSurface3", "IDirectDrawSurface4",
    "IDirectDrawSurface7", "IDirectDrawPalette", "IDirectDrawClipper" };

static const char* kDDNames[] = { "QueryInterface", "AddRef", "Release", "Compact", "CreateClipper",
    "CreatePalette", "CreateSurface", "DuplicateSurface", "EnumDisplayModes", "EnumSurfaces",
    "FlipToGDISurface", "GetCaps", "GetDisplayMode", "GetFourCCCodes", "GetGDISurface",
    "GetMonitorFrequency", "GetScanLine", "GetVerticalBlankStatus", "Initialize", "RestoreDisplayMode",
    "SetCooperativeLevel", "SetDisplayMode", "WaitForVerticalBlank", "GetAvailableVidMem",
    "GetSurfaceFromDC", "RestoreAllSurfaces", "TestCooperativeLevel", "GetDeviceIdentifier",
    "StartModeTest", "EvaluateMode" };
static const char* kSurfNames[] = { "QueryInterface", "AddRef", "Release", "AddAttachedSurface",
    "AddOverlayDirtyRect", "Blt", "BltBatch", "BltFast", "DeleteAttachedSurface", "EnumAttachedSurfaces",
    "EnumOverlayZOrders", "Flip", "GetAttachedSurface", "GetBltStatus", "GetCaps", "GetClipper",
    "GetColorKey", "GetDC", "GetFlipStatus", "GetOverlayPosition", "GetPalette", "GetPixelFormat",
    "GetSurfaceDesc", "Initialize", "IsLost", "Lock", "ReleaseDC", "Restore", "SetClipper", "SetColorKey",
    "SetOverlayPosition", "SetPalette", "Unlock", "UpdateOverlay", "UpdateOverlayDisplay",
    "UpdateOverlayZOrder", "GetDDInterface", "PageLock", "PageUnlock", "SetSurfaceDesc", "SetPrivateData",
    "GetPrivateData", "FreePrivateData", "GetUniquenessValue", "ChangeUniquenessValue", "SetPriority",
    "GetPriority", "SetLOD", "GetLOD" };
static const char* kPalNames[] = { "QueryInterface", "AddRef", "Release", "GetCaps", "GetEntries",
    "Initialize", "SetEntries" };
static const char* kClipNames[] = { "QueryInterface", "AddRef", "Release", "GetClipList", "GetHWnd",
    "Initialize", "IsClipListChanged", "SetClipList", "SetHWnd" };

static const char* MethodName(int k, int i)
{
    if (k <= K_DD7) return kDDNames[i];
    if (k <= K_S7)  return kSurfNames[i];
    if (k == K_PAL) return kPalNames[i];
    return kClipNames[i];
}

struct Slot { volatile LONG count; void* orig; };
static Slot g_slots[K_COUNT][64];

static LONG Hit(int k, int i) { return InterlockedIncrement(&g_slots[k][i].count); }
#define ORIG(T, k, i) ((T)g_slots[k][i].orig)

// ---------------------------------------------------------------- flag decoding
struct FlagName { DWORD v; const char* n; };
static void Flags(char* out, size_t cap, DWORD f, const FlagName* t)
{
    out[0] = 0;
    for (; t->n; ++t)
        if (f & t->v) { strncat_s(out, cap, t->n, _TRUNCATE); strncat_s(out, cap, "|", _TRUNCATE); f &= ~t->v; }
    char rest[16];
    if (f) { sprintf_s(rest, "0x%lx", f); strncat_s(out, cap, rest, _TRUNCATE); }
    else if (out[0]) out[strlen(out) - 1] = 0;
    else strcpy_s(out, cap, "0");
}
static const FlagName kCoop[] = { {DDSCL_FULLSCREEN,"FULLSCREEN"}, {DDSCL_ALLOWREBOOT,"ALLOWREBOOT"},
    {DDSCL_NOWINDOWCHANGES,"NOWINDOWCHANGES"}, {DDSCL_NORMAL,"NORMAL"}, {DDSCL_EXCLUSIVE,"EXCLUSIVE"},
    {DDSCL_ALLOWMODEX,"ALLOWMODEX"}, {DDSCL_SETFOCUSWINDOW,"SETFOCUSWINDOW"},
    {DDSCL_SETDEVICEWINDOW,"SETDEVICEWINDOW"}, {DDSCL_CREATEDEVICEWINDOW,"CREATEDEVICEWINDOW"},
    {DDSCL_MULTITHREADED,"MULTITHREADED"}, {DDSCL_FPUSETUP,"FPUSETUP"}, {DDSCL_FPUPRESERVE,"FPUPRESERVE"}, {0,0} };
static const FlagName kCaps[] = { {DDSCAPS_PRIMARYSURFACE,"PRIMARY"}, {DDSCAPS_BACKBUFFER,"BACKBUFFER"},
    {DDSCAPS_FRONTBUFFER,"FRONTBUFFER"}, {DDSCAPS_COMPLEX,"COMPLEX"}, {DDSCAPS_FLIP,"FLIP"},
    {DDSCAPS_OFFSCREENPLAIN,"OFFSCREENPLAIN"}, {DDSCAPS_SYSTEMMEMORY,"SYSTEMMEMORY"},
    {DDSCAPS_VIDEOMEMORY,"VIDEOMEMORY"}, {DDSCAPS_LOCALVIDMEM,"LOCALVIDMEM"}, {DDSCAPS_3DDEVICE,"3DDEVICE"},
    {DDSCAPS_TEXTURE,"TEXTURE"}, {DDSCAPS_PALETTE,"PALETTE"}, {DDSCAPS_OVERLAY,"OVERLAY"},
    {DDSCAPS_MODEX,"MODEX"}, {DDSCAPS_OWNDC,"OWNDC"}, {DDSCAPS_ZBUFFER,"ZBUFFER"}, {0,0} };
static const FlagName kSD[] = { {DDSD_CAPS,"CAPS"}, {DDSD_HEIGHT,"HEIGHT"}, {DDSD_WIDTH,"WIDTH"},
    {DDSD_PITCH,"PITCH"}, {DDSD_BACKBUFFERCOUNT,"BACKBUFFERCOUNT"}, {DDSD_CKSRCBLT,"CKSRCBLT"},
    {DDSD_CKDESTBLT,"CKDESTBLT"}, {DDSD_PIXELFORMAT,"PIXELFORMAT"}, {DDSD_LPSURFACE,"LPSURFACE"},
    {DDSD_REFRESHRATE,"REFRESHRATE"}, {0,0} };
static const FlagName kBlt[] = { {DDBLT_COLORFILL,"COLORFILL"}, {DDBLT_KEYSRC,"KEYSRC"},
    {DDBLT_KEYSRCOVERRIDE,"KEYSRCOVERRIDE"}, {DDBLT_KEYDEST,"KEYDEST"}, {DDBLT_WAIT,"WAIT"},
    {DDBLT_ASYNC,"ASYNC"}, {DDBLT_DDFX,"DDFX"}, {DDBLT_ROP,"ROP"}, {DDBLT_DONOTWAIT,"DONOTWAIT"}, {0,0} };
static const FlagName kPal[] = { {DDPCAPS_4BIT,"4BIT"}, {DDPCAPS_8BIT,"8BIT"}, {DDPCAPS_8BITENTRIES,"8BITENTRIES"},
    {DDPCAPS_ALLOW256,"ALLOW256"}, {DDPCAPS_PRIMARYSURFACE,"PRIMARYSURFACE"}, {DDPCAPS_INITIALIZE,"INITIALIZE"},
    {DDPCAPS_VSYNC,"VSYNC"}, {0,0} };

static void RectStr(char* out, size_t cap, const RECT* r)
{
    if (r) sprintf_s(out, cap, "(%ld,%ld)-(%ld,%ld)", r->left, r->top, r->right, r->bottom);
    else strcpy_s(out, cap, "NULL");
}

// DDSURFACEDESC and DDSURFACEDESC2 share the layout of every field read here.
static void DescStr(char* out, size_t cap, const void* p)
{
    const DDSURFACEDESC* d = (const DDSURFACEDESC*)p;
    if (!d) { strcpy_s(out, cap, "NULL"); return; }
    char f[256], c[256];
    Flags(f, sizeof f, d->dwFlags, kSD);
    Flags(c, sizeof c, d->ddsCaps.dwCaps, kCaps);
    int n = sprintf_s(out, cap, "size=%lu flags=%s %lux%lu pitch=%ld backbuffers=%lu caps=%s",
        d->dwSize, f, d->dwWidth, d->dwHeight, d->lPitch, d->dwBackBufferCount, c);
    if (n > 0 && (d->dwFlags & DDSD_PIXELFORMAT))
        sprintf_s(out + n, cap - n, " pf[flags=0x%lx bpp=%lu R=%08lx G=%08lx B=%08lx]",
            d->ddpfPixelFormat.dwFlags, d->ddpfPixelFormat.dwRGBBitCount, d->ddpfPixelFormat.dwRBitMask,
            d->ddpfPixelFormat.dwGBitMask, d->ddpfPixelFormat.dwBBitMask);
}

static const char* IidName(REFIID r)
{
    struct { const GUID* g; const char* n; } t[] = {
        {&IID_IUnknown,"IUnknown"}, {&IID_IDirectDraw,"IDirectDraw"}, {&IID_IDirectDraw2,"IDirectDraw2"},
        {&IID_IDirectDraw4,"IDirectDraw4"}, {&IID_IDirectDraw7,"IDirectDraw7"},
        {&IID_IDirectDrawSurface,"IDirectDrawSurface"}, {&IID_IDirectDrawSurface2,"IDirectDrawSurface2"},
        {&IID_IDirectDrawSurface3,"IDirectDrawSurface3"}, {&IID_IDirectDrawSurface4,"IDirectDrawSurface4"},
        {&IID_IDirectDrawSurface7,"IDirectDrawSurface7"}, {&IID_IDirectDrawPalette,"IDirectDrawPalette"},
        {&IID_IDirectDrawClipper,"IDirectDrawClipper"} };
    for (auto& e : t) if (IsEqualGUID(r, *e.g)) return e.n;
    static char buf[64];
    sprintf_s(buf, "{%08lX-%04X-%04X-...}", r.Data1, r.Data2, r.Data3);
    return buf;
}

static int KindForIid(REFIID r)
{
    if (IsEqualGUID(r, IID_IDirectDraw))  return K_DD1;
    if (IsEqualGUID(r, IID_IDirectDraw2)) return K_DD2;
    if (IsEqualGUID(r, IID_IDirectDraw4)) return K_DD4;
    if (IsEqualGUID(r, IID_IDirectDraw7)) return K_DD7;
    if (IsEqualGUID(r, IID_IDirectDrawSurface))  return K_S1;
    if (IsEqualGUID(r, IID_IDirectDrawSurface2)) return K_S2;
    if (IsEqualGUID(r, IID_IDirectDrawSurface3)) return K_S3;
    if (IsEqualGUID(r, IID_IDirectDrawSurface4)) return K_S4;
    if (IsEqualGUID(r, IID_IDirectDrawSurface7)) return K_S7;
    if (IsEqualGUID(r, IID_IDirectDrawPalette))  return K_PAL;
    if (IsEqualGUID(r, IID_IDirectDrawClipper))  return K_CLIP;
    return -1;
}

static void PatchObject(void* obj, int k);

// ---------------------------------------------------------------- IDirectDraw* hooks
template<int K> HRESULT WINAPI DD_QueryInterface(void* self, REFIID riid, void** out)
{
    Hit(K, 0);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, REFIID, void**), K, 0)(self, riid, out);
    Log("%s::QueryInterface(%s) -> 0x%08lx obj=%p", kKindName[K], IidName(riid), hr, out ? *out : 0);
    if (SUCCEEDED(hr) && out) PatchObject(*out, KindForIid(riid));
    return hr;
}
template<int K> HRESULT WINAPI DD_CreateClipper(void* self, DWORD f, LPDIRECTDRAWCLIPPER* out, IUnknown* u)
{
    Hit(K, 4);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, DWORD, LPDIRECTDRAWCLIPPER*, IUnknown*), K, 4)(self, f, out, u);
    Log("%s::CreateClipper(0x%lx) -> 0x%08lx clipper=%p", kKindName[K], f, hr, out ? *out : 0);
    if (SUCCEEDED(hr) && out) PatchObject(*out, K_CLIP);
    return hr;
}
template<int K> HRESULT WINAPI DD_CreatePalette(void* self, DWORD f, LPPALETTEENTRY e, LPDIRECTDRAWPALETTE* out, IUnknown* u)
{
    Hit(K, 5);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, DWORD, LPPALETTEENTRY, LPDIRECTDRAWPALETTE*, IUnknown*), K, 5)(self, f, e, out, u);
    char fs[128]; Flags(fs, sizeof fs, f, kPal);
    Log("%s::CreatePalette(%s) -> 0x%08lx palette=%p", kKindName[K], fs, hr, out ? *out : 0);
    if (SUCCEEDED(hr) && out) PatchObject(*out, K_PAL);
    return hr;
}
template<int K> HRESULT WINAPI DD_CreateSurface(void* self, void* desc, void** out, IUnknown* u)
{
    Hit(K, 6);
    char ds[512]; DescStr(ds, sizeof ds, desc);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, void*, void**, IUnknown*), K, 6)(self, desc, out, u);
    Log("%s::CreateSurface(%s) -> 0x%08lx surface=%p", kKindName[K], ds, hr, out ? *out : 0);
    if (SUCCEEDED(hr) && out)
        PatchObject(*out, K == K_DD7 ? K_S7 : K == K_DD4 ? K_S4 : K_S1);
    return hr;
}
template<int K> HRESULT WINAPI DD_GetDisplayMode(void* self, void* desc)
{
    LONG n = Hit(K, 12);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, void*), K, 12)(self, desc);
    if (n <= kDetailCalls) { char ds[512]; DescStr(ds, sizeof ds, desc); Log("%s::GetDisplayMode -> 0x%08lx %s", kKindName[K], hr, ds); }
    return hr;
}
template<int K> HRESULT WINAPI DD_SetCooperativeLevel(void* self, HWND w, DWORD f)
{
    Hit(K, 20);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, HWND, DWORD), K, 20)(self, w, f);
    char fs[256]; Flags(fs, sizeof fs, f, kCoop);
    Log("%s::SetCooperativeLevel(hwnd=%p, %s) -> 0x%08lx", kKindName[K], w, fs, hr);
    return hr;
}
template<int K> HRESULT WINAPI DD_SetDisplayMode1(void* self, DWORD w, DWORD h, DWORD bpp)
{
    Hit(K, 21);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD), K, 21)(self, w, h, bpp);
    Log("%s::SetDisplayMode(%lux%lux%lu) -> 0x%08lx", kKindName[K], w, h, bpp, hr);
    return hr;
}
template<int K> HRESULT WINAPI DD_SetDisplayMode2(void* self, DWORD w, DWORD h, DWORD bpp, DWORD hz, DWORD f)
{
    Hit(K, 21);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD, DWORD, DWORD), K, 21)(self, w, h, bpp, hz, f);
    Log("%s::SetDisplayMode(%lux%lux%lu @%luHz flags=0x%lx) -> 0x%08lx", kKindName[K], w, h, bpp, hz, f, hr);
    return hr;
}
template<int K> void* DDHook(int i)
{
    switch (i) {
    case 0:  return (void*)&DD_QueryInterface<K>;
    case 4:  return (void*)&DD_CreateClipper<K>;
    case 5:  return (void*)&DD_CreatePalette<K>;
    case 6:  return (void*)&DD_CreateSurface<K>;
    case 12: return (void*)&DD_GetDisplayMode<K>;
    case 20: return (void*)&DD_SetCooperativeLevel<K>;
    case 21: return K == K_DD1 ? (void*)&DD_SetDisplayMode1<K> : (void*)&DD_SetDisplayMode2<K>;
    }
    return 0;
}

// ---------------------------------------------------------------- IDirectDrawSurface* hooks
template<int K> HRESULT WINAPI S_QueryInterface(void* self, REFIID riid, void** out)
{
    Hit(K, 0);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, REFIID, void**), K, 0)(self, riid, out);
    Log("%s(%p)::QueryInterface(%s) -> 0x%08lx obj=%p", kKindName[K], self, IidName(riid), hr, out ? *out : 0);
    if (SUCCEEDED(hr) && out) PatchObject(*out, KindForIid(riid));
    return hr;
}
template<int K> HRESULT WINAPI S_Blt(void* self, LPRECT dr, void* src, LPRECT sr, DWORD f, LPDDBLTFX fx)
{
    LONG n = Hit(K, 5);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, LPRECT, void*, LPRECT, DWORD, LPDDBLTFX), K, 5)(self, dr, src, sr, f, fx);
    if (n <= kDetailCalls) {
        char a[64], b[64], fs[128]; RectStr(a, sizeof a, dr); RectStr(b, sizeof b, sr); Flags(fs, sizeof fs, f, kBlt);
        Log("%s(%p)::Blt(dst=%s src=%p %s flags=%s fill=0x%lx) -> 0x%08lx", kKindName[K], self, a, src, b, fs,
            (fx && (f & DDBLT_COLORFILL)) ? fx->dwFillColor : 0, hr);
    }
    return hr;
}
template<int K> HRESULT WINAPI S_BltFast(void* self, DWORD x, DWORD y, void* src, LPRECT sr, DWORD t)
{
    LONG n = Hit(K, 7);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, DWORD, DWORD, void*, LPRECT, DWORD), K, 7)(self, x, y, src, sr, t);
    if (n <= kDetailCalls) { char b[64]; RectStr(b, sizeof b, sr);
        Log("%s(%p)::BltFast(%lu,%lu src=%p %s trans=0x%lx) -> 0x%08lx", kKindName[K], self, x, y, src, b, t, hr); }
    return hr;
}
template<int K> HRESULT WINAPI S_Flip(void* self, void* target, DWORD f)
{
    LONG n = Hit(K, 11);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, void*, DWORD), K, 11)(self, target, f);
    if (n <= kDetailCalls) Log("%s(%p)::Flip(target=%p flags=0x%lx) -> 0x%08lx", kKindName[K], self, target, f, hr);
    return hr;
}
template<int K> HRESULT WINAPI S_GetAttachedSurface(void* self, LPDDSCAPS caps, void** out)
{
    Hit(K, 12);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, LPDDSCAPS, void**), K, 12)(self, caps, out);
    char c[256]; Flags(c, sizeof c, caps ? caps->dwCaps : 0, kCaps);
    Log("%s(%p)::GetAttachedSurface(%s) -> 0x%08lx surface=%p", kKindName[K], self, c, hr, out ? *out : 0);
    if (SUCCEEDED(hr) && out) PatchObject(*out, K);
    return hr;
}
template<int K> HRESULT WINAPI S_GetDC(void* self, HDC* dc)
{
    LONG n = Hit(K, 17);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, HDC*), K, 17)(self, dc);
    if (n <= kDetailCalls) Log("%s(%p)::GetDC -> 0x%08lx", kKindName[K], self, hr);
    return hr;
}
template<int K> HRESULT WINAPI S_Lock(void* self, LPRECT r, void* desc, DWORD f, HANDLE h)
{
    LONG n = Hit(K, 25);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, LPRECT, void*, DWORD, HANDLE), K, 25)(self, r, desc, f, h);
    if (n <= kDetailCalls) { char a[64], ds[512]; RectStr(a, sizeof a, r); DescStr(ds, sizeof ds, SUCCEEDED(hr) ? desc : 0);
        Log("%s(%p)::Lock(%s flags=0x%lx) -> 0x%08lx %s", kKindName[K], self, a, f, hr, ds); }
    return hr;
}
template<int K> HRESULT WINAPI S_SetColorKey(void* self, DWORD f, LPDDCOLORKEY ck)
{
    LONG n = Hit(K, 29);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, DWORD, LPDDCOLORKEY), K, 29)(self, f, ck);
    if (n <= kDetailCalls) Log("%s(%p)::SetColorKey(flags=0x%lx key=%lu..%lu) -> 0x%08lx", kKindName[K], self, f,
        ck ? ck->dwColorSpaceLowValue : 0, ck ? ck->dwColorSpaceHighValue : 0, hr);
    return hr;
}
template<int K> HRESULT WINAPI S_SetPalette(void* self, void* pal)
{
    LONG n = Hit(K, 31);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, void*), K, 31)(self, pal);
    if (n <= kDetailCalls) Log("%s(%p)::SetPalette(%p) -> 0x%08lx", kKindName[K], self, pal, hr);
    return hr;
}
template<int K> void* SurfHook(int i)
{
    switch (i) {
    case 0:  return (void*)&S_QueryInterface<K>;
    case 5:  return (void*)&S_Blt<K>;
    case 7:  return (void*)&S_BltFast<K>;
    case 11: return (void*)&S_Flip<K>;
    case 12: return (void*)&S_GetAttachedSurface<K>;
    case 17: return (void*)&S_GetDC<K>;
    case 25: return (void*)&S_Lock<K>;
    case 29: return (void*)&S_SetColorKey<K>;
    case 31: return (void*)&S_SetPalette<K>;
    }
    return 0;
}

// ---------------------------------------------------------------- palette / clipper hooks
static HRESULT WINAPI P_SetEntries(void* self, DWORD f, DWORD start, DWORD count, LPPALETTEENTRY e)
{
    LONG n = Hit(K_PAL, 6);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, DWORD, DWORD, DWORD, LPPALETTEENTRY), K_PAL, 6)(self, f, start, count, e);
    if (n <= kDetailCalls) Log("IDirectDrawPalette(%p)::SetEntries(start=%lu count=%lu) -> 0x%08lx", self, start, count, hr);
    return hr;
}
static HRESULT WINAPI C_SetHWnd(void* self, DWORD f, HWND w)
{
    Hit(K_CLIP, 8);
    HRESULT hr = ORIG(HRESULT(WINAPI*)(void*, DWORD, HWND), K_CLIP, 8)(self, f, w);
    Log("IDirectDrawClipper(%p)::SetHWnd(%p) -> 0x%08lx", self, w, hr);
    return hr;
}

static void* TypedHook(int k, int i)
{
    switch (k) {
    case K_DD1: return DDHook<K_DD1>(i);
    case K_DD2: return DDHook<K_DD2>(i);
    case K_DD4: return DDHook<K_DD4>(i);
    case K_DD7: return DDHook<K_DD7>(i);
    case K_S1:  return SurfHook<K_S1>(i);
    case K_S2:  return SurfHook<K_S2>(i);
    case K_S3:  return SurfHook<K_S3>(i);
    case K_S4:  return SurfHook<K_S4>(i);
    case K_S7:  return SurfHook<K_S7>(i);
    case K_PAL: return i == 6 ? (void*)&P_SetEntries : 0;
    case K_CLIP: return i == 8 ? (void*)&C_SetHWnd : 0;
    }
    return 0;
}

// ---------------------------------------------------------------- vtable patching
// Methods without a typed hook get a 13-byte thunk:  lock inc [count] ; jmp [orig]
static BYTE* g_thunks; static int g_thunkUsed;
static void* MakeThunk(Slot* s)
{
    BYTE* t = g_thunks + g_thunkUsed; g_thunkUsed += 16;
    t[0] = 0xF0; t[1] = 0xFF; t[2] = 0x05; *(void**)(t + 3) = (void*)&s->count;
    t[7] = 0xFF; t[8] = 0x25; *(void**)(t + 9) = (void*)&s->orig;
    return t;
}

static void* g_vtbls[64]; static int g_nVtbls;
static void PatchObject(void* obj, int k)
{
    if (!obj || k < 0) return;
    void** vt = *(void***)obj;
    EnterCriticalSection(&g_cs);
    for (int i = 0; i < g_nVtbls; ++i) if (g_vtbls[i] == vt) { LeaveCriticalSection(&g_cs); return; }
    if (g_nVtbls < 64 && g_slots[k][0].orig == 0) {
        g_vtbls[g_nVtbls++] = vt;
        DWORD old;
        VirtualProtect(vt, kSlots[k] * sizeof(void*), PAGE_READWRITE, &old);
        for (int i = 0; i < kSlots[k]; ++i) {
            g_slots[k][i].orig = vt[i];
            void* h = TypedHook(k, i);
            vt[i] = h ? h : MakeThunk(&g_slots[k][i]);
        }
        VirtualProtect(vt, kSlots[k] * sizeof(void*), old, &old);
        FlushInstructionCache(GetCurrentProcess(), 0, 0);
        LeaveCriticalSection(&g_cs);
        Log("instrumented %s vtable %p (%d methods)", kKindName[k], vt, kSlots[k]);
        return;
    }
    LeaveCriticalSection(&g_cs);
    Log("NOTE: %s object %p has a second vtable %p, not instrumented", kKindName[k], obj, vt);
}

// ---------------------------------------------------------------- summary
static void WriteSummary()
{
    char path[MAX_PATH]; sprintf_s(path, "%s\\ddraw_proxy_summary.txt", g_dir);
    FILE* f = 0; if (fopen_s(&f, path, "w") || !f) return;
    fprintf(f, "ddraw proxy call counts at tick %lu\n", GetTickCount());
    for (int k = 0; k < K_COUNT; ++k) {
        if (!g_slots[k][0].orig) continue;
        fprintf(f, "\n%s\n", kKindName[k]);
        bool seen[64] = {};
        for (;;) {   // descending by count
            int best = -1;
            for (int i = 0; i < kSlots[k]; ++i)
                if (!seen[i] && g_slots[k][i].count && (best < 0 || g_slots[k][i].count > g_slots[k][best].count)) best = i;
            if (best < 0) break;
            seen[best] = true;
            fprintf(f, "  %10ld  %s\n", g_slots[k][best].count, MethodName(k, best));
        }
    }
    fclose(f);
}
static DWORD WINAPI SummaryThread(void*) { for (;;) { Sleep(2000); WriteSummary(); } }

// ---------------------------------------------------------------- exports
static HMODULE g_real;
static const char* kFwdNames[] = { "AcquireDDThreadLock", "CompleteCreateSysmemSurface", "D3DParseUnknownCommand",
    "DDGetAttachedSurfaceLcl", "DDInternalLock", "DDInternalUnlock", "DSoundHelp", "DirectDrawCreateClipper",
    "DirectDrawEnumerateA", "DirectDrawEnumerateExA", "DirectDrawEnumerateExW", "DirectDrawEnumerateW",
    "DllCanUnloadNow", "DllGetClassObject", "GetDDSurfaceLocal", "GetOLEThunkData", "GetSurfaceFromDC",
    "RegisterSpecialCase", "ReleaseDDThreadLock", "SetAppCompatData" };
extern "C" FARPROC g_fwd[20] = {};
#define FWD(i, name) extern "C" __declspec(naked) void Fwd_##name() { __asm { jmp dword ptr [g_fwd + i * 4] } }
FWD(0, AcquireDDThreadLock) FWD(1, CompleteCreateSysmemSurface) FWD(2, D3DParseUnknownCommand)
FWD(3, DDGetAttachedSurfaceLcl) FWD(4, DDInternalLock) FWD(5, DDInternalUnlock) FWD(6, DSoundHelp)
FWD(7, DirectDrawCreateClipper) FWD(8, DirectDrawEnumerateA) FWD(9, DirectDrawEnumerateExA)
FWD(10, DirectDrawEnumerateExW) FWD(11, DirectDrawEnumerateW) FWD(12, DllCanUnloadNow) FWD(13, DllGetClassObject)
FWD(14, GetDDSurfaceLocal) FWD(15, GetOLEThunkData) FWD(16, GetSurfaceFromDC) FWD(17, RegisterSpecialCase)
FWD(18, ReleaseDDThreadLock) FWD(19, SetAppCompatData)

static bool Init()
{
    if (g_real) return true;
    char sys[MAX_PATH]; GetSystemWow64DirectoryA(sys, MAX_PATH);   // SysWOW64 for a 32-bit process
    if (!sys[0]) GetSystemDirectoryA(sys, MAX_PATH);
    strcat_s(sys, "\\ddraw.dll");
    g_real = LoadLibraryA(sys);
    if (!g_real) { Log("FATAL: cannot load %s (error %lu)", sys, GetLastError()); return false; }
    for (int i = 0; i < 20; ++i) g_fwd[i] = GetProcAddress(g_real, kFwdNames[i]);
    Log("loaded real %s", sys);
    CloseHandle(CreateThread(0, 0, SummaryThread, 0, 0, 0));
    return true;
}

extern "C" HRESULT WINAPI Proxy_DirectDrawCreate(GUID* guid, LPDIRECTDRAW* out, IUnknown* u)
{
    if (!Init()) return DDERR_GENERIC;
    auto real = (HRESULT(WINAPI*)(GUID*, LPDIRECTDRAW*, IUnknown*))GetProcAddress(g_real, "DirectDrawCreate");
    HRESULT hr = real(guid, out, u);
    Log("DirectDrawCreate(guid=%p) -> 0x%08lx dd=%p", guid, hr, out ? *out : 0);
    if (SUCCEEDED(hr) && out) PatchObject(*out, K_DD1);
    return hr;
}
extern "C" HRESULT WINAPI Proxy_DirectDrawCreateEx(GUID* guid, void** out, REFIID riid, IUnknown* u)
{
    if (!Init()) return DDERR_GENERIC;
    auto real = (HRESULT(WINAPI*)(GUID*, void**, REFIID, IUnknown*))GetProcAddress(g_real, "DirectDrawCreateEx");
    HRESULT hr = real(guid, out, riid, u);
    Log("DirectDrawCreateEx(guid=%p, %s) -> 0x%08lx dd=%p", guid, IidName(riid), hr, out ? *out : 0);
    if (SUCCEEDED(hr) && out) PatchObject(*out, KindForIid(riid));
    return hr;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, void*)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(h);
        InitializeCriticalSection(&g_cs);
        GetModuleFileNameA(0, g_dir, MAX_PATH);
        char* s = strrchr(g_dir, '\\'); if (s) *s = 0;
        char path[MAX_PATH]; sprintf_s(path, "%s\\ddraw_proxy.log", g_dir);
        g_log = _fsopen(path, "w", _SH_DENYWR);
        g_thunks = (BYTE*)VirtualAlloc(0, 64 * 1024, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
        char exe[MAX_PATH]; GetModuleFileNameA(0, exe, MAX_PATH);
        Log("ddraw proxy attached to %s", exe);
        Init();
    } else if (reason == DLL_PROCESS_DETACH) {
        WriteSummary();
        Log("detach");
        if (g_log) fclose(g_log);
    }
    return TRUE;
}
