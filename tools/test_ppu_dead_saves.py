#!/usr/bin/env python3
"""Tests for ppu_dead_saves.py on small lifted-looking functions. Run: python tools/test_ppu_dead_saves.py"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ppu_dead_saves as ds


class F:
    def __init__(self, name, lines):
        self.name = name
        self.body_lines = list(lines)


PROLOGUE = [
    "    uint64_t _cs_31 = ctx->gpr[31];",
    "    vm_write64(ctx->gpr[1] + -0x80, ctx->gpr[1]); ctx->gpr[1] += -0x80;",
    "    ctx->gpr[0] = ctx->lr;",
    "    vm_write64(ctx->gpr[1] + 0x90, ctx->gpr[0]);",
    "    vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[31]);",
]
EPILOGUE = [
    "    ctx->gpr[0] = vm_read64(ctx->gpr[1] + 0x90);",
    "    ctx->gpr[31] = _cs_31;",
    "    ctx->gpr[1] = ctx->gpr[1] + (int64_t)(0x80);",
    "    ctx->lr = ctx->gpr[0];",
    "    return;",
]
SAVE = "    vm_write64(ctx->gpr[1] + 0x78, ctx->gpr[31]);"
FAILED = []


def check(what, ok):
    print(("PASS: " if ok else "FAIL: ") + what)
    if not ok:
        FAILED.append(what)


def run(funcs, mode="on", tails=None):
    fs = [F(n, b) for n, b in funcs]
    done, why = ds.apply(fs, mode, tails)
    return {f.name: f.body_lines for f in fs}, done, why


body, done, _ = run([("func_00001000", PROLOGUE + ["    ctx->gpr[31] = ctx->gpr[3];"] + EPILOGUE)])
b = body["func_00001000"]
check("a callee-save nothing reads goes", SAVE not in b and done["gpr"] == 1)
check("the saved LR is reloaded from a copy", "    ctx->gpr[0] = _cs_lr;" in b and "    uint64_t _cs_lr = ctx->lr;" in b)
check("the LR store stays, as a frame store", any("vm_write64_frame(ctx->gpr[1] + 0x90" in l for l in b))
check("the back chain stays, as a frame store", any("vm_write64_frame(ctx->gpr[1] + -0x80" in l for l in b))

body, done, _ = run([("func_00001000", PROLOGUE + ["    ctx->gpr[31] = ctx->gpr[3];"] + EPILOGUE)], "poison")
check("poison keeps a store, with the poison value", any(ds.POISON in l and "0x78" in l for l in body["func_00001000"]))

body, _, why = run([("func_00001000", PROLOGUE + ["    ctx->gpr[4] = vm_read64(ctx->gpr[1] + 0x78);"] + EPILOGUE)])
check("a load of the slot keeps the save", SAVE in body["func_00001000"] and why["the slot is read"] == 1)

body, _, why = run([("func_00001000", PROLOGUE + ["    ctx->gpr[5] = ctx->gpr[1] + (int64_t)(0x78);"] + EPILOGUE)])
check("an address taken of the slot keeps the save", SAVE in body["func_00001000"] and why["the slot's address is taken"] == 1)

body, _, _ = run([("func_00001000", PROLOGUE + ["    ctx->gpr[5] = ctx->gpr[1] | 0x0;"] + EPILOGUE)])
check("r1 copied keeps every save", SAVE in body["func_00001000"])

fragment = ["    uint64_t _cs_31 = vm_read64(ctx->gpr[1] + 0x78);", "    ctx->gpr[31] = _cs_31;", "    return;"]
jump = "    { g_trampoline_fn = (void(*)(void*))func_00002000; return; }"
body, _, why = run([("func_00001000", PROLOGUE + [jump]), ("func_00002000", fragment)])
check("a tail-called piece reading the frame keeps the save", SAVE in body["func_00001000"] and why["a callee reads it"] == 1)

body, _, why = run([("func_00001000", list(PROLOGUE)), ("func_00002000", fragment)], tails={"func_00001000": jump})
check("so does one reached by falling through", SAVE in body["func_00001000"] and why["a callee reads it"] == 1)

body, _, _ = run([("func_00001000", PROLOGUE + EPILOGUE[:-1] + [jump]), ("func_00002000", fragment)])
check("a tail call after the frame is popped reads the caller's frame, not this one", SAVE not in body["func_00001000"])

late = list(PROLOGUE)
late.insert(1, "    ctx->gpr[31] = ctx->gpr[4];")
body, _, why = run([("func_00001000", late + EPILOGUE)])
check("a register written before its save keeps it", SAVE in body["func_00001000"] and why["register written before the save"] == 1)

fsave = "    { uint64_t tmp; memcpy(&tmp, &ctx->fpr[30], 8); vm_write64(ctx->gpr[1] + 0x70, tmp); }"
frest = "    { uint64_t tmp = vm_read64(ctx->gpr[1] + 0x70); memcpy(&ctx->fpr[30], &tmp, 8); }"
body, done, _ = run([("func_00001000", PROLOGUE + [fsave, "    ctx->fpr[30] = 1.0;", frest] + EPILOGUE)])
b = body["func_00001000"]
check("an fpr save goes and its reload takes a copy", fsave not in b and "    memcpy(&ctx->fpr[30], &_fs_30, 8);" in b and done["fpr"] == 1)

toc = "    vm_write64(ctx->gpr[1] + 0x28, ctx->gpr[2]);"
body, done, _ = run([("func_00001000", PROLOGUE + [toc, toc] + EPILOGUE)])
check("the saved TOC goes when nothing reloads it", toc not in body["func_00001000"] and done["gpr"] == 3)

print("%d failed" % len(FAILED))
sys.exit(1 if FAILED else 0)
