import struct, sys, os
R = sys.argv[1]
def rd(p): return open(os.path.join(R, p), 'rb').read()

JR_RA = 0x03E00008
def score(buf):
    """Count 'jr ra' under several byte orders."""
    out = {}
    n = len(buf) // 4
    be = struct.unpack('>%dI' % n, buf[:n*4])
    le = struct.unpack('<%dI' % n, buf[:n*4])
    out['BE'] = be.count(JR_RA)
    out['LE'] = le.count(JR_RA)
    hs = [((w & 0xFFFF) << 16) | (w >> 16) for w in be]
    out['BE-halfswap'] = hs.count(JR_RA)
    hl = [((w & 0xFFFF) << 16) | (w >> 16) for w in le]
    out['LE-halfswap'] = hl.count(JR_RA)
    return out

print('== BIOS files')
for f in ['brom1.bin', 'bios_us.bin', 'bios_export.bin', 'from1.bin', 'rom1.bin']:
    b = rd('hng64/' + f)
    print(f, len(b), score(b), b[:16].hex())

def interleave16(a, b):
    out = bytearray(len(a) * 2)
    for i in range(0, len(a), 2):
        out[i*2:i*2+2] = a[i:i+2]
        out[i*2+2:i*2+4] = b[i:i+2]
    return bytes(out)

print('== program ROMs individually')
prs = sorted(f for f in os.listdir(os.path.join(R, 'buriki')) if 'pr' in f)
for f in prs:
    b = rd('buriki/' + f)
    print(f, score(b), b[:16].hex())

print('== program ROMs interleaved in pairs (16-bit)')
for i in range(0, len(prs), 2):
    a, b = rd('buriki/' + prs[i]), rd('buriki/' + prs[i+1])
    il = interleave16(a, b)
    print(prs[i], '+', prs[i+1], score(il), il[:32].hex())
