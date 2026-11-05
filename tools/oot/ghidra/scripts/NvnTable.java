// Recover the NVN function-pointer table: for every nvn* name string, find the code that
// passes it to the proc-address loader and the global slot the returned pointer is stored to.
// Labels each slot pfn_<name> (USER_DEFINED) and types it as a pointer.
// Usage: -postScript NvnTable.java <outdir>
import ghidra.app.script.GhidraScript;
import ghidra.app.plugin.core.analysis.ConstantPropagationContextEvaluator;
import ghidra.program.util.SymbolicPropogator;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import ghidra.program.model.lang.*;
import ghidra.program.model.scalar.Scalar;
import ghidra.program.model.data.*;
import java.io.*;
import java.util.*;

public class NvnTable extends GhidraScript {
    PrintWriter log;
    @Override
    public void run() throws Exception {
        String outdir = getScriptArgs().length > 0 ? getScriptArgs()[0] : "/tmp";
        log = new PrintWriter(new FileWriter(outdir + "/nvn-table.log"));
        Listing l = currentProgram.getListing();
        ReferenceManager rm = currentProgram.getReferenceManager();
        FunctionManager fm = currentProgram.getFunctionManager();
        Map<Address, String> strs = new HashMap<>();
        DataIterator it = l.getDefinedData(true);
        while (it.hasNext()) {
            Data d = it.next();
            if (!d.hasStringValue()) continue;
            Object v = d.getValue();
            if (!(v instanceof String)) continue;
            String s = (String) v;
            if (s.matches("nvn[A-Z][A-Za-z0-9_]*")) strs.put(d.getAddress(), s);
        }
        log.printf("nvn name strings: %d%n", strs.size());
        Map<Function, Integer> hist = new HashMap<>();
        int dataRefs = 0, noRefs = 0;
        List<Address> dataRefFrom = new ArrayList<>();
        for (Address a : strs.keySet()) {
            ReferenceIterator ri = rm.getReferencesTo(a);
            boolean any = false;
            while (ri.hasNext()) {
                Reference r = ri.next(); any = true;
                Function f = fm.getFunctionContaining(r.getFromAddress());
                if (f != null) hist.merge(f, 1, Integer::sum);
                else { dataRefs++; if (dataRefFrom.size() < 20) dataRefFrom.add(r.getFromAddress()); }
            }
            if (!any) noRefs++;
        }
        log.printf("string refs: from-functions=%d data-refs=%d strings-without-refs=%d%n", hist.size(), dataRefs, noRefs);
        for (Address a : dataRefFrom) log.println("  dataref from " + a);
        List<Map.Entry<Function, Integer>> hl = new ArrayList<>(hist.entrySet());
        hl.sort((x, y) -> y.getValue() - x.getValue());
        for (int i = 0; i < Math.min(25, hl.size()); i++) {
            Function f = hl.get(i).getKey();
            log.printf("  loader? %s %s refs=%d size=%d callers=%d%n", f.getEntryPoint(), f.getName(), hl.get(i).getValue(),
                f.getBody().getNumAddresses(), f.getCallingFunctions(monitor).size());
        }
        // slot recovery
        TreeMap<Address, String> slots = new TreeMap<>(); Map<Address, Address> got = new HashMap<>();
        Map<String, List<Address>> byName = new TreeMap<>();
        int dumped = 0;
        for (Map.Entry<Function, Integer> e : hl) {
            if (e.getValue() < 3) continue;
            Function f = e.getKey();
            SymbolicPropogator sp = new SymbolicPropogator(currentProgram);
            sp.flowConstants(f.getEntryPoint(), f.getBody(), new ConstantPropagationContextEvaluator(monitor, true), true, monitor);
            String pending = null, callName = null; boolean called = false; int found = 0;
            InstructionIterator ii = l.getInstructions(f.getBody(), true);
            int n = 0;
            while (ii.hasNext()) {
                Instruction ins = ii.next();
                if (dumped < 1 && n < 120) log.println("    " + ins.getAddress() + "  " + ins);
                n++;
                for (Reference r : ins.getReferencesFrom()) {
                    String nm = strs.get(r.getToAddress());
                    if (nm != null) { pending = nm; }
                }
                FlowType ft = ins.getFlowType();
                if (ft.isCall()) { callName = pending; pending = null; called = true; continue; }
                String mn = ins.getMnemonicString();
                if (callName != null && called && (mn.equals("str") || mn.equals("stur"))) {
                    String pend = callName;
                    Object[] o0 = ins.getOpObjects(0);
                    if (o0.length != 1 || !(o0[0] instanceof Register) || !((Register) o0[0]).getName().equals("x0")) continue;
                    Address tgt = null;
                    for (Reference r : ins.getReferencesFrom())
                        if (r.getReferenceType().isWrite() || r.getReferenceType().isData()) { tgt = r.getToAddress(); break; }
                    if (tgt == null) {
                        Object[] o1 = ins.getOpObjects(1);
                        Register base = null; long off = 0;
                        for (Object o : o1) {
                            if (o instanceof Register) base = (Register) o;
                            else if (o instanceof Scalar) off = ((Scalar) o).getSignedValue();
                        }
                        if (base != null) {
                            SymbolicPropogator.Value v = sp.getRegisterValue(ins.getAddress(), base);
                            if (v != null && !v.isRegisterRelativeValue())
                                tgt = toAddr(v.getValue() + off);
                            else if (v != null) log.printf("    reg-relative store %s %s (%s+%d) for %s%n", ins.getAddress(), ins, v.getRelativeRegister(), v.getValue() + off, pend);
                        }
                    }
                    if (tgt != null) {
                        slots.put(tgt, pend);
                        Object[] ob = ins.getOpObjects(1);
                        Register br = null; for (Object o : ob) if (o instanceof Register) br = (Register) o;
                        Instruction p = ins.getPrevious();
                        for (int k = 0; k < 8 && p != null && br != null; k++, p = p.getPrevious()) {
                            if (p.getMnemonicString().equals("ldr") && p.getOpObjects(0).length == 1 && br.equals(p.getOpObjects(0)[0])) {
                                for (Reference r : p.getReferencesFrom()) if (r.getReferenceType().isRead()) got.put(tgt, r.getToAddress());
                                break;
                            }
                        }
                        byName.computeIfAbsent(pend, k -> new ArrayList<>()).add(tgt);
                        found++;
                        if (found <= 3) log.printf("    MAP %s <- %s via %s %s%n", tgt, pend, ins.getAddress(), ins);
                    } else log.printf("    UNRESOLVED store %s %s for %s%n", ins.getAddress(), ins, pend);
                    callName = null; called = false;
                }
            }
            dumped++;
            log.printf("  function %s: %d slots mapped (of %d string refs)%n", f.getEntryPoint(), found, e.getValue());
        }
        log.printf("slots mapped: %d, distinct names: %d%n", slots.size(), byName.size());
        // label
        SymbolTable st = currentProgram.getSymbolTable();
        // drop pfn_ labels from earlier (possibly wrong) runs
        List<Symbol> old = new ArrayList<>();
        for (String pat : new String[]{"pfn_nvn*", "got_pfn_nvn*"}) { SymbolIterator osi = st.getSymbolIterator(pat, true);
        while (osi.hasNext()) old.add(osi.next()); }
        for (Symbol s : old) s.delete();
        log.printf("removed %d old pfn_ labels%n", old.size());
        PrintWriter tab = new PrintWriter(new FileWriter(outdir + "/nvn-slots.txt"));
        Address prev = null;
        for (Map.Entry<Address, String> e : slots.entrySet()) {
            Address a = e.getKey();
            String lab = "pfn_" + e.getValue();
            List<Address> same = byName.get(e.getValue());
            if (same.size() > 1) lab += "_" + same.indexOf(a);
            tab.printf("%s %s %s got=%s%n", a, prev == null ? "-" : Long.toString(a.subtract(prev)), lab, got.get(a));
            prev = a;
            try {
                boolean has = false;
                for (Symbol s : st.getSymbols(a)) if (s.getName().equals(lab)) has = true;
                if (!has) { Symbol s = st.createLabel(a, lab, SourceType.USER_DEFINED); s.setPrimary(); }
                Address g = got.get(a);
                if (g != null) st.createLabel(g, "got_" + lab, SourceType.USER_DEFINED);
                Data d = l.getDataAt(a);
                if (d == null || !d.isDefined()) {
                    clearListing(a, a.add(7));
                    createData(a, new PointerDataType());
                }
            } catch (Exception ex) { log.println("  label fail " + a + " " + ex); }
        }
        tab.close();
        // names never mapped
        int unm = 0;
        for (String s : new TreeSet<>(strs.values())) if (!byName.containsKey(s)) { if (unm < 400) log.println("  unmapped " + s); unm++; }
        log.printf("unmapped names: %d%n", unm);
        log.close();
        println("NvnTable done: " + slots.size() + " slots");
    }
}
