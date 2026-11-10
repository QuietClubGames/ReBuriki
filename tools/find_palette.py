"""Find where a captured palette lives in the program ROM image.

Usage: find_palette.py <run log with [pal N] lines> <program image (big-endian)>
Searches for the palette as 32-bit big-endian words (the palette RAM format 00RRGGBB),
and as 15/16-bit packed colours, and prints matching offsets.
"""
import re, struct, sys

log, image = sys.argv[1:3]
pals = {}
for line in open(log, errors='replace'):
    m = re.match(r'\[pal (\d+)\] (.*)', line)
    if m:
        pals[int(m.group(1))] = [int(x, 16) for x in m.group(2).split()]
frames = sorted(pals)
same = all(pals[f] == pals[frames[0]] for f in frames)
print('frames:', frames, '| identical palettes:', same)
pal = pals[frames[0]]
print('first 8 colours:', ' '.join('%06X' % c for c in pal[:8]), '| distinct colours:', len(set(pal)))

rom = open(image, 'rb').read()
# Use a distinctive run of 16 colours that aren't all equal.
for start in range(0, 256 - 16):
    run = pal[start:start + 16]
    if len(set(run)) >= 8:
        break
needle32 = b''.join(struct.pack('>I', c) for c in run)
hits = [m.start() - start * 4 for m in re.finditer(re.escape(needle32), rom)]
print('as 32-bit words: palette starts at ROM offsets', ['0x%X' % h for h in hits][:10])

def rgb555(c):
    r, g, b = (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
for name, fn in (('RGB555 BE', lambda c: struct.pack('>H', rgb555(c))), ('RGB555 LE', lambda c: struct.pack('<H', rgb555(c)))):
    needle = b''.join(fn(c) for c in run)
    hits = [m.start() - start * 2 for m in re.finditer(re.escape(needle), rom)]
    print('as %s: starts at' % name, ['0x%X' % h for h in hits][:10])
