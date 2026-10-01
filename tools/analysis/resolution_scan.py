# Counts screen-size constants (800/600, 1024/768, 640/480, ...) used as immediate operands in
# the game exe (usage: python resolution_scan.py <path to exe>) and lists SetDisplayMode-like call sites. Needs: pip install capstone.
# Results: docs/resolution-investigation.md
import struct, re, collections, capstone
from capstone import x86
import sys
EXE = sys.argv[1] if len(sys.argv) > 1 else "Antietam_Win11.exe"   # path to the game exe
d = open(EXE, 'rb').read()
IB, TVA, TRAW, TSIZE = 0x400000, 0x1000, 0x1000, 0xae352
code = d[TRAW:TRAW+TSIZE]
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32); md.detail = True
md.skipdata = True
VALS = {800:'800',600:'600',1024:'1024',768:'768',640:'640',480:'480',799:'799',599:'599',400:'400(=800/2)',300:'300(=600/2)'}
hits = collections.defaultdict(list)
prev = []
for ins in md.disasm(code, IB+TVA):
    if ins.id == 0: continue
    for op in ins.operands:
        v = None
        if op.type == x86.X86_OP_IMM: v = op.imm & 0xffffffff
        elif op.type == x86.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0: v = None
        if v in VALS:
            hits[v].append((ins.address, f"{ins.mnemonic} {ins.op_str}"))
for v in VALS:
    print(f"{VALS[v]:>12}: {len(hits[v])} immediate operands")
# pairs: 800 and 600 within 32 bytes
pairs = [a for a,_ in hits[800] if any(0 < abs(b-a) <= 40 for b,_ in hits[600])]
print('800 with 600 nearby (<=40 bytes):', len(pairs))
pairs2 = [a for a,_ in hits[1024] if any(0 < abs(b-a) <= 40 for b,_ in hits[768])]
pairs3 = [a for a,_ in hits[640] if any(0 < abs(b-a) <= 40 for b,_ in hits[480])]
print('1024+768 nearby:', [hex(x) for x in pairs2]); print('640+480 nearby:', [hex(x) for x in pairs3])
# functions touched: group 800/600 hits by 4KB page as rough function count
print('distinct 4KB code pages with 800/600:', len({a>>12 for a,_ in hits[800]+hits[600]}))
# by mnemonic
print('800 by mnemonic:', collections.Counter(s.split()[0] for _,s in hits[800]))
print('600 by mnemonic:', collections.Counter(s.split()[0] for _,s in hits[600]))
for v in (1024,768,640,480):
    for a,s in hits[v][:12]: print(f'  {v}: {a:#x} {s}')
# SetDisplayMode call: IDirectDraw vtable +0x54
for ins in md.disasm(code, IB+TVA):
    if ins.id and ins.mnemonic=='call' and '+ 0x54]' in ins.op_str:
        print('call [reg+0x54] (SetDisplayMode candidate) at', hex(ins.address), ins.op_str)
