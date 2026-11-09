"""Convert 32/24-bit BMP files to PNG, optionally cropped and placed side by side (no extra libraries).

Usage: bmp2png.py out.png in1.bmp [in2.bmp ...] [--crop x,y,w,h] [--scale 0.5]
"""
import struct, sys, zlib

args = sys.argv[1:]
out = args.pop(0)
crop = None
scale = 1.0
files = []
while args:
    a = args.pop(0)
    if a == '--crop':
        crop = [int(v) for v in args.pop(0).split(',')]
    elif a == '--scale':
        scale = float(args.pop(0))
    else:
        files.append(a)

def read_bmp(path):
    d = open(path, 'rb').read()
    off = struct.unpack('<I', d[10:14])[0]
    w, h = struct.unpack('<ii', d[18:26])
    bpp = struct.unpack('<H', d[28:30])[0] // 8
    flip = h > 0
    h = abs(h)
    row = (w * bpp + 3) & ~3
    px = d[off:]
    def get(x, y):
        yy = h - 1 - y if flip else y
        i = yy * row + x * bpp
        return px[i + 2], px[i + 1], px[i]
    return w, h, get

imgs = []
for f in files:
    w, h, get = read_bmp(f)
    x0, y0, cw, ch = crop if crop else (0, 0, w, h)
    ow, oh = int(cw * scale), int(ch * scale)
    rows = []
    for y in range(oh):
        r = bytearray()
        for x in range(ow):
            r += bytes(get(x0 + int(x / scale), y0 + int(y / scale)))
        rows.append(r)
    imgs.append((ow, oh, rows))

gap = 8
W = sum(i[0] for i in imgs) + gap * (len(imgs) - 1)
H = max(i[1] for i in imgs)
raw = bytearray()
for y in range(H):
    raw += b'\x00'
    for n, (w, h, rows) in enumerate(imgs):
        raw += rows[y] if y < h else bytes(w * 3)
        if n < len(imgs) - 1:
            raw += bytes([30, 30, 30]) * gap

def chunk(t, data):
    c = struct.pack('>I', len(data)) + t + data
    return c + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) \
    + chunk(b'IDAT', zlib.compress(bytes(raw), 6)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print('%s: %dx%d' % (out, W, H))
