# Step 1: emulated mode switch (smav2 ddraw.dll)

## What it does
- **SetCooperativeLevel:** `EXCLUSIVE|FULLSCREEN` becomes `NORMAL`.
- **SetDisplayMode(800,600,8):** recorded only; the monitor is never switched.
- **RestoreDisplayMode:** no-op.
- **GetDisplayMode:** reports the requested 800x600x8 (palettized).
- **Game window:**
  - Restyled to a captioned window (`windowed`) or popup (`borderless`) with an 800x600 client area, centred on the monitor's work area.
  - Subclassed, so `WM_WINDOWPOSCHANGING`/`WM_STYLECHANGING` keep it that way.
  - The cursor is clipped to the client area while the window is active (`clip_cursor=1`).
- **GetSystemMetrics(SM_CXSCREEN/SM_CYSCREEN):** in the game exe's import table, reports 800/600.
- **Implementation:** the real IDirectDraw object is used; only its vtable entries for these methods are replaced.

## Test (2026-10-01, SMAntietam1210.exe, -campaign 1 and -campaign 3)
- **Desktop:** stayed at 1920x1200 the whole time.
- **Window:** game window was style 0x86000000 (WS_POPUP) at (0,0)-(800,600); it became a captioned window at (557,261)-(1363,890) with an 800x600 client area.
- **Main menu:** renders correctly with **correct colours** (`shot_windowed_1.png`). The game draws 8-bit DIB sections with their own colour tables, which GDI converts to the 32-bit desktop correctly.
- **Open questions:**
  - Palette animation (`AnimatePalette`) has no effect on a true-colour desktop; check whether any in-game effect relies on it.
  - Mouse edge-scrolling and input in a battle.

## Windowed "freeze" (2026-10-01)
**Symptom:** a windowed battle freezes after the window loses focus, and Windows shows "Not Responding".

**Investigation (debugger attached to the frozen game):**
- **Main thread:** still running the battle loop. The frame limiter at `0x40B24C–0x40B25A` (busy-waits on `timeGetTime` in 16 ms steps) has a deadline that tracks the current time.
- **Drawing:** BitBlt calls continue, so the stall detector stays quiet.
- **Messages:** `SendMessageTimeout(WM_NULL)` succeeds, but `IsHungAppWindow` = TRUE. A **`Ghost`-class window owned by DWM** covers the game window.
- **Input handling:** the game's message pumps (`0x480140`, `0x480230`) only peek or remove selected message ranges, skipping the mouse (0x200–0x209) and keyboard (0x100–0x108) ranges. The game reads input with GetAsyncKeyState/GetCursorPos.
- **Result:** input messages sit in the queue, Windows decides the window is hung, and DWM shows a frozen ghost image on top of the still-running game. Exclusive fullscreen never shows ghosts, which is why this is windowed-only.

**Fix:** `DisableProcessWindowsGhosting()` at DLL load (windowed/borderless modes). `keep_focus=1` is also kept on.

## Scaling play-test (2026-10-01)
Borderless, scale=auto (2× → 1600×1200 on 1920×1200), nearest filter. Play-testing confirmed that all clicks land correctly and that edge scrolling works.

## Modes overview (2026-10-01)
| `mode=` | Monitor | Picture | Presented by |
|---|---|---|---|
| `fullscreen` | Switched to 800×600×8 by the game | Original | Original DirectDraw/GDI (DLL passes through) |
| `exclusive` | Switched to 800×600×32 by DXGI | 1:1 | D3D11 exclusive fullscreen |
| `borderless` | Unchanged | Whole-factor scale, centred | D3D11 or GDI |
| `windowed` | Unchanged | Any 4:3 size, resizable | D3D11 or GDI |
