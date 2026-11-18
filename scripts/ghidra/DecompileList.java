// Decompile a list of functions to one text file.
// analyzeHeadless <proj> <name> -process EBOOT.ELF -noanalysis -readOnly
//   -scriptPath scripts/ghidra -postScript DecompileList.java <out.txt> <addr> [addr...]
//@category racacit
import ghidra.app.decompiler.DecompInterface;
import ghidra.app.decompiler.DecompileResults;
import ghidra.app.script.GhidraScript;
import ghidra.program.model.address.Address;
import ghidra.program.model.listing.Function;

import java.io.PrintWriter;

public class DecompileList extends GhidraScript {
    @Override
    public void run() throws Exception {
        String[] args = getScriptArgs();
        DecompInterface di = new DecompInterface();
        di.openProgram(currentProgram);
        try (PrintWriter out = new PrintWriter(args[0], "UTF-8")) {
            for (int i = 1; i < args.length; i++) {
                Address a = toAddr(args[i]);
                Function f = getFunctionContaining(a);
                if (f == null) f = createFunction(a, null);
                if (f == null) { out.println("// " + args[i] + ": no function"); continue; }
                DecompileResults r = di.decompileFunction(f, 120, monitor);
                out.println("// ==== " + args[i] + " (" + f.getName() + " @ " + f.getEntryPoint() + ")");
                out.println(r.decompileCompleted() ? r.getDecompiledFunction().getC() : "// failed: " + r.getErrorMessage());
            }
        }
        di.dispose();
    }
}
