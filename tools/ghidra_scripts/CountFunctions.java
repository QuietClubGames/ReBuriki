// Prints how many functions Ghidra found, and writes their addresses to a text file.
// Usage (post-script): CountFunctions.java <output path>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.Function;
import java.io.PrintWriter;

public class CountFunctions extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        int n = 0;
        PrintWriter out = args.length > 0 ? new PrintWriter(args[0]) : null;
        for (Function f : currentProgram.getFunctionManager().getFunctions(true)) {
            n++;
            if (out != null) {
                out.printf("%s 0x%x%n", f.getEntryPoint(), f.getBody().getNumAddresses());
            }
        }
        if (out != null) out.close();
        println("FUNCTION_COUNT " + n);
    }
}
