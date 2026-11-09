"""Combine the runtime's .bmp frame dumps into one PNG contact sheet (no extra libraries).

Usage: contact_sheet.py <frames dir> <out.png> [scale_div=2] [columns=6] [first] [last]
"""
import glob, os, struct, sys, zlib

frames_dir, out = sys.argv[1], sys.argv[2]
div = int(sys.argv[3]) if len(sys.argv) > 3 else 2
cols = int(sys.argv[4]) if len(sys.argv) > 4 else 6
lo = int(sys.argv[5]) if len(sys.argv) > 5 else 0
hi = int(sys.argv[6]) if len(sys.argv) > 6 else 10**9

def read_bmp(path):
    d = open(path, 'rb').read()
    w, h = struct.unpack('<ii', d[18:26])
    off = struct.unpack('<I', d[10:14])[0]
    return w, abs(h), d[off:]

paths = sorted(p for p in glob.glob(os.path.join(frames_dir, 'frame_*.bmp'))
               if lo <= int(os.path.basename(p)[6:11]) <= hi)
if not paths:
    sys.exit('no frames')
w, h, _ = read_bmp(paths[0])
tw, th = w // div, h // div
rows = (len(paths) + cols - 1) // cols
pad = 4
W, H = cols * (tw + pad), rows * (th + pad + 10)
img = bytearray([40]) * (W * H * 3)

# tiny 3x5 digit font for frame labels
FONT = {'0': '111101101101111', '1': '010110010010111', '2': '111001111100111', '3': '111001111001111',
        '4': '101101111001001', '5': '111100111001111', '6': '111100111101111', '7': '111001001001001',
        '8': '111101111101111', '9': '111101111001111'}

def put(x, y, rgb):
    if 0 <= x < W and 0 <= y < H:
        i = (y * W + x) * 3
        img[i:i + 3] = bytes(rgb)

for n, p in enumerate(paths):
    _, _, px = read_bmp(p)
    ox, oy = (n % cols) * (tw + pad), (n // cols) * (th + pad + 10)
    for y in range(th):
        for x in range(tw):
            i = ((y * div) * w + x * div) * 4
            put(ox + x, oy + 10 + y, (px[i + 2], px[i + 1], px[i]))
    label = os.path.basename(p)[6:11].lstrip('0') or '0'
    for k, ch in enumerate(label):
        bits = FONT[ch]
        for b in range(15):
            if bits[b] == '1':
                put(ox + k * 4 + b % 3, oy + 2 + b // 3, (255, 255, 0))

raw = b''.join(b'\x00' + bytes(img[y * W * 3:(y + 1) * W * 3]) for y in range(H))
def chunk(t, data):
    c = struct.pack('>I', len(data)) + t + data
    return c + struct.pack('>I', zlib.crc32(t + data) & 0xffffffff)
png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0)) \
    + chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
print('%d frames -> %s (%dx%d)' % (len(paths), out, W, H))
