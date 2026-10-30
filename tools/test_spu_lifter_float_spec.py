#!/usr/bin/env python3
"""--float-spec: the speculative body of a pure straight-line function.

The fixture's first function has five float ops, an LS read, a conditional exit
to the second function and a return. Lifted with float_spec=4 it gets a
speculative body: registers in locals, the speculative float forms, and at each
exit a range check that falls back to the exact copy, then a commit of the
registers written so far. A store, an internal branch or too few float ops keep
the ordinary body. Built and run against the ordinary lift, every register and
the PC must come out the same, in range (no fallback) and out of range
(FLT_MAX + FLT_MAX, extended-range operands: the exact copy runs).

Usage:
    python3 tools/test_spu_lifter_float_spec.py
"""

import os
import platform
import re
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


BI_R0 = rr(0x1A8, 0, 0, 0)
PROGRAM = [
    rr(0x2C4, 4, 3, 5),                   # 0x00 fa   $5, $3, $4
    rr(0x2C6, 3, 5, 6),                   # 0x04 fm   $6, $5, $3
    rrr(0xE, 7, 4, 6, 5),                 # 0x08 fma  $7, $6, $4, $5
    rr(0x2C5, 3, 7, 8),                   # 0x0C fs   $8, $7, $3
    ri10(0x34, 0, 12, 11),                # 0x10 lqd  $11, 0($12)
    ri16(0x040, (0x24 - 0x14) >> 2, 9),   # 0x14 brz  $9, 0x24       (the second function)
    ri10(0x1C, 1, 8, 10),                 # 0x18 ai   $10, $8, 1
    rr(0x2C4, 11, 8, 3),                  # 0x1C fa   $3, $8, $11    ($3 read at entry, written here)
    BI_R0,                                # 0x20 bi   $0
    ri16(0x081, 7, 3),                    # 0x24 il   $3, 7
    BI_R0,                                # 0x28 bi   $0
]
BOUNDS = [(0x00, 0x24), (0x24, 0x2C)]

DRIVER = r'''
#include "spu_context.h"
#include <stdio.h>
#include <string.h>
#if defined(SPU_FLOAT_RZ_ONLY)
#include <immintrin.h>
#endif
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
volatile int g_spu_direct = 0;              /* SPU_DIRECT off: every hop drains */
uint8_t g_spu_stack_reset_hint[1024];
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }
void ref_spu_func_00000000(spu_context* ctx);
void spec_spu_func_00000000(spu_context* ctx);

static uint8_t ls_a[0x40000], ls_b[0x40000];
static spu_context a, b;

static void run(void (*f)(spu_context*), spu_context* c)
{
    f(c);
    while (g_spu_trampoline_fn) {
        void (*fn)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        fn(c);
    }
}

static void init(spu_context* c, uint8_t* ls, uint32_t x, uint32_t y, uint32_t z)
{
    memset(c->gpr, 0, sizeof c->gpr);
    c->ls = ls;
    c->pc = 0;
    for (int i = 0; i < 4; i++) {
        c->gpr[3]._u32[i] = x + (uint32_t)i;
        c->gpr[4]._u32[i] = y;
    }
    c->gpr[9]._u32[0] = z;
    c->gpr[12]._u32[0] = 0x100;
    c->gpr[0]._u32[0] = 0x1000;
    static const uint8_t word[16] = { 0x3F, 0xC0, 0, 0,  0x7F, 0x7F, 0xFF, 0xFF,  0, 0, 0, 1,  0xBF, 0x80, 0, 0 };
    memcpy(ls + 0x100, word, sizeof word);
}

int main(void)
{
    static const uint32_t cases[][3] = {     /* $3, $4, $9 */
        { 0x3F800000u, 0x40000000u, 1 },     /* 1 + 2: in range, returns */
        { 0x3F800000u, 0x40000000u, 0 },     /* in range, the exit to 0x24 */
        { 0x7F7FFFFFu, 0x7F7FFFFFu, 1 },     /* FLT_MAX + FLT_MAX: extended range */
        { 0x7F7FFFFFu, 0x7F7FFFFFu, 0 },
        { 0x00000001u, 0x80000000u, 1 },     /* a denormal and -0 */
        { 0x7FFFFFFFu, 0x3F800000u, 1 },     /* an extended-range operand */
    };
#if defined(SPU_FLOAT_RZ_ONLY)
    _mm_setcsr(_mm_getcsr() | 0xE040u);     /* SPU_MXCSR_RZFZ: round toward zero, FTZ, DAZ */
#endif
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        init(&a, ls_a, cases[k][0], cases[k][1], cases[k][2]);
        init(&b, ls_b, cases[k][0], cases[k][1], cases[k][2]);
        run(ref_spu_func_00000000, &a);
        run(spec_spu_func_00000000, &b);
        int bad = -1;
        for (int r = 0; r < 128 && bad < 0; r++)
            if (memcmp(&a.gpr[r], &b.gpr[r], sizeof a.gpr[r])) bad = r;
        if (bad >= 0)
            printf("case %u: $%d %08X %08X %08X %08X, speculative %08X %08X %08X %08X\n", k, bad,
                   a.gpr[bad]._u32[0], a.gpr[bad]._u32[1], a.gpr[bad]._u32[2], a.gpr[bad]._u32[3],
                   b.gpr[bad]._u32[0], b.gpr[bad]._u32[1], b.gpr[bad]._u32[2], b.gpr[bad]._u32[3]);
        else if (a.pc != b.pc)
            printf("case %u: pc %X, speculative %X\n", k, a.pc, b.pc);
        else
            printf("ok %X %08X\n", a.pc, a.gpr[8]._u32[0]);
    }
    return 0;
}
'''


def lift(program=PROGRAM, float_spec=4, prefix=""):
    insns = disassemble_spu(b''.join(struct.pack('>I', w) for w in program), 0)
    lifter = SPULifter(prefix=prefix)
    lifter.func_starts = {s for s, _ in BOUNDS}
    lifter.float_spec = float_spec
    for s, e in BOUNDS:
        lifter.lift_function(insns, s, e)
    return lifter


def spec_body(lifter, addr=0):
    return '\n'.join(next(f for f in lifter.functions if f.start_addr == addr).spec_lines)


class FloatSpecShape(unittest.TestCase):
    def test_pure_function_gets_speculative_body(self):
        body = spec_body(lift())
        self.assertIn('spu_spec_acc spu_acc = spu_spec_init();', body)
        for r in (3, 4, 9, 12):              # read before written: loaded at entry
            self.assertIn(f'u128 spu_r{r} = ctx->gpr[{r}];', body)
        for r in (5, 6, 7, 8, 10, 11):       # written first: no load
            self.assertIn(f'u128 spu_r{r};', body)
        self.assertEqual(len(re.findall(r'spu_f[a-z]+_sp\([^()]*, &spu_acc\)', body)), 5)
        self.assertNotRegex(body, r'spu_f(a|s|m|ma)\(')

    def test_exits_check_then_commit_what_was_written_before_them(self):
        body = spec_body(lift())
        check = 'if (SPU_SPEC_OUT(spu_acc)) SPU_TAILCALL(spu_func_00000000_exact(ctx));'
        side = next(line for line in body.split('\n') if 'SPU_DIRECT' in line)
        self.assertTrue(side.strip().startswith('if (spu_r9._u32[0] == 0) { ' + check), side)
        self.assertEqual(re.findall(r'ctx->gpr\[(\d+)\] = ', side), ['5', '6', '7', '8', '11'])
        ret = next(line for line in body.split('\n') if 'SPU_RET' in line)
        self.assertIn(check, ret)
        self.assertEqual(re.findall(r'ctx->gpr\[(\d+)\] = ', ret), ['3', '5', '6', '7', '8', '10', '11'])
        self.assertTrue(ret.rstrip().endswith('SPU_RET(ctx); }'), ret)

    def test_ordinary_body_is_the_exact_copy(self):
        src = lift().emit_source('lifted.h')
        exact = src.index('static void spu_func_00000000_exact(spu_context* ctx) {')
        spec = src.index('void spu_func_00000000(spu_context* ctx) {')
        self.assertLess(exact, spec)
        self.assertIn('spu_fa(ctx->gpr[3], ctx->gpr[4])', src[exact:spec])
        self.assertNotIn('_exact', src[src.index('void spu_func_00000024('):])

    def test_left_alone(self):
        self.assertEqual(spec_body(lift(float_spec=6)), '')          # five float ops
        self.assertEqual(spec_body(lift(), 0x24), '')                 # none
        store = list(PROGRAM)
        store[6] = ri10(0x24, 0, 12, 10)                              # stqd $10, 0($12)
        self.assertEqual(spec_body(lift(store)), '')
        loop = list(PROGRAM)
        loop[5] = ri16(0x040, (0x04 - 0x14) >> 2, 9)                  # brz $9, 0x04: a loop
        self.assertEqual(spec_body(lift(loop)), '')
        self.assertEqual(spec_body(lift(float_spec=0)), '')


class FloatSpecRun(unittest.TestCase):
    def test_same_registers_and_pc_as_the_ordinary_lift(self):
        cc = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
        if not cc:
            if os.environ.get('CI'):
                self.fail('C compiler required in CI')
            self.skipTest('C compiler unavailable')
        # The native speculative forms: SPU_FLOAT_RZ_ONLY on x86 with AVX2 (run in the RZ
        # mode); elsewhere the forms are the exact ones and only the plumbing is checked.
        x86 = platform.machine().lower() in ('x86_64', 'amd64')
        flags = ['-mavx2', '-mfma', '-DSPU_FLOAT_RZ_ONLY'] if x86 else []
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            context = (RUNTIME / 'spu_context.h').as_posix()
            helpers = (RUNTIME / 'spu_helpers.h').as_posix()
            srcs = []
            for name, spec in (('ref', 0), ('spec', 4)):
                lifter = lift(float_spec=spec, prefix=name + '_')
                (root / f'{name}.h').write_text(lifter.emit_header().replace(
                    '../../runtime/spu/spu_context.h', context))
                (root / f'{name}.c').write_text(lifter.emit_source(f'{name}.h').replace(
                    '../../runtime/spu/spu_helpers.h', helpers))
                srcs.append(str(root / f'{name}.c'))
            self.assertIn('spec_spu_func_00000000_exact', (root / 'spec.c').read_text())
            (root / 'driver.c').write_text(DRIVER)
            exe = root / ('proof.exe' if sys.platform == 'win32' else 'proof')
            built = subprocess.run([cc, '-O1', '-DSPU_LS_NO_DIAG', *flags, '-I', str(RUNTIME),
                                    '-o', str(exe), *srcs, str(root / 'driver.c'), '-lm'],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            lines = ran.stdout.splitlines()
            self.assertEqual(len(lines), 6, ran.stdout)
            for line in lines:
                self.assertTrue(line.startswith('ok '), ran.stdout)
            # the two exits: back to the link, and the branch to the second function
            self.assertEqual(lines[0].split()[1], '1000')
            self.assertEqual(lines[1].split()[1], '1000')    # 0x24 returns through $0 too


if __name__ == '__main__':
    unittest.main()
