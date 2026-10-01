// =====================================================================================
//  smav2 ddraw.dll - Direct3D 11 presenter (see present_d3d11.h for the overview)
// =====================================================================================
#define WIN32_LEAN_AND_MEAN
#include "present_d3d11.h"
#include <d3d11.h>
#include <dxgi1_6.h>   // IDXGIFactory6::EnumAdapterByGpuPreference (pick the GPU)
#include <string.h>
#include <stdlib.h>

// Precompiled shaders (build.bat runs fxc on shaders.hlsl -> build\shader_*.h).
#include "shader_vs.h"        // g_VS_Fullscreen
#include "shader_nearest.h"   // g_PS_Nearest
#include "shader_linear.h"    // g_PS_Linear
#include "shader_sharp.h"     // g_PS_Sharp
#include "shader_scale2x.h"   // g_PS_Scale2x

#define SAFE_RELEASE(p) do { if (p) { (p)->Release(); (p) = 0; } } while (0)

// ---------------------------------------------------------------- shared state
// Everything below the lock is written by the game thread (frame, layout) and read by the
// render thread; the D3D objects are used by the render thread only.
static CRITICAL_SECTION s_lock;
static HANDLE   s_thread, s_wake, s_ready;
static volatile bool s_quit, s_ok, s_active;
static D3DLogFn s_log;

static HWND  s_hwnd;
static int   s_texW, s_texH;
static bool  s_vsync, s_flip;
static bool  s_exclusive;             // mode=exclusive: real 800x600 fullscreen via DXGI
static int   s_gpuPref;               // 0 default adapter, 1 high performance, 2 power saving
static DWORD s_lastFsTry;             // rate limit for re-entering fullscreen

static BYTE* s_frame;                 // latest frame from the game (texW*texH*4)
static bool  s_frameNew;              // not yet uploaded
static bool  s_redraw;                // present again even without a new frame
static int   s_cw, s_ch;              // client size
static RECT  s_pic;                   // picture rectangle in the client area
static D3DFilter s_filter;
static bool  s_layoutChanged;
static volatile LONG s_presents;
static LONG  s_dumpAt;               // debug: write the back buffer of present #N to a .bmp
static char  s_dumpPath[MAX_PATH];

// render-thread objects
static ID3D11Device*           s_dev;
static ID3D11DeviceContext*    s_ctx;
static IDXGISwapChain1*        s_swap;
static ID3D11RenderTargetView* s_rtv;
static ID3D11Texture2D*        s_tex;
static ID3D11ShaderResourceView* s_srv;
static ID3D11VertexShader*     s_vs;
static ID3D11PixelShader*      s_ps[4];
static ID3D11SamplerState*     s_sampPoint, *s_sampLinear;
static ID3D11Buffer*           s_cb;
static int s_bufW, s_bufH;            // current swap-chain size

#define LOG(...) do { if (s_log) s_log(__VA_ARGS__); } while (0)

// ---------------------------------------------------------------- device setup
static void ReleaseDevice()
{
    if (s_swap && s_exclusive) s_swap->SetFullscreenState(FALSE, 0);   // give the desktop mode back
    SAFE_RELEASE(s_rtv); SAFE_RELEASE(s_srv); SAFE_RELEASE(s_tex); SAFE_RELEASE(s_cb);
    SAFE_RELEASE(s_sampPoint); SAFE_RELEASE(s_sampLinear);
    for (int i = 0; i < 4; ++i) SAFE_RELEASE(s_ps[i]);
    SAFE_RELEASE(s_vs); SAFE_RELEASE(s_swap); SAFE_RELEASE(s_ctx); SAFE_RELEASE(s_dev);
}

// Render target view for the swap chain's back buffer.
static bool CreateRTV()
{
    ID3D11Texture2D* bb = 0;
    if (FAILED(s_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return false;
    HRESULT hr = s_dev->CreateRenderTargetView(bb, 0, &s_rtv);
    bb->Release();
    return SUCCEEDED(hr);
}

// Creates device, swap chain, frame texture, shaders, samplers and constant buffer.
static bool CreateDevice()
{
    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };

    // Which GPU: on laptops with two GPUs (e.g. Intel + NVIDIA), D3D11CreateDevice(NULL) gets the
    // power-saving integrated one. gpu=high asks Windows for the high-performance GPU instead
    // (the same choice "Graphics settings > High performance" makes).
    IDXGIAdapter1* pick = 0;
    if (s_gpuPref) {
        IDXGIFactory6* f6 = 0;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&f6))) {
            f6->EnumAdapterByGpuPreference(0, s_gpuPref == 1 ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                                             : DXGI_GPU_PREFERENCE_MINIMUM_POWER,
                                           __uuidof(IDXGIAdapter1), (void**)&pick);
            f6->Release();
        }
    }
    HRESULT hr = D3D11CreateDevice(pick, pick ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, 0,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 3, D3D11_SDK_VERSION, &s_dev, 0, &s_ctx);
    SAFE_RELEASE(pick);
    if (FAILED(hr)) { LOG("D3D11: D3D11CreateDevice failed 0x%08lx", hr); return false; }

    // Swap chain on the game window, via the device's own DXGI factory.
    IDXGIDevice* dxdev = 0; IDXGIAdapter* adapter = 0; IDXGIFactory2* factory = 0;
    s_dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxdev);
    // Input latency: allow only one frame to be queued ahead of the display (default is 3).
    IDXGIDevice1* dxdev1 = 0;
    if (SUCCEEDED(s_dev->QueryInterface(__uuidof(IDXGIDevice1), (void**)&dxdev1))) {
        dxdev1->SetMaximumFrameLatency(1);
        dxdev1->Release();
    }
    if (dxdev) dxdev->GetAdapter(&adapter);
    if (adapter) {                                     // log which GPU we ended up on
        DXGI_ADAPTER_DESC ad; char name[128] = "?";
        if (SUCCEEDED(adapter->GetDesc(&ad))) WideCharToMultiByte(CP_ACP, 0, ad.Description, -1, name, sizeof name, 0, 0);
        LOG("D3D11: GPU \"%s\" (gpu=%s)", name, s_gpuPref == 1 ? "high" : s_gpuPref == 2 ? "low" : "default");
    }
    if (adapter) adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory);
    if (!factory) { SAFE_RELEASE(adapter); SAFE_RELEASE(dxdev); LOG("D3D11: no IDXGIFactory2"); return false; }

    RECT rc; GetClientRect(s_hwnd, &rc);
    s_bufW = max(1, (int)rc.right); s_bufH = max(1, (int)rc.bottom);
    DXGI_SWAP_CHAIN_DESC1 sd = {};
    sd.Width = s_bufW; sd.Height = s_bufH;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = s_flip ? DXGI_SWAP_EFFECT_FLIP_DISCARD : DXGI_SWAP_EFFECT_DISCARD;
    if (!s_flip) sd.BufferCount = 1;
    if (s_exclusive) sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;   // let DXGI change the monitor mode
    hr = factory->CreateSwapChainForHwnd(s_dev, s_hwnd, &sd, 0, 0, &s_swap);
    if (SUCCEEDED(hr))   // the game window is never DXGI-fullscreen; keep Alt+Enter away from it
        factory->MakeWindowAssociation(s_hwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
    factory->Release(); adapter->Release(); dxdev->Release();
    if (FAILED(hr)) { LOG("D3D11: CreateSwapChainForHwnd (%s) failed 0x%08lx", s_flip ? "flip" : "blt", hr); return false; }
    if (!CreateRTV()) { LOG("D3D11: render target view failed"); return false; }

    // The frame texture: CPU-writable, updated with Map(WRITE_DISCARD) once per new frame.
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = s_texW; td.Height = s_texH; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8X8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(s_dev->CreateTexture2D(&td, 0, &s_tex)) ||
        FAILED(s_dev->CreateShaderResourceView(s_tex, 0, &s_srv))) { LOG("D3D11: frame texture failed"); return false; }

    if (FAILED(s_dev->CreateVertexShader(g_VS_Fullscreen, sizeof g_VS_Fullscreen, 0, &s_vs)) ||
        FAILED(s_dev->CreatePixelShader(g_PS_Nearest, sizeof g_PS_Nearest, 0, &s_ps[D3DF_NEAREST])) ||
        FAILED(s_dev->CreatePixelShader(g_PS_Linear,  sizeof g_PS_Linear,  0, &s_ps[D3DF_LINEAR]))  ||
        FAILED(s_dev->CreatePixelShader(g_PS_Sharp,   sizeof g_PS_Sharp,   0, &s_ps[D3DF_SHARP]))   ||
        FAILED(s_dev->CreatePixelShader(g_PS_Scale2x, sizeof g_PS_Scale2x, 0, &s_ps[D3DF_SCALE2X]))) {
        LOG("D3D11: shader creation failed"); return false;
    }

    D3D11_SAMPLER_DESC smp = {};
    smp.AddressU = smp.AddressV = smp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    smp.MaxLOD = D3D11_FLOAT32_MAX;
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    s_dev->CreateSamplerState(&smp, &s_sampPoint);
    smp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s_dev->CreateSamplerState(&smp, &s_sampLinear);

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(s_dev->CreateBuffer(&bd, 0, &s_cb))) { LOG("D3D11: constant buffer failed"); return false; }

    D3D_FEATURE_LEVEL fl = s_dev->GetFeatureLevel();
    LOG("D3D11: device ready (feature level %x), swap chain %dx%d %s, vsync %d",
        fl, s_bufW, s_bufH, s_flip ? "flip-discard" : "blt-discard", s_vsync);
    return true;
}

// Debug: copies the current back buffer to a CPU texture and writes it as a 32-bit BMP. Used to
// verify the GPU output when no screenshot of the desktop can be taken.
static void DumpBackBuffer()
{
    ID3D11Texture2D* bb = 0; ID3D11Texture2D* st = 0;
    if (FAILED(s_swap->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb))) return;
    D3D11_TEXTURE2D_DESC d; bb->GetDesc(&d);
    d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
    if (SUCCEEDED(s_dev->CreateTexture2D(&d, 0, &st))) {
        s_ctx->CopyResource(st, bb);
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(s_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            HANDLE f = CreateFileA(s_dumpPath, GENERIC_WRITE, 0, 0, CREATE_ALWAYS, 0, 0);
            if (f != INVALID_HANDLE_VALUE) {
                BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
                ih.biSize = sizeof ih; ih.biWidth = d.Width; ih.biHeight = -(LONG)d.Height;
                ih.biPlanes = 1; ih.biBitCount = 32; ih.biCompression = BI_RGB;
                fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih;
                fh.bfSize = fh.bfOffBits + d.Width * d.Height * 4;
                DWORD w;
                WriteFile(f, &fh, sizeof fh, &w, 0); WriteFile(f, &ih, sizeof ih, &w, 0);
                for (UINT y = 0; y < d.Height; ++y) WriteFile(f, (BYTE*)m.pData + y * m.RowPitch, d.Width * 4, &w, 0);
                CloseHandle(f);
                LOG("D3D11: back buffer of present #%ld written to %s", s_presents + 1, s_dumpPath);
            }
            s_ctx->Unmap(st, 0);
        }
        st->Release();
    }
    bb->Release();
}

// mode=exclusive: switches the monitor to the game's resolution (closest mode DXGI offers,
// 32-bit) and makes the swap chain exclusive-fullscreen. Called after device creation and
// whenever the game window is active again after Alt-Tab (DXGI leaves fullscreen by itself when
// the window loses focus). Returns false if the switch failed.
static bool EnterFullscreen()
{
    s_lastFsTry = GetTickCount();
    IDXGIOutput* out = 0;
    if (FAILED(s_swap->GetContainingOutput(&out))) { LOG("D3D11: exclusive: no output"); return false; }
    DXGI_MODE_DESC want = {}, mode = {};
    want.Width = s_texW; want.Height = s_texH; want.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    HRESULT hr = out->FindClosestMatchingMode(&want, &mode, s_dev);
    if (FAILED(hr)) { out->Release(); LOG("D3D11: exclusive: no display mode near %dx%d (0x%08lx)", s_texW, s_texH, hr); return false; }
    s_swap->ResizeTarget(&mode);
    hr = s_swap->SetFullscreenState(TRUE, out);
    out->Release();
    if (FAILED(hr)) { LOG("D3D11: exclusive: SetFullscreenState failed 0x%08lx", hr); return false; }
    SAFE_RELEASE(s_rtv);
    s_ctx->OMSetRenderTargets(0, 0, 0);
    hr = s_swap->ResizeBuffers(0, mode.Width, mode.Height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH);
    if (FAILED(hr) || !CreateRTV()) { LOG("D3D11: exclusive: ResizeBuffers failed 0x%08lx", hr); return false; }
    s_bufW = mode.Width; s_bufH = mode.Height;
    EnterCriticalSection(&s_lock);                  // picture fills the whole mode
    s_cw = mode.Width; s_ch = mode.Height; SetRect(&s_pic, 0, 0, mode.Width, mode.Height);
    s_redraw = true;
    LeaveCriticalSection(&s_lock);
    LOG("D3D11: exclusive fullscreen %ux%u @ %u/%u Hz", mode.Width, mode.Height,
        mode.RefreshRate.Numerator, mode.RefreshRate.Denominator);
    return true;
}

// mode=exclusive: back to fullscreen after Alt-Tab once the game window is in front again.
static void CheckFullscreen()
{
    if (!s_exclusive || GetTickCount() - s_lastFsTry < 1000) return;
    BOOL fs = FALSE;
    s_swap->GetFullscreenState(&fs, 0);
    if (!fs && GetForegroundWindow() == s_hwnd && !IsIconic(s_hwnd)) {
        LOG("D3D11: exclusive: game window active again, re-entering fullscreen");
        EnterFullscreen();
    }
}

// ---------------------------------------------------------------- one frame
// Returns false on device loss.
static bool RenderOnce()
{
    // Take a consistent snapshot of layout/frame under the lock (cheap), upload outside it.
    EnterCriticalSection(&s_lock);
    bool newFrame = s_frameNew, layout = s_layoutChanged;
    int cw = s_cw, ch = s_ch; RECT pic = s_pic; D3DFilter f = s_filter;
    s_frameNew = false; s_layoutChanged = false; s_redraw = false;
    if (newFrame) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(s_ctx->Map(s_tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            const BYTE* src = s_frame;
            BYTE* dst = (BYTE*)m.pData;
            for (int y = 0; y < s_texH; ++y, src += s_texW * 4, dst += m.RowPitch)
                memcpy(dst, src, s_texW * 4);
            s_ctx->Unmap(s_tex, 0);
        }
    }
    LeaveCriticalSection(&s_lock);

    // Window resized/maximised: resize the swap chain buffers to the client size.
    // (Exclusive fullscreen: EnterFullscreen sized the buffers to the display mode.)
    if (!s_exclusive && (layout || cw != s_bufW || ch != s_bufH) && cw > 0 && ch > 0 && (cw != s_bufW || ch != s_bufH)) {
        SAFE_RELEASE(s_rtv);
        s_ctx->OMSetRenderTargets(0, 0, 0);
        HRESULT hr = s_swap->ResizeBuffers(0, cw, ch, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) { LOG("D3D11: ResizeBuffers(%d,%d) failed 0x%08lx", cw, ch, hr); return false; }
        s_bufW = cw; s_bufH = ch;
        if (!CreateRTV()) return false;
    }

    const float black[4] = { 0, 0, 0, 1 };
    s_ctx->OMSetRenderTargets(1, &s_rtv, 0);
    s_ctx->ClearRenderTargetView(s_rtv, black);       // letterbox bars

    D3D11_VIEWPORT vp = { (float)pic.left, (float)pic.top, (float)(pic.right - pic.left),
                          (float)(pic.bottom - pic.top), 0, 1 };
    s_ctx->RSSetViewports(1, &vp);
    float params[4] = { (float)s_texW, (float)s_texH, vp.Width, vp.Height };
    s_ctx->UpdateSubresource(s_cb, 0, 0, params, 0, 0);

    s_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    s_ctx->IASetInputLayout(0);
    s_ctx->VSSetShader(s_vs, 0, 0);
    s_ctx->PSSetShader(s_ps[f], 0, 0);
    s_ctx->PSSetShaderResources(0, 1, &s_srv);
    ID3D11SamplerState* samps[2] = { s_sampPoint, s_sampLinear };
    s_ctx->PSSetSamplers(0, 2, samps);
    s_ctx->PSSetConstantBuffers(0, 1, &s_cb);
    s_ctx->Draw(3, 0);

    if (s_dumpAt && s_presents + 1 == s_dumpAt) DumpBackBuffer();
    HRESULT hr = s_swap->Present(s_vsync ? 1 : 0, 0);   // DXGI_STATUS_OCCLUDED (minimised/alt-tabbed) is fine
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        LOG("D3D11: device lost (0x%08lx, reason 0x%08lx)", hr, s_dev->GetDeviceRemovedReason());
        return false;
    }
    InterlockedIncrement(&s_presents);
    return true;
}

// ---------------------------------------------------------------- render thread
static DWORD WINAPI RenderThread(void*)
{
    s_ok = CreateDevice();
    if (s_ok && s_exclusive && !EnterFullscreen()) s_ok = false;   // caller falls back to borderless
    SetEvent(s_ready);                                 // D3D_Start waits for this
    if (!s_ok) { ReleaseDevice(); return 0; }

    int recoveries = 0;
    while (!s_quit) {
        WaitForSingleObject(s_wake, 250);              // woken per frame; 250 ms keeps bars fresh
        if (s_quit) break;
        CheckFullscreen();
        bool work;
        EnterCriticalSection(&s_lock);
        work = s_frameNew || s_redraw || s_layoutChanged;
        LeaveCriticalSection(&s_lock);
        if (!work) continue;
        if (!RenderOnce()) {
            // Device lost (driver update/reset, GPU switch): rebuild once, then give up -> GDI.
            ReleaseDevice();
            if (++recoveries > 1 || !CreateDevice() || (s_exclusive && !EnterFullscreen())) {
                LOG("D3D11: giving up, falling back to GDI");
                ReleaseDevice();
                s_active = false;
                return 0;
            }
            EnterCriticalSection(&s_lock); s_frameNew = s_redraw = true; LeaveCriticalSection(&s_lock);
        }
    }
    ReleaseDevice();
    return 0;
}

// ---------------------------------------------------------------- public API
// Waits for `h` while still handling messages *sent* to this thread's windows. Needed because
// D3D_Start/D3D_Stop run on the game's window thread, and DXGI (mode switch, SetFullscreenState)
// sends messages to the game window from the render thread: a plain wait would deadlock.
static DWORD WaitPumpingSent(HANDLE h, DWORD ms)
{
    DWORD start = GetTickCount();
    for (;;) {
        DWORD left = ms - min(ms, GetTickCount() - start);
        DWORD r = MsgWaitForMultipleObjects(1, &h, FALSE, left, QS_SENDMESSAGE);
        if (r != WAIT_OBJECT_0 + 1) return r;            // signalled or timed out
        MSG m; PeekMessageA(&m, 0, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);   // dispatches sent msgs
    }
}

bool D3D_Start(HWND hwnd, int texW, int texH, bool vsync, bool flipModel, bool exclusive, int gpuPref, D3DLogFn log)
{
    if (s_thread) return s_active;
    s_gpuPref = gpuPref;
    s_log = log; s_hwnd = hwnd; s_texW = texW; s_texH = texH; s_vsync = vsync; s_flip = flipModel;
    s_exclusive = exclusive;
    InitializeCriticalSection(&s_lock);
    s_frame = (BYTE*)calloc((size_t)texW * texH, 4);
    s_wake = CreateEventA(0, FALSE, FALSE, 0);
    s_ready = CreateEventA(0, TRUE, FALSE, 0);
    s_quit = false; s_ok = false;
    s_thread = CreateThread(0, 0, RenderThread, 0, 0, 0);
    if (WaitPumpingSent(s_ready, 8000) != WAIT_OBJECT_0 || !s_ok) {
        LOG("D3D11: presenter could not start");
        D3D_Stop();
        return false;
    }
    s_active = true;
    return true;
}

void D3D_Stop()
{
    if (!s_thread) return;
    s_quit = true; s_active = false;
    SetEvent(s_wake);
    WaitPumpingSent(s_thread, 3000);
    CloseHandle(s_thread); s_thread = 0;
    s_exclusive = false;
    CloseHandle(s_wake); CloseHandle(s_ready);
    free(s_frame); s_frame = 0;
    DeleteCriticalSection(&s_lock);
}

bool D3D_Active() { return s_active; }

void D3D_SetLayout(int clientW, int clientH, int picX, int picY, int picW, int picH, D3DFilter f)
{
    if (!s_thread) return;
    EnterCriticalSection(&s_lock);
    s_cw = clientW; s_ch = clientH;
    SetRect(&s_pic, picX, picY, picX + picW, picY + picH);
    s_filter = f;
    s_layoutChanged = true;
    LeaveCriticalSection(&s_lock);
    SetEvent(s_wake);
}

void D3D_SubmitFrame(const void* bits)
{
    if (!s_active) return;
    EnterCriticalSection(&s_lock);
    memcpy(s_frame, bits, (size_t)s_texW * s_texH * 4);
    s_frameNew = true;
    LeaveCriticalSection(&s_lock);
    SetEvent(s_wake);
}

void D3D_Refresh()
{
    if (!s_active) return;
    EnterCriticalSection(&s_lock); s_redraw = true; LeaveCriticalSection(&s_lock);
    SetEvent(s_wake);
}

LONG D3D_PresentCount() { return s_presents; }

void D3D_DebugDump(LONG presentNumber, const char* bmpPath)
{
    s_dumpAt = presentNumber;
    strncpy_s(s_dumpPath, bmpPath, _TRUNCATE);
}
