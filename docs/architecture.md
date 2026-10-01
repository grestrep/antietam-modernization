*Sid Meier's Antietam! · Windows 10/11*

# How our ddraw.dll works

The game was built in 2000 for a 800×600, 256-colour fullscreen screen. Our DLL sits between the game and Windows, keeps the game believing it has that screen, and shows its picture the modern way: enlarged, sharp, in a window or borderless, through the graphics card.

## 1. Where the DLL sits

The game asks Windows for `ddraw.dll` by name. Windows looks in the game folder first, so it gets ours. We load the real Windows DirectDraw ourselves and pass most things on. Only the parts that matter are changed.

```mermaid
flowchart TB
  subgraph Game["Antietam_Win11.exe (your unchanged Antietam.exe)"]
    G1["Game logic + its own CPU renderer<br/>builds every 800×600 frame in memory"]
    G2["Mode switch<br/>DirectDraw: 800×600×8"]
    G3["Copies finished frames to its window<br/>GDI: GetDC, BitBlt, TextOut..."]
    G4["Reads the mouse<br/>GetCursorPos, ScreenToClient, GetAsyncKeyState"]
  end
  subgraph Ours["smav2 ddraw.dll (game folder)"]
    D1["DirectDraw interception<br/>fakes the mode switch"]
    D2["Import hooks<br/>redirect drawing + translate mouse"]
    D3["Virtual screen<br/>800×600 picture in memory"]
    D4["Presenter<br/>Direct3D 11 (GPU) or GDI (CPU)"]
    D5["Window manager<br/>borderless / windowed / exclusive"]
  end
  subgraph Win["Windows"]
    W1["Real ddraw.dll"]
    W2["GDI"]
    W3["Direct3D 11 / DXGI"]
    W4["Your monitor, e.g. 1920×1200"]
  end
  G2 --> D1 --> W1
  G3 --> D2 --> D3 --> D4
  G4 --> D2
  D4 --> W3 --> W4
  D4 -. renderer=gdi .-> W2 --> W4
  D1 --> D5
```

## 2. Start-up: faking the 800×600 switch

The game only uses DirectDraw for the screen-mode switch. We let it create the real DirectDraw object, then swap five of its functions for ours. From then on the game thinks the monitor is at 800×600; in reality nothing changes, and we take over its window.

```mermaid
sequenceDiagram
  participant G as Game
  participant D as Our DLL
  participant W as Windows
  G->>D: DirectDrawCreate()
  D->>W: real DirectDrawCreate()
  D-->>G: DirectDraw object (5 functions patched)
  G->>D: SetCooperativeLevel(EXCLUSIVE fullscreen)
  D->>W: SetCooperativeLevel(NORMAL)
  Note over D: take over the game window:<br/>borderless 1920×1200 or a window,<br/>start presenter + watchdog
  G->>D: SetDisplayMode(800, 600, 8 bit)
  Note over D: remember 800×600, monitor untouched
  D-->>G: OK
  G->>D: GetSystemMetrics(screen size)
  D-->>G: 800 × 600 (the size it expects)
```

> With `mode=fullscreen` none of this happens: the DLL passes every call straight to Windows and the game runs exactly like the original.

## 3. One frame: from the game to your screen

The game draws into what it thinks is its window. Our hooks redirect every drawing call into the **virtual screen**, an invisible 800×600 picture. When the game finishes a frame, or draws even a small change such as a hover highlight, we present: enlarge the picture and put it on the real screen.

```mermaid
flowchart LR
  A["Game draws frame<br/>BitBlt / TextOut / FillRect<br/>on its window DC"] --> B{"Our hooks:<br/>is it the game window?"}
  B -- yes --> C["Draw into virtual screen<br/>800×600, 32-bit"]
  B -- no --> X["Real GDI<br/>(menus of other windows etc.)"]
  C --> P{"When to present?"}
  P -- "frame finished<br/>or big blit" --> S["Present now"]
  P -- "small change<br/>(hover, drag box)" --> T["Present now, at most<br/>every 2 ms (6 ms GDI)"]
  P -- "had to wait" --> F["Flush from timeGetTime /<br/>GetAsyncKeyState hooks<br/>(called thousands of times a second)"]
  S --> R["Presenter"]
  T --> R
  F --> R
```

## 4. The presenter: two threads, no waiting

With `renderer=d3d11` the game thread only copies the finished picture (about 0.2 ms) and signals a separate render thread. That thread owns the graphics card connection, uploads the picture as a texture, runs the chosen filter shader and waits for v-sync. The game never waits for the GPU, so its own timing stays exactly as Firaxis made it.

```mermaid
flowchart LR
  subgraph T1["Game thread"]
    a1["Present()"] --> a2["copy 800×600 frame<br/>into shared buffer"] --> a3["wake render thread"]
  end
  subgraph T2["Render thread (ours)"]
    b1["upload as GPU texture"] --> b2["clear black bars"] --> b3["draw picture rectangle<br/>with filter shader"] --> b4["Present with v-sync<br/>(1 frame queued max)"]
  end
  a3 --> b1
  b4 --> M["Monitor"]
```

| Filter | What it does | Best for |
|---|---|---|
| `nearest` | Each game pixel becomes a solid block | Whole factors: 2× (your 1600×1200) |
| `sharp` | Solid blocks, blended only along their edges | Odd sizes: 1024×768, 1280×960 |
| `linear` | Smooth blend everywhere | Softer look |
| `scale2x` | Rounds off jagged diagonal pixel edges | Trying a smoothed pixel-art look |
| `auto` | nearest at whole factors, sharp otherwise | Default |

> If Direct3D 11 cannot start, or the graphics driver resets twice, the DLL switches to the GDI presenter by itself: the CPU does the enlarging with Windows' old drawing functions.

## 5. The mouse: translating coordinates

The game expects a 800×600 screen with the mouse between 0 and 799. On your monitor the picture is 1600×1200 starting 160 pixels from the left. Our hooks translate every position the game reads, and the cursor is kept inside the picture so edge scrolling works.

```mermaid
flowchart LR
  M["Real mouse<br/>screen (1100, 600)"] --> H["ScreenToClient hook<br/>(1100 − 160) ÷ 2, 600 ÷ 2"] --> V["Game sees<br/>(470, 300)"]
  V2["Game asks to put something at<br/>game (470, 300)"] --> H2["ClientToScreen hook<br/>160 + 470 × 2, 300 × 2"] --> R["Real screen<br/>(1100, 600)"]
```

- **Mouse messages** (clicks, moves) are translated the same way before the game's window code sees them.
- **Clip**: while the game is active the cursor cannot leave the picture, so touching the edge scrolls the map.
- **Any size**: the maths is exact for any window size, which is why resizing and maximising work.

## 6. Keeping an old game happy in a modern window

| Problem | Cause | What the DLL does |
|---|---|---|
| Fake "Not Responding" freeze | The game never reads its mouse/keyboard messages, so Windows covered it with a frozen ghost image | Turns ghosting off for the game |
| Game pausing on focus loss | Written for exclusive fullscreen | Never tells the game it lost focus (`keep_focus`) |
| Game moving its window to (0,0) | It still thinks it is fullscreen | Ignores the game's own window moves; you can still move/resize it |
| Mouse lag on hover and drag | Small updates waited for a timer the game only processes every ~160 ms | Presents small changes immediately (79 ms → 2 ms) |

## 7. The four modes

| `mode=` | Monitor | Picture | Drawn by |
|---|---|---|---|
| `borderless` (default) | Stays 1920×1200 | Whole-factor enlarged, centred (2× = 1600×1200) | Direct3D 11 or GDI |
| `windowed` | Stays 1920×1200 | Any 4:3 size, resizable, e.g. 1024×768 | Direct3D 11 or GDI |
| `exclusive` | Switches to 800×600 (full colour) | 1:1 | Direct3D 11 exclusive fullscreen |
| `fullscreen` | Switches to 800×600×256 colours | Original | The game itself (DLL passes through) |

## 8. Files

| File (`ddraw/`) | Role |
|---|---|
| `smav2_ddraw.cpp` | DirectDraw interception, window manager, import hooks, virtual screen, GDI presenter, watchdog |
| `present_d3d11.cpp / .h` | Direct3D 11 presenter and its render thread, exclusive fullscreen |
| `shaders.hlsl` | The filter shaders (compiled into the DLL at build time) |
| `ddraw.def` | The DLL's export list, identical to Windows' ddraw.dll |
| `smav2.ini` | Your settings: mode, scale, window size, filter, renderer, v-sync |
| `build.bat` | Builds everything with Visual Studio |

> The Windows 11 crash fixes are in the DLL too: it redirects the game's wrong memory-free calls (GlobalFree) to the game's own correct routine, and the installer runs the game under a new name (`Antietam_Win11.exe`) so Windows' broken compatibility fix for `Antietam.exe` doesn't apply. Your `Antietam.exe` is never modified.

---
An HTML version with the same diagrams is in `architecture.html`; open it in a browser. The technical details are in `ddraw-dll.md`.
