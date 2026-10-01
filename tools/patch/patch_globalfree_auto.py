"""Windows 11 fix for Sid Meier's Antietam! (any build): the GlobalFree patch.

THE BUG
    The game allocates memory with the C runtime (malloc/new, statically linked MSVC6 CRT)
    but frees some of it with the Win32 function GlobalFree(). The exe never even imports
    GlobalAlloc, so *every* GlobalFree call is such a mismatch. Windows 9x tolerated this;
    on Windows 10/11 it corrupts the heap and the game crashes in ntdll.dll (~30 s in).
    (Windows normally hides this with its EmulateHeap compatibility shim, but that shim
    itself crashes on Windows 11; renaming the exe avoids the shim, this patch fixes the bug.)

THE FIX
    1. Write a 22-byte stub into unused zero padding at the end of the .text section:
           GlobalFree_fix(p):  if (p) free(p);  return NULL;      (stdcall, like GlobalFree)
       where free() is the game's own CRT free.
    2. Grow the .text VirtualSize so the stub is mapped as code.
    3. Redirect every reference to the GlobalFree import slot to the stub:
           FF 15 <slot>   call [GlobalFree]     ->  E8 <rel32>  call stub ; 90 nop
           8B 2D <slot>   mov ebp,[GlobalFree]  ->  BD <stub>   mov ebp, stub ; 90 nop
           (same for ebx 8B 1D/BB, esi 8B 35/BE, edi 8B 3D/BF)
       Every replacement has the same length (6 bytes), so no other code moves.
    Nothing else in the exe changes. All addresses are auto-detected; the script aborts
    if anything is ambiguous or a reference would be left unpatched.

USAGE
    python patch_globalfree_auto.py <original Antietam.exe> <output exe>
    e.g.  python patch_globalfree_auto.py Antietam.exe SMAntietam1210.exe
    Give the output a name other than Antietam.exe (Windows applies the crashing
    EmulateHeap shim by file name).

Tested on v9.84 (770,048 bytes: 65 references) and v12.10 (806,912 bytes: 68 references).
See docs/exe-patch.md for the resulting code, and docs/WINDOWS11_FIX.md for the story.
"""
import re
import struct
import sys

# Byte signature of the MSVC6 CRT free():
#   56             push esi
#   8B 74 24 08    mov  esi, [esp+8]
#   85 F6          test esi, esi
#   74 ??          je   short ...
#   6A 09          push 9              ; _HEAP_LOCK
#   E8 ...         call _lock
FREE_SIGNATURE = re.compile(re.escape(bytes.fromhex('568b74240885f674')) + b'.' +
                            re.escape(bytes.fromhex('6a09e8')), re.S)

# For "mov reg, [GlobalFree]" (8B /r with disp32) -> "mov reg, imm32" (B8+r)
MOV_REG_PATCHES = ((b'\x8b\x2d', 0xbd),   # ebp
                   (b'\x8b\x1d', 0xbb),   # ebx
                   (b'\x8b\x35', 0xbe),   # esi
                   (b'\x8b\x3d', 0xbf))   # edi


def make_stub(stub_va, free_va):
    """GlobalFree-compatible replacement that calls the CRT free(). 22 bytes."""
    return (bytes.fromhex('8b442404')          # mov  eax, [esp+4]    ; p
            + bytes.fromhex('85c0')            # test eax, eax
            + bytes.fromhex('740b')            # je   ret             ; free(NULL): nothing to do
            + bytes.fromhex('50')              # push eax
            + b'\xe8' + struct.pack('<i', free_va - (stub_va + 14))   # call free  (cdecl)
            + bytes.fromhex('83c404')          # add  esp, 4
            + bytes.fromhex('31c0')            # xor  eax, eax        ; GlobalFree returns NULL on success
            + bytes.fromhex('c20400'))         # ret  4               ; stdcall, 1 argument


def main(src, dst):
    d = bytearray(open(src, 'rb').read())

    # --- PE headers and sections -------------------------------------------------------
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    opt = pe + 24                                           # optional header
    optsz = struct.unpack_from('<H', d, pe + 20)[0]
    nsec = struct.unpack_from('<H', d, pe + 6)[0]
    image_base = struct.unpack_from('<I', d, opt + 28)[0]
    # (name, header offset, VirtualSize, VirtualAddress, SizeOfRawData, PointerToRawData)
    secs = [(bytes(d[opt + optsz + 40 * i:opt + optsz + 40 * i + 8]).rstrip(b'\0'), opt + optsz + 40 * i)
            + struct.unpack_from('<IIII', d, opt + optsz + 40 * i + 8) for i in range(nsec)]
    name, text_hdr, text_vsize, text_rva, text_rawsize, text_raw = secs[0]
    assert name == b'.text', 'first section is not .text'

    def rva_to_off(rva):
        for _, _, vs, v, rs, ra in secs:
            if v <= rva < v + max(vs, rs):
                return rva - v + ra

    def text_off_to_va(o):
        return image_base + text_rva + o - text_raw

    def cstr(o):
        return bytes(d[o:d.index(b'\0', o)]).decode('latin1')

    # --- 1. find the GlobalFree import slot (IAT entry) ------------------------------------
    slot = None
    o = rva_to_off(struct.unpack_from('<I', d, opt + 104)[0])   # import directory
    while True:
        orig_thunk, _, _, name_rva, first_thunk = struct.unpack_from('<IIIII', d, o)
        if not name_rva:
            break
        if cstr(rva_to_off(name_rva)).upper() == 'KERNEL32.DLL':
            t = rva_to_off(orig_thunk or first_thunk)
            i = 0
            while (v := struct.unpack_from('<I', d, t)[0]):
                if not v >> 31 and cstr(rva_to_off(v) + 2) == 'GlobalFree':
                    slot = image_base + first_thunk + 4 * i
                t += 4
                i += 1
        o += 20
    assert slot, 'GlobalFree import not found'
    slot_bytes = struct.pack('<I', slot)

    # --- 2. find the CRT free() -------------------------------------------------------------
    hits = [m.start() for m in FREE_SIGNATURE.finditer(bytes(d))]
    assert len(hits) == 1, f'CRT free not uniquely found: {hits}'
    free_va = text_off_to_va(hits[0])

    # --- 3. write the stub into the zero padding after the end of .text ---------------------
    stub_va = image_base + text_rva + ((text_vsize + 0x1f) & ~0xf)   # 16-byte aligned, gap after code
    s = stub_va - image_base - text_rva + text_raw
    assert s + 32 <= text_raw + text_rawsize and not any(d[s - 16:s + 32]), 'no free code cave'
    d[s:s + 22] = make_stub(stub_va, free_va)
    new_vsize = (stub_va + 0x40 - image_base - text_rva + 0xf) & ~0xf
    struct.pack_into('<I', d, text_hdr + 8, new_vsize)              # .text VirtualSize

    # --- 4. redirect every reference to the GlobalFree slot ---------------------------------
    def text():
        return bytes(d[text_raw:text_raw + text_rawsize])

    n = 0
    for m in list(re.finditer(re.escape(b'\xff\x15' + slot_bytes), text())):    # call [GlobalFree]
        p = text_raw + m.start()
        d[p:p + 6] = b'\xe8' + struct.pack('<i', stub_va - (text_off_to_va(p) + 5)) + b'\x90'
        n += 1
    for opcode, mov_imm in MOV_REG_PATCHES:                                       # mov reg,[GlobalFree]
        for m in list(re.finditer(re.escape(opcode + slot_bytes), text())):
            p = text_raw + m.start()
            d[p:p + 6] = bytes([mov_imm]) + struct.pack('<I', stub_va) + b'\x90'
            n += 1
    left = text().count(slot_bytes)
    assert left == 0, f'{left} unhandled references to GlobalFree remain'

    open(dst, 'wb').write(d)
    print(f'GlobalFree slot {slot:#x}, CRT free {free_va:#x}, stub {stub_va:#x}, '
          f'patched {n} references -> {dst}')


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
