// Apply C prototypes to selected pfn_nvn* slots (and their got_ entries) and to known wrappers,
// so the decompiler shows call arguments. Prototypes for NVN2-only entry points are guesses
// widened to 6 x undefined8 so no argument register is dropped.
// Usage: -postScript NvnTypes.java
import ghidra.app.script.GhidraScript;
import ghidra.app.cmd.function.ApplyFunctionSignatureCmd;
import ghidra.program.model.data.*;
import ghidra.program.model.listing.*;
import ghidra.program.model.symbol.*;
import ghidra.program.model.address.*;
import java.util.*;

public class NvnTypes extends GhidraScript {
    DataTypeManager dtm;
    DataType u64 = UnsignedLongLongDataType.dataType, i32 = IntegerDataType.dataType, vp = new PointerDataType(VoidDataType.dataType);
    FunctionDefinition def(String name, DataType ret, Object... ps) {
        FunctionDefinitionDataType f = new FunctionDefinitionDataType(new CategoryPath("/nvn"), name, dtm);
        List<ParameterDefinition> l = new ArrayList<>();
        for (int i = 0; i < ps.length; i += 2) l.add(new ParameterDefinitionImpl((String) ps[i + 1], (DataType) ps[i], null));
        f.setArguments(l.toArray(new ParameterDefinition[0]));
        f.setReturnType(ret);
        return (FunctionDefinition) dtm.addDataType(f, DataTypeConflictHandler.REPLACE_HANDLER);
    }
    void slot(String nvn, FunctionDefinition f) throws Exception {
        SymbolTable st = currentProgram.getSymbolTable();
        for (String pre : new String[]{"pfn_", "got_pfn_"}) {
            for (Symbol s : st.getGlobalSymbols(pre + nvn)) {
                Address a = s.getAddress();
                DataType t = new PointerDataType(f, dtm);
                if (pre.equals("got_pfn_")) t = new PointerDataType(t, dtm);
                clearListing(a, a.add(7));
                createData(a, t);
                println("typed " + pre + nvn + " @ " + a);
            }
        }
    }
    void wrap(long addr, FunctionDefinition f) {
        Function fn = getFunctionAt(toAddr(addr));
        if (fn == null) { println("no function at " + Long.toHexString(addr)); return; }
        ApplyFunctionSignatureCmd c = new ApplyFunctionSignatureCmd(fn.getEntryPoint(), f, SourceType.USER_DEFINED);
        c.applyTo(currentProgram);
        println("signature " + fn.getEntryPoint() + " " + f.getPrototypeString());
    }
    @Override
    public void run() throws Exception {
        dtm = currentProgram.getDataTypeManager();
        DataType v = VoidDataType.dataType;
        slot("nvnCommandBufferBindStorageBuffer", def("nvnCommandBufferBindStorageBuffer_t", v, vp, "cmd", i32, "stage", i32, "index", u64, "gpuAddr", u64, "size"));
        slot("nvnCommandBufferBindUniformBuffer", def("nvnCommandBufferBindUniformBuffer_t", v, vp, "cmd", i32, "stage", i32, "index", u64, "gpuAddr", u64, "size"));
        slot("nvnCommandBufferBindStorageBuffers", def("nvnCommandBufferBindStorageBuffers_t", v, vp, "cmd", i32, "stage", i32, "first", i32, "count", vp, "ranges"));
        slot("nvnCommandBufferBindUniformBuffers", def("nvnCommandBufferBindUniformBuffers_t", v, vp, "cmd", i32, "stage", i32, "first", i32, "count", vp, "ranges"));
        slot("nvnCommandBufferDispatchCompute", def("nvnCommandBufferDispatchCompute_t", v, vp, "cmd", i32, "groupsX", i32, "groupsY", i32, "groupsZ"));
        slot("nvnCommandBufferDispatchComputeIndirect", def("nvnCommandBufferDispatchComputeIndirect_t", v, vp, "cmd", u64, "indirectAddr"));
        slot("nvnCommandBufferSetShaderScratchMemory", def("nvnCommandBufferSetShaderScratchMemory_t", v, vp, "cmd", vp, "pool", u64, "offset", u64, "size"));
        slot("nvnCommandBufferBindProgram", def("nvnCommandBufferBindProgram_t", v, vp, "cmd", vp, "program", i32, "stageBits"));
        slot("nvnCommandBufferPushComputeConstants", def("nvnCommandBufferPushComputeConstants_t", v, vp, "cmd", u64, "a1", u64, "a2", u64, "a3", u64, "a4", u64, "a5"));
        for (String g : new String[]{"nvnCommandBufferDrawMeshTasks", "nvnCommandBufferDrawMeshTasksIndirect", "nvnCommandBufferMultiDrawMeshTasksIndirect",
                "nvnCommandBufferMultiDrawMeshTasksIndirectCount", "nvnCommandBufferDrawMeshTasks3D", "nvnCommandBufferDrawMeshTasks3DIndirect",
                "nvnCommandBufferMultiDrawMeshTasks3DIndirect", "nvnCommandBufferMultiDrawMeshTasks3DIndirectCount", "nvnCommandBufferEvaluateDlss",
                "nvnCommandBufferDrawElementsIndirect", "nvnCommandBufferDrawArraysIndirect", "nvnCommandBufferMultiDrawElementsIndirectCount",
                "nvnCommandBufferMultiDrawArraysIndirectCount", "nvnCommandBufferCopyBufferToBuffer", "nvnCommandBufferClearBuffer", "nvnCommandBufferBarrier"})
            slot(g, def(g + "_t", v, vp, "cmd", u64, "a1", u64, "a2", u64, "a3", u64, "a4", u64, "a5"));
        // engine wrappers (cmd object holds NVNcommandBuffer* at +0xd8 / +0xd0)
        wrap(0x71009cba40L, def("DispatchWrapD8_t", v, vp, "self", i32, "groupsX", i32, "groupsY", i32, "groupsZ"));
        wrap(0x710129d7f0L, def("DispatchWrapD0_t", v, vp, "self", i32, "groupsX", i32, "groupsY", i32, "groupsZ"));
        wrap(0x710129e080L, def("GfxSetStorageBuffer_t", v, vp, "self", i32, "slot", i32, "gfxStage", vp, "gpuAddrObj", u64, "size"));
    }
}
