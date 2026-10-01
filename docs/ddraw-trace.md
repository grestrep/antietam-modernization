# DirectDraw usage trace (logging proxy)

Tool: `tools/logger` (a proxy ddraw.dll that forwards to SysWOW64\ddraw.dll and counts and logs every method of IDirectDraw 1/2/4/7, IDirectDrawSurface 1–7, IDirectDrawPalette and IDirectDrawClipper).
Exe: `SMAntietam1210.exe -campaign 1` (v12.10). Runs: two menu-only runs of 20–60 s, and one user session of about 1 minute that may have included a battle. Raw logs and summaries are in `tools/logger/logs/`.

## Every DirectDraw call the game makes
| # | Call | Notes |
|---|---|---|
| 1 | `DirectDrawCreate(NULL)` | IDirectDraw v1 only. No QueryInterface for DD2/4/7, no Direct3D. |
| 2 | `SetCooperativeLevel(hwnd, FULLSCREEN\|EXCLUSIVE)` | |
| 3 | `SetDisplayMode(800, 600, 8)` | |
| 4 | `SetCooperativeLevel(NULL, NORMAL)` | Drops exclusive mode right after the mode switch |
| 5 | `CreatePalette(DDPCAPS_8BIT)` | `SetEntries` is never called |
| 6 | `CreateSurface(DDSCAPS_PRIMARYSURFACE)` | No back buffer, no Flip |
| 7 | 8× `QueryInterface(IUnknown)`, 4× `GetColorKey`, 1× `GetSurfaceDesc` | Housekeeping |
| – | **No Blt / BltFast / Lock / Flip / SetEntries at all**, in the menus or in the user session | |
| 8 | `RestoreDisplayMode`, Releases | On quit |

## Conclusion
DirectDraw is used purely to switch the monitor to 800x600x8. All drawing is done with GDI into the game window: CreateDIBSection, SetDIBColorTable, BitBlt, StretchBlt, CreatePalette/RealizePalette/AnimatePalette, SetSystemPaletteUse.
