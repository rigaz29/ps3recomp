#!/usr/bin/env python3
"""--loop-locals: a single-block loop's SPU registers in C locals.

The fixture's loop reads the quadword it stored the pass before, adds two
registers to it and stores the sum, sixteen times: an LS store in every pass,
which is what made the lifted loop write its registers out and read them back.
With --loop-locals its registers are loaded before the loop's label, renamed
inside, and stored back after the branch back and in the pending-interrupt exit
of a merged chunk entry. A loop with another way out is left as it was. Built
and run against the lift without it, unmerged and merged, on random registers,
every register, the PC and the local store must come out the same, and an
interrupt pending at the loop head must leave the same state behind.

Usage:
    python3 tools/test_spu_lifter_loop_locals.py
"""

import os
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
from spu_lifter import SPULifter, merge_chunk_groups  # noqa: E402


def ri16(op9, i16, rt):
    return (op9 << 23) | ((i16 & 0xFFFF) << 7) | rt


def ri10(op8, i10, ra, rt):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt


def rr(op11, rb, ra, rt):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


PROGRAM = [
    ri16(0x081, 16, 3),                     # 0x00 il   $3, 16
    ri16(0x081, 0x100, 5),                  # 0x04 il   $5, 0x100
    ri16(0x081, 0x300, 10),                 # 0x08 il   $10, 0x300
    ri10(0x34, -1, 5, 8),                   # 0x0C lqd  $8, -16($5)     <- the loop
    rr(0x0C0, 8, 6, 6),                     # 0x10 a    $6, $6, $8
    rr(0x0C0, 7, 6, 6),                     # 0x14 a    $6, $6, $7
    ri10(0x24, 0, 5, 6),                    # 0x18 stqd $6, 0($5)
    ri10(0x34, 0, 10, 9),                   # 0x1C lqd  $9, 0($10)      a count kept in LS:
    ri10(0x1C, 1, 9, 9),                    # 0x20 ai   $9, $9, 1       the loop run again from
    ri10(0x24, 0, 10, 9),                   # 0x24 stqd $9, 0($10)      stale registers counts on
    ri10(0x1C, 16, 5, 5),                   # 0x28 ai   $5, $5, 16
    ri10(0x1C, -1, 3, 3),                   # 0x2C ai   $3, $3, -1
    ri16(0x042, (0x0C - 0x30) >> 2, 3),     # 0x30 brnz $3, 0x0C
    rr(0x1A8, 0, 0, 0),                     # 0x34 bi   $0
]
LOOP = 0x0C
BOUNDS = [(0x00, LOOP), (LOOP, 0x38)]

# The same loop with a branch out of its middle (brz $8 past the loop).
EXIT_PROGRAM = PROGRAM[:5] + [ri16(0x040, (0x34 - 0x14) >> 2, 8)] + PROGRAM[6:]

# The same loop with no LS store (both stqd made ai $11, $11, 1): nothing to gain.
NOSTORE_PROGRAM = PROGRAM[:6] + [ri10(0x1C, 1, 11, 11)] + PROGRAM[7:9] + [ri10(0x1C, 1, 11, 11)] + PROGRAM[10:]

DRIVER = r'''
#include "spu_context.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
volatile int g_spu_direct = 0;
volatile int g_spu_ijmp_local = 0;
int spu_ijmp_shadow(spu_context* c, uint32_t pc, void (*fn)(spu_context*)) { (void)c; (void)pc; (void)fn; return 0; }
uint8_t g_spu_stack_reset_hint[1024];
static int done;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; done = 1; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }
void ref_spu_func_00000000(spu_context* ctx);
void ll_spu_func_00000000(spu_context* ctx);

/* irq 2: the lifted code's interrupt check reads this instead of event_status
 * (the test rewrites it), pending at its fifth look: mid-loop. */
static int ev_calls;
uint32_t spu_test_event(spu_context* c) { return ++ev_calls == 5 ? c->event_mask : 0u; }

static uint8_t ls_a[0x40000], ls_b[0x40000];
static spu_context a, b;
static uint64_t s_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17; return (uint32_t)(s_rng >> 16); }

/* The drain: an enabled pending interrupt is taken (acknowledged) before a transfer. */
static void run(void (*f)(spu_context*), spu_context* c)
{
    done = 0;
    f(c);
    while (g_spu_trampoline_fn && !done) {
        void (*fn)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        if (c->int_enable && (c->event_status & c->event_mask)) c->event_status = 0;
        fn(c);
    }
}

int main(int argc, char** argv)
{
    const int irq = argc > 1 ? atoi(argv[1]) : 0;
    int bad = 0;
    for (int k = 0; k < 2000 && !bad; k++) {
        memset(&a, 0, sizeof a);
        memset(ls_a, 0, sizeof ls_a);
        a.ls = ls_a;
        for (int r = 1; r < 128; r++)
            for (int w = 0; w < 4; w++) a.gpr[r]._u32[w] = rnd();
        a.gpr[0]._u32[0] = 0x1000;
        for (int i = 0xF0; i < 0x300; i++) ls_a[i] = (uint8_t)rnd();
        a.steps = 1;
        if (irq) { a.int_enable = 1; a.event_mask = 1; a.event_status = irq == 1; }
        b = a;
        memcpy(ls_b, ls_a, sizeof ls_a);
        b.ls = ls_b;
        ev_calls = 0;
        run(ref_spu_func_00000000, &a);
        const int ev_ref = ev_calls;
        ev_calls = 0;
        run(ll_spu_func_00000000, &b);
        if (irq == 2 && (ev_ref < 5 || ev_calls != ev_ref)) {
            printf("case %d: the interrupt check ran %d and %d times\n", k, ev_ref, ev_calls);
            bad = 1;
        }
        for (int r = 0; r < 128 && !bad; r++)
            if (memcmp(&a.gpr[r], &b.gpr[r], sizeof a.gpr[r])) {
                printf("case %d: $%d differs\n", k, r);
                bad = 1;
            }
        if (!bad && a.pc != b.pc) { printf("case %d: pc %X, with locals %X\n", k, a.pc, b.pc); bad = 1; }
        if (!bad && memcmp(ls_a, ls_b, sizeof ls_a)) { printf("case %d: the local store differs\n", k); bad = 1; }
        if (!bad && a.event_status != b.event_status) { printf("case %d: interrupt state differs\n", k); bad = 1; }
    }
    if (!bad) puts("ok");
    return bad;
}
'''


def lift(program=PROGRAM, merge=False, locals_=True, prefix="", skip=(), max_regs=0):
    insns = disassemble_spu(b''.join(struct.pack('>I', w) for w in program), 0)
    lifter = SPULifter(prefix=prefix)
    lifter.func_starts = {s for s, _ in BOUNDS}
    lifter.loop_locals = locals_
    lifter.loop_locals_skip = set(skip)
    lifter.loop_locals_max = max_regs
    groups = merge_chunk_groups(insns, BOUNDS, 4096, entry_points={0}) if merge else {}
    for s, e in BOUNDS:
        lifter.lift_function(insns, s, groups.get(s, e))
    return lifter


LOCAL = re.compile(r'\bspu_l\d+\b')   # a loop local, not spu_ls_*


def body(lifter, addr):
    return next(f for f in lifter.functions if f.start_addr == addr).body_lines


class LoopLocalsShape(unittest.TestCase):
    def test_the_loop_runs_on_locals(self):
        lifter = lift()
        lines = body(lifter, LOOP)
        text = '\n'.join(lines)
        lab = lines.index('loc_0000000C:')
        back = next(i for i, l in enumerate(lines) if 'goto loc_0000000C;' in l)
        self.assertTrue(lines[lab - 1].lstrip().startswith('spu_l3 = ctx->gpr[3];'), lines[lab - 1])
        self.assertIn('spu_l7 = ctx->gpr[7];', lines[lab - 1])
        self.assertNotIn('ctx->gpr', '\n'.join(lines[lab + 1:back + 1]))
        self.assertIn('ctx->gpr[6] = spu_l6;', lines[back + 1])
        self.assertNotIn('ctx->gpr[7] =', lines[back + 1])   # read only
        self.assertIn('u128 spu_l8;', text)
        self.assertEqual(lifter.loop_locals_n, 1)

    def test_the_merged_entry_check_stores_them_back(self):
        lines = body(lift(merge=True), 0)
        lab = lines.index('loc_0000000C:')
        self.assertIn('ctx->int_enable', lines[lab + 1])
        for r in (3, 5, 6, 8, 9):          # what the loop writes
            self.assertIn(f'ctx->gpr[{r}] = spu_l{r};', lines[lab + 1])
        for r in (7, 10):                  # only read: never stored
            self.assertNotIn(f'ctx->gpr[{r}] = spu_l{r};', lines[lab + 1])

    def test_a_loop_with_another_way_out_is_left_alone(self):
        lifter = lift(EXIT_PROGRAM)
        self.assertEqual(lifter.loop_locals_n, 0)
        self.assertIsNone(LOCAL.search('\n'.join(body(lifter, LOOP))))

    def test_off_without_the_option(self):
        lifter = lift(locals_=False)
        self.assertIsNone(LOCAL.search('\n'.join(body(lifter, LOOP))))

    def test_a_loop_without_a_store_runs_on_locals_too(self):
        # Float-heavy loops without one gained as well: the float forms' out-of-line
        # paths made them reload too.
        self.assertEqual(lift(NOSTORE_PROGRAM).loop_locals_n, 1)

    def test_a_skipped_head_is_left_alone(self):
        lifter = lift(skip={LOOP})
        self.assertEqual(lifter.loop_locals_n, 0)
        self.assertIsNone(LOCAL.search('\n'.join(body(lifter, LOOP))))

    def test_the_register_cap(self):
        # The loop names $3, $5-$10: seven registers.
        self.assertEqual(lift(max_regs=6).loop_locals_n, 0)
        self.assertEqual(lift(max_regs=7).loop_locals_n, 1)


class LoopLocalsRun(unittest.TestCase):
    def run_pair(self, merge, irq=0, program=PROGRAM):
        cc = shutil.which('clang') or shutil.which('cc')
        if not cc:
            if os.environ.get('CI'):
                self.fail('C compiler required in CI')
            self.skipTest('C compiler unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            context = (RUNTIME / 'spu_context.h').as_posix()
            helpers = (RUNTIME / 'spu_helpers.h').as_posix()
            srcs = []
            for prefix, on in (('ref_', False), ('ll_', True)):
                lifter = lift(program, merge=merge, locals_=on, prefix=prefix)
                (root / f'{prefix}lifted.h').write_text(lifter.emit_header().replace(
                    '../../runtime/spu/spu_context.h', context))
                src = lifter.emit_source(f'{prefix}lifted.h').replace(
                    '../../runtime/spu/spu_helpers.h', helpers)
                if irq == 2:
                    inc = f'#include "{prefix}lifted.h"\n'
                    self.assertIn(inc, src)
                    src = src.replace(inc, inc + 'uint32_t spu_test_event(spu_context* c);\n', 1).replace(
                        '(ctx->event_status & ctx->event_mask)', '(spu_test_event(ctx) & ctx->event_mask)')
                (root / f'{prefix}lifted.c').write_text(src)
                srcs.append(str(root / f'{prefix}lifted.c'))
            (root / 'driver.c').write_text(DRIVER)
            exe = root / ('proof.exe' if sys.platform == 'win32' else 'proof')
            built = subprocess.run([cc, '-O2', '-fno-strict-aliasing', '-DSPU_LS_NO_DIAG', '-I', str(RUNTIME),
                                    '-o', str(exe),
                                    *srcs, str(root / 'driver.c')], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe), str(irq)], capture_output=True, text=True, timeout=60)
            self.assertEqual(ran.returncode, 0, ran.stdout + ran.stderr)
            self.assertEqual(ran.stdout.strip(), 'ok')

    def test_unmerged_matches(self):
        self.run_pair(False)

    def test_merged_matches(self):
        self.run_pair(True)

    def test_merged_with_an_interrupt_pending_matches(self):
        self.run_pair(True, irq=1)

    def test_merged_with_an_interrupt_mid_loop_matches(self):
        self.run_pair(True, irq=2)

    def test_without_a_store_matches(self):
        self.run_pair(False, program=NOSTORE_PROGRAM)
        self.run_pair(True, irq=2, program=NOSTORE_PROGRAM)


if __name__ == '__main__':
    unittest.main()
