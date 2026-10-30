#!/usr/bin/env python3
"""Dump Frostbite (2015) reflected type data from the game's in-memory exe image.
   dai_sdk.py <exe_image.bin> <out.txt>   (exe_image.bin: the decrypted exe image dumped from memory, offset == RVA)
ClassInfoData layout found in Mass Effect Andromeda (x64):
  +0  const char* name      +8 u16 flags ((flags>>4)&0x1F = kind; 3 = class, 2 = value struct, 8 = enum...)
  +10 u16 size              +24 TypeInfo* (runtime object)   +32 u16 alignment?, u16 field count (low byte)
  +40 TypeInfo* super       +48 FieldInfoData* fields
FieldInfoData (24 bytes): +0 name, +8 u16 flags, +10 u16 offset, +16 TypeInfo* type
A runtime TypeInfo's first qword points to its TypeInfoData (whose first qword is the type name)."""
import struct, sys, re

d = open(sys.argv[1], "rb").read()
BASE = 0x140000000
N = len(d)

def q(o): return struct.unpack_from("<Q", d, o)[0] if 0 <= o <= N - 8 else 0
def cstr(va):
    o = va - BASE
    if not (0 < o < N): return None
    e = d.find(b"\0", o, o + 160)
    if e <= o: return None
    s = d[o:e]
    return s.decode("latin1") if all(32 <= c < 127 for c in s) else None
def typename(ti):
    data = q(ti - BASE)
    return cstr(q(data - BASE)) if data else None

classes = {}
for o in range(0, N - 64, 8):
    name = q(o)
    if not (BASE < name < BASE + N): continue
    fl = struct.unpack_from("<H", d, o + 8)[0]
    kind = (fl >> 4) & 0x1F
    if kind not in (2, 3): continue
    fields = q(o + 0x30)
    if not (BASE < fields < BASE + N): continue
    nm = cstr(name)
    if not nm or not re.match(r"^[A-Za-z_][A-Za-z0-9_:<>,\- ]*$", nm): continue
    fo = fields - BASE
    f0 = cstr(q(fo))
    if not f0: continue
    size = struct.unpack_from("<H", d, o + 10)[0]
    cnt = d[o + 0x19]   # field count (DAI: u8 at +0x19)
    if cnt == 0: continue
    flist = []
    ok = True
    for i in range(cnt):
        r = fo + i * 24
        fname = cstr(q(r))
        if not fname: ok = False; break
        ffl, foff = struct.unpack_from("<HH", d, r + 8)
        ftype = typename(q(r + 16)) or f"?{q(r + 16):#x}"
        flist.append((foff, fname, ftype, (ffl >> 4) & 0x1F))
    if not ok: continue
    sup = typename(q(o + 0x20)) or cstr(q(q(o+0x20)-BASE)) or ""
    classes.setdefault(nm, (o, kind, size, sup, flist))

with open(sys.argv[2], "w") as f:
    for nm in sorted(classes):
        o, kind, size, sup, flist = classes[nm]
        f.write(f"{'class' if kind == 3 else 'struct'} {nm} : {sup}  // size {size:#x}, data exe+{o:X}\n")
        for foff, fname, ftype, fk in sorted(flist):
            f.write(f"    +{foff:#06x} {ftype} {fname}\n")
print(f"{len(classes)} classes/structs -> {sys.argv[2]}")
