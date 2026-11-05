# SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

import os
import struct
import tempfile
import unittest

import mme_dis

ZERO, IMM, IMMPAIR, IMM32, LOAD0, LOAD1 = 24, 25, 26, 27, 28, 29


def encode(end=0, pred_mode=0, pred_reg=0, lanes=((0, ZERO, ZERO, ZERO, 0),) * 2,
           out=((0, 0), (0, 0))):
    """Code words (in upload order) for one instruction. lanes: (op, dst, src_a, src_b, imm)."""
    value = end | (pred_mode << 1) | (pred_reg << 5)
    for lane, (op, dst, src_a, src_b, imm) in enumerate(lanes):
        base = 36 * lane
        value |= op << (10 + base) | dst << (15 + base) | src_a << (20 + base)
        value |= src_b << (25 + base) | (imm & 0xFFFF) << (30 + base)
    value |= out[0][0] << 82 | out[0][1] << 85 | out[1][0] << 89 | out[1][1] << 92
    low, mid, high = value & 0xFFFFFFFF, (value >> 32) & 0xFFFFFFFF, value >> 64
    return [high, mid, low]


NOP = (0, ZERO, ZERO, ZERO, 0)


def at(words, pc):
    """The instruction in `words` (upload order) as if it were at `pc`."""
    return mme_dis.Instruction(pc, (words[2], words[1], words[0]))


class DecodeTest(unittest.TestCase):
    def text(self, **fields):
        return mme_dis.decode(encode(**fields), 0).text()

    def test_fields_round_trip(self):
        words = encode(end=1, pred_mode=5, pred_reg=3,
                       lanes=((14, 7, 1, LOAD1, 0x1234), (21, 9, 2, IMM, 0xFFFE)),
                       out=((5, 1), (2, 4)))
        inst = mme_dis.decode(words, 0)
        self.assertTrue(inst.end)
        self.assertEqual((inst.pred_mode, inst.pred_reg), (5, 3))
        self.assertEqual(inst.op, [14, 21])
        self.assertEqual(inst.dst, [7, 9])
        self.assertEqual(inst.src_a, [1, 2])
        self.assertEqual(inst.src_b, [LOAD1, IMM])
        self.assertEqual(inst.imm, [0x1234, 0xFFFE])
        self.assertEqual(inst.out_method, [5, 2])
        self.assertEqual(inst.out_emit, [1, 4])
        self.assertEqual(inst.load, [False, True])

    def test_count_write(self):
        text = self.text(lanes=((0, 1, LOAD0, ZERO, 0x135E), NOP), out=((5, 1), (0, 0)))
        self.assertIn("R1 = ADD LOAD0, ZERO", text)
        self.assertIn("mthd <- IMM0 (0x35e vertex_buffer.count, inc 1)", text)
        self.assertIn("emit <- ALU0", text)
        self.assertIn("pops LOAD0", text)

    def test_register_and_data_ram(self):
        text = self.text(lanes=((22, 2, ZERO, IMM, 0x35D), (30, 3, 1, ZERO, 0)))
        self.assertIn("R2 = STATE[ZERO + #0x35d]  ; reg 0x35d vertex_buffer.first", text)
        self.assertIn("R3 = DREAD[R1]", text)
        text = self.text(lanes=((31, ZERO, 2, 3, 0), (16, 4, 4, 5, (8 << 10) | (4 << 5) | 2)))
        self.assertIn("DWRITE[R2] = R3", text)
        self.assertIn("R4 = MERGE R4, R5, dst_bit=8, size=4, src_bit=2", text)

    def test_branches(self):
        inst = at(encode(lanes=((29, ZERO, 1, ZERO, 0x8000 | 5), NOP)), 0x10)
        self.assertIn("BEQ R1, ZERO if true -> 0x15", inst.text())
        inst = at(encode(lanes=((25, ZERO, 1, 2, 0x4000 | (-3 & 0x1FFF)), NOP)), 0x10)
        self.assertIn("BLT R1, R2 if false -> 0xd (cancels end)", inst.text())
        inst = mme_dis.decode(encode(lanes=((29, ZERO, ZERO, ZERO, 0x8000 | 0x1000), NOP)), 0)
        self.assertIn("-> STOP", inst.text())
        self.assertIn("LOOP R1, body=3 -> ends at 0x6",
                      at(encode(lanes=((23, ZERO, 1, ZERO, 3), NOP)), 4).text())

    def test_predicate_and_immediates(self):
        text = self.text(pred_mode=5, pred_reg=6,
                         lanes=((0, 1, IMM, IMMPAIR, -2), (0, 2, IMM32, ZERO, 7)))
        self.assertTrue(text.startswith("if R6 TFUU: [T] R1 = ADD #-2, #7(pair)"))
        self.assertIn("[F] R2 = ADD #0xfffe0007, ZERO", text)

    def test_listing_stops_after_delay_slot(self):
        code = []
        code += encode(lanes=((0, 1, LOAD0, ZERO, 0), NOP))
        code += encode(end=1)
        code += encode(lanes=((0, 2, 1, IMM, 1), NOP))  # delay slot
        code += encode(lanes=((0, 3, 1, IMM, 2), NOP))  # not listed
        lines = mme_dis.listing(code, 0, labels={0: "macro"})
        self.assertEqual(lines[0], "macro:")
        self.assertEqual(len(lines), 4)
        self.assertIn("END", lines[2])
        self.assertIn("R2 = ADD R1, #1", lines[3])
        self.assertEqual(len(mme_dis.listing(code, 1, count=3)), 3)

    def test_files(self):
        code = encode(lanes=((22, 1, ZERO, IMM, 0x35E), NOP), out=((5, 1), (0, 0))) + \
            encode(end=1) + encode()
        with tempfile.TemporaryDirectory() as directory:
            code_path = os.path.join(directory, "mme_code.bin")
            with open(code_path, "wb") as file:
                file.write(struct.pack("<%dI" % len(code), *code))
            positions = os.path.join(directory, "mme_positions.txt")
            with open(positions, "w") as file:
                file.write("# comment\n0 0x0\n1 0x40\n")
            self.assertEqual(mme_dis.read_words(code_path), code)
            self.assertEqual(mme_dis.read_positions(positions), {0: 0, 1: 0x40})
            self.assertEqual(mme_dis.main([code_path, "--positions", positions, "--start", "0"]), 0)


if __name__ == "__main__":
    unittest.main()
