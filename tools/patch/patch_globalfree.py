"""Original, hand-addressed GlobalFree patch for the v9.84 build ONLY (Antietam.exe, 770,048 bytes).

This is the first version of the fix. It produced SMA\\SMAntietam.exe and is kept so that
file can be reproduced exactly. For any other build, or for a new patch, use
patch_globalfree_auto.py: same fix, but all addresses are auto-detected. (The auto
version places the stub 16 bytes later, at 0x4A6850, so its output is not byte-identical
to this one.)

What it does (see patch_globalfree_auto.py for the full explanation):
  * writes a 22-byte stub at 0x4A6840 (zero padding after .text):
        if (p) free(p); return NULL;         free() = the game's CRT free at 0x49C68A
  * grows .text VirtualSize 0xA5832 -> 0xA5900 so the stub is mapped
  * redirects all 65 references to the GlobalFree import slot (0x4A712C) to the stub:
        56x  FF 15 <slot>  call [GlobalFree]     -> E8 <rel32> 90   call stub; nop
         7x  8B 2D <slot>  mov ebp,[GlobalFree]  -> BD <stub>  90   mov ebp, stub; nop
         2x  8B 1D <slot>  mov ebx,[GlobalFree]  -> BB <stub>  90   mov ebx, stub; nop
The asserts below refuse to touch any other build.

Usage:  python patch_globalfree.py Antietam.exe SMAntietam.exe
"""
import re
import struct
import sys

src, dst = sys.argv[1], sys.argv[2]
d = bytearray(open(src, 'rb').read())

IB, TEXT_VA, TEXT_RAW = 0x400000, 0x1000, 0x1000   # image base, .text RVA, .text file offset
FREE = 0x49c68a    # the game's statically linked MSVC6 CRT free()
SLOT = 0x4a712c    # IAT entry of KERNEL32!GlobalFree
STUB = 0x4a6840    # where the stub goes (zero padding after the last code byte)

# --- sanity checks: is this the v9.84 exe? -------------------------------------------------
pe = struct.unpack_from('<I', d, 0x3c)[0]
opt = pe + 24
optsz = struct.unpack_from('<H', d, pe + 20)[0]
sh = opt + optsz                                    # first section header
assert d[sh:sh + 5] == b'.text'
vsize = struct.unpack_from('<I', d, sh + 8)[0]
assert vsize == 0xa5832, 'not the v9.84 build'
fo = lambda va: va - IB - TEXT_VA + TEXT_RAW        # virtual address -> file offset
assert d[fo(FREE):fo(FREE) + 5] == bytes.fromhex('568b742408')   # free(): push esi; mov esi,[esp+8]
s = fo(STUB)
assert not any(d[s - 8:s + 32]), 'code cave is not empty'

# --- 1. the stub: GlobalFree-compatible (stdcall, 1 arg, returns NULL), calls CRT free -----
stub = (bytes.fromhex('8b44240485c0740b50')      # mov eax,[esp+4]; test eax,eax; je +0B; push eax
        + b'\xe8' + struct.pack('<i', FREE - (STUB + 14))   # call free
        + bytes.fromhex('83c40431c0c20400'))     # add esp,4; xor eax,eax; ret 4
assert len(stub) == 22
d[s:s + 22] = stub

# --- 2. make the stub part of the mapped .text section -------------------------------------
struct.pack_into('<I', d, sh + 8, 0xa5900)

# --- 3. redirect every GlobalFree reference (6-byte patches, nothing moves) ----------------
n = 0
for m in list(re.finditer(re.escape(b'\xff\x15' + struct.pack('<I', SLOT)), bytes(d))):
    va = IB + TEXT_VA + m.start() - TEXT_RAW
    d[m.start():m.start() + 6] = b'\xe8' + struct.pack('<i', STUB - (va + 5)) + b'\x90'
    n += 1
for op, mov in ((b'\x8b\x2d', 0xbd), (b'\x8b\x1d', 0xbb)):   # mov ebp/ebx,[GlobalFree]
    for m in list(re.finditer(re.escape(op + struct.pack('<I', SLOT)), bytes(d))):
        d[m.start():m.start() + 6] = bytes([mov]) + struct.pack('<I', STUB) + b'\x90'
        n += 1
assert struct.pack('<I', SLOT) not in d[TEXT_RAW:TEXT_RAW + 0xa6000], 'unpatched GlobalFree reference left'

open(dst, 'wb').write(d)
print('patched sites:', n)
