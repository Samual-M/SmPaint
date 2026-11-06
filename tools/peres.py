#!/usr/bin/env python3
"""Minimal PE resource extractor. Usage: peres.py <exe> <outdir>"""
import struct, sys, os

RT = {1:'CURSOR',2:'BITMAP',3:'ICON',4:'MENU',5:'DIALOG',6:'STRING',7:'FONTDIR',8:'FONT',
      9:'ACCELERATOR',10:'RCDATA',11:'MESSAGETABLE',12:'GROUP_CURSOR',14:'GROUP_ICON',
      16:'VERSION',24:'MANIFEST'}

def main(exe, out):
    d = open(exe,'rb').read()
    pe = struct.unpack_from('<I', d, 0x3c)[0]
    assert d[pe:pe+4] == b'PE\0\0'
    nsec = struct.unpack_from('<H', d, pe+6)[0]
    optsz = struct.unpack_from('<H', d, pe+20)[0]
    opt = pe+24
    magic = struct.unpack_from('<H', d, opt)[0]
    ddoff = opt + (96 if magic == 0x10b else 112)
    rva, size = struct.unpack_from('<II', d, ddoff + 2*8)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        name, vs, va, rs, ro = struct.unpack_from('<8sIIII', d, so+i*40)
        secs.append((va, max(vs, rs), ro))
    def r2o(r):
        for va, sz, ro in secs:
            if va <= r < va+sz: return r - va + ro
        raise ValueError(hex(r))
    base = r2o(rva)
    def name_at(off):
        n = struct.unpack_from('<H', d, base+off)[0]
        return d[base+off+2: base+off+2+n*2].decode('utf-16le')
    def walk(off):
        _, _, _, _, nn, ni = struct.unpack_from('<IIHHHH', d, base+off)
        ents = []
        for i in range(nn+ni):
            nm, o = struct.unpack_from('<II', d, base+off+16+i*8)
            key = name_at(nm & 0x7fffffff) if nm & 0x80000000 else nm
            ents.append((key, o))
        return ents
    res = []
    for t, to in walk(0):
        for n, no in walk(to & 0x7fffffff):
            for l, lo in walk(no & 0x7fffffff):
                dr, sz, cp, _ = struct.unpack_from('<IIII', d, base+lo)
                res.append((t, n, l, d[r2o(dr): r2o(dr)+sz]))
    os.makedirs(out, exist_ok=True)
    idx = []
    for t, n, l, data in res:
        tn = RT.get(t, str(t)) if isinstance(t, int) else t
        sub = os.path.join(out, tn); os.makedirs(sub, exist_ok=True)
        fn = f"{n}_{l}.bin"
        open(os.path.join(sub, fn), 'wb').write(data)
        idx.append(f"{tn}\t{n}\t{l}\t{len(data)}")
    open(os.path.join(out, 'index.tsv'), 'w').write("\n".join(idx)+"\n")
    print(len(res), "resources")

main(sys.argv[1], sys.argv[2])
