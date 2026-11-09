// Writes a plain disassembly listing of one or more address ranges.
// Usage (post-script): DumpListing.java <output path> <start_hex>:<end_hex> [more ranges...]
import ghidra.app.cmd.disassemble.DisassembleCommand;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.address.AddressSet;
import ghidra.program.model.listing.Function;
import ghidra.program.model.listing.Instruction;
import java.io.PrintWriter;

public class DumpListing extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        try (PrintWriter out = new PrintWriter(args[0])) {
            for (int r = 1; r < args.length; r++) {
                String[] p = args[r].split(":");
                Address a = toAddr(Long.parseUnsignedLong(p[0], 16));
                Address b = toAddr(Long.parseUnsignedLong(p[1], 16));
                new DisassembleCommand(new AddressSet(a, b), null, true).applyTo(currentProgram, monitor);
                out.printf("==== %s - %s%n", a, b);
                Address cur = a;
                while (cur.compareTo(b) <= 0) {
                    Function f = getFunctionAt(cur);
                    if (f != null) out.printf("%n%s:%n", f.getName());
                    Instruction ins = getInstructionAt(cur);
                    if (ins != null) {
                        out.printf("  %s  %08x  %s%n", cur, getInt(cur), ins);
                        cur = cur.add(ins.getLength());
                    } else {
                        out.printf("  %s  %08x  .word%n", cur, getInt(cur));
                        cur = cur.add(4);
                    }
                }
            }
        }
    }
}
