#!/usr/bin/env python3
# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

"""Disassembles Turing (TU104/GA10x) MME macro code, as Lichii runs it (src/video_core/macro_turing.h).

    python3 -I tools/nx2/mme_dis.py /tmp/mme_code.bin --positions /tmp/mme_positions.txt --start 0x24b
    python3 -I tools/nx2/mme_dis.py /tmp/mme_code.bin --positions /tmp/mme_positions.txt --all
    python3 -I tools/nx2/mme_dis.py /tmp/mme_code.bin --start 0x24b --count 40 --data-ram /tmp/mme_data_ram.bin

mme_code.bin, mme_data_ram.bin and mme_positions.txt are written by EDEN_NX2_MESH_COUNT_TRACE=1 on
the first zero task count from a macro (in EDEN_NX2_MESH_COUNT_TRACE_DIR, default /tmp). The code
is the uploaded words in order; instruction N is words 3N..3N+2 with word 3N+2 holding bits 0-31.
Starts are instruction indices. Without --count a listing runs to the instruction after the first
END (its delay slot).

Operands: R0-R23, ZERO, IMM (the lane's signed 16-bit immediate), IMMPAIR (the other lane's),
IMM32 (imm0 << 16 | imm1), LOAD0/LOAD1 (parameter FIFO words popped by the instruction). STATE
reads a 3D register (Lichii's op 22), DREAD/DWRITE the data RAM (op 30/31). Outputs set the method
(address & 0xfff, increment bits 12-15) or emit a value to it.
"""

import argparse
import struct
import sys

ALU_OPS = [
    "ADD", "ADDC", "SUB", "SUBB", "MUL", "MULH", "MULU", "EXTENDED",
    "CLZ", "SLL", "SRL", "SRA", "AND", "NAND", "OR", "XOR",
    "MERGE", "SLT", "SLTU", "SLE", "SLEU", "SEQ", "STATE", "LOOP",
    "JAL", "BLT", "BLTU", "BLE", "BLEU", "BEQ", "DREAD", "DWRITE",
]

OUTPUTS = {
    0: None, 1: "ALU0", 2: "ALU1", 3: "LOAD0", 4: "LOAD1", 5: "IMM0", 6: "IMM1",
    8: "IMMHIGH0", 9: "IMMHIGH1", 10: "IMM32",
}

PREDICATES = [
    "UUUU", "TTTT", "FFFF", "TTUU", "FFUU", "TFUU", "TUUU", "FUUU",
    "UUTT", "UUTF", "UUTU", "UUFT", "UUFF", "UUFU", "UUUT", "UUUF",
]

# A few Maxwell 3D methods (register index) the mesh count trace cares about.
METHOD_NAMES = {
    0x154: "mme_dma.address_high", 0x155: "mme_dma.address_low", 0x156: "mme_dma.ram_address",
    0x157: "MME_DMA_READ", 0x158: "MME_DMA_READ_FIFOED",
    0x35D: "vertex_buffer.first", 0x35E: "vertex_buffer.count",
    0x453: "mesh_control", 0x585: "draw.end", 0x586: "draw.begin",
    0x6C0: "report_semaphore.address_high", 0x6C1: "report_semaphore.address_low",
    0x6C2: "report_semaphore.payload", 0x6C3: "report_semaphore.query",
}


def s16(value):
    return value - 0x10000 if value & 0x8000 else value


def immediate(value):
    """A sign-extended 16-bit immediate: decimal when small, hex otherwise."""
    signed = s16(value)
    if -10 < signed < 10:
        return "#%d" % signed
    return "#%s%#x" % ("-" if signed < 0 else "", abs(signed))


class Instruction:
    """One 96-bit instruction's fields, as TuringMacroInterpreter::Decode extracts them."""

    def __init__(self, pc, words):
        self.pc = pc
        self.words = tuple(words)  # (bits 0-31, 32-63, 64-95)
        value = words[0] | (words[1] << 32) | (words[2] << 64)

        def field(low, count):
            return (value >> low) & ((1 << count) - 1)

        self.end = field(0, 1) != 0
        self.pred_mode = field(1, 4)
        self.pred_reg = field(5, 5)
        self.op = [field(10, 5), field(46, 5)]
        self.dst = [field(15, 5), field(51, 5)]
        self.src_a = [field(20, 5), field(56, 5)]
        self.src_b = [field(25, 5), field(61, 5)]
        self.imm = [field(30, 16), field(66, 16)]
        self.out_method = [field(82, 3), field(89, 3)]
        self.out_emit = [field(85, 4), field(92, 4)]
        self.load = [False, False]
        for i in range(2):
            reg, out = 28 + i, 3 + i
            self.load[i] = (self.pred_reg == reg or reg in self.src_a or reg in self.src_b
                            or out in self.out_method or out in self.out_emit)

    def operand(self, reg, lane):
        if reg < 24:
            return "R%d" % reg
        if reg == 24:
            return "ZERO"
        if reg == 25:
            return immediate(self.imm[lane])
        if reg == 26:
            return immediate(self.imm[1 - lane]) + "(pair)"
        if reg == 27:
            return "#%#x" % ((self.imm[0] << 16) | self.imm[1])
        if reg == 28:
            return "LOAD0"
        if reg == 29:
            return "LOAD1"
        return "INVALID%d" % reg

    def constant(self, reg, lane):
        """The operand's value when it is a constant, else None."""
        if reg == 24:
            return 0
        if reg == 25:
            return s16(self.imm[lane]) & 0xFFFFFFFF
        if reg == 26:
            return s16(self.imm[1 - lane]) & 0xFFFFFFFF
        if reg == 27:
            return (self.imm[0] << 16) | self.imm[1]
        return None

    def output_value(self, selector):
        """The output's value when it is a constant, else None."""
        return {5: self.imm[0], 6: self.imm[1], 8: self.imm[0] >> 12, 9: self.imm[1] >> 12,
                10: (self.imm[0] << 16) | self.imm[1]}.get(selector)

    def lane_text(self, lane):
        op = self.op[lane]
        name = ALU_OPS[op]
        dst = self.dst[lane]
        x = self.operand(self.src_a[lane], lane)
        y = self.operand(self.src_b[lane], lane)
        imm = self.imm[lane]
        target = "R%d = " % dst if dst < 24 else ("" if dst == 24 else "INVALID%d = " % dst)
        if op == 0 and dst == 24 and self.src_a[lane] == 24 and self.src_b[lane] == 24:
            return "nop"
        if op == 16:
            return "%sMERGE %s, %s, dst_bit=%d, size=%d, src_bit=%d" % (
                target, x, y, (imm >> 10) & 63, (imm >> 5) & 31, imm & 31)
        if op == 22:
            a = self.constant(self.src_a[lane], lane)
            b = self.constant(self.src_b[lane], lane)
            text = "%sSTATE[%s + %s]" % (target, x, y)
            if a is not None and b is not None:
                method = (a + b) & 0xFFFF
                text += "  ; reg %#x%s" % (method, " " + METHOD_NAMES[method]
                                           if method in METHOD_NAMES else "")
            return text
        if op == 23:
            return "LOOP %s, body=%d -> ends at %#x" % (x, imm, self.pc + imm - 1)
        if op == 24:
            return "%sJAL -> %#x" % (target, self.pc + (imm & 0x7FFF))
        if 25 <= op <= 29:
            offset = ((imm << 19) & 0xFFFFFFFF)
            offset = (offset - (1 << 32) if offset & 0x80000000 else offset) >> 19
            where = "STOP" if offset == -4096 else "%#x" % (self.pc + offset)
            return "%s %s, %s if %s -> %s%s" % (
                name, x, y, "true" if imm & 0x8000 else "false", where,
                " (cancels end)" if imm & 0x4000 else "")
        if op == 30:
            return "%sDREAD[%s]" % (target, x)
        if op == 31:
            return "DWRITE[%s] = %s" % (x, y)
        if op == 7:
            a = self.constant(self.src_a[lane], lane)
            return "EXTENDED %s%s" % (x, "  ; load barrier" if a == 0x1000 else "")
        if op in (5, 8):
            return "%s%s %s" % (target, name, "carry" if op == 5 else x)
        return "%s%s %s, %s" % (target, name, x, y)

    def output_text(self, lane):
        parts = []
        method_sel, emit_sel = self.out_method[lane], self.out_emit[lane]
        if method_sel:
            text = "mthd <- %s" % OUTPUTS.get(method_sel, "INVALID%d" % method_sel)
            value = self.output_value(method_sel)
            if value is not None:
                method = value & 0xFFF
                text += " (%#x%s, inc %d)" % (method, " " + METHOD_NAMES[method]
                                              if method in METHOD_NAMES else "",
                                              (value >> 12) & 15)
            parts.append(text)
        if emit_sel:
            text = "emit <- %s" % OUTPUTS.get(emit_sel, "INVALID%d" % emit_sel)
            value = self.output_value(emit_sel)
            if value is not None:
                text += " (%#x)" % value
            parts.append(text)
        return ", ".join(parts)

    def text(self):
        lanes = []
        predicate = PREDICATES[self.pred_mode]
        for lane in range(2):
            alu = self.lane_text(lane)
            out = self.output_text(lane)
            cond_alu = predicate[lane] if self.pred_mode else "U"
            cond_out = predicate[2 + lane] if self.pred_mode else "U"
            prefix_alu = "" if cond_alu == "U" else "[%s] " % cond_alu
            prefix_out = "" if cond_out == "U" else "[%s] " % cond_out
            lane_parts = []
            if alu != "nop" or not out:
                lane_parts.append(prefix_alu + alu)
            if out:
                lane_parts.append(prefix_out + out)
            lanes.append("; ".join(lane_parts))
        head = ""
        if self.pred_mode:
            head = "if %s %s: " % (self.operand(self.pred_reg, 0), predicate)
        pops = [name for name, used in (("LOAD0", self.load[0]), ("LOAD1", self.load[1])) if used]
        tail = ""
        if pops:
            tail += "  ; pops %s" % "+".join(pops)
        if self.end:
            tail += "  ; END (after the next instruction)"
        return "%s%s | %s%s" % (head, lanes[0], lanes[1], tail)


def decode(code, pc):
    words = (code[pc * 3 + 2], code[pc * 3 + 1], code[pc * 3])
    return Instruction(pc, words)


def read_words(path):
    with open(path, "rb") as file:
        data = file.read()
    return list(struct.unpack("<%dI" % (len(data) // 4), data[: len(data) // 4 * 4]))


def read_positions(path):
    """Slot -> start from mme_positions.txt ('slot 0xstart' lines; '#' comments)."""
    positions = {}
    with open(path) as file:
        for line in file:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            slot, start = line.split()
            positions[int(slot, 0)] = int(start, 0)
    return positions


def listing(code, start, count=None, labels=None):
    """Lines for the instructions from `start`: `count` of them, or to the END's delay slot."""
    labels = labels or {}
    lines = []
    pc = start
    end_seen = None
    while pc * 3 + 2 < len(code):
        if pc in labels:
            lines.append("%s:" % labels[pc])
        inst = decode(code, pc)
        lines.append("  %04x: %08x %08x %08x  %s" % (pc, inst.words[2], inst.words[1],
                                                    inst.words[0], inst.text()))
        if count is not None:
            if pc - start + 1 >= count:
                break
        elif end_seen is not None:
            break
        elif inst.end:
            end_seen = pc
        pc += 1
    return lines


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("code", help="mme_code.bin")
    parser.add_argument("--positions", help="mme_positions.txt (labels the macro starts)")
    parser.add_argument("--start", type=lambda v: int(v, 0), action="append",
                        help="instruction index to disassemble from (repeatable)")
    parser.add_argument("--count", type=int, help="instructions to list (default: to END)")
    parser.add_argument("--all", action="store_true", help="list every macro in --positions")
    parser.add_argument("--data-ram", help="mme_data_ram.bin: prints --dram words")
    parser.add_argument("--dram", default="0:0x40",
                        help="data RAM range ADDR:COUNT for --data-ram (default 0:0x40)")
    args = parser.parse_args(argv)

    code = read_words(args.code)
    labels = {}
    positions = read_positions(args.positions) if args.positions else {}
    for slot, start in sorted(positions.items()):
        labels.setdefault(start, "macro slot %d (method %#x) start %#x" % (
            slot, 0xE00 + slot * 2, start))
    starts = list(args.start or [])
    if args.all:
        starts += sorted(set(positions.values()))
    if not starts and not args.data_ram:
        parser.error("give --start, --all (with --positions) or --data-ram")
    for start in starts:
        print("\n".join(listing(code, start, args.count, labels)))
        print()
    if args.data_ram:
        dram = read_words(args.data_ram)
        address, count = (int(v, 0) for v in args.dram.split(":"))
        for base in range(address, min(address + count, len(dram)), 8):
            row = dram[base: min(base + 8, address + count)]
            print("dram %#05x: %s" % (base, " ".join("%08x" % w for w in row)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
