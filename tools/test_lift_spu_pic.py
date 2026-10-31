#!/usr/bin/env python3
"""lift_spu_pic.py and the runtime's position-independent pieces.

A synthetic big-endian ELF64 holds one piece: an entry that loads from the
constant pool after the code (lqr), calls a helper (brsl) and stores into the
pool (stqr). Checks that the piece is lifted with its code end at the pool,
that the registration TU carries the instructions only (the pool is not
verified: the piece stores into it) and the whole piece as its span. On POSIX
it then links the registration TU and the lifted C against the SPU runtime:
  - the piece inside a block GET from an EA it is not registered under is
    found by its bytes, wherever the block put it, with pic_base set;
  - a store into its pool keeps it; a foreign GET into its code ends it, and
    changed code is not adopted;
  - a miss the dispatch cache kept is looked at again once the piece is there;
  - inside another image's resident span that image decides, even where it
    has no function.
No game data is involved."""
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "tools/lift_spu_pic.py"
LIFT_BASE, IMAGE = 0x30000, 400


def ri16(op, imm, rt): return (op << 23) | ((imm & 0xFFFF) << 7) | rt
def lqr(pc, tgt, rt): return ri16(0x067, (tgt - pc) >> 2, rt)
def stqr(pc, tgt, rt): return ri16(0x047, (tgt - pc) >> 2, rt)
def brsl(pc, tgt, rt=0): return ri16(0x066, (tgt - pc) >> 2, rt)
def ai(rt, ra, imm): return (0x1C << 24) | ((imm & 0x3FF) << 14) | (ra << 7) | rt
BI_R0, LNOP, STOP = 0x35000000, 0x00200000, 0x00000000

CODE = [
    lqr(0x00, 0x20, 3),     # 0x00  entry: a constant from the pool
    brsl(0x04, 0x14),       # 0x04  call the helper
    stqr(0x08, 0x30, 3),    # 0x08  the result into the pool
    STOP,                   # 0x0C
    LNOP,                   # 0x10
    ai(3, 3, 1),            # 0x14  helper
    BI_R0,                  # 0x18
    LNOP,                   # 0x1C
]
CODE_END = 0x20
PIECE = b"".join(struct.pack(">I", w) for w in CODE) + bytes([0x11] * 16) + bytes(16)

SEG_VA = 0x00800000
PIECE_EA = SEG_VA + 0x200


def ppu_elf():
    seg = bytearray(0x400)
    seg[PIECE_EA - SEG_VA:PIECE_EA - SEG_VA + len(PIECE)] = PIECE
    phoff, data_off = 0x40, 0x100
    eh = b"\x7fELF" + bytes([2, 2, 1]) + bytes(9)
    eh += struct.pack(">HHIQQQIHHHHHH", 2, 21, 1, 0, phoff, 0, 0, 0x40, 0x38, 1, 0x40, 0, 0)
    ph = struct.pack(">IIQQQQQQ", 1, 5, data_off, SEG_VA, SEG_VA, len(seg), len(seg), 0x10)
    out = bytearray(eh + ph)
    out += bytes(data_off - len(out)) + seg
    return bytes(out)


failures = []
def check(ok, what):
    print(("PASS " if ok else "FAIL ") + what)
    if not ok:
        failures.append(what)


with tempfile.TemporaryDirectory(prefix="lift-spu-pic-") as tmp:
    tmp = Path(tmp)
    (tmp / "ppu.elf").write_bytes(ppu_elf())
    (tmp / "pieces.json").write_text(json.dumps([{"ea": hex(PIECE_EA), "size": len(PIECE), "entry": "0x0"}]))
    r = subprocess.run([sys.executable, str(TOOL), "--elf", str(tmp / "ppu.elf"),
                        "--list", str(tmp / "pieces.json"), "--out", str(tmp / "out"),
                        "--symbol-prefix", "synth"], capture_output=True, text=True)
    if r.returncode:
        print(r.stdout, r.stderr)
        raise SystemExit("lift_spu_pic failed")
    manifest = json.loads((tmp / "out/pic_manifest.json").read_text())
    check(len(manifest) == 1 and manifest[0]["image"] == IMAGE and manifest[0]["size"] == len(PIECE),
          "one piece, image 400, its whole size")
    check(manifest[0]["code_end"] == hex(CODE_END), "code ends where the pool starts")
    name = manifest[0]["name"]
    src = (tmp / "out" / name / "spu_recomp.c").read_text()
    starts = {int(x, 16) for x in re.findall(r"^void \w*spu_func_([0-9A-F]{8})\(spu_context\* (?:SPU_RESTRICT )?ctx\)\s*\{",
                                              src, re.M)}
    check({LIFT_BASE, LIFT_BASE + 0x14} <= starts, "entry and helper lifted")
    reg = (tmp / "out/pic_register.c").read_text()
    arr = re.search(r"static const uint8_t code_%d\[(\d+)\] = \{([^}]*)\}" % IMAGE, reg)
    got = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-F]{2})", arr.group(2))) if arr else b""
    check(got == PIECE[:CODE_END], "the verified bytes are the instructions, not the pool")
    check(re.search(r"spu_overlay_register_pic_region\(0x%08Xu, 0x30000u, %du, %d, &seg, 1\)"
                    % (PIECE_EA, len(PIECE), IMAGE), reg) is not None,
          "registered under its EA with the whole piece as its span")

    if os.name == "nt":
        print("SKIP runtime adoption: fixture requires the POSIX compiler/linker")
    else:
        spu = ROOT / "runtime/spu"
        driver = tmp / "driver.c"
        driver.write_text(r'''
#include "spu_interp.h"
#include "spu_overlay_residency.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern void spu_overlay_note_get(spu_context*, uint32_t, const uint8_t*, uint32_t);
extern void (*spu_lookup(uint32_t addr, int image_id))(spu_context*);
extern const unsigned char piece[];
extern const unsigned piece_len;
uint8_t* vm_base;
uint32_t ppu_vm_size;
void spu_thread_publish_ctx(uint32_t tid, void* c) { (void)tid; (void)c; abort(); }
void ppu_resv_break_store(uint64_t ea) { (void)ea; abort(); }
void ps3_ww_report_inline(uint32_t a, uint64_t v, int w) { (void)a; (void)v; (void)w; abort(); }
unsigned long long ps3_ms_now(void) { abort(); }
uint32_t g_ww_lo = 0xFFFFFFFFu, g_ww_hi;
int g_resv_store_active;
uint32_t g_barrier_sync_watch;
uint32_t g_ydkj_real_spurs_ea, g_ydkj_real_taskset_ea, g_ydkj_real_taskid;
void spurs_ef_set_from_spu(uint32_t ea, uint16_t bits) { (void)ea; (void)bits; abort(); }
void ydkj_wake_all_event_flags(void) { abort(); }
void spurs_pm_build_context(spu_context* c, uint32_t a, uint32_t b, uint32_t d) {
    (void)c; (void)a; (void)b; (void)d; abort();
}
uint32_t g_spu_image_src_ea, g_spu_image_ls_start, g_spu_image_span;
static int failed;
static void check(int ok, const char* what) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); failed |= !ok; }
static void get(spu_context* c, uint32_t ea, uint32_t lsa, const void* bytes, uint32_t size)
{
    memcpy(c->ls + lsa, bytes, size);
    spu_overlay_note_get(c, ea, c->ls + lsa, size);
}
int main(void)
{
    static spu_context ctx;
    spu_context_init(&ctx, 0);
    const spu_lifted_fn entry = (spu_lifted_fn)spu_lookup(LIFT_BASE, IMAGE);
    check(entry != NULL, "the piece's entry is registered");

    /* A 0x200-byte block from a heap EA, the piece at +0x80 (LS 0x12340),
     * its pool as its stqr left it. */
    const uint32_t place = 0x12340;
    check(spu_lifted_lookup(&ctx, place) == NULL, "nothing there before the block");
    static uint8_t block[0x200];
    memset(block, 0x77, sizeof block);
    memcpy(block + 0x80, piece, piece_len);
    memset(block + 0x80 + 0x30, 0xAB, 16);
    get(&ctx, 0x00900000u, place - 0x80, block, sizeof block);
    check(spu_lifted_lookup(&ctx, place) == entry && ctx.pic_base == place - LIFT_BASE,
          "found by its bytes where the block put it, its pool written, pic_base set");

    /* A foreign GET into its code ends it; changed code is not adopted. */
    static uint8_t bad[16];
    memcpy(bad, piece + 0x10, 16);
    bad[4] ^= 1;
    get(&ctx, 0x00A00000u, place + 0x10, bad, 16);
    check(spu_lifted_lookup(&ctx, place) == NULL, "changed code stays interpreted");

    /* The miss is kept; a GET that ends no span brings the bytes back. */
    get(&ctx, 0x00B00000u, place + 0x10, piece + 0x10, 16);
    check(spu_lifted_lookup(&ctx, place) == entry, "a kept miss is looked at again");

    /* A second copy where nothing was looked up before: the resolver itself
     * finds it, and each copy keeps its own pic_base. */
    get(&ctx, 0x00D00000u, 0x18000u, piece, piece_len);
    check(spu_lifted_lookup(&ctx, 0x18000u) == entry && ctx.pic_base == 0x18000u - LIFT_BASE,
          "a second copy found where nothing was looked up before");
    check(spu_lifted_lookup(&ctx, place) == entry && ctx.pic_base == place - LIFT_BASE,
          "the first copy still resident, with its own pic_base");

    /* A module carrying the piece's bytes at LS 0x20100, with no function there. */
    static uint8_t mod[0x400];
    memset(mod, 0x40, sizeof mod);
    memcpy(mod + 0x100, piece, CODE_END);
    const spu_overlay_code_segment seg = { 0x20000u, sizeof mod, mod };
    check(spu_overlay_register_verified_region(0x00C00000u, 0x20000u, sizeof mod, 900, &seg, 1),
          "module registered");
    get(&ctx, 0x00C00000u, 0x20000u, mod, sizeof mod);
    check(spu_lifted_lookup(&ctx, 0x20100) == NULL, "inside the module's span the module decides");
    int live = 0;
    for (unsigned s = 0; s < SPU_RESIDENT_CODE_SLOTS; ++s)
        live |= ctx.resident_code[s].image_id == 900;
    check(live, "the module stays resident");
    return failed;
}
'''.replace("LIFT_BASE", hex(LIFT_BASE) + "u").replace("CODE_END", hex(CODE_END))
   .replace("IMAGE", str(IMAGE)))
        (tmp / "piece.c").write_text(
            f"const unsigned char piece[] = {{{','.join(str(x) for x in PIECE)}}};\n"
            f"const unsigned piece_len = {len(PIECE)};\n")
        sources = ["spu_channels.c", "spu_drain.c", "spu_jobcap.c", "spu_lockstep.c", "spu_coherency.c",
                   "spu_workload.c", "spurs_policy.c", "spurs_job.c", "spu_tsp_weak.c",
                   "spu_vm_pagemap.c", "spurs_policy_blob_weak.c", "spu_interp.c"]
        binary = tmp / "adopt"
        subprocess.run([os.environ.get("CC", "cc"), "-std=gnu17", "-O1", "-UNDEBUG", "-w",
                        "-I" + str(ROOT / "include"), "-I" + str(spu),
                        "-I" + str(ROOT / "runtime/platform"),
                        str(driver), str(tmp / "piece.c"), str(tmp / "out/pic_register.c"),
                        str(tmp / "out" / name / "spu_recomp.c"),
                        *[str(spu / s) for s in sources],
                        str(ROOT / "runtime/platform/win32_compat.c"), "-pthread", "-lm",
                        "-o", str(binary)], check=True)
        clean = {k: v for k, v in os.environ.items() if not k.startswith(("SPU_", "SPURS_", "YDKJ_", "LBP_"))}
        # Twice: with the dispatch cache (its kept misses) and without (the resolver alone).
        for label, extra in (("", {}), (" without the dispatch cache", {"SPU_DISPATCH_CACHE": "0"})):
            r = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10, cwd=tmp,
                               env=dict(clean, **extra))
            print(r.stdout.strip())
            if r.stderr.strip():
                print(r.stderr.strip()[-2000:])
            check(r.returncode == 0, "runtime adoption of a piece found by its bytes" + label)

if failures:
    raise SystemExit("FAILED: " + "; ".join(failures))
print("lift_spu_pic: all checks passed")
