"""The code sections that get recompiled. Shared by make_symbols.py and the build script.

Each section is a raw big-endian code image loaded at `vram`. Only functions whose entry
falls inside [lo, hi) are recompiled.
"""
import os
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

def p(*parts):
    return os.path.join(ROOT, *parts)

SECTIONS = [
    # Main game code: cart ROM 0x100000-0x1FFFFF, copied to RAM 0x80000000 by the BIOS.
    dict(name='.text', image=p('work', 'buriki_code_80000000.bin'), vram=0x80000000,
         funcs=p('work', 'functions_raw.txt'), extra=p('tools', 'extra_functions.txt'),
         lo=0x80000000, hi=0x80100000),
    # BIOS service routines: BIOS 0x8000-0xBFFF. The BIOS copies them to RAM 0x80004000 at
    # boot and the game calls that copy through function pointers. Recompiled at their
    # original address (0x9FC08000+); the runtime maps the RAM-copy addresses onto them.
    dict(name='.bios', image=p('work', 'bios_9fc00000.bin'), vram=0x9FC00000,
         funcs=p('work', 'bios_functions_raw.txt'), extra=None,
         lo=0x9FC08100, hi=0x9FC0C000,
         # The BIOS is hand-written assembly that N64Recomp doesn't fully understand, and the
         # game only uses a few services. So only functions listed here are included; the
         # runtime names any other service the game calls ("indirect call to ...").
         only={0x9FC08100, 0x9FC09300, 0x9FC09B04, 0x9FC09B24}),
]

COMBINED_ROM = p('work', 'recomp_rom.bin')
