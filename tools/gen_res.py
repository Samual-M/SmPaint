#!/usr/bin/env python3
"""Generate src/resdata.c from resources extracted from the original mspaint.exe.
Usage: gen_res.py <extracted res dir> <out.c>"""
import sys, os, struct, glob
from PIL import Image
R, OUT = sys.argv[1], sys.argv[2]
PNG = R + '/png'
o = []
w = o.append

def img(name):
    return Image.open(f'{PNG}/{name}.png').convert('RGBA')

def argb_words(im, key=None):
    out = []
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = im.getpixel((x, y))
            if key is not None and (r, g, b) == key: a = 0
            out.append(f'0x{(a << 24) | (r << 16) | (g << 8) | b:08x}u')
    return out

def arr(name, vals, per=12, typ='const uint32_t'):
    w(f'{typ} {name}[{len(vals)}] = {{')
    for i in range(0, len(vals), per): w('  ' + ','.join(vals[i:i+per]) + ',')
    w('};')

def cstr(s):
    out = '"'
    for b in s.encode('utf-8'):
        c = chr(b)
        if c == '"': out += '\\"'
        elif c == '\\': out += '\\\\'
        elif c == '\n': out += '\\n'
        elif c == '\t': out += '\\t'
        elif 32 <= b < 127 and c != '?': out += c
        else: out += f'\\{b:03o}'
    return out + '"'

w('/* GENERATED from the original mspaint.exe resources - do not edit */')
w('#include "resdata.h"')
# toolbox glyphs (red = transparent)
t = img('bitmap_859')
arr('res_toolglyph', argb_words(t, (255, 0, 0)))
arr('res_textglyph', argb_words(img('bitmap_102'), (192, 192, 192)))
arr('res_zoomlabels', argb_words(img('bitmap_862')))
arr('res_airbrush', argb_words(img('bitmap_864')))
arr('res_seltrans', argb_words(img('bitmap_863')))
arr('res_icon_size', argb_words(img('bitmap_3550')))
arr('res_icon_pos', argb_words(img('bitmap_3551')))
for n in range(851, 859):
    arr(f'res_brushmask{n-850}', argb_words(img(f'bitmap_{n}')))
for i in (16, 17, 18, 19):
    arr(f'res_stretchicon{i-16}', argb_words(img(f'icon_{i}')))
for i, nm in ((7, 48), (8, 32), (9, 16)):
    arr(f'res_appicon{nm}', argb_words(img(f'icon_{i}')))
# cursors: id 20..35 : hotspot + 32x32 ARGB (black/white, transparent) ; XOR parts approximated
cur_ids = list(range(20, 35 + 1))
rows = []
for cid in cur_ids:
    hx, hy, cw, ch = map(int, open(f'{PNG}/cursor_{cid}.txt').read().split())
    im = img(f'cursor_{cid}')
    arr(f'res_cursor_{cid}', argb_words(im))
w('const ResCursor res_cursors[16] = {')
for cid in cur_ids:
    hx, hy, cw, ch = map(int, open(f'{PNG}/cursor_{cid}.txt').read().split())
    w(f'  {{{hx},{hy},res_cursor_{cid}}},')
w('};')

# strings
strs = {}
for f in glob.glob(f'{R}/STRING/*.bin'):
    bid = int(os.path.basename(f).split('_')[0]); d = open(f, 'rb').read(); p = 0
    for i in range(16):
        n = struct.unpack_from('<H', d, p)[0]; p += 2
        s = d[p:p+n*2].decode('utf-16le'); p += n*2
        if n: strs[(bid-1)*16+i] = s
w(f'const ResString res_strings[{len(strs)}] = {{')
for k in sorted(strs): w(f'  {{{k},{cstr(strs[k])}}},')
w('};')
w(f'const int res_nstrings = {len(strs)};')

# menus
def wz(d, p):
    e = p
    while d[e:e+2] != b'\0\0': e += 2
    return d[p:e].decode('utf-16le'), e+2
def menu_items(d):
    ver, hsz = struct.unpack_from('<HH', d, 0); p = 4 + hsz; res = []
    def items(p, depth):
        while True:
            fl = struct.unpack_from('<H', d, p)[0]; p += 2
            if fl & 0x10:
                s, p = wz(d, p); res.append((1, 0, s, depth)); p = items(p, depth+1)
            else:
                i = struct.unpack_from('<H', d, p)[0]; p += 2; s, p = wz(d, p)
                res.append((2 if (i or s) else 3, i, s, depth))
            if fl & 0x80: return p
    items(p, 0); return res
w('const ResMenuItem res_menu_main[] = {')
for t_, i, s, dp in menu_items(open(f'{R}/MENU/2_1033.bin', 'rb').read()):
    w(f'  {{{t_},{i},{dp},{cstr(s)}}},')
w('  {0,0,0,0}};')
for nm, num in (('text', 113), ('sel', 118)):
    w(f'const ResMenuItem res_menu_{nm}[] = {{')
    for t_, i, s, dp in menu_items(open(f'{R}/MENU/{num}_1033.bin', 'rb').read()):
        w(f'  {{{t_},{i},{dp},{cstr(s)}}},')
    w('  {0,0,0,0}};')

# dialogs
def dlg(d):
    p = 18
    style, ex, nit, x, y, cx, cy = struct.unpack_from('<IIHhhhh', d, 0)
    def wstr(p):
        if struct.unpack_from('<H', d, p)[0] == 0xffff: return '', p+4
        return wz(d, p)
    p = 18
    if struct.unpack_from('<H', d, p)[0]: _, p = wstr(p)
    else: p += 2
    if struct.unpack_from('<H', d, p)[0]: _, p = wstr(p)
    else: p += 2
    title, p = wz(d, p)
    if style & 0x40:
        p += 2; _, p = wz(d, p)
    ctl = []
    for i in range(nit):
        p = (p+3) & ~3
        st, ex, ix, iy, icx, icy, iid = struct.unpack_from('<IIhhhhH', d, p); p += 18
        if struct.unpack_from('<H', d, p)[0] == 0xffff:
            cl = struct.unpack_from('<H', d, p+2)[0]; p += 4
        else: s_, p = wz(d, p); cl = 0
        if struct.unpack_from('<H', d, p)[0] == 0xffff:
            t = 'ord%d' % struct.unpack_from('<H', d, p+2)[0]; p += 4
        else: t, p = wz(d, p)
        cd = struct.unpack_from('<H', d, p)[0]; p += 2 + cd
        ctl.append((iid, cl, st, ix, iy, icx, icy, t))
    return title, cx, cy, ctl
names = {105: 'attributes', 108: 'zoom', 112: 'fliprot', 113: 'stretch', 4001: 'grid', 133: 'pagesetup'}
for num, nm in names.items():
    title, cx, cy, ctl = dlg(open(f'{R}/DIALOG/{num}_1033.bin', 'rb').read())
    w(f'const ResCtl res_dlg_{nm}_ctl[] = {{')
    for c in ctl: w(f'  {{{c[0]},{c[1]},0x{c[2]:08x}u,{c[3]},{c[4]},{c[5]},{c[6]},{cstr(c[7])}}},')
    w('};')
    w(f'const ResDlg res_dlg_{nm} = {{{cstr(title)},{cx},{cy},{len(ctl)},res_dlg_{nm}_ctl}};')
open(OUT, 'w').write('\n'.join(o) + '\n')
print('ok', len(o), 'lines')
