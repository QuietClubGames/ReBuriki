"""Find functions that use low-level CPU control instructions N64Recomp can't translate.

These are "kernel" pieces (exception handling, cache/TLB management). They're listed as
ignored in the recomp config and rewritten by hand in the runtime (runtime/kernel.cpp).

Usage: find_kernel_funcs.py <syms.toml> <report.txt>
"""
import re, struct, sys
import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (the installer's Python doesn't add it)
from sections import COMBINED_ROM
syms, report_path = sys.argv[1:3]
rom = open(COMBINED_ROM, 'rb').read()
COP0_NAMES = {0: 'Index', 2: 'EntryLo0', 3: 'EntryLo1', 5: 'PageMask', 8: 'BadVAddr', 9: 'Count',
              10: 'EntryHi', 11: 'Compare', 12: 'Status', 13: 'Cause', 14: 'EPC', 15: 'PRId',
              16: 'Config', 28: 'TagLo', 30: 'ErrorEPC'}

def classify(w):
    op = w >> 26
    if op == 0x10:  # COP0
        rs = (w >> 21) & 0x1F
        rd = (w >> 11) & 0x1F
        if rs in (0, 1, 4, 5):
            kind = {0: 'mfc0', 1: 'dmfc0', 4: 'mtc0', 5: 'dmtc0'}[rs]
            if rd == 12 and rs in (0, 4):
                return None  # Status reads/writes are supported
            return '%s %s' % (kind, COP0_NAMES.get(rd, str(rd)))
        if rs == 0x10:
            fn = w & 0x3F
            return {0x18: 'eret', 0x01: 'tlbr', 0x02: 'tlbwi', 0x06: 'tlbwr', 0x08: 'tlbp'}.get(fn, 'cop0 fn %x' % fn)
        return 'cop0 rs %x' % rs
    return None

text = open(syms).read()
hits = []
for block in text.split('[[section]]')[1:]:
    rom_base = int(re.search(r'rom = 0x([0-9A-F]+)', block).group(1), 16)
    vram = int(re.search(r'vram = 0x([0-9A-F]+)', block).group(1), 16)
    for name, v, s in re.findall(r'name = "(\w+)", vram = 0x([0-9A-F]+), size = 0x([0-9A-F]+)', block):
        v, s = int(v, 16), int(s, 16)
        found = set()
        for off in range(rom_base + v - vram, rom_base + v - vram + s, 4):
            c = classify(struct.unpack('>I', rom[off:off + 4])[0])
            if c:
                found.add(c)
        if found:
            hits.append((name, sorted(found)))

with open(report_path, 'w') as f:
    for name, found in hits:
        f.write('%s: %s\n' % (name, ', '.join(found)))
sys.stderr.write('%d kernel functions\n' % len(hits))

# Every kernel function needs a hand-written version, so it must be in the config's ignored list.
config = open(syms.replace('.syms.toml', '.toml')).read()
missing = [name for name, _ in hits if '"%s"' % name not in config]
if missing:
    sys.exit('these functions use CPU control instructions but are not in [patches] ignored: '
             + ', '.join(missing) + '\nAdd them to buriki.toml and write them in runtime/kernel.cpp.')
