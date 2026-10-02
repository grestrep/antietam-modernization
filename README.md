# Antietam Modernization Project

**An unofficial fix that makes *Sid Meier's Antietam!* and *South Mountain* (Firaxis, 1999–2000) playable on Windows 10 and 11.**

- **No more crashes:** fixes the crashes at "Loading Sounds" and about 30 seconds into a game.
- **Modern display:** borderless fullscreen or a resizable window. Your monitor never changes resolution, and Alt-Tab works instantly.
- **Sharp scaling:** the 800×600 picture is enlarged with sharp pixels (2× = 1600×1200) or a crisp filter at any 4:3 size.
- **Graphics card:** rendered through Direct3D 11 on your graphics card, with v-sync and the classic GDI path as a fallback.
- **No exe patching:** your game files are never modified. Everything happens in one drop-in `ddraw.dll`.

> Unofficial fan project. Not affiliated with, endorsed or sponsored by Firaxis Games, 2K or Take-Two Interactive. *Sid Meier's Antietam!* and *Sid Meier's South Mountain* are trademarks of their respective owners. **You need your own copy of the game.**

---

## Install (players)

**You need:**
- **The game: *Sid Meier's Antietam!*** with one of its two official patches. The *Civil War Collection* already includes the final one.
  - **v3.0 Final Patch** (with South Mountain), Aug 2000: what the Civil War Collection installs. **Needed for South Mountain.**
  - **[v2.0 Beta Patch](https://www.moddb.com/games/sid-meiers-gettysburg/downloads/antietam-beta-patch-v2-0)**, Feb 2000: the community uses it for player-made battle packs, which the final patch's maps don't support. **Antietam only:** this older build can't run South Mountain.

  The installer checks which one you have and creates the matching shortcuts.
- **Windows 10 or 11.** Nothing else: no compatibility modes, no "Run as administrator", no DirectX installer.

**Steps:**
1. **Download** the latest `antietam-modernization-x.y.z.zip` from [Releases](../../releases).
2. **Unzip it into your game folder**, the folder that contains `Antietam.exe`. In the Civil War Collection this is `…\Sid Meier's Civil War Collection\SMA`.
3. **Double-click `install.bat`** in that folder.
4. **Play** with the new desktop shortcuts: **Antietam (Win11)** and, with the v3.0 Final Patch, **South Mountain (Win11)**.

The installer:
- **Checks** which game version you have.
- **Backs up** an existing `ddraw.dll`/`smav2.ini`.
- **Installs** `ddraw.dll` + `smav2.ini`.
- **Makes a renamed copy** of your exe, `Antietam_Win11.exe`, without changing it. Windows applies a broken compatibility fix to any program called `Antietam.exe`, and the copy avoids it.
- **Creates** the desktop shortcuts.

Several copies of the game? Run the installer in each folder.

**Uninstall:** run `uninstall.bat` in the same folder. Everything goes back exactly as it was.

## Settings
Open `smav2.ini` in the game folder with Notepad. Each option is explained in the file.

| Setting | Options |
|---|---|
| `mode` | `borderless` (default: fills the screen, picture centred), `windowed` (resizable window), `exclusive` (real 800×600 fullscreen), `fullscreen` (the original behaviour; the DLL stays out of the way) |
| `window_size` | Windowed size, e.g. `1024x768`, `1152x864`, `1280x960` |
| `scale` | `auto` (largest whole factor that fits) or `1`, `2`, `3` |
| `filter` | `auto`, `nearest` (sharp pixels), `sharp` (crisp at any size), `linear` (smooth), `scale2x` (smooths jagged diagonals) |
| `renderer` | `d3d11` (graphics card, default) or `gdi` (CPU) |
| `gpu` | `high` (dedicated GPU, default), `low` (integrated), `default` |
| `vsync` | `1` / `0` |
| `skip_intro` (section `[game]`) | `1` = skip the logo videos at startup, `0` = play them (default) |
| `max_fps` (section `[game]`) | Frame limit, mainly for edge-scroll speed: `40` (default), lower = slower scrolling, `0` = unlimited (original, very fast) |

## Problems?
- **Bug reports:** `smav2_ddraw.log` in the game folder records what the DLL did. Please attach it to a [bug report](../../issues/new/choose), with your Windows version, graphics card and game version.
- **Intro videos don't show:** set `swap=blt`.
- **To compare with the original:** set `mode=fullscreen`.

## Supported game versions
The two official patches each install their own `Antietam.exe`. The internal version numbers (v12.10, v9.84) are what the exe reports about itself and what our log file shows.

| Patch | Exe | Size | Date | Antietam | South Mountain | Battle packs |
|---|---|---|---|---|---|---|
| **v3.0 Final Patch w/ South Mountain** (Civil War Collection) | v12.10 | 806,912 B | 2000-08-07 (`0x398EF5B0`) | Yes | Yes | No: opens the South Mountain menu instead |
| **[v2.0 Beta Patch](https://www.moddb.com/games/sid-meiers-gettysburg/downloads/antietam-beta-patch-v2-0)** (player-made battle packs) | v9.84 | 770,048 B | 2000-02-11 (`0x38A45BF6`) | Yes | No: lists the scenarios, but loads the Antietam map and quits | Yes |
| Original retail v1.0 | — | 1,142,784 B | 1999-11-04 | Not supported | — | — |

Sources: [dvwjr's "Antietam! on XP" guide](https://warfare.ueuo.com/smx/GOS/Antietam_on_XP.html) lists the files in each patch, and the readme of the Beta Patch download confirms the sizes and dates. The exe in Ernie's South Mountain battle pack is byte-identical to the v3.0 exe, so South Mountain works with the v3.0 exe both as the official add-on and as a JSGME mod.

**Campaigns:** each map set has a number: 1 = Antietam, 3 = South Mountain, 20–99 = player-made battle packs. The v3.0 exe takes it from its `-campaign` switch, so its shortcuts pass `-campaign 1` or `-campaign 3`. The v2.0 beta exe reads `Campaign=N` from `antietam.ini`, so its shortcut passes nothing. Our DLL never changes the campaign.

## Mods and battle packs
- **Player-made battle packs** (e.g. Austerlitz, `ANGVforSMA`) need the **v2.0 Beta Patch exe**. Enable the pack in JSGME (the usual mod manager for these packs) and start the game with the **Antietam (Win11)** shortcut. The pack's own `antietam.ini` selects its map. Tested with Austerlitz: menu, scenario list and battle all work.
- The **v3.0 exe can't play battle packs.** It only knows Antietam and South Mountain, and opens South Mountain for any other campaign number (tested).
- **Ernie's South Mountain battle pack** is the exception: it ships the v3.0 exe and works with it.
- Both exes can live side by side: install in two copies of the game folder (one per exe). The installer adds the folder name to the second set of shortcuts.

**Old Windows 98 DLLs:** the original setup copied Windows 98 versions of `setupapi.dll` and `cfgmgr32.dll` into the game folder, which breaks sound and stability on newer Windows. If they are there, the installer moves them into `_modernization_backup` (the uninstaller puts them back).

---

## How it works (developers)
The game imports `ddraw.dll` by name. Windows loads the copy in the game folder first, so our DLL:
1. **Fixes the crashes.** It redirects the game's `GlobalFree` calls (made on memory from its own C runtime, which corrupts the heap on modern Windows) to the game's own `free()`. The new exe name avoids Windows' EmulateHeap shim.
2. **Fakes the 800×600×8 mode switch** and takes over the game window.
3. **Redirects the game's GDI drawing** into an off-screen 800×600 "virtual screen" and presents it scaled through Direct3D 11 (or GDI), translating mouse coordinates both ways.
4. **Fixes windowed-mode issues:** DWM "Not Responding" ghosting, focus loss and present pacing (input lag).

Start with **[docs/architecture.md](docs/architecture.md)** (diagrams). Details:

| Document | Contents |
|---|---|
| [docs/ddraw-dll.md](docs/ddraw-dll.md) | Design and code guide for the DLL |
| [docs/WINDOWS11_FIX.md](docs/WINDOWS11_FIX.md) | The crash investigation |
| [docs/exe-patch.md](docs/exe-patch.md) | The GlobalFree fix as code |
| [docs/ddraw-trace.md](docs/ddraw-trace.md) | What the game does with DirectDraw |
| [docs/mode-switch.md](docs/mode-switch.md) | Windowed mode, the ghost-window "freeze", latency |
| [docs/resolution-investigation.md](docs/resolution-investigation.md) | Why 800×600 is hard-coded |

### Build
Requires Visual Studio 2022 or newer with **Desktop development with C++** (x86 tools + Windows SDK).
```
ddraw\build.bat            -> ddraw\build\ddraw.dll (+ .pdb)
tools\logger\build.bat     -> tools\logger\build\ddraw.dll (DirectDraw call tracer)
scripts\package.ps1        -> dist\antietam-modernization-<version>.zip
```

**VS Code:**
1. Open the repo folder and install the C/C++ extension.
2. Set the environment variable `ANTIETAM_GAME_DIR` to a game folder that has been installed with `install.bat`.
3. Use it:
   - **Ctrl+Shift+B** builds.
   - **F5** builds, deploys to `ANTIETAM_GAME_DIR` and starts the game under the debugger.

CI (GitHub Actions) builds every push and attaches the release zip to tags.

### Layout
| Path | What |
|---|---|
| `ddraw/` | The DLL: `smav2_ddraw.cpp`, `present_d3d11.cpp/.h`, `shaders.hlsl`, `ddraw.def`, `smav2.ini`, `build.bat` |
| `installer/` | `install.ps1/.bat`, `uninstall.ps1/.bat` |
| `scripts/` | Packaging |
| `tools/logger/` | Logging proxy `ddraw.dll` used to trace the game's DirectDraw calls |
| `tools/patch/` | The original exe patchers (historical; not needed with the DLL) |
| `tools/analysis/` | Analysis scripts |
| `docs/` | Write-ups and diagrams |

## License
[MIT](LICENSE) for everything in this repository. The game itself is not included and remains the property of its owners.
