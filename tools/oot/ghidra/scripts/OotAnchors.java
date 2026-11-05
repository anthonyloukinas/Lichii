// Post-analysis anchor report: function/string counts, keyword string hits, nvn* symbols.
// Usage: -postScript OotAnchors.java <outfile>
import ghidra.app.script.GhidraScript;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.data.StringDataInstance;
import java.io.*;
import java.util.*;
import java.util.regex.*;

public class OotAnchors extends GhidraScript {
    @Override
    public void run() throws Exception {
        String out = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/tmp/oot-anchors.txt";
        PrintWriter pw = new PrintWriter(new FileWriter(out));
        Listing l = currentProgram.getListing();
        FunctionManager fm = currentProgram.getFunctionManager();
        int nf = fm.getFunctionCount();
        int nNamed = 0;
        for (Function f : fm.getFunctions(true)) if (!f.getName().startsWith("FUN_")) nNamed++;
        int nExt = 0;
        for (Function f : fm.getExternalFunctions()) nExt++;
        pw.printf("program %s  image base %s%n", currentProgram.getName(), currentProgram.getImageBase());
        pw.printf("functions: %d (non-FUN_ named: %d), external functions: %d%n", nf, nNamed, nExt);
        Pattern kw = Pattern.compile("(?i)(nvn|bnsh|shader|\\.bfsha|\\.nxsh|glsl|spirv|shaderarchive|fps|performance|quality|framerate|frame_rate|60)");
        List<String> hits = new ArrayList<>();
        int ns = 0;
        DataIterator it = l.getDefinedData(true);
        while (it.hasNext() && !monitor.isCancelled()) {
            Data d = it.next();
            if (!d.hasStringValue()) continue;
            ns++;
            String s = StringDataInstance.getStringDataInstance(d).getStringValue();
            if (s != null && kw.matcher(s).find() && s.length() < 300)
                hits.add(d.getAddress() + "  " + s.replace("\n", "\\n"));
        }
        pw.printf("defined strings: %d%nkeyword string hits: %d%n", ns, hits.size());
        for (String h : hits) pw.println("  STR " + h);
        SymbolTable st = currentProgram.getSymbolTable();
        TreeSet<String> nvn = new TreeSet<>();
        TreeSet<String> nn = new TreeSet<>();
        SymbolIterator si = st.getAllSymbols(true);
        while (si.hasNext()) {
            Symbol s = si.next();
            String n = s.getName();
            String tag = (s.isExternal() ? "ext " : "    ") + s.getSymbolType() + " " + s.getAddress() + " ";
            if (n.startsWith("nvn")) nvn.add(n + "\t" + tag);
            else if (n.startsWith("_ZN2nn") || n.contains("nn::")) nn.add(n);
        }
        TreeSet<String> nvnNames = new TreeSet<>();
        for (String s : nvn) nvnNames.add(s.split("\t")[0]);
        pw.printf("nvn* symbols: %d entries, %d distinct names%n", nvn.size(), nvnNames.size());
        for (String s : nvn) pw.println("  NVN " + s);
        pw.printf("nn:: (mangled _ZN2nn*) symbols: %d%n", nn.size());
        for (String s : nn) pw.println("  NN " + s);
        pw.close();
        println("anchors written to " + out);
    }
}
