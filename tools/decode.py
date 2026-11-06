#!/usr/bin/env python3
"""Decode extracted resources: strings, menus, dialogs, accelerators, bitmaps->PNG, icons/cursors->PNG."""
import struct, sys, os, glob, io
from PIL import Image

R = sys.argv[1]

def rle4(src, w, h):
    rowb = ((w*4+31)//32)*4
    idx = [[0]*w for _ in range(h)]
    x = y = 0; i = 0
    while i < len(src) and y < h:
        n, v = src[i], src[i+1]; i += 2
        if n:
            for k in range(n):
                if x < w: idx[y][x] = (v >> 4) if k % 2 == 0 else (v & 15)
                x += 1
        elif v == 0: x = 0; y += 1
        elif v == 1: break
        elif v == 2: x += src[i]; y += src[i+1]; i += 2
        else:
            for k in range(v):
                b = src[i + k//2]
                if x < w: idx[y][x] = (b >> 4) if k % 2 == 0 else (b & 15)
                x += 1
            nb = (v+1)//2; i += nb + (nb & 1)
    out = bytearray(rowb*h)
    # rows stored bottom-up in dib; rle rows start at bottom as well (y=0 is bottom)
    for yy in range(h):
        for xx in range(w):
            out[yy*rowb + xx//2] |= idx[yy][xx] << (4 if xx % 2 == 0 else 0)
    return bytes(out)

def dib_to_image(data, mask_follows=False):
    hs, w, h, planes, bpp, comp, szimg, xp, yp, clrused, clrimp = struct.unpack_from('<IiiHHIIiiII', data, 0)
    if mask_follows: h //= 2
    ncol = clrused or ((1 << bpp) if bpp <= 8 else 0)
    pal = []
    p = hs
    for i in range(ncol):
        b, g, r, _ = data[p+i*4:p+i*4+4]; pal.append((r, g, b))
    p += ncol*4
    rowb = ((w*bpp+31)//32)*4
    px = data[p:]
    if comp == 2:
        px = rle4(px, w, h); rowb = ((w*4+31)//32)*4
    img = Image.new('RGBA', (w, h))
    pix = img.load()
    for y in range(h):
        row = px[(h-1-y)*rowb:(h-y)*rowb]
        for x in range(w):
            if bpp == 4:
                v = (row[x//2] >> (4 if x % 2 == 0 else 0)) & 15; c = pal[v]; a = 255
            elif bpp == 8:
                c = pal[row[x]]; a = 255
            elif bpp == 1:
                v = (row[x//8] >> (7-x % 8)) & 1; c = pal[v]; a = 255
            elif bpp == 24:
                c = (row[x*3+2], row[x*3+1], row[x*3]); a = 255
            elif bpp == 32:
                c = (row[x*4+2], row[x*4+1], row[x*4]); a = row[x*4+3]
            else: raise ValueError(bpp)
            pix[x, y] = c + (a,)
    mask = None
    if mask_follows:
        mrow = ((w+31)//32)*4
        mp = p + rowb*h
        mdata = data[mp:]
        mask = []
        for y in range(h):
            r_ = mdata[(h-1-y)*mrow:(h-y)*mrow]
            mask.append([(r_[x//8] >> (7-x % 8)) & 1 for x in range(w)])
    return img, mask, bpp

# ---- strings
def decode_strings():
    out = {}
    for f in glob.glob(f'{R}/STRING/*.bin'):
        bid = int(os.path.basename(f).split('_')[0])
        d = open(f, 'rb').read(); p = 0
        for i in range(16):
            n = struct.unpack_from('<H', d, p)[0]; p += 2
            s = d[p:p+n*2].decode('utf-16le'); p += n*2
            if n: out[(bid-1)*16+i] = s
    with open(f'{R}/strings.txt', 'w') as fo:
        for k in sorted(out): fo.write(f"{k}\t{out[k]!r}\n")
    return out

# ---- menus
def decode_menu(d):
    ver, hsz = struct.unpack_from('<HH', d, 0)
    p = 4 + hsz
    lines = []
    def items(p, depth):
        while True:
            flags = struct.unpack_from('<H', d, p)[0]; p += 2
            if flags & 0x10:  # popup
                s = '';
                e = p
                while d[e:e+2] != b'\0\0': e += 2
                s = d[p:e].decode('utf-16le'); p = e+2
                lines.append('  '*depth + f'POPUP {s!r} flags={flags:#x}')
                p = items(p, depth+1)
            else:
                id_ = struct.unpack_from('<H', d, p)[0]; p += 2
                e = p
                while d[e:e+2] != b'\0\0': e += 2
                s = d[p:e].decode('utf-16le'); p = e+2
                if id_ == 0 and not s: lines.append('  '*depth + 'SEPARATOR')
                else: lines.append('  '*depth + f'ITEM {id_} {s!r} flags={flags:#x}')
            if flags & 0x80: return p
    items(p, 0)
    return lines

# ---- dialogs
def dlg(d):
    lines = []
    p = 0
    def wstr(p):
        if struct.unpack_from('<H', d, p)[0] == 0xffff: return f'ord{struct.unpack_from("<H", d, p+2)[0]}', p+4
        e = p
        while d[e:e+2] != b'\0\0': e += 2
        return d[p:e].decode('utf-16le'), e+2
    ext = struct.unpack_from('<HH', d, 0) == (1, 0xffff)
    if ext:
        ver, sig, hid, ex, st, style, nit, x, y, cx, cy = struct.unpack_from('<HHIIIIHhhhh', d, 0); p = 26
    else:
        style, ex, nit, x, y, cx, cy = struct.unpack_from('<IIHhhhh', d, 0); p = 18
    menu, p = wstr(p) if struct.unpack_from('<H', d, p)[0] else ('', p+2)
    cls, p = wstr(p) if struct.unpack_from('<H', d, p)[0] else ('', p+2)
    title, p = wstr(p)
    font = None
    if style & 0x40:
        pt = struct.unpack_from('<H', d, p)[0]; p += 2
        if ext: p += 4
        fname, p = wstr(p); font = (pt, fname)
    lines.append(f'DIALOG style={style:#x} pos=({x},{y}) size=({cx},{cy}) title={title!r} font={font}')
    for i in range(nit):
        p = (p+3) & ~3
        if ext:
            hid, ex, st, ix, iy, icx, icy, iid = struct.unpack_from('<IIIhhhhI', d, p); p += 24
        else:
            st, ex, ix, iy, icx, icy, iid = struct.unpack_from('<IIhhhhH', d, p); p += 18
        cl, p = wstr(p)
        if struct.unpack_from('<H', d, p)[0] == 0xffff:
            t = f'ord{struct.unpack_from("<H", d, p+2)[0]}'; p += 4
        else: t, p = wstr(p)
        cd = struct.unpack_from('<H', d, p)[0]; p += 2 + cd
        lines.append(f'  CTRL id={iid} class={cl} style={st:#x} pos=({ix},{iy}) size=({icx},{icy}) text={t!r}')
    return lines

def main():
    strs = decode_strings()
    with open(f'{R}/menus.txt', 'w') as fo:
        for f in sorted(glob.glob(f'{R}/MENU/*.bin'), key=lambda x: int(os.path.basename(x).split('_')[0])):
            fo.write(f'=== MENU {os.path.basename(f)}\n')
            fo.write('\n'.join(decode_menu(open(f, 'rb').read()))+'\n')
    with open(f'{R}/dialogs.txt', 'w') as fo:
        for f in sorted(glob.glob(f'{R}/DIALOG/*.bin'), key=lambda x: int(os.path.basename(x).split('_')[0])):
            fo.write(f'=== DIALOG {os.path.basename(f)}\n')
            fo.write('\n'.join(dlg(open(f, 'rb').read()))+'\n')
    with open(f'{R}/accel.txt', 'w') as fo:
        for f in sorted(glob.glob(f'{R}/ACCELERATOR/*.bin')):
            d = open(f, 'rb').read()
            fo.write(f'=== ACCEL {os.path.basename(f)}\n')
            for i in range(0, len(d), 8):
                fl, key, cmd = struct.unpack_from('<HHH', d, i)
                fo.write(f'flags={fl:#x} key={key:#x} cmd={cmd}\n')
                if fl & 0x80: break
    os.makedirs(f'{R}/png', exist_ok=True)
    for f in glob.glob(f'{R}/BITMAP/*.bin'):
        d = open(f, 'rb').read()
        img, _, bpp = dib_to_image(d)
        n = os.path.basename(f).split('_')[0]
        img.save(f'{R}/png/bitmap_{n}.png')
        print('bitmap', n, img.size, bpp)
    for f in glob.glob(f'{R}/ICON/*.bin'):
        d = open(f, 'rb').read()
        try:
            img, mask, bpp = dib_to_image(d, True)
            pix = img.load()
            for y in range(img.size[1]):
                for x in range(img.size[0]):
                    if mask[y][x]: pix[x, y] = (0, 0, 0, 0)
            n = os.path.basename(f).split('_')[0]
            img.save(f'{R}/png/icon_{n}.png'); print('icon', n, img.size, bpp)
        except Exception as e:
            print('icon fail', f, e)
    for f in glob.glob(f'{R}/CURSOR/*.bin'):
        d = open(f, 'rb').read()
        hx, hy = struct.unpack_from('<HH', d, 0)
        img, mask, bpp = dib_to_image(d[4:], True)
        n = os.path.basename(f).split('_')[0]
        # cursor: AND mask + XOR color; store both
        w, h = img.size
        open(f'{R}/png/cursor_{n}.txt', 'w').write(f'{hx} {hy} {w} {h}\n')
        pix = img.load()
        img2 = Image.new('RGBA', (w, h)); p2 = img2.load()
        for y in range(h):
            for x in range(w):
                p2[x, y] = (0, 0, 0, 0) if mask[y][x] and pix[x, y][:3] == (0, 0, 0) else pix[x, y]
        img2.save(f'{R}/png/cursor_{n}.png'); print('cursor', n, (w, h), bpp, (hx, hy))

main()
