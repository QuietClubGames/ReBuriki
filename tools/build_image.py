"""Build big-endian program image from 007pr01b + 007pr02b and dump its header."""
import struct, sys, os
R, OUT = sys.argv[1], sys.argv[2]
a = open(os.path.join(R, 'buriki/007pr01b.81'), 'rb').read()
b = open(os.path.join(R, 'buriki/007pr02b.82'), 'rb').read()
il = bytearray(len(a) * 2)
for i in range(0, len(a), 2):
    il[i*2:i*2+2] = a[i:i+2]
    il[i*2+2:i*2+4] = b[i:i+2]
n = len(il) // 4
words = struct.unpack('<%dI' % n, il)
be = struct.pack('>%dI' % n, *words)
open(OUT, 'wb').write(be)
print('wrote', OUT, len(be))

def hexdump(buf, base, length):
    for off in range(0, length, 16):
        row = buf[off:off+16]
        asc = ''.join(chr(c) if 32 <= c < 127 else '.' for c in row)
        print('%08x  %s  %s' % (base + off, ' '.join(row[i:i+4].hex() for i in range(0, 16, 4)), asc))
hexdump(be, 0, 0x200)

# where does code density start? count jr ra per 64KB
print('jr ra per 512KB block:')
cnt = []
for blk in range(0, len(be), 0x80000):
    w = struct.unpack('>%dI' % (0x80000 // 4), be[blk:blk+0x80000])
    cnt.append(w.count(0x03E00008))
print(cnt)
# last non-0xFF / non-zero byte
end = len(be)
while end > 0 and be[end-1] in (0, 0xFF):
    end -= 1
print('used size ~0x%x' % end)
