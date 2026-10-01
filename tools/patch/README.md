# Exe patchers (historical)

These scripts patch a **copy** of `Antietam.exe` so that its `GlobalFree` calls go to the game's own CRT `free()`. That was the first Windows 11 fix (see `docs/exe-patch.md`).

**You don't need them.** The released `ddraw.dll` applies the same fix at load time without modifying any file. They're kept as documentation and for research. Never distribute a patched exe: it is the game publisher's copyrighted program.

- `patch_globalfree_auto.py <Antietam.exe> <output.exe>`: any build. All addresses are auto-detected, and it refuses to patch if anything is ambiguous.
- `patch_globalfree.py`: the original v9.84-only version.
