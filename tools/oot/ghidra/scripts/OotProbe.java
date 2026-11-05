// Generic probe driven by a spec file. Lines:
//   slot <regex> [maxReaders] [maxCallers]   readers of labelled pfn_ slots (regex on label) + callers of the readers
//   str <exact string>                       functions referencing the defined string
//   sym <regex>                              functions calling/referencing symbols whose name matches
//   func <hexaddr> [depthCallers]            decompile function containing addr, list callers (and decompile them if depth>0)
//   callers <hexaddr> [max]                  decompile up to max callers of the function at addr
// Writes <outdir>/probe-<specname>.txt and <outdir>/decomp/<entry>.c
// Usage: -postScript OotProbe.java <specfile> <outdir>
import ghidra.app.script.GhidraScript;
import ghidra.app.decompiler.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.io.*;
import java.nio.file.*;
import java.util.*;
import java.util.regex.*;

public class OotProbe extends GhidraScript {
    PrintWriter out; String outdir; DecompInterface di; Set<Address> done = new HashSet<>();
    FunctionManager fm; ReferenceManager rm;

    String decomp(Function f) throws Exception {
        if (f == null) return "";
        File fo = new File(outdir + "/decomp/" + f.getEntryPoint() + ".c");
        if (done.contains(f.getEntryPoint()) || fo.exists()) { done.add(f.getEntryPoint()); return fo.getName(); }
        done.add(f.getEntryPoint());
        DecompileResults r = di.decompileFunction(f, 90, monitor);
        String c = (r != null && r.decompileCompleted()) ? r.getDecompiledFunction().getC() : ("// decompile failed: " + (r == null ? "null" : r.getErrorMessage()));
        try (PrintWriter p = new PrintWriter(new FileWriter(fo))) {
            p.printf("// %s @ %s size=%d%n", f.getName(), f.getEntryPoint(), f.getBody().getNumAddresses());
            p.print(c);
        }
        return fo.getName();
    }
    List<Function> callers(Function f, int max, int[] dataRefsOut) {
        List<Function> res = new ArrayList<>(); Set<Function> seen = new HashSet<>();
        int dr = 0;
        ReferenceIterator ri = rm.getReferencesTo(f.getEntryPoint());
        while (ri.hasNext()) {
            Reference r = ri.next();
            Function c = fm.getFunctionContaining(r.getFromAddress());
            if (r.getReferenceType().isCall() && c != null) { if (seen.add(c) && res.size() < max) res.add(c); }
            else dr++;
        }
        if (dataRefsOut != null) { dataRefsOut[0] = dr; dataRefsOut[1] = seen.size(); }
        return res;
    }
    String fn(Function f) { return f == null ? "null" : f.getEntryPoint() + " " + f.getName() + " sz=" + f.getBody().getNumAddresses(); }

    void refsTo(Address a, String what, int maxDec, int maxCallers) throws Exception {
        Map<Function, List<Address>> readers = new LinkedHashMap<>();
        ReferenceIterator ri = rm.getReferencesTo(a);
        while (ri.hasNext()) {
            Reference r = ri.next();
            Function f = fm.getFunctionContaining(r.getFromAddress());
            readers.computeIfAbsent(f, k -> new ArrayList<>()).add(r.getFromAddress());
        }
        out.printf("== %s @ %s : %d referencing functions%n", what, a, readers.size());
        int nd = 0;
        for (Map.Entry<Function, List<Address>> e : readers.entrySet()) {
            Function f = e.getKey();
            int[] cnt = new int[2];
            List<Function> cl = f == null ? List.of() : callers(f, maxCallers, cnt);
            String file = (f != null && nd++ < maxDec) ? decomp(f) : "";
            out.printf("  R %s at %s  callers=%d nonCallRefs=%d %s%n", fn(f), e.getValue(), cnt[1], cnt[0], file);
            if (f != null && f.getBody().getNumAddresses() < 0x300) {
                for (Function c : cl) out.printf("     C %s %s%n", fn(c), maxCallers > 0 ? decomp(c) : "");
            }
        }
    }

    @Override
    public void run() throws Exception {
        String spec = getScriptArgs()[0]; outdir = getScriptArgs()[1];
        new File(outdir + "/decomp").mkdirs();
        String sn = new File(spec).getName().replaceAll("\\..*", "");
        out = new PrintWriter(new FileWriter(outdir + "/probe-" + sn + ".txt"));
        fm = currentProgram.getFunctionManager(); rm = currentProgram.getReferenceManager();
        di = new DecompInterface();
        DecompileOptions opt = new DecompileOptions();
        di.setOptions(opt); di.toggleCCode(true); di.toggleSyntaxTree(false);
        di.openProgram(currentProgram);
        SymbolTable st = currentProgram.getSymbolTable();
        Map<String, Address> strIdx = null;
        for (String line : Files.readAllLines(Paths.get(spec))) {
            line = line.trim();
            if (line.isEmpty() || line.startsWith("#")) continue;
            String[] w = line.split("\\s+", 2);
            String cmd = w[0], arg = w.length > 1 ? w[1] : "";
            out.println("### " + line);
            try {
            if (cmd.equals("slot")) {
                String[] p = arg.split("\\s+");
                Pattern re = Pattern.compile(p[0]);
                int md = p.length > 1 ? Integer.parseInt(p[1]) : 40, mc = p.length > 2 ? Integer.parseInt(p[2]) : 40;
                SymbolIterator si = st.getSymbolIterator("pfn_*", true);
                while (si.hasNext()) {
                    Symbol s = si.next();
                    if (!re.matcher(s.getName()).matches()) continue;
                    refsTo(s.getAddress(), s.getName(), md, mc);
                    for (Symbol g : st.getGlobalSymbols("got_" + s.getName())) refsTo(g.getAddress(), g.getName(), md, mc);
                }
            } else if (cmd.equals("str")) {
                if (strIdx == null) {
                    strIdx = new HashMap<>();
                    DataIterator it = currentProgram.getListing().getDefinedData(true);
                    while (it.hasNext()) { Data d = it.next(); if (d.hasStringValue() && d.getValue() instanceof String) strIdx.putIfAbsent((String) d.getValue(), d.getAddress()); }
                }
                Address a = strIdx.get(arg);
                if (a == null) { out.println("  string not found"); continue; }
                refsTo(a, "\"" + arg + "\"", 30, 20);
            } else if (cmd.equals("ref")) {
                String[] p = arg.split("\\s+");
                Address a = toAddr(Long.parseUnsignedLong(p[0].replace("0x", ""), 16));
                refsTo(a, "ref " + a, p.length > 1 ? Integer.parseInt(p[1]) : 30, p.length > 2 ? Integer.parseInt(p[2]) : 0);
            } else if (cmd.equals("strre")) {
                String[] p = arg.split("\\s+");
                Pattern re = Pattern.compile(p[0]);
                int md = p.length > 1 ? Integer.parseInt(p[1]) : 0;
                DataIterator it = currentProgram.getListing().getDefinedData(true);
                int n = 0;
                while (it.hasNext() && n < 300) {
                    Data d = it.next();
                    if (!d.hasStringValue() || !(d.getValue() instanceof String)) continue;
                    String sv = (String) d.getValue();
                    if (!re.matcher(sv).find() || sv.length() > 200) continue;
                    n++;
                    StringBuilder sb = new StringBuilder();
                    ReferenceIterator ri = rm.getReferencesTo(d.getAddress());
                    while (ri.hasNext()) { Reference r = ri.next(); Function f = fm.getFunctionContaining(r.getFromAddress());
                        sb.append(f == null ? "d:" + r.getFromAddress() : f.getEntryPoint().toString()).append(' ');
                        if (f != null && md > 0) { decomp(f); } }
                    out.printf("  S %s \"%s\" <- %s%n", d.getAddress(), sv.replace("\n", "\\n"), sb);
                }
            } else if (cmd.equals("sym")) {
                Pattern re = Pattern.compile(arg);
                SymbolIterator si = st.getAllSymbols(true);
                List<Symbol> hits = new ArrayList<>();
                while (si.hasNext()) { Symbol s = si.next(); if (re.matcher(s.getName()).find()) hits.add(s); }
                for (Symbol s : hits) {
                    out.printf("  SYM %s %s ext=%s%n", s.getName(), s.getAddress(), s.isExternal());
                    refsTo(s.getAddress(), s.getName(), 30, 20);
                    // thunks pointing at an external
                    if (s.getObject() instanceof Function) {
                        Function f = (Function) s.getObject();
                        for (Address t : f.getFunctionThunkAddresses(true) == null ? new Address[0] : f.getFunctionThunkAddresses(true))
                            refsTo(t, "thunk " + t, 30, 20);
                    }
                }
            } else if (cmd.equals("func") || cmd.equals("callers")) {
                String[] p = arg.split("\\s+");
                Address a = toAddr(Long.parseUnsignedLong(p[0].replace("0x", ""), 16));
                Function f = fm.getFunctionContaining(a);
                int n = p.length > 1 ? Integer.parseInt(p[1]) : 0;
                int[] cnt = new int[2];
                List<Function> cl = f == null ? List.of() : callers(f, Math.max(n, 30), cnt);
                out.printf("  F %s %s callers=%d nonCallRefs=%d%n", fn(f), cmd.equals("func") ? decomp(f) : "", cnt[1], cnt[0]);
                int k = 0;
                for (Function c : cl) out.printf("     C %s %s%n", fn(c), k++ < n ? decomp(c) : "");
            }
            } catch (Exception ex) { out.println("  ERROR " + ex); }
            out.flush();
        }
        out.close(); di.dispose();
        println("probe done");
    }
}
