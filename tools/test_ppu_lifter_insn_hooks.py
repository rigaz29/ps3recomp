#!/usr/bin/env python3
"""--insn-hooks: a port function is called before the hooked instruction.

1. Each hook becomes `fn(ctx, 0xADDRu);` after the instruction's branch label
   and before its code, so at a loop head it runs on every pass, and fn is
   declared before the function (extern "C" in C++).
2. The serial and the parallel lift emit the same.
3. A hook at an address no lifted function covers stops the lift.

Run: python tools/test_ppu_lifter_insn_hooks.py
"""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

CODE = bytes.fromhex(
    "38600000"   # 0x10000 li    r3, 0
    "38630001"   # 0x10004 addi  r3, r3, 1      <- loop head
    "2f830004"   # 0x10008 cmpwi cr7, r3, 4
    "409dfff8"   # 0x1000C ble   cr7, 0x10004
    "4e800020")  # 0x10010 blr


def lift(root, hooks, jobs="1"):
    (root / "hooks.json").write_text(json.dumps({"hooks": hooks}))
    out = root / ("out" + jobs)
    cmd = [sys.executable, str(Path(__file__).with_name("ppu_lifter.py")),
           str(root / "code.bin"), "--raw", "--base", "0x10000",
           "--functions", str(root / "functions.json"), "--jobs", jobs,
           "--insn-hooks", str(root / "hooks.json"), "--output", str(out)]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True, timeout=120)
    text = "".join(p.read_text() for p in sorted(out.glob("ppu_recomp_*.cpp"))) \
        if r.returncode == 0 else ""
    return r, text


def main():
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "code.bin").write_bytes(CODE)
        (root / "functions.json").write_text(
            json.dumps([{"start": "0x10000", "end": "0x10014"}]))
        hooks = [{"addr": "0x10004", "fn": "port_poll"}]
        r, text = lift(root, hooks)
        assert r.returncode == 0, r.stderr
        assert 'extern "C" void port_poll(ppu_context* ctx, uint32_t addr);' in text, text
        head = text.index("loc_00010004:")
        call = text.index("port_poll(ctx, 0x00010004u);  /* insn hook */")
        addi = text.index("ctx->gpr[3] + (int64_t)(1)", head)
        assert head < call < addi, text
        assert "goto loc_00010004;" in text, text

        r2, text2 = lift(root, hooks, "2")
        assert r2.returncode == 0, r2.stderr
        body = lambda t: t[t.index("void func_00010000"):]
        assert body(text2) == body(text), (text, text2)

        r, _ = lift(root, [{"addr": "0x20000", "fn": "port_poll"}])
        assert r.returncode != 0 and "0x00020000" in r.stderr, r.stderr[-400:]
        r, _ = lift(root, [{"addr": "0x10004", "fn": "not an identifier"}])
        assert r.returncode != 0 and "not a C identifier" in r.stderr, r.stderr[-400:]
    print("PASS: insn hooks run before their instruction, after its label")


if __name__ == "__main__":
    main()
