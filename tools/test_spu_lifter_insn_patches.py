#!/usr/bin/env python3
"""spu_lifter.py --insn-patches: a patched SPU instruction is lifted in both forms
behind its guard, `if (guard) { new } else { original }`.

1. The shape: both forms on the patched line, the guard declared in the source.
2. Refused: an original that is not the word in the image, a replacement that
   branches, and a patch no lifted function covers.
3. A run: built with a driver, the guard at 0 gives the original's registers,
   at 1 the replacement's (`and $4,$5,$25` -> `il $4,0`, the shape of God of
   War: Ascension's Disable Mesh Trimming).

Usage:
    python3 tools/test_spu_lifter_insn_patches.py
"""

import json
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
from spu_lifter import SPULifter  # noqa: E402


def ri10(op8, i10, ra, rt):
    return (op8 << 24) | ((i10 & 0x3FF) << 14) | (ra << 7) | rt


def rr(op11, rb, ra, rt):
    return (op11 << 21) | (rb << 14) | (ra << 7) | rt


AND_4_5_25 = 0x18264284                     # and $4, $5, $25
IL_4_0 = 0x40800004                         # il  $4, 0
PROGRAM = [
    AND_4_5_25,                             # 0x00
    ri10(0x1C, 1, 4, 6),                    # 0x04 ai $6, $4, 1
    rr(0x1A8, 0, 0, 0),                     # 0x08 bi $0
]
GOOD = {0x00: (AND_4_5_25, IL_4_0, "g_trim")}

DRIVER = r'''
#include "spu_context.h"
#include <stdio.h>
#include <string.h>
SPU_THREAD_LOCAL void (*g_spu_trampoline_fn)(spu_context*);
volatile int g_spu_direct = 0;
uint8_t g_spu_stack_reset_hint[1024];
int g_trim;
void spu_indirect_branch(spu_context* ctx) { (void)ctx; }
void spu_register_function(uint32_t addr, void (*fn)(spu_context*)) { (void)addr; (void)fn; }
void spu_func_00000000(spu_context* ctx);

static uint8_t ls[0x40000];
static spu_context c;

int main(void)
{
    for (int on = 0; on < 2; on++) {
        memset(&c, 0, sizeof c);
        c.ls = ls;
        c.gpr[0]._u32[0] = 0x1000;
        for (int w = 0; w < 4; w++) { c.gpr[5]._u32[w] = 0xF0F0F0F0u + w; c.gpr[25]._u32[w] = 0x0FF00FF0u; }
        g_trim = on;
        spu_func_00000000(&c);
        while (g_spu_trampoline_fn) { void (*f)(spu_context*) = g_spu_trampoline_fn; g_spu_trampoline_fn = 0; f(&c); }
        printf("%d %08X %08X\n", on, c.gpr[4]._u32[1], c.gpr[6]._u32[1]);
    }
    return 0;
}
'''


def lift(program=PROGRAM, patches=GOOD):
    insns = disassemble_spu(b''.join(struct.pack('>I', w) for w in program), 0)
    lifter = SPULifter()
    lifter.func_starts = {0}
    lifter.insn_patches = dict(patches)
    lifter.lift_function(insns, 0, 4 * len(program))
    return lifter


class Shape(unittest.TestCase):
    def test_both_forms_behind_the_guard(self):
        lifter = lift()
        body = '\n'.join(lifter.functions[0].body_lines)
        self.assertIn('if (g_trim) { ctx->gpr[4] = spu_il(0); } else '
                      '{ ctx->gpr[4] = spu_and(ctx->gpr[5], ctx->gpr[25]); }'
                      '  /* insn patch 0x0: 18264284 -> 40800004 */', body)
        self.assertIn('extern int g_trim;', lifter.emit_source('lifted.h'))
        self.assertEqual(lifter.insn_patches_landed, {0})

    def test_no_patches_no_guard(self):
        src = lift(patches={}).emit_source('lifted.h')
        self.assertNotIn('extern int g_', src)
        self.assertNotIn('insn patch', src)


class Refused(unittest.TestCase):
    def test_wrong_original(self):
        with self.assertRaises(SystemExit) as e:
            lift(patches={0: (AND_4_5_25 ^ 1, IL_4_0, 'g_trim')})
        self.assertIn('the patch expects 0x18264285', str(e.exception))

    def test_branch_replacement(self):
        with self.assertRaises(SystemExit) as e:
            lift(patches={0: (AND_4_5_25, rr(0x1A8, 0, 0, 0), 'g_trim')})   # bi $0
        self.assertIn('cannot be lifted as one', str(e.exception))

    def test_patch_outside_every_function(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            elf = root / 'img.bin'
            elf.write_bytes(b''.join(struct.pack('>I', w) for w in PROGRAM))
            (root / 'p.json').write_text(json.dumps({'patches': [
                {'addr': '0x40', 'orig': '0x18264284', 'new': '0x40800004', 'guard': 'g_trim'}]}))
            r = subprocess.run([sys.executable, str(TOOLS / 'spu_lifter.py'), str(elf),
                                '--base', '0', '--insn-patches', str(root / 'p.json'),
                                '--output', str(root / 'out')], capture_output=True, text=True)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn('insn patch(es) in no lifted function: 0x40', r.stderr)
            # optional: reported, the lift goes on
            (root / 'p.json').write_text(json.dumps({'patches': [
                {'addr': '0x40', 'orig': '0x18264284', 'new': '0x40800004', 'guard': 'g_trim',
                 'optional': True}]}))
            r = subprocess.run([sys.executable, str(TOOLS / 'spu_lifter.py'), str(elf),
                                '--base', '0', '--insn-patches', str(root / 'p.json'),
                                '--output', str(root / 'out')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertIn('optional insn patch at 0x40: in no lifted function, not applied', r.stdout)


class Run(unittest.TestCase):
    def test_guard_selects_the_form(self):
        cc = shutil.which('clang') or shutil.which('cc') or shutil.which('gcc')
        if not cc:
            if os.environ.get('CI'):
                self.fail('C compiler required in CI')
            self.skipTest('C compiler unavailable')
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            lifter = lift()
            (root / 'lifted.h').write_text(lifter.emit_header().replace(
                '../../runtime/spu/spu_context.h', (RUNTIME / 'spu_context.h').as_posix()))
            (root / 'lifted.c').write_text(lifter.emit_source('lifted.h').replace(
                '../../runtime/spu/spu_helpers.h', (RUNTIME / 'spu_helpers.h').as_posix()))
            (root / 'driver.c').write_text(DRIVER)
            exe = root / ('run.exe' if sys.platform == 'win32' else 'run')
            built = subprocess.run([cc, '-O1', '-DSPU_LS_NO_DIAG', '-I', str(RUNTIME), '-o', str(exe),
                                    str(root / 'lifted.c'), str(root / 'driver.c'), '-lm'],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            ran = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
            self.assertEqual(ran.returncode, 0, ran.stderr)
            # $5 lane 1 = F0F0F0F1, $25 = 0FF00FF0: and = 00F000F0, +1 = 00F000F1
            self.assertEqual(ran.stdout.split('\n')[:2], ['0 00F000F0 00F000F1', '1 00000000 00000001'])


if __name__ == '__main__':
    unittest.main()
