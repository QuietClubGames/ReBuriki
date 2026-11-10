"""Search all program/data ROMs for a run of palette colours captured from the running game.

Usage: find_colours.py <roms dir> <run log> <frame> <first index> <count>
Builds the full 24 MB cart image (pr01-06, interleaved like the runtime) and searches for the
colours as big-endian 32-bit words, and as RGB555/RGB565 (both byte orders).
"""
import os, re, struct, sys

roms, log, frame, first, count = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), int(sys.argv[5])
pal = None
for line in open(log, errors='replace'):
    m = re.match(r'\[pal %s\] (.*)' % frame, line)
    if m:
        pal = [int(x, 16) for x in m.group(1).split()]
        break
run = pal[first:first + count]
print('searching for colours %d-%d:' % (first, first + count - 1), ' '.join('%06X' % c for c in run[:8]), '...')

def pair(a, b):
    lo = open(os.path.join(roms, 'buriki', a), 'rb').read()
    hi = open(os.path.join(roms, 'buriki', b), 'rb').read()
    out = bytearray(len(lo) * 2)
    for i in range(0, len(lo), 2):
        # host word = hi16 << 16 | lo16 (each 16-bit chip word little-endian); store big-endian
        out[i * 2:i * 2 + 4] = bytes([hi[i + 1], hi[i], lo[i + 1], lo[i]])
    return bytes(out)

cart = pair('007pr01b.81', '007pr02b.82') + pair('007pr03a.83', '007pr04a.84') + pair('007pr05a.85', '007pr06a.86')
def rgb(c): return (c >> 16) & 255, (c >> 8) & 255, c & 255
encodings = {
    'BE32 00RRGGBB': lambda c: struct.pack('>I', c),
    'RGB555 BE': lambda c: struct.pack('>H', ((rgb(c)[0] >> 3) << 10) | ((rgb(c)[1] >> 3) << 5) | (rgb(c)[2] >> 3)),
    'RGB555 LE': lambda c: struct.pack('<H', ((rgb(c)[0] >> 3) << 10) | ((rgb(c)[1] >> 3) << 5) | (rgb(c)[2] >> 3)),
    'BGR555 BE': lambda c: struct.pack('>H', ((rgb(c)[2] >> 3) << 10) | ((rgb(c)[1] >> 3) << 5) | (rgb(c)[0] >> 3)),
    'RGB565 BE': lambda c: struct.pack('>H', ((rgb(c)[0] >> 3) << 11) | ((rgb(c)[1] >> 2) << 5) | (rgb(c)[2] >> 3)),
}
for name, enc in encodings.items():
    needle = b''.join(enc(c) for c in run)
    hits = [m.start() for m in re.finditer(re.escape(needle), cart)]
    size = len(enc(0))
    print('%-14s %s' % (name, ['0x%X (palette start 0x%X)' % (h, h - first * size) for h in hits][:6]))
