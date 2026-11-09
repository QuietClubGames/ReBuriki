"""Guess where the code region gets loaded by matching JAL targets to function starts."""
import struct, sys, collections
be = open(sys.argv[1], 'rb').read()
n = len(be) // 4
W = struct.unpack('>%dI' % n, be)
LO, HI = 0x100000 // 4, 0x200000 // 4
# probable function starts: 2 words after jr ra (skip delay slot), skipping padding
starts = set()
for i in range(LO, HI):
    if W[i] == 0x03E00008:
        j = i + 2
        while j < HI and W[j] == 0:
            j += 1
        starts.add(j * 4)
# JAL targets (26-bit index), also where jal sits
jals = collections.Counter()
regions = collections.Counter()
for i in range(LO, HI):
    if W[i] >> 26 == 3:
        t = (W[i] & 0x3FFFFFF) << 2
        jals[t] += 1
        regions[t >> 20] += 1
print('JAL target regions (MB within 256MB segment):', regions.most_common(8))
# test offsets: target - delta == rom offset of a function start
best = collections.Counter()
for t, c in jals.items():
    for s in starts:
        pass
# faster: for each candidate delta derived from region, count hits
for t in jals:
    for s in list(starts)[:0]:
        pass
cands = collections.Counter()
sl = sorted(starts)
import bisect
sample = [t for t, _ in jals.most_common(300)]
for t in sample:
    for s in sl:
        d = t - s
        if d % 0x1000 == 0:
            cands[d] += 1
for d, c in cands.most_common(5):
    hits = sum(1 for t in jals if (t - d) in starts)
    print('delta 0x%08x -> %d/%d JAL targets land on function starts' % (d & 0xFFFFFFFF, hits, len(jals)))
print('function starts found:', len(starts), ' distinct JAL targets:', len(jals))
