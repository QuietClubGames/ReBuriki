"""Turn Ghidra's exported function ranges into an N64Recomp symbols file.

Usage: make_symbols.py <out.toml>

Sections come from tools/sections.py. Their images are concatenated into one ROM file
(work/recomp_rom.bin) because N64Recomp takes a single ROM. Each function's size is cut at
the next function's entry and trailing zero padding is trimmed, so nothing overlaps.
"""
import struct, sys
from sections import SECTIONS, COMBINED_ROM

out_path = sys.argv[1]
rom = bytearray()
toml = []
summary = []

for sec in SECTIONS:
    code = open(sec['image'], 'rb').read()
    rom_base = len(rom)
    rom += code
    vram = sec['vram']

    funcs = []
    for line in open(sec['funcs']):
        e, end = (int(x, 16) for x in line.split())
        funcs.append((e, end))
    known = {e for e, _ in funcs}
    if sec['extra']:
        for line in open(sec['extra']):
            line = line.split('#')[0].strip()
            if line and int(line, 16) not in known:
                funcs.append((int(line, 16), vram + len(code)))
    funcs = sorted(f for f in funcs if sec['lo'] <= f[0] < sec['hi'])
    only = sec.get('only')

    def word(addr):
        off = addr - vram
        return struct.unpack('>I', code[off:off + 4])[0]

    rows = []
    for i, (e, end) in enumerate(funcs):
        nxt = funcs[i + 1][0] if i + 1 < len(funcs) else sec['hi']
        end = min(end, nxt)
        while end - 8 > e and word(end - 4) == 0 and word(end - 8) != 0x03E00008:
            end -= 4
        if end > e and (only is None or e in only):
            rows.append((e, end - e))

    toml.append('[[section]]\nname = "%s"\nrom = 0x%X\nvram = 0x%08X\nsize = 0x%X\n\nfunctions = [\n'
                % (sec['name'], rom_base, vram, len(code)))
    for e, size in rows:
        toml.append('    { name = "func_%08X", vram = 0x%08X, size = 0x%X },\n' % (e, e, size))
    toml.append(']\n\n')
    summary.append('%s: %d functions, %08X-%08X' % (sec['name'], len(rows), rows[0][0], rows[-1][0] + rows[-1][1]))

open(COMBINED_ROM, 'wb').write(rom)
open(out_path, 'w').write(''.join(toml))
print('; '.join(summary))
