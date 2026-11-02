#!/usr/bin/env python3
"""--ijmp-local: a computed jump to an entry of its own C function is a goto.

The SPU compiler ends a branch-free loop with `selb $rX, $head, $exit, $cond;
bi $rX`, the head's address loaded by an `ila`. Lifted, every pass went back
through spu_indirect_branch and the drain loop. With --ijmp-local the jump
compares its target with the entries of its function an `ila` loads and goes
there with a goto when SPU_IJMP_LOCAL allows (SPU_DIRECT's tests): the loop
stays in its function and only the exit reaches the dispatcher. The fixture
counts r3 to 100 that way, unmerged and merged (--merge-chunks), with the
gotos on, off, and checked against the dispatcher (shadow); and with `bie`,
whose pending interrupt must still reach the drain loop.

Usage:
    python3 tools/test_spu_lifter_ijmp_local.py
"""

import os
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


def ri18(op7, i18, rt):
    return (op7 << 25) | ((i18 & 0x3FFFF) << 7) | rt


def ri10(op8, i10, ra, rt):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt


def rr(op11, rb, ra, rt):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


def rrr(op4, rt, rb, ra, rc):
    return (op4 << 28) | (rt << 21) | (rb << 14) | (ra << 7) | rc


def program(bi_flags=0):
    return [
        ri16(0x081, 0, 3),                # 0x00 il   $3, 0
        ri18(0x21, 0x08, 4),              # 0x04 ila  $4, 0x08       the loop head
        ri10(0x1C, 1, 3, 3),              # 0x08 ai   $3, $3, 1      <- head
        ri10(0x7C, 100, 3, 6),            # 0x0C ceqi $6, $3, 100
        rrr(0x8, 7, 0, 4, 6),             # 0x10 selb $7, $4, $0, $6  head, or r0 when done
        rr(0x1A8, bi_flags, 7, 0),        # 0x14 bi   $7  (bie with 0x10: the E bit)
    ]


# The head is a chunk of its own, as find_spu_functions splits at an ila target.
BOUNDS = [(0x00, 0x08), (0x08, 0x18)]

DRIVER = r'''
#include "lifted.h"
#include <stdio.h>
#include <stdlib.h>
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
volatile int g_spu_direct = 0;          /* SPU_DIRECT: every direct transfer drains */
volatile int g_spu_ijmp_local = 1;
uint8_t g_spu_stack_reset_hint[1024];
static int dispatches, shadow_calls, shadow_ok, done;
int spu_ijmp_shadow(spu_context* ctx, uint32_t pc, void (*fn)(spu_context*)) {
    (void)ctx; shadow_calls++;
    return shadow_ok && pc == 0x8 && fn == spu_func_00000008;
}
/* The dispatcher: the head has its own function; anything else is the exit. */
void spu_indirect_branch(spu_context* ctx) {
    dispatches++;
    if ((ctx->pc & SPU_LS_MASK) == 0x8) spu_func_00000008(ctx);
    else done = 1;
}
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }
int main(int argc, char** argv) {
    static spu_context c;
    unsigned irq_at = 0;
    if (argc < 4) return 2;
    g_spu_ijmp_local = atoi(argv[1]);
    shadow_ok = atoi(argv[2]);
    c.steps = 1;                        /* not the run's first transfer */
    c.gpr[0]._u32[0] = 0x400;           /* the loop's exit */
    c.event_mask = 1; c.event_status = (unsigned)atoi(argv[3]);
    spu_func_00000000(&c);
    /* The drain: deliver a pending enabled interrupt before each transfer. */
    while (g_spu_trampoline_fn && !done) {
        void (*fn)(spu_context*) = g_spu_trampoline_fn;
        g_spu_trampoline_fn = 0;
        if (c.int_enable && (c.event_status & c.event_mask)) {
            c.event_status = 0; irq_at = c.gpr[3]._u32[0];
        }
        fn(&c);
    }
    printf("%u %d %d %d %u\n", c.gpr[3]._u32[0], done, dispatches, shadow_calls, irq_at);
    return 0;
}
'''


def lift(merge, local=True, bi_flags=0):
    insns = disassemble_spu(b''.join(struct.pack('>I', w) for w in program(bi_flags)), 0)
    lifter = SPULifter()
    lifter.ijmp_local = local
    lifter.func_starts = {s for s, _ in BOUNDS}
    groups = merge_chunk_groups(insns, BOUNDS, 4096, entry_points={0}) if merge else {}
    for s, e in BOUNDS:
        lifter.lift_function(insns, s, groups.get(s, e))
    return lifter


class IjmpLocal(unittest.TestCase):
    GOTO = ('if ((ctx->pc & SPU_LS_MASK) == 0x8u) if (SPU_IJMP_LOCAL(ctx, 0x8u, '
            'spu_func_00000008)) { ctx->pc = 0x8u; goto ijmp_00000008; } '
            'g_spu_trampoline_fn = spu_indirect_branch; return;')

    def body(self, lifter, addr):
        return '\n'.join(next(f for f in lifter.functions if f.start_addr == addr).body_lines)

    def test_head_function_jumps_to_its_own_start(self):
        lifter = lift(False)
        head = self.body(lifter, 8)
        self.assertTrue(head.startswith('ijmp_00000008: ;'), head)
        self.assertIn(self.GOTO, head)
        self.assertNotIn('ijmp_', self.body(lifter, 0))   # no computed jump there
        self.assertEqual(lifter.ijmp_sites, 1)

    def test_merged_run_jumps_to_its_merged_head(self):
        merged = self.body(lift(True), 0)
        # the label ahead of the merged entry's own label and interrupt check
        self.assertIn('ijmp_00000008: ;\nloc_00000008:\n    if (ctx->int_enable', merged)
        self.assertIn(self.GOTO, merged)

    def test_speculative_float_body_keeps_the_dispatcher(self):
        # The same loop with a float op at its head: a --float-spec body. Its jump
        # goes through the dispatcher in both bodies, as without --ijmp-local.
        words = program()
        words[2:2] = [rr(0x2C4, 5, 5, 5)]            # 0x08 fa $5, $5, $5  <- head
        insns = disassemble_spu(b''.join(struct.pack('>I', w) for w in words), 0)
        lifter = SPULifter()
        lifter.ijmp_local = True
        lifter.float_spec = 1
        lifter.func_starts = {0, 8}
        for s, e in [(0x00, 0x08), (0x08, 0x1C)]:
            lifter.lift_function(insns, s, e)
        head = next(f for f in lifter.functions if f.start_addr == 8)
        self.assertTrue(head.spec_lines)
        self.assertNotIn('ijmp_', '\n'.join(head.body_lines + head.spec_lines))
        self.assertEqual(lifter.ijmp_sites, 0)

    def test_off_without_the_option(self):
        for merge in (False, True):
            lifter = lift(merge, local=False)
            for f in lifter.functions:
                self.assertNotIn('ijmp_', '\n'.join(f.body_lines))
            self.assertEqual(lifter.ijmp_sites, 0)

    def run_program(self, merge, mode, shadow_ok=1, pending=0, bi_flags=0):
        cc = shutil.which('clang') or shutil.which('cc')
        if not cc:
            if os.environ.get('CI'):
                self.fail('C compiler required in CI')
            self.skipTest('C compiler unavailable')
        lifter = lift(merge, bi_flags=bi_flags)
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            context = (RUNTIME / 'spu_context.h').as_posix()
            helpers = (RUNTIME / 'spu_helpers.h').as_posix()
            (root / 'lifted.h').write_text(lifter.emit_header().replace(
                '../../runtime/spu/spu_context.h', context))
            (root / 'lifted.c').write_text(lifter.emit_source('lifted.h').replace(
                '../../runtime/spu/spu_helpers.h', helpers))
            (root / 'driver.c').write_text(DRIVER)
            exe = root / ('proof.exe' if sys.platform == 'win32' else 'proof')
            built = subprocess.run([cc, '-O1', '-I', str(RUNTIME), '-o', str(exe),
                                    str(root / 'lifted.c'), str(root / 'driver.c')],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe), str(mode), str(shadow_ok), str(pending)],
                                 capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            return [int(x) for x in ran.stdout.split()]

    def test_loop_stays_in_its_function(self):
        # r3, done, dispatches, shadow lookups, r3 when the interrupt was taken
        for merge in (False, True):
            self.assertEqual(self.run_program(merge, 1), [100, 1, 1, 0, 0])

    def test_off_at_run_time_dispatches_every_pass(self):
        # unmerged: 99 passes back to the head and the exit; merged: the first
        # pass runs in the merged run, so the same
        for merge in (False, True):
            self.assertEqual(self.run_program(merge, 0), [100, 1, 100, 0, 0])

    def test_shadow_takes_the_goto_only_when_the_lookup_agrees(self):
        for merge in (False, True):
            self.assertEqual(self.run_program(merge, 2), [100, 1, 1, 99, 0])
            self.assertEqual(self.run_program(merge, 2, shadow_ok=0), [100, 1, 100, 99, 0])

    def test_pending_interrupt_goes_through_the_drain(self):
        # `bie $7` enables interrupts with one pending: that jump goes to the drain,
        # which takes it (r3 = 1); the passes after it stay in the function.
        for merge in (False, True):
            self.assertEqual(self.run_program(merge, 1, pending=1, bi_flags=0x10),
                             [100, 1, 2, 0, 1])


if __name__ == '__main__':
    unittest.main()
