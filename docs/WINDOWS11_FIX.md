# Sid Meier's Antietam! on Windows 11: crash analysis and fix

> **Investigation log.** This is how the two Windows 11 crashes were found and first fixed, by patching a copy of the exe.
> The released project no longer patches the exe. `ddraw.dll` applies the same GlobalFree fix at load time (see [ddraw-dll.md](ddraw-dll.md) §3.0), and the installer runs the game under a new name (`Antietam_Win11.exe`).
> Names like `SMAntietam.exe` / `SMAntietam1210.exe` below are the patched copies made during the investigation.

---

## 1. Environment

| Item | Value |
|---|---|
| OS | Windows 11 Home 10.0.26200 (x64; the game runs under WOW64 as 32-bit) |
| Game folder | `<game folder>` |
| `Antietam.exe` | 770,048 bytes, PE timestamp `0x38a45bf6` (2000-02-11) |
| `sound.dll` | 577,536 bytes, PE timestamp `0x3855032a` (1999-12-13), Firaxis' own sound engine |
| Other copy (not used) | `SMA - Copy\Antietam.exe`: 806,912 bytes, `0x398ef5b0` (2000-08-07 build) |

### Symptom
The game starts, the main menu shows **"Loading Sounds"**, and then the game hangs and crashes to the desktop.

---

## 2. Investigation

### 2.1 Windows Application event log
Two kinds of crash were logged (all `0xc0000005`, access violation):

| Faulting module | Offset | When it happened |
|---|---|---|
| `AcGenral.DLL` 10.0.26100.9444 | `0xb5427`, `0xb5f76`, `0xb5251`, `0xb604f` (varies) | Exe named `Antietam.exe`, compatibility shims active |
| `winmmbase.dll` 10.0.26100.9278 | `0x5ae3` | Exe named `Antietam.exe` with a lighter or overridden layer set |

`AcGenral.DLL` is not part of the game. It is Windows' **Application Compatibility shim engine** ("General" shim pack).

### 2.2 Compatibility layers that were set
In `HKCU` and `HKLM\Software\Microsoft\Windows NT\CurrentVersion\AppCompatFlags\Layers`:

- `HKCU` … `SMA\Antietam.exe` = `~ DWM8And16BitMitigation RUNASADMIN WINXPSP3`
- `HKLM` … `SMA\Antietam.exe` = `$ DWM8And16BitMitigation`

### 2.3 PE imports (what the game uses)
- **Antietam.exe:** DDRAW (`DirectDrawCreate`), DPLAYX, WINMM (mmio\*, timers, `mciSendCommandA`), MSVFW32, GDI32, USER32, KERNEL32, ADVAPI32, SHELL32, ole32.
  - It loads `sound.dll` at runtime with `LoadLibrary`/`GetProcAddress`.
  - It imports **`GlobalFree` but not `GlobalAlloc`/`GlobalLock`/`LocalAlloc`**. This turned out to be the key clue (see Issue 2).
- **sound.dll:** DSOUND (ordinals 1 and 2, `DirectSoundCreate`/`DirectSoundEnumerateA`), WINMM (waveIn\*, mmio\* including the direct-buffer APIs `mmioSetBuffer`/`mmioGetInfo`/`mmioAdvance`/`mmioSetInfo`, timers), ole32. It does not use Miles (no `AIL_`/`mss32`).

### 2.4 Sound data
- There are 175 `.wav` files, all valid PCM RIFF files (`fmt `, `data`, plus `LIST`/`smpl`/`cue ` chunks). There are no truncated or malformed chunks, so the sound files themselves are not the cause.

### 2.5 Debugging
The game was run under `gdb` (MSYS2, WOW64 32-bit target) to capture the stack at the moment of the crash.

---

## 3. Issue 1: Windows' built-in EmulateHeap shim crashes while sounds load

### Cause
Windows' built-in compatibility database (`sysmain.sdb`) has an entry that matches a program **named `Antietam.exe`**. It applies the **EmulateHeap** shim from `AcGenral.DLL` automatically, whatever you choose on the Compatibility tab. EmulateHeap replaces the Win32 heap with an emulation of the Windows 9x heap.

On current Windows 11 builds this shim crashes. The captured stack:

```
#0  AcGenral.dll   (EmulateHeap internals; mov eax,[ebx] -> access violation)
#1-#4 AcGenral.dll
#5  winmmbase.dll!mmioSetBuffer+0x14f
#6  sound.dll      (WAV loader: mmioSetBuffer(hmmio, NULL, <data chunk size>, 0))
...
```

`sound.dll` asks mmio to reallocate its I/O buffer to the size of each WAV's `data` chunk. The reallocation goes through the shimmed heap and crashes inside `AcGenral`. That is the "Loading Sounds" crash. The other signature (`winmmbase.dll+0x5ae3`) is the same failure reached by a slightly different path.

Removing `WINXPSP3`/`RUNASADMIN` from the HKCU layer did **not** help. EmulateHeap comes from the system database, not from the user-selected layers.

### Fix
**Rename the executable**, so the system database entry (which matches on the file name) no longer applies.
- Created `SMAntietam.exe` as a copy of `Antietam.exe`. The original is left in place, unmodified.
- The same workaround is reported on VOGONS for this exact crash.

### Result
The game now gets past "Loading Sounds" and runs. With the shim gone, though, it crashes about 30 seconds later in `ntdll.dll` (Issue 2). EmulateHeap had been hiding a real bug in the game.

---

## 4. Issue 2: the game frees memory with the wrong function (`GlobalFree` on `malloc` memory)

### Cause
With EmulateHeap gone, the renamed exe crashed in `ntdll.dll` (offsets `0x47fc4`, `0x50528`). Under the debugger, the heap manager reported:

```
HEAP[SMAntietam.exe]: Invalid address specified to RtlFreeHeap( 00A50000, 02A155B8 )
  ntdll!RtlFreeHeap
  KERNELBASE!GlobalFree
  Antietam.exe+0x72cca
```

Disassembly at `0x472ba0` (the palette setup routine):

```
call 0x49af6e          ; malloc(0x404)          -> esi   (statically linked MSVC CRT heap)
...                    ; fill LOGPALETTE
call [CreatePalette]
push esi
call [GlobalFree]      ; <-- frees a malloc() block with GlobalFree()
```

The game allocates with the C runtime's `malloc`/`new` (a private CRT heap), but frees with Win32 `GlobalFree`, which works on the process heap. Windows 9x tolerated this mismatch. On NT-based Windows it corrupts the heap and crashes later. This is exactly why Microsoft attached EmulateHeap to this game.

The exe never imports `GlobalAlloc`, so **every** `GlobalFree` call in it is the same bug. There are 65 references in total:
- 56 × `call dword ptr [GlobalFree]` (`FF 15 2C 71 4A 00`)
- 7 × `mov ebp, [GlobalFree]` (`8B 2D …`), followed later by `call ebp`
- 2 × `mov ebx, [GlobalFree]` (`8B 1D …`), followed later by `call ebx`

### Fix: binary patch of `SMAntietam.exe` only
Script: `tools/patch/patch_globalfree.py` (`python patch_globalfree.py Antietam.exe SMAntietam.exe`).

1. **Added a 22-byte stub** in unused padding at the end of `.text` (VA `0x4A6840`). It behaves like `GlobalFree` (`__stdcall`, 1 argument, returns NULL on success), but calls the game's own CRT `free()` (found at `0x49C68A`, the MSVC6 `free` with small-block-heap lookup and `HeapFree(_crtheap)`):
   ```asm
   4A6840  mov  eax,[esp+4]
   4A6844  test eax,eax
   4A6846  je   4A6853
   4A6848  push eax
   4A6849  call 0049C68A      ; CRT free()
   4A684E  add  esp,4
   4A6851  xor  eax,eax       ; GlobalFree returns NULL on success
   4A6853  ret  4
   ```
2. **Extended `.text` VirtualSize** from `0xA5832` to `0xA5900` so the stub is part of the mapped section.
3. **Redirected all 65 references** to the stub:
   - `FF 15 <IAT>` → `E8 <rel32 to stub> 90` (direct call + NOP, same 6 bytes)
   - `8B 2D <IAT>` → `BD <stub> 90` (`mov ebp, stub`)
   - `8B 1D <IAT>` → `BB <stub> 90` (`mov ebx, stub`)
4. Checked: no references to the `GlobalFree` import slot remain in `.text`, and the stub disassembles as intended.

### Result
No heap errors. The game runs stably, and a full large battle plays without crashing.

---

## 5. Compatibility settings

The test machine had Windows XP SP3 / Run as administrator / DWM8And16BitMitigation compatibility modes set on `Antietam.exe`. They are not needed and were removed. The renamed exe needs no compatibility settings at all.

## 7. Patched v12.10 build (final Civil War Collection patch, Aug 2000)

The exe originally in `SMA` is **v9.84** (Feb 2000). `SMA - Copy\Antietam.exe` is **v12.10** (806,912 bytes, PE timestamp `0x398ef5b0`), the final retail release (Civil War Collection 1.00f).

The v9.84 exe is byte-identical to the **Antietam v9.84 beta patch** (the beta patch's `Antietam.exe`, same MD5). Comparing the two readmes, v9.84 already contains almost every documented fix and the v9.84 features (F9 preferences, Ctrl-B block art, brigade incoming-fire display). v12.10 adds:

- **South Mountain integration:** the `-campaign N` startup switch and campaign selection. `-campaign 1` = Antietam, `-campaign 3` = South Mountain. This is required to play Crampton's Gap and Turner's Gap.
- **One extra fix:** extra clicks on the pick-player screen no longer cancel the scenario intro. Apart from the exe and the readme, the game data in both folders is identical; the only other differences are files the game writes itself (`gmapd.pcx`, `quit.sav`, `antietam.ini`).

**Which patch is which** (from [dvwjr's "Antietam! on XP" guide](https://warfare.ueuo.com/smx/GOS/Antietam_on_XP.html) and the Beta Patch readme): v9.84 (770,048 B, 2000-02-11) is the exe of the **v2.0 Beta Patch**, which the community uses for player-made battle packs. v12.10 (806,912 B, 2000-08-07) is the exe of the **v3.0 Final Patch w/ South Mountain**, which the Civil War Collection installs and Ernie's South Mountain battle pack also ships. The original retail exe is v1.0 (1,142,784 B, 1999-11-04). Here the Collection was installed (v12.10, kept in `SMA - Copy`) and then the Beta Patch was applied on top, which replaced it with v9.84.

v12.10 has the same two issues, and the same two fixes were applied:
- **Built the patched copy with a generic script.** `tools/patch/patch_globalfree_auto.py` auto-detects the `GlobalFree` import slot, the CRT `free()` (by its MSVC6 byte signature) and the code cave. It refuses to write if anything is ambiguous or if any `GlobalFree` reference is left unpatched.
  - v12.10 values: `GlobalFree` slot `0x4B012C`, CRT `free` `0x4A4D33`, stub `0x4AF370`, **68** references patched.
  - Run on v9.84, it finds the same slot, `free()` and 65 references as the original script.
- **Output:** `SMAntietam1210.exe`. The new name avoids the EmulateHeap entry.
- **Test:** the copy ran for 60 s or more with no crash events, and the user confirmed it plays fine (2026-10-01).
- **How to start:** `-campaign 1` = Antietam, `-campaign 3` = South Mountain (v12.10 only).

| Exe | Version | Status |
|---|---|---|
| `SMAntietam1210.exe` | v12.10 | **Recommended** (latest bug fixes) |
| `SMAntietam.exe` | v9.84 | Works (tested with a large battle) |

---

## 8. Notes
- **Don't apply compatibility modes** (XP SP3, Run as admin, etc.) to `SMAntietam.exe`. It doesn't need them, and XP-era layers can bring heap shims back.
- **Patch matches this build only.** The patch is specific to the 770,048-byte, `0x38a45bf6` build of `Antietam.exe`. The script checks the section size, the `free()` prologue and the empty code cave, and stops if they don't match.
- **Gettysburg may have the same problem.** `SMG\lee.exe` probably has the same two problems (the EmulateHeap entry matched on its name, and the same GlobalFree misuse), and the same approach should work there.
- Reference: VOGONS forum thread on this crash (rename-the-exe workaround): https://www.vogons.org/viewtopic.php?p=1433627
