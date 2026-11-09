"""Print loudness per second of a 16-bit stereo WAV, to check audio without listening.

Usage: wav_stats.py <file.wav> [seconds_per_row=1]
"""
import math, struct, sys
d = open(sys.argv[1], 'rb').read()
step = float(sys.argv[2]) if len(sys.argv) > 2 else 1.0
rate = struct.unpack('<I', d[24:28])[0]
data = d[44:]
n = len(data) // 4
samples = struct.unpack('<%dh' % (n * 2), data[:n * 4])
per = int(rate * step)
print('%d Hz, %.1f s' % (rate, n / rate))
for start in range(0, n, per):
    seg = samples[start * 2:(start + per) * 2]
    if not seg:
        break
    rms = math.sqrt(sum(s * s for s in seg) / len(seg))
    peak = max(abs(s) for s in seg)
    clip = sum(1 for s in seg if abs(s) >= 32767)
    bar = '#' * int(rms / 500)
    print('%6.1fs  rms %6.0f  peak %6d  clipped %5d  %s' % (start / rate, rms, peak, clip, bar))
