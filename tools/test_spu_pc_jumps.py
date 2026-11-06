#!/usr/bin/env python3
"""`brsl $2, .+4; ai $2, $2, 8; bid $2` is a branch to a constant, not a jump into a ladder.

The SDK turns interrupts off and on with it around every DMA of a job. The function
finder took it for a Duff's device and made 17 functions of the code after it, and
each one went through the dispatcher.

Usage:
    python3 tools/test_spu_pc_jumps.py
"""

import struct
import sys
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))

from find_spu_functions import collect_duff_targets  # noqa: E402
from spu_disasm import disassemble_spu  # noqa: E402
from spu_lifter import SPULifter, compute_pc_jumps  # noqa: E402


def ri16(op9, i16, rt):
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt


def ri10(op8, i10, ra, rt):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt


def rr(op11, rb, ra, rt):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


BRSL_NEXT = ri16(0x066, 1, 2)            # brsl $2, .+4
AI_8 = ri10(0x1C, 8, 2, 2)               # ai   $2, $2, 8
BID = rr(0x1A8, 0, 2, 0) | 0x80000       # bid  $2
BIE = rr(0x1A8, 0, 2, 0) | 0x40000       # bie  $2
IL = ri16(0x081, 1, 3)                   # il   $3, 1
RET = rr(0x1A8, 0, 0, 0)                 # bi   $0


def insns(words):
    return disassemble_spu(b"".join(struct.pack(">I", w) for w in words), 0)


class PcJumps(unittest.TestCase):
    def test_the_idiom_is_a_branch_to_the_next_instruction(self):
        self.assertEqual(compute_pc_jumps(insns([BRSL_NEXT, AI_8, BID, IL, RET])), {0x08: 0x0C})

    def test_a_branch_into_the_idiom_leaves_it_alone(self):
        brz = ri16(0x040, 2, 7)                       # 0x00 brz $7, 0x08: the bid, with any r2
        self.assertEqual(compute_pc_jumps(insns([brz, BRSL_NEXT, AI_8, BID, IL, RET])), {})

    def test_no_ladder_with_nothing_but_the_ai(self):
        got = collect_duff_targets(insns([BRSL_NEXT, AI_8, BID, IL, IL, IL, RET]), 0, 0x40)
        self.assertEqual(got, {0x0C})

    def test_a_real_ladder_keeps_its_entries(self):
        add = rr(0x0C0, 4, 2, 2)                      # a $2, $2, $4: the jump into it
        got = collect_duff_targets(insns([BRSL_NEXT, AI_8, add, BID] + [IL] * 12 + [RET]), 0, 0x80)
        self.assertIn(0x0C, got)
        self.assertIn(0x14, got)
        self.assertIn(0x20, got)

    def test_the_lift_has_no_dispatcher_and_keeps_the_bit(self):
        for word, bit in ((BID, "ctx->int_enable = 0;"), (BIE, "ctx->int_enable = 1;")):
            code = insns([BRSL_NEXT, AI_8, word, IL, RET])
            lifter = SPULifter()
            lifter.pc_jump = compute_pc_jumps(code)
            lifter.func_starts = {0x00, 0x04, 0x0C}
            text = "\n".join(lifter.lift_function(code, 0x04, 0x0C).body_lines
                             if lifter.lift_function(code, 0x04, 0x0C) else
                             lifter.functions[-1].body_lines)
            self.assertIn(bit, text)
            self.assertNotIn("spu_indirect_branch", text)
            self.assertIn("spu_func_0000000C", text)


if __name__ == "__main__":
    unittest.main()
