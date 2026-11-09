// Seeds function entry points Ghidra's auto-analysis missed, re-analyzes, then exports
// every function as "entry_hex end_hex" (end = one past the highest address in its body).
//
// Seeds:
//   1. Every JAL target inside the program (always a real function entry).
//   2. The first non-zero word after each "jr ra" + delay slot, if it isn't already inside a
//      function (catches functions only reached through pointers).
//
//   3. Optional: addresses listed (one hex per line) in a seeds file, always treated as entries.
//
// Usage (post-script): SeedAndExport.java <output path> [seeds file]
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.cmd.function.CreateFunctionCmd;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.FunctionManager;
import ghidra.program.model.mem.Memory;
import java.io.PrintWriter;
import java.util.TreeSet;

public class SeedAndExport extends GhidraScript {
    static final int JR_RA = 0x03E00008;

    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        Memory mem = currentProgram.getMemory();
        FunctionManager fm = currentProgram.getFunctionManager();
        Address start = mem.getMinAddress();
        long base = start.getOffset();
        long size = mem.getMaxAddress().getOffset() - base + 1;

        int[] w = new int[(int) (size / 4)];
        for (int i = 0; i < w.length; i++) {
            w[i] = mem.getInt(start.add(i * 4L));
        }

        TreeSet<Long> jalTargets = new TreeSet<>();
        TreeSet<Long> afterReturn = new TreeSet<>();
        for (int i = 0; i < w.length; i++) {
            if ((w[i] >>> 26) == 3) {
                long t = (base & 0xF0000000L) | ((long) (w[i] & 0x3FFFFFF) << 2);
                if (t >= base && t < base + size) jalTargets.add(t);
            }
            if (w[i] == JR_RA) {
                int j = i + 2;
                while (j < w.length && w[j] == 0) j++;
                if (j < w.length) afterReturn.add(base + j * 4L);
            }
        }
        println("SEEDS jal=" + jalTargets.size() + " after_jr_ra=" + afterReturn.size());

        if (args.length > 1) {
            for (String line : java.nio.file.Files.readAllLines(java.nio.file.Path.of(args[1]))) {
                line = line.split("#")[0].trim();
                if (!line.isEmpty()) jalTargets.add(Long.parseUnsignedLong(line, 16));
            }
        }

        int created = 0;
        for (long t : jalTargets) {
            created += seed(t, fm, true);
        }
        for (long t : afterReturn) {
            created += seed(t, fm, false);
        }
        println("SEEDED " + created);
        analyzeChanges(currentProgram);

        int n = 0;
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (Function f : fm.getFunctions(true)) {
                long entry = f.getEntryPoint().getOffset();
                long end = f.getBody().getMaxAddress().getOffset() + 1;
                out.printf("%08x %08x%n", entry, end);
                n++;
            }
        }
        println("EXPORTED " + n);
    }

    int seed(long off, FunctionManager fm, boolean force) {
        Address a = toAddr(off);
        if (fm.getFunctionAt(a) != null) return 0;
        if (!force && fm.getFunctionContaining(a) != null) return 0;
        new DisassembleCommand(a, null, true).applyTo(currentProgram, monitor);
        if (getInstructionAt(a) == null) return 0;
        if (force) {
            Function owner = fm.getFunctionContaining(a);
            if (owner != null) {
                // A real entry point sits inside another function's body: shrink that one so
                // both can exist; analysis redraws the bodies afterwards.
                AddressSet body = new AddressSet(owner.getBody());
                body.deleteRange(a, owner.getBody().getMaxAddress());
                try { owner.setBody(body); } catch (Exception e) { return 0; }
            }
        }
        return new CreateFunctionCmd(a).applyTo(currentProgram, monitor) ? 1 : 0;
    }
}
