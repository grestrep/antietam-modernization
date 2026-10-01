# Resolution investigation: can Antietam run at anything but 800×600?

Read-only analysis of `SMAntietam1210.exe` (v12.10 plus the GlobalFree patch), 2026-10-01.
Tools: Capstone disassembly of the whole `.text` section, string and constant scans, and PCX header survey of the game data.

## Short answer
- **No built-in alternative resolution exists.** There is no ini key, command-line switch, menu option or code path for 640×480, 1024×768 or anything else. The screen size is 800×600, hard-coded.
- **A true 1024×768 (more battlefield visible) would mean patching on the order of 100+ code sites, plus new interface art.** This is days to weeks of reverse engineering, with layout risk. **Scaling** the 800×600 picture (`docs/ddraw-dll.md` §6) needs none of that.

## Evidence

### 1. No configuration for resolution
- **Command-line switches** found in the exe's strings: `-campaign N` (map set) and `-nohome` (purpose unknown). Nothing about resolution.
- **ini keys** (`antietam.ini`, `[Sid Meier's Antietam]`): Interface Pref., Master Volume, Campaign, FiringWidth, ShowFiringDamage, SeparateFiringColors, CenterDescription, bOverloadBarOrder and the bar-order keys. Nothing about screen size.
- **No strings** such as "resolution", "1024", "640x", "800x" or "screen size".
- **Runtime trace** (`docs/ddraw-trace.md`): the game calls `SetDisplayMode(800,600,8)` and never enumerates display modes (`EnumDisplayModes` count 0). It doesn't even look at what the monitor supports.

### 2. Hard-coded screen-size constants (immediate operands in the code)
| Value | Count | Notes |
|---|---|---|
| 800 (0x320) | 54 | 44× `push`, 6× `mov`, 4× `cmp` |
| 600 (0x258) | 49 | 31× `push`, 14× `cmp`, 4× `mov` |
| 800 and 600 within 40 bytes of each other | **32 sites** | Typical pattern `push 600; push 800` (15 exact occurrences, e.g. 0x408027, 0x42B58A, 0x4424B1, 0x445B45): full-screen blits/clears and screen-size rectangles |
| 400 / 300 (half of 800 / 600) | 54 / 17 | Likely centring maths; some are unrelated |
| 799 / 599 | 1 / 4 | Clipping to the last pixel |
| 1024 / 768 | 57 / 14 | Only 2 sites near each other. Practically all are unrelated (0x400 flags, buffer sizes) |
| 640 / 480 | 36 / 121 | 12 paired range checks, e.g. 0x4074B4 `0 ≤ x < 640 && 0 ≤ y < 480`. These match the **640×480 sprite-sheet size** of the unit art (98 PCX files such as `artillry.pcx` and `brigade.pcx`), not a screen mode |

The 800/600 constants are spread over **24 different 4 KB code pages**, i.e. many separate routines (menus, map view, panels, reports, dialogs), not one central setting.

### 3. Art is made for 800×600
Survey of the 692 PCX files in the game folder:
| Size | Files | Use |
|---|---|---|
| **800×600** | 115 | Full-screen backgrounds: menus, reports, book and briefing screens |
| 800×500 | 8 | Map overview images (e.g. `crampmap1.pcx`) |
| 640×480 | 98 | Unit sprite sheets |
| 320×240 / 320×238 | 133 / 27 | Scenario pictures and maps |
| Small (54×78, 110×160, 70×210, …) | many | Portraits and buttons, positioned for an 800-pixel-wide layout |

Even with every code constant changed, full-screen screens would show 800×600 art in a 1024×768 frame. The bottom command panel and the menu bar are laid out for 800 pixels.

## What a true 1024×768 would take (estimate)
1. **Mode switch:** trivial. Our ddraw.dll can already report any mode.
2. **Code:**
   - Review roughly **100–150 immediate-operand sites** (the 32 paired 800/600 sites, about 70 other 800/600 uses, and relevant 400/300/799/599 uses).
   - Decide per site whether it's the screen size or something else, and patch it.
   - Then find **computed** sizes (values loaded from tables or globals), which a constant scan can't see.
3. **Map view:** the battlefield viewport is the part worth enlarging. It's probably confined to fewer routines, but its size feeds scrolling, picking (mouse → tile) and the minimap.
4. **Interface and art:** the panels and full-screen screens either get centred at 800×600 inside 1024×768 (with borders) or need new art.
5. **Testing** every screen: menus, battle, reports, multiplayer lobby, the editor (if any).

**Realistic path if wanted later:** a research spike that widens only the battlefield map view and keeps every menu at 800×600 (centred). If that works without picking or scrolling bugs, extend from there.

## Recommendation
Do **scaling** in the ddraw.dll first (2×, or fit-to-screen with correct aspect ratio). It's quick, reversible and fixes the "too small on a modern monitor" problem. Treat a true higher resolution as a separate research project.
