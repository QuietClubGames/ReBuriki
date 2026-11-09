"""Post-process N64Recomp's C output for the Buriki runtime.

1. Make it valid C++ (the runtime compiles it as C++ so memory access can go through proxy
   objects, see recomp/runtime/hw_mem.h). Jump-table code declares "gpr jr_addend_X = v;"
   mid-function, and C++ forbids a goto jumping past an initialised declaration. Splitting
   it into a declaration plus an assignment is allowed and behaves the same.

2. Add a safe point after every branch label (HW_LABEL_TICK, see runtime/trace.h). Every loop
   goes through a label, so virtual time keeps moving and interrupts get delivered even in
   loops that call no functions (e.g. waiting for vblank). Labels sit on instruction
   boundaries (N64Recomp copies delay-slot instructions into each branch), so taking an
   interrupt there is safe.

Usage: fix_generated.py <generated dir>
"""
import glob, os, re, sys
decl = re.compile(r'^(\s*)gpr (jr_addend_\w+) = (.*);\s*$', re.M)
label = re.compile(r'^(L_[0-9A-F]{8}):\s*$', re.M)
n_decl = n_label = 0
for path in glob.glob(os.path.join(sys.argv[1], 'funcs_*.c')):
    src = open(path).read()
    if 'HW_LABEL_TICK' in src:
        continue  # already processed
    src, a = decl.subn(r'\1gpr \2; \2 = \3;', src)
    src, b = label.subn(r'\1:\n    HW_LABEL_TICK()', src)
    n_decl += a
    n_label += b
    open(path, 'w').write(src)
print('split %d jump-table declarations, added %d loop safe points' % (n_decl, n_label))
