#!/usr/bin/env python3
"""ppu_cache_regs.py: the guest registers of a lifted body in C locals.

Checks the rewrite of small bodies in both modes: where a register is loaded from the context, where it goes
back to it, and that a statement handing the context out sees it exact.

Usage:
    python tools/test_ppu_cache_regs.py
"""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppu_cache_regs as c  # noqa: E402

BODY = [
    "ctx->gpr[3] = ctx->gpr[4] + 1;",
    "vm_write32(ctx->gpr[1] + 8, (uint32_t)ctx->gpr[3]);",
    "func_00010200(ctx);",
    "ctx->gpr[5] = ctx->gpr[3] << 2;",
    "if ((--ctx->ctr != 0)) goto loc_00010010;",
    "loc_00010008:",
    "ctx->fpr[1] = ctx->fpr[2] * 2.0;",
    "if (ctx->gpr[5] == 0) { g_trampoline_fn = (void(*)(void*))func_00010300; return; }",
    "loc_00010010:",
    "ctx->gpr[6] = ctx->gpr[5] + ctx->gpr[3];",
    "return;",
]


def index(lines, text):
    for i, line in enumerate(lines):
        if text in line:
            return i
    raise AssertionError("%r not in %r" % (text, lines))


class CacheRegs(unittest.TestCase):
    def test_write_through(self):
        out = c.rewrite(BODY, "wt")
        i = index(out, "_rg3 = _rg4 + 1;")
        self.assertIn("ctx->gpr[3] = _rg3;", out[i + 1])
        # after the call, what is read again comes from the context
        call = out.index("func_00010200(ctx);")
        self.assertIn("_rg3 = ctx->gpr[3];", out[call + 1])
        # ctr: stored on the goto edge and on the way on
        self.assertIn("{ ctx->ctr = _rctr; goto loc_00010010; }", out[index(out, "--_rctr")])
        self.assertEqual(out[index(out, "--_rctr") + 1].strip(), "ctx->ctr = _rctr;")
        # a write-through body stores nothing at a return
        self.assertEqual(out[-1].strip(), "return;")

    def test_write_back(self):
        out = c.rewrite(BODY, "wb")
        call = out.index("func_00010200(ctx);")
        self.assertIn("ctx->gpr[3] = _rg3;", out[call - 1])
        self.assertNotIn("ctx->gpr[3] = _rg3;", out[index(out, "_rg3 = _rg4 + 1;") + 1])
        # the trampoline's return takes everything that may have changed
        tramp = out[index(out, "g_trampoline_fn")]
        for s in ("ctx->gpr[5] = _rg5;", "ctx->fpr[1] = _rf1;", "ctx->ctr = _rctr;"):
            self.assertIn(s, tramp)
        # the goto edge loads what the label's code has in its local on the other way in
        self.assertIn("{ _rf1 = ctx->fpr[1]; goto loc_00010010; }", out[index(out, "--_rctr")])
        last = out[-1]
        for s in ("ctx->gpr[5] = _rg5;", "ctx->gpr[6] = _rg6;", "ctx->ctr = _rctr;", "return;"):
            self.assertIn(s, last)

    def test_bodies_left_alone(self):
        self.assertEqual(c.rewrite(["goto loc_DEADBEEF;"], "wb"), ["goto loc_DEADBEEF;"])
        plain = ["vm_write32(0x1000, 0);", "return;"]
        self.assertEqual(c.rewrite(plain, "wt"), plain)
        lazy = ["PPU_LF_SET(0, (ctx->cr >> 28));", "return;"]   # CR stays with --lazy-flags
        self.assertEqual(c.rewrite(lazy, "wb"), lazy)

    def test_hand_off_verbatim(self):
        body = ["ctx->gpr[3] = 1;", "ps3_hle_call(0x1234u, ctx); return;"]
        out = c.rewrite(body, "wb")
        self.assertIn("ps3_hle_call(0x1234u, ctx); return;", out)
        self.assertIn("ctx->gpr[3] = _rg3;", out[out.index("ps3_hle_call(0x1234u, ctx); return;") - 1])


if __name__ == "__main__":
    unittest.main()
