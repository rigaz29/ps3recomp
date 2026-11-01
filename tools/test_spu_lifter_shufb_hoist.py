#!/usr/bin/env python3
"""shufb_hoist: a shufb whose control register a loop never writes gets the
control prepared once, before the loop.

The fixture's loop has four shufb: two on a control nothing in the loop writes
($6, twice, and $8), one on a control the loop counts up ($11). Lifted, the
loop head gets spu_shufc_prep for $6 and $8 and those three shufb become
spu_shufb_pre; the one on $11 stays spu_shufb. A call in the loop, or a branch
into its head from before it, leaves every shufb as it is. Built and run against
the lift without the hoist, on random registers whose controls cover every
selector class, every register and the PC must come out the same.

Usage:
    python3 tools/test_spu_lifter_shufb_hoist.py
"""

import os
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
RUNTIME = TOOLS.parent / 'runtime' / 'spu'
sys.path.insert(0, str(TOOLS))

from spu_disasm import disassemble_spu  # noqa: E402
from spu_lifter import SPULifter  # noqa: E402


def ri16(op9, i16, rt):
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt


def ri10(op8, i10, ra, rt):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt


def rr(op11, rb, ra, rt):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


def rrr(op4, rt, rb, ra, rc):
    return (op4 << 28) | (rt << 21) | (rb << 14) | (ra << 7) | rc


def shufb(rt, ra, rb, rc):
    return rrr(0xB, rt, rb, ra, rc)


BI_R0 = rr(0x1A8, 0, 0, 0)
PROGRAM = [
    ri16(0x081, 3, 10),                     # 0x00 il    $10, 3
    shufb(5, 3, 4, 6),                      # 0x04 shufb $5, $3, $4, $6     (the loop)
    shufb(7, 5, 3, 8),                      # 0x08 shufb $7, $5, $3, $8
    ri10(0x1C, 1, 11, 11),                  # 0x0C ai    $11, $11, 1
    shufb(12, 7, 5, 11),                    # 0x10 shufb $12, $7, $5, $11   ($11 changes)
    shufb(3, 12, 4, 6),                     # 0x14 shufb $3, $12, $4, $6
    ri10(0x1C, -1, 10, 10),                 # 0x18 ai    $10, $10, -1
    ri16(0x042, (0x04 - 0x1C) >> 2, 10),    # 0x1C brnz  $10, 0x04
    BI_R0,                                  # 0x20 bi    $0
]
BOUNDS = [(0x00, 0x24)]

DRIVER = r'''
#include "spu_context.h"
#include <stdio.h>
#include <string.h>
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
volatile int g_spu_direct = 0;
uint8_t g_spu_stack_reset_hint[1024];
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }
void ref_spu_func_00000000(spu_context* ctx);
void hoist_spu_func_00000000(spu_context* ctx);

static uint8_t ls_a[0x40000], ls_b[0x40000];
static spu_context a, b;
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)(s_rng >> 16); }

static void run(void (*f)(spu_context*), spu_context* c)
{
    f(c);
    while (g_spu_trampoline_fn) {
        void (*fn)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        fn(c);
    }
}

int main(void)
{
    int bad = 0;
    for (int k = 0; k < 2000 && !bad; k++) {
        memset(&a, 0, sizeof a);
        a.ls = ls_a;
        for (int r = 1; r < 128; r++)
            for (int w = 0; w < 4; w++) a.gpr[r]._u32[w] = rnd();
        a.gpr[0]._u32[0] = 0x1000;
        b = a;
        b.ls = ls_b;
        run(ref_spu_func_00000000, &a);
        run(hoist_spu_func_00000000, &b);
        for (int r = 0; r < 128 && !bad; r++)
            if (memcmp(&a.gpr[r], &b.gpr[r], sizeof a.gpr[r])) {
                printf("case %d: $%d differs\n", k, r);
                bad = 1;
            }
        if (!bad && a.pc != b.pc) { printf("case %d: pc %X, hoisted %X\n", k, a.pc, b.pc); bad = 1; }
    }
    if (!bad) puts("ok");
    return bad;
}
'''


def lift(program=PROGRAM, hoist=True, prefix=""):
    insns = disassemble_spu(b''.join(struct.pack('>I', w) for w in program), 0)
    lifter = SPULifter(prefix=prefix)
    lifter.func_starts = {s for s, _ in BOUNDS}
    lifter.shufb_hoist = hoist
    for s, e in BOUNDS:
        lifter.lift_function(insns, s, e)
    return lifter


def body(lifter):
    return '\n'.join(lifter.functions[0].body_lines)


class ShufbHoistShape(unittest.TestCase):
    def test_invariant_controls_are_prepared_before_the_loop(self):
        lines = lift().functions[0].body_lines
        text = '\n'.join(lines)
        self.assertEqual(lines[:2], ['    spu_shufc spu_sc_4_6;', '    spu_shufc spu_sc_4_8;'])
        prep6 = 'spu_sc_4_6 = spu_shufc_prep(ctx->gpr[6]);'
        prep8 = 'spu_sc_4_8 = spu_shufc_prep(ctx->gpr[8]);'
        self.assertIn(prep6, text)
        self.assertIn(prep8, text)
        self.assertLess(text.index(prep8), text.index('loc_00000004:'))
        self.assertGreater(text.index(prep6), text.index('ctx->gpr[10] = '))   # after the il
        self.assertEqual(text.count('spu_shufb_pre('), 3)
        self.assertEqual(text.count('&spu_sc_4_6)'), 2)
        self.assertIn('spu_shufb(ctx->gpr[7], ctx->gpr[5], ctx->gpr[11])', text)

    def test_left_alone(self):
        self.assertNotIn('spu_shufb_pre', body(lift(hoist=False)))
        call = list(PROGRAM)
        call[3] = ri16(0x066, (0x40 - 0x0C) >> 2, 0)            # brsl $0, 0x40: a call in the loop
        self.assertNotIn('spu_shufb_pre', body(lift(call)))
        entry = list(PROGRAM)
        entry[0] = ri16(0x040, (0x04 - 0x00) >> 2, 10)          # brz $10, 0x04: into the head from before
        self.assertNotIn('spu_shufb_pre', body(lift(entry)))


class ShufbHoistRun(unittest.TestCase):
    def test_same_registers_and_pc_as_the_lift_without_it(self):
        cc = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
        if not cc:
            if os.environ.get('CI'):
                self.fail('C compiler required in CI')
            self.skipTest('C compiler unavailable')
        x86 = platform.machine().lower() in ('x86_64', 'amd64')
        flags = ['-msse4.1'] if x86 else []          # the prepared form's SIMD path
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            context = (RUNTIME / 'spu_context.h').as_posix()
            helpers = (RUNTIME / 'spu_helpers.h').as_posix()
            srcs = []
            for name, hoist in (('ref', False), ('hoist', True)):
                lifter = lift(hoist=hoist, prefix=name + '_')
                (root / f'{name}.h').write_text(lifter.emit_header().replace(
                    '../../runtime/spu/spu_context.h', context))
                (root / f'{name}.c').write_text(lifter.emit_source(f'{name}.h').replace(
                    '../../runtime/spu/spu_helpers.h', helpers))
                srcs.append(str(root / f'{name}.c'))
            self.assertIn('spu_shufb_pre', (root / 'hoist.c').read_text())
            (root / 'driver.c').write_text(DRIVER)
            exe = root / ('proof.exe' if sys.platform == 'win32' else 'proof')
            built = subprocess.run([cc, '-O1', '-DSPU_LS_NO_DIAG', *flags, '-I', str(RUNTIME),
                                    '-o', str(exe), *srcs, str(root / 'driver.c'), '-lm'],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), 'ok')


if __name__ == '__main__':
    unittest.main()
