# Changelog

## 0.11.0 (beta) - 2026-10-03
- **Mouse-wheel zoom (`[game] wheel_zoom`, on by default):** each wheel notch is one press of the game's own zoom keys (Z = in, X = out), so the wheel steps through its six zoom levels. Ignored while a game dialog is open.
- **Numpad map scrolling (`[game] numpad_map_scroll`, on by default):** numpad 8/4/6/2 scroll the battle map up/left/right/down, and 5 also scrolls down for an 8/4/5/6 layout like W/A/S/D (two keys held = diagonal), using the game's own edge scrolling, so the speed matches the mouse and `max_fps`. The arrow keys still rotate the view. With Num Lock on, scrolling starts at once (the keys are read directly, as the game reads its messages only every ~150 ms); with Num Lock off it works too, but starts about 0.2 s late; outside the battle map and in dialogs the numpad keys behave as before.

## 0.10.0 (beta) - 2026-10-02
- **Skip the intro videos (`[game] skip_intro=1`, off by default):** the logo videos are seeked to their end before they play, so the game gets its normal "finished" notification and goes straight to the menu.
- **Intro videos in long folder paths:** MCI rejects file names longer than about 128 characters, and the game then waited forever on a black screen. A failed video open is now retried with the short (8.3) path.
- **Scenario lists at full size:** the scenario list and the Battle History chapter list are separate windows the game draws at 1:1 size; they are now enlarged like the rest of the screen, and clicks land on the right item.
- **Pop-ups inside the game window:** "Select scenario type", the Save/Load dialogs and the Battle History text pane used to appear at the monitor's top-left corner. They now appear inside the game picture (centred dialogs stay centred), at their original size.
- **Edge-scroll speed limit (`[game] max_fps`, default 40):** the game moves the map a step per frame and runs hundreds of frames per second on modern PCs, so edge scrolling was far too fast. The DLL now paces whole-screen frames to `max_fps` (same approach as DDrawCompat's FpsLimiter: one high-resolution wait per frame, no catch-up bursts). Menus, dialogs and highlights are not paced. `0` turns it off.

## 0.9.0 (beta) - 2026-10-01
First public beta. Requires Sid Meier's Antietam! with the v3.0 Final Patch (exe v12.10, as in the Civil War Collection: Antietam + South Mountain) or the v2.0 Beta Patch (exe v9.84: Antietam only).

- **Old Windows 98 DLLs:** the installer moves `setupapi.dll` / `cfgmgr32.dll` out of the game folder if present.

### Fixes for Windows 10/11
- **"Loading Sounds" crash:** fixed. The game now runs as `Antietam_Win11.exe`, an unchanged copy made by the installer, which avoids Windows' EmulateHeap compatibility shim.
- **Crash about 30 s into a game:** fixed. The game's `GlobalFree` calls are redirected to its own `free()` at load time, so no exe patching is needed.
- **Fake "Not Responding" freeze in a window:** fixed. DWM window ghosting is disabled.
- **Game pausing or freezing after losing focus in a window:** fixed (`keep_focus`).

### Display
- **Emulated 800×600×8 mode switch:** the monitor keeps its resolution.
- **Modes:** `borderless` (default), `windowed` (any 4:3 size, resizable/maximizable), `exclusive` (real 800×600 via DXGI) and `fullscreen` (original behaviour).
- **Scaling:** integer scaling, plus filters `nearest`, `sharp` (sharp-bilinear), `linear` and `scale2x`.
- **Direct3D 11 renderer:** flip model, v-sync, its own render thread, picks the high-performance GPU, and falls back to GDI.
- **Per-monitor DPI awareness.**
- **Low-latency present pacing:** about 2 ms from drawing to screen, down from about 79 ms.
- **Shortcuts match the build:** v12.10 gets Antietam (`-campaign 1`) and South Mountain (`-campaign 3`). v9.84 gets one Antietam shortcut with no switch, so JSGME battle packs keep their own `Campaign=` setting. The DLL never changes the campaign.
- **Mouse:** coordinate translation for any size, and the cursor is clipped to the picture so edge scrolling works.

### Tooling
- **Installer/uninstaller:** runs in the game folder, backs up existing files and creates shortcuts.
- **Logging:** a log with game build identification, plus an optional hang watchdog.
