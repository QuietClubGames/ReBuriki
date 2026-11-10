"""Time per function from a BURIKI_PROFILE sample file.

Usage: profile_report.py <exe> <samples file> [how many lines]
Needs llvm-nm on PATH (the exe must not be stripped). Addresses in the samples are matched to the nearest
symbol below them; samples outside the exe (Windows, the graphics driver) are grouped as "(outside the exe)".
"""
import bisect, collections, struct, subprocess, sys

exe, path = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
data = open(path, 'rb').read()
base, n = struct.unpack('<QQ', data[:16])
samples = struct.unpack('<%dQ' % n, data[16:16 + 8 * n])

syms = []
out = subprocess.run(['llvm-nm', '--numeric-sort', '--defined-only', '-C', exe], capture_output=True, text=True).stdout
for line in out.splitlines():
    parts = line.split(None, 2)
    if len(parts) == 3 and parts[1] in 'TtWw':
        syms.append((int(parts[0], 16), parts[2]))
syms.sort()
addrs = [a for a, _ in syms]
image_base = 0x140000000          # the link-time base llvm-nm reports addresses against
lo, hi = addrs[0], addrs[-1] + 0x100000

count = collections.Counter()
for rip in samples:
    a = rip - base + image_base
    if a < lo or a > hi:
        count['(outside the exe)'] += 1
        continue
    i = bisect.bisect_right(addrs, a) - 1
    count[syms[i][1] if i >= 0 else '?'] += 1

print('%d samples' % n)
acc = 0
for name, c in count.most_common(top):
    acc += c
    print('%6.2f%%  %6.2f%%  %s' % (100.0 * c / n, 100.0 * acc / n, name[:110]))
