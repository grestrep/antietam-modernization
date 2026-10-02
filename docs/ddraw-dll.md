# smav2 ddraw.dll: design and code guide

Source: `ddraw/smav2_ddraw.cpp`, `ddraw/present_d3d11.cpp/.h`, `ddraw/shaders.hlsl` (one file, about 600 lines with comments), `ddraw/ddraw.def`, `ddraw/smav2.ini`, `ddraw/build.bat`.

## 1. Why a ddraw.dll
The game exe imports `DDRAW.dll` by name. Windows looks for a DLL in the exe's folder before System32/SysWOW64 (`ddraw.dll` is not a "KnownDLL"), so a `ddraw.dll` placed next to the exe is loaded instead of Windows' own. That lets us change how the game talks to DirectDraw **without touching the exe**. Deleting the file, or setting `mode=fullscreen`, restores the original behaviour.

## 2. What the game needs from DirectDraw
The trace (`docs/ddraw-trace.md`) shows the game only uses DirectDraw to switch the monitor to **800×600, 8-bit**. It creates a palette and a primary surface it never draws to. All drawing is GDI into its own window. So the DLL needs to emulate only the mode switch, not rendering.

## 3. Architecture

```
 SMAntietam1210.exe
   │  DirectDrawCreate()                     ┌──────────────────────────────────────────┐
   ├───────────────────────────────────────► │ smav2 ddraw.dll (game folder)            │
   │                                         │  Proxy_DirectDrawCreate                  │
   │                                         │   └ real DirectDrawCreate ──────────────►│ SysWOW64\ddraw.dll
   │                                         │   └ PatchDD: replace 5 vtable entries    │
   │  dd->SetCooperativeLevel/SetDisplayMode │  DD_* hooks (emulate mode, own window)   │
   ├───────────────────────────────────────► │                                          │
   │  other ddraw exports                    │  Fwd_* naked jumps ─────────────────────►│ SysWOW64\ddraw.dll
   │  GetSystemMetrics / BitBlt / StretchBlt │  IAT hooks in the exe                    │
   │  window messages                        │  WndProc (subclass) → game's WndProc     │
   └─────────────────────────────────────────┴──────────────────────────────────────────┘
```

### 3.0 Game fixes (every mode, `ApplyGameFixes`)
These let the **unmodified** `Antietam.exe` run, so no patched Firaxis exe is needed or distributed:
- **GlobalFree:** the game frees CRT memory with `GlobalFree` (`docs/exe-patch.md`). Instead of patching the 65/68 call sites, the DLL points the exe's `KERNEL32!GlobalFree` **import** at `Hook_GlobalFree`, which calls the game's own CRT `free()`. All call sites read that import, including the `mov reg,[GlobalFree]` forms, so the effect is identical to the exe patch. `free()` is located by its MSVC6 signature (`56 8B 74 24 08 85 F6 74 ?? 6A 09 E8`, exactly one match per build). If it isn't found, the fix is skipped and a warning is logged.
- **EmulateHeap shim:** Windows applies it to programs named `Antietam.exe`, and it crashes. It is matched by file name, so the installer copies the player's exe to `Antietam_Win11.exe` (unchanged). If `AcGenral.dll` is loaded anyway, a warning is logged.
- **Campaigns (not touched):** 1 = Antietam, 3 = South Mountain, 20–99 = player-made battle packs. v12.10 (v3.0 Final Patch) takes `-campaign N` on its command line, knows only 1 and 3, and opens South Mountain for anything else. v9.84 (v2.0 Beta Patch) reads `Campaign=N` from `antietam.ini`, where JSGME battle packs put their own value. An earlier DLL version copied `-campaign` into `antietam.ini`; that overwrote the battle pack's setting, so it was removed (tested with the Austerlitz pack, 2026-10-01). v9.84 can't run South Mountain: it lists the scenarios but loads the Antietam map and quits when the battle starts.
- **Build identification:** the log names the build (PE timestamp `0x38A45BF6` = v9.84, `0x398EF5B0` = v12.10) for bug reports.
- **DPI awareness** (`dpi_aware=1`, all modes except `fullscreen`): `SetProcessDpiAwarenessContext(PER_MONITOR_AWARE_V2)` runs in DllMain, before the game creates its window. Otherwise Windows would scale the window a second time on 125–200% displays.

Tested with the original, unpatched v9.84 and v12.10 exes, renamed: the fix was applied (`free` at `0x49C68A` / `0x4A4D33`), and the game ran 50–70 s with no crash. Before the fix, it crashed about 30 s in.

### 3.1 Export forwarding (`FWD` macro, `LoadReal`)
- **Same export table as Windows:** `ddraw.def` lists the 22 exports of the Windows `ddraw.dll`, with the same names and ordinals.
- **One real implementation:** `DirectDrawCreate` → `Proxy_DirectDrawCreate`.
- **Everything else is forwarded** through `Fwd_<name>`. Each is a 6-byte naked function `jmp [g_fwd + i*4]`, so stack and registers reach the real function untouched and no prototypes are needed.
- **Loading the real DLL:** `LoadReal()` loads `SysWOW64\ddraw.dll` by **full path**, because `LoadLibrary("ddraw.dll")` would find this DLL again. It then fills `g_fwd`.

### 3.2 Vtable patching (`PatchDD`, `Hook`, `DD_*` templates)
Writing a full COM wrapper (our own `IDirectDraw` object forwarding 23 methods) would be more code for no gain. Instead the **real** object is given to the game, and five entries of its vtable are replaced:

| Slot | Method | Hook |
|---|---|---|
| 0 | QueryInterface | Patches any `IDirectDraw`/`IDirectDraw2` it returns; warns on v4/v7 |
| 12 | GetDisplayMode | Reports the emulated mode (800×600×8, palettized) |
| 19 | RestoreDisplayMode | No-op |
| 20 | SetCooperativeLevel | `EXCLUSIVE\|FULLSCREEN` → `NORMAL`. The first window handle seen becomes the game window, which is then taken over (`ApplyWindow`) and the watchdog started |
| 21 | SetDisplayMode | Only records the mode, then resizes the window. The monitor is never switched |

- **Shared vtables:** a vtable is shared by all objects of an interface version and lives in read-only data of the system DLL, hence `VirtualProtect`. The originals are kept in `g_orig[kind][slot]` and called from the hooks.
- **Why templates:** the hooks are templates on the interface kind only so that `IDirectDraw` and `IDirectDraw2` keep separate originals. `IDirectDraw2::SetDisplayMode` takes two extra arguments, hence `DD_SetDisplayMode1/2`.

### 3.3 The game window (`ApplyWindow`, `TargetRect`, `WndProc`)
- **`ApplyWindow`:**
  - Turns the game's `WS_POPUP` fullscreen window into a captioned window (`windowed`) or a popup (`borderless`).
  - Subclasses its window procedure with `SetWindowLong(GWL_WNDPROC)`.
  - Moves it to `TargetRect()`: a client area of exactly 800×600, centred in the monitor's work area, or where the player dragged it.
- **`WndProc`** sits in front of the game's window procedure:
  - **`WM_WINDOWPOSCHANGING`:** forces position and size. The game still tries to put itself at (0,0) 800×600. While the player drags (`WM_ENTERSIZEMOVE`…`WM_EXITSIZEMOVE`) only the size is forced, and the new position is remembered.
  - **`WM_STYLECHANGING`:** rewrites style changes made by the game.
  - **Cursor clipping:** `WM_ACTIVATE`, `WM_MOVE` and `WM_SIZE` clip the cursor to the client area, needed for edge scrolling (`clip_cursor`).
  - **`keep_focus`:** swallows `WM_ACTIVATEAPP(FALSE)`, `WM_ACTIVATE(WA_INACTIVE)` and `WM_KILLFOCUS`. The game was written for exclusive fullscreen and is never told it lost focus.
  - **Logging:** focus, palette and size messages are logged (capped).

### 3.4 IAT hooks in the game exe (`HookIat`)
The exe's import address table is resolved before our `DllMain` runs, because `ddraw.dll` is one of its static imports. Replacing an entry affects **only calls made by the game exe**.

| Import | Hook | Why |
|---|---|---|
| `USER32!GetSystemMetrics` | `SM_CXSCREEN/SM_CYSCREEN` return the emulated mode | The game sizes things from the "screen", which after a real mode switch would be 800×600 |
| Drawing and coordinate functions | See §3.6 | Scaling |
| `GDI32!BitBlt`, `GDI32!StretchBlt` | Unchanged, but counted (`g_blits`) | Frame counter for the stall detector |

### 3.5 DWM ghosting (`DllMain`)
The game reads input directly (`GetAsyncKeyState`, `GetCursorPos`), and its `PeekMessage` loops skip the mouse and keyboard message ranges, so input messages are never read. After 5 s Windows considers the window hung, and DWM covers it with a frozen **ghost** window while the game keeps running underneath. `DisableProcessWindowsGhosting()` turns that off for the process. Exclusive fullscreen is never ghosted, which is why the original never showed this (`docs/mode-switch.md`).

### 3.6 Scaling (virtual screen)
The game always renders an 800×600 picture. With scaling, that picture goes to an off-screen bitmap instead of the window, and we copy it enlarged into the window.

**Layout** (`ComputeLayout`, `LayoutForClient`):
- **`borderless`:** the window covers the whole monitor, and the picture is enlarged by a **whole factor**. `scale=auto` picks the largest that fits (1920×1200 → **2×** = 1600×1200); `scale=N` is fixed but reduced if it doesn't fit. The picture is centred, with black bars (`g_offX/g_offY`).
- **`windowed`:** a normal window (`WS_OVERLAPPEDWINDOW`) whose client area is `window_size` (e.g. `1024x768`, `1152x864`, `1280x960`). Without `window_size` it is the whole-factor size from `scale`; if it is too big for the work area it is reduced, keeping 4:3.
  - **Resizing:** the player can resize and maximise. `WM_SIZING` keeps the client area at 4:3 while a border is dragged; the edge being dragged decides whether width or height leads. `WM_GETMINMAXINFO` sets the minimum at half size.
  - **Letterboxing:** on `WM_SIZE`, `LayoutForClient` fits the largest 4:3 picture into the new client area. A maximised window gets black bars.
  - **Game moves ignored:** the game's own `MoveWindow`/`SetWindowPos` on its main window is dropped (only z-order/show changes pass), so it can't snap the window back to (0,0) 800×600.
- **Picture size:** `g_pw × g_ph`, any size. Coordinates are converted exactly with `x_real = off + x·g_pw/800` and back (`ToRealX/Y`, `VirtToReal`, `RealToVirt`), so the mouse mapping follows whatever size the window has.
- **Always on:** the virtual screen is used in windowed and borderless mode even at 1:1, so a later resize needs no switch-over.

**Filters** (`filter=`, chosen by `ChooseFilter`):

| Filter | How | Look |
|---|---|---|
| `nearest` | `StretchBlt` with `COLORONCOLOR` | Pixel-exact at 2×, 3×; uneven at 1.28× (some rows/columns doubled, others not) |
| `linear` | `StretchBlt` with `HALFTONE` | Soft at any size |
| `sharp` | Nearest to the next whole factor `k = ceil(scale)` into `g_pdc` (e.g. 2× = 1600×1200 for 1.28×), then `HALFTONE` down to the target | Crisp edges, even pixels at any size ("sharp bilinear") |
| `auto` (default) | `nearest` for whole factors, `sharp` otherwise | |

Tested windowed at 1024×768 (1.28×), 1152×864 (1.44×) and 1280×960 (1.60×): `sharp` was chosen automatically, and screenshots are in `docs/shot_windowed_*.png`. Maximise and restore worked: the picture went to 1505×1129 with bars, then back.

**Redirecting the drawing** (IAT hooks in the exe):

| Game call | While scaling |
|---|---|
| `GetDC(game window)` | Returns the virtual screen `g_vdc` (32-bit 800×600 DIB section) |
| `ReleaseDC(…, g_vdc)` | Presents (the game finished drawing) |
| `BeginPaint(game window)` | Real BeginPaint validates the window; the game gets `g_vdc` with `rcPaint` = whole screen |
| `EndPaint` | Real EndPaint, then Present |
| `BitBlt`, `StretchBlt`, `FillRect`, `TextOutA`, `SelectClipRgn`, `SelectPalette`, `RealizePalette`, `GetSystemPaletteEntries` | The window's real DC (`g_realDC`; the class is `CS_OWNDC`, so it is one fixed handle) is swapped for `g_vdc`. This covers DCs the game keeps |

The game's 8-bit DIB sections carry their own colour tables, so blitting them into the 32-bit virtual screen keeps colours correct.

**Presenting** (`Present`) and **pacing**:
- **Copy:** `StretchBlt` of `g_vdc` into the window at `(g_offX, g_offY)` (or the D3D11 presenter, §3.7). Then the black bars are painted.
- **When:** the game gives no "frame finished" signal for small updates such as a hover highlight or a drag rectangle. So:
  - **Frame end:** `PresentSoon` runs on ReleaseDC/EndPaint and on blits covering at least ¼ of the screen.
  - **Any other drawing:** `MarkDirty` presents immediately, unless the last present was less than 2 ms (D3D11) or 6 ms (GDI) ago.
  - **Changes that had to wait:** these are flushed by `FlushIfDue()`, which runs inside the hooks of **`timeGetTime`** (the game's frame limiter spins on it) and **`GetAsyncKeyState`** (input polling). Both are called constantly.
  - **Timer backstop:** a 16 ms `WM_TIMER` remains as a last resort. **It is not relied on:** the game's message loop only picks up `WM_TIMER` about every 160 ms.
- **Clock:** all pacing uses `QueryPerformanceCounter` (`NowMs`). `GetTickCount` only moves in 15.6 ms steps.
- **Measured** (menu with hover, then idle), change-to-present latency:

  | Version | Average | Maximum |
  |---|---|---|
  | Before the pacing fix (GetTickCount + WM_TIMER) | 78.8 ms | 159 ms |
  | After, GDI | 3.4 ms | 7 ms |
  | After, D3D11 | 2.0 ms | 17 ms |

  This was the "mouse lag" noticed in play-testing on hover and when dragging units. `smav2_ddraw.log` logs these numbers every 10 s ("present stats").
- **D3D11 queue:** `SetMaximumFrameLatency(1)` allows only one queued frame, which keeps display latency low. `vsync=0` lowers it a little further.

**Coordinates.** "Client coordinates" of the game window are virtual (0–799, 0–599); screen coordinates stay real.

| Path | Translation |
|---|---|
| `ScreenToClient(game window)` | real screen → `(p − origin − offset) / s` |
| `ClientToScreen(game window)` | virtual → `origin + offset + v·s` |
| `GetWindowRect(game window)` | 800×600 at the picture's screen position |
| Mouse messages `WM_MOUSEMOVE … WM_MBUTTONDBLCLK` | lParam translated real → virtual in our WndProc |
| `MoveWindow`/`SetWindowPos` on child windows of the game window | Scaled |
| Panels (`WS_CHILD` windows of the game window the exe creates, e.g. the scenario list, the Battle History chapter list) | Created/moved scaled; `GetDC`/`BeginPaint` return an off-screen bitmap of the panel's game size, copied enlarged into the real window after each drawing call; mouse messages translated in a `PeekMessageA` hook (the game reads them from its message loop) |
| Pop-ups (`WS_POPUP` windows owned by the game window: "Select scenario type", Save/Load dialogs, the Battle History text pane) | Original size, moved into the picture: the centre of the spot the game chose is mapped into the picture. A child window later created at exactly a pop-up's spot (the Battle History pane is recreated that way) is treated the same |
| `SetWindowRgn(game window)` | Ignored (it would cut the monitor-sized window) |
| Cursor clip | The picture rectangle, so edge scrolling triggers at the picture's edge and the cursor never enters the bars |

`GetCursorPos`/`SetCursorPos` are left real: the game converts with ScreenToClient/ClientToScreen, which are translated. Popup windows the game places with ClientToScreen land on the right spot.

**Known limits:**
- **Mouse cursor:** stays at normal size.
- **Intro videos:** an MCI AVI child window plays at native 640×480, centred.
- **Pop-ups:** placed inside the picture but kept at their original size (a deliberate choice).

### 3.7 Direct3D 11 presenter (`renderer=d3d11`; `present_d3d11.cpp`, `shaders.hlsl`)
This replaces the GDI `StretchBlt` in `Present()` with the GPU. Everything before that (virtual screen, hooks, layout, mouse mapping) is unchanged and shared with the GDI renderer.

```
 game thread                                   render thread (owns all D3D objects)
 ───────────                                   ───────────────────────────────────
 Present():  GdiFlush()                         wait for wake-up event (≤250 ms)
             D3D_SubmitFrame(g_vbits) ──copy──► frame buffer (lock)
             (memcpy ~0.2 ms, never waits)      ├ Map(WRITE_DISCARD) → 800×600 BGRX texture
 WM_SIZE / layout: D3D_SetLayout() ───────────► ├ ResizeBuffers to the client size if changed
                                                ├ clear black, viewport = picture rect
                                                ├ full-screen triangle + filter pixel shader
                                                └ Present(vsync)
```

- **GPU choice:** `gpu=high` (the default) asks `IDXGIFactory6::EnumAdapterByGpuPreference(HIGH_PERFORMANCE)` for the dedicated GPU. On a hybrid laptop (Intel Iris Xe + RTX 3050 Ti), plain `D3D11CreateDevice(NULL)` picked the Intel GPU, so the NVIDIA overlay never appeared. `gpu=low` picks the integrated GPU and `gpu=default` lets Windows decide. The chosen GPU is logged.
- **Device and swap chain:** `D3D11CreateDevice` (hardware, feature level 11_0/10_1/10_0), then a **flip-model** swap chain (`FLIP_DISCARD`, 2 buffers) on the game window. `swap=blt` uses `DISCARD` (bit-blt model) instead, for older systems or if the intro videos don't show. `MakeWindowAssociation(NO_ALT_ENTER|NO_WINDOW_CHANGES)` keeps DXGI from touching the window.
- **Threading:** the game thread never blocks on the GPU or v-sync. It only copies the finished frame and signals. All D3D calls happen on the render thread, so the game's own timing is unaffected by `vsync=1`.
- **Shaders:** precompiled at build time by `fxc` (`build.bat`) into `build/shader_*.h` and embedded in the DLL. Nothing is compiled at run time.

| `filter=` | Shader | Look |
|---|---|---|
| `nearest` | `PS_Nearest`, point sampler | Pixel-exact blocks |
| `linear` | `PS_Linear`, bilinear sampler | Soft |
| `sharp` | `PS_Sharp`, exact sharp-bilinear: solid texel interior, blend only within ½ screen pixel of texel edges | Crisp and even at any factor |
| `scale2x` | `PS_Scale2x`, Scale2x/EPX: each texel split 2×2, corners take a neighbour's colour along diagonal edges | Smoother diagonals in the pixel art |
| `auto` | `nearest` for whole factors, `sharp` otherwise | |

- **Fallback:** if device or swap-chain creation fails, `D3D_Start` returns false and the GDI path is used (logged as "renderer: Direct3D 11 not available -> GDI"). On device loss (`DXGI_ERROR_DEVICE_REMOVED/RESET`, e.g. a driver update) the render thread rebuilds once. A second failure switches to GDI for the rest of the session.
- **Verification without a desktop screenshot:** `[debug] d3d_dump=N` writes the back buffer of present #N to `smav2_d3d_dump.bmp`. Used during development (menu render confirmed with `scale2x` and `sharp`).
- **CPU:** the game itself busy-waits in its frame limiter (`timeGetTime` loop, `docs/mode-switch.md`), so the process uses about one CPU core with either renderer. The difference is only in our part: GDI `HALFTONE` scaling costs a few ms of CPU per present, D3D11 about 0.2 ms (the memcpy).

#### Exclusive fullscreen (`mode=exclusive`)
This is a real mode switch, done the modern way. The window becomes an 800×600 popup at the monitor's corner. The D3D11 swap chain is created with `DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH`, and `EnterFullscreen()` then:
1. finds the closest 800×600 display mode (`FindClosestMatchingMode`, 32-bit, any refresh rate);
2. calls `ResizeTarget` and `SetFullscreenState(TRUE)`;
3. resizes the buffers to the mode.

The picture is shown 1:1 (filter `nearest`). Everything else (virtual screen, hooks, mouse) is the same as in borderless mode.

- **`renderer` is forced to `d3d11`** in this mode.
- **Alt-Tab:** DXGI leaves fullscreen by itself when the window loses focus and restores the desktop mode. `CheckFullscreen()` on the render thread re-enters fullscreen once the game window is in front again (checked at most once per second).
- **Exit and crash:** `ReleaseDevice()` calls `SetFullscreenState(FALSE)` before releasing the swap chain. If the process dies, Windows restores the desktop mode itself (tested with a forced kill: back at 1920×1200).
- **Deadlock avoidance:** DXGI sends messages to the game window from the render thread during mode switches. `D3D_Start`/`D3D_Stop` run on the window thread, so they wait with `WaitPumpingSent()`, which dispatches *sent* messages while waiting.
- **Fallback:** if the switch fails, the window becomes borderless at scale 1 (the same 800×600 picture, centred, monitor unchanged), and the D3D11 presenter is tried again without the switch. This is logged.
- **Tested 2026-10-01:** the switch happened, showing "exclusive fullscreen 800x600 @ 120 Hz" in the log, and the desktop mode came back after exit. The visible result still needs a check by the player, because no desktop screenshots were possible during the test.

### 3.8 Diagnostics (`Log`, `Watchdog`, `SampleThread`)
- **`smav2_ddraw.log`:** one line per event, with a `GetTickCount` timestamp, flushed per line.
- **Watchdog thread** (`hang_watchdog=1`), once per second:
  - **HANG:** `SendMessageTimeout(WM_NULL)` fails for 2 s.
  - **IsHungAppWindow:** Windows' own "Not Responding" verdict changed.
  - **STALL:** no `BitBlt/StretchBlt` for 5 s while responsive. This is harmless on static menus.
- **Thread samples:** on HANG or STALL the game thread is suspended 5 times briefly. Its registers and every stack value inside the exe's code range (likely return addresses) are logged, so the log shows where the game is stuck.

## 4. Configuration (`smav2.ini`)
| Key | Default | Meaning |
|---|---|---|
| `[display] mode` | `windowed` (shipped ini: `borderless`) | `windowed`, `borderless` (whole monitor, picture centred), `exclusive` (real 800×600 switch via D3D11), or `fullscreen` (pure pass-through = original behaviour) |
| `[display] scale` | `auto` | Borderless (and windowed without `window_size`): `auto` = the largest whole factor that fits, or `1`, `2`, `3`, … |
| `[display] window_size` | none | Windowed client size, e.g. `1024x768`, `1152x864`, `1280x960` |
| `[display] filter` | `auto` | `auto`, `nearest`, `sharp`, `linear`, `scale2x` (d3d11 only) |
| `[display] renderer` | `gdi` (shipped ini: `d3d11`) | `d3d11` (GPU, §3.7) or `gdi` |
| `[display] vsync` | `1` | d3d11: wait for the monitor refresh |
| `[display] swap` | `flip` | d3d11: `flip` (modern) or `blt` (compatibility) |
| `[display] gpu` | `high` | d3d11: `high` (dedicated GPU), `low` (integrated) or `default` |
| `[debug] d3d_dump` | `0` | d3d11: write frame #N to `smav2_d3d_dump.bmp` |
| `[display] dpi_aware` | `1` | Per-monitor DPI awareness (no double scaling on 125–200% displays) |
| `[display] clip_cursor` | `1` | Keep the mouse in the window while active |
| `[display] keep_focus` | `1` | Hide focus loss from the game |
| `[game] skip_intro` | `0` | `1`: logo videos (MCI, file name contains "logo") are seeked to the end before `MCI_PLAY`, so they finish at once. The `mciSendCommandA` hook also retries a failed open with the 8.3 short path (MCI rejects names over ~128 characters) |
| `[game] max_fps` | `40` | Frame limit (edge-scroll speed); `0` = unlimited. Paces only whole-screen blits (0,0 800×600), see `FrameLimit()` |
| `[debug] log` | `1` | Write `smav2_ddraw.log` |
| `[debug] hang_watchdog` | `0` | HANG/STALL detector with stack samples |

With `mode=fullscreen` nothing is patched: no vtable hooks, no IAT hooks, no ghosting change, no window changes.

## 5. Build, deploy, revert
```
ddraw\build.bat                       -> ddraw\build\ddraw.dll + smav2.ini
copy ddraw\build\ddraw.dll  <game>\   ; copy ddraw\build\smav2.ini <game>\
revert: delete ddraw.dll, smav2.ini, smav2_ddraw.log from the game folder (or mode=fullscreen)
```

## 6. Known limits and next steps
- **Palette animation:** `AnimatePalette` has no effect on a true-colour desktop. Whether any in-game effect depends on it is still unchecked.
- **Supported interfaces:** only `IDirectDraw` and `IDirectDraw2` are emulated, which is all this game uses.
