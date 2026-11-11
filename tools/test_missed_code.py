#!/usr/bin/env python3
"""tools/missed_code.py against a made-up executable and profile, and the runtime's recorder
(runtime/diag/missed_code.c) on its own, run twice on one file."""
import json
import os
import re
import shlex
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
import missed_code                                              # noqa: E402

TOOL = str(root / "tools" / "missed_code.py")
LNOP = struct.pack(">I", 0x00200000)
VADDR = 0x10000
PIECE, BRA_PIECE, MODULE, LISTED, PPU = 0x11000, 0x12000, 0x13000, 0x14000, 0x15000


def make_elf():
    seg = bytearray(0x6000)
    for off in range(0, len(seg), 4):
        seg[off:off + 4] = LNOP
    seg[BRA_PIECE - VADDR + 0x20:BRA_PIECE - VADDR + 0x24] = struct.pack(">I", 0x060 << 23)    # bra
    seg[PPU - VADDR:PPU - VADDR + 0x40] = struct.pack(">I", 0x60000000) * 0x10
    # Each entry the profile names is told apart by its bytes.
    for ea, tag in ((PIECE + 0x10, 1), (PIECE + 0x20, 2), (BRA_PIECE + 0x10, 3), (MODULE + 0x80, 4), (MODULE + 0xC4, 5),
                    (LISTED + 0x10, 6), (LISTED + 0x20, 7), (PIECE + 0x400, 8), (MODULE + 0xF8, 9)):
        seg[ea - VADDR:ea - VADDR + 4] = struct.pack(">I", 0x40800000 | tag)
    head = bytearray(0x40)
    head[:6] = b"\x7fELF\x02\x02"
    struct.pack_into(">Q", head, 0x20, 0x40)
    struct.pack_into(">HH", head, 0x36, 56, 1)
    ph = struct.pack(">IIQQQQQQ", 1, 5, 0x1000, VADDR, VADDR, len(seg), len(seg), 0x1000)
    return bytes(head) + ph + bytes(0x1000 - 0x40 - 56) + bytes(seg), bytes(seg)


def sig_at(seg, ea):
    return seg[ea - VADDR:ea - VADDR + 16].hex().upper()


def spu(seg, ea, off, size, owner, pc, steps, get=True):
    return "spu %s ea=%08X size=%d off=%X pc=%05X owner=%d calls=3 steps=%d" % (
        sig_at(seg, ea + off), ea if get else 0, size if get else 0, off if get else 0, pc, owner, steps)


def tool(args, check=True):
    r = subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True)
    if check and r.returncode:
        print(r.stdout, r.stderr)
        raise SystemExit("missed_code.py failed")
    return r


def test_tool():
    elf, seg = make_elf()
    with tempfile.TemporaryDirectory(prefix="missed-code-") as tmp:
        t = Path(tmp)
        (t / "elf").write_bytes(elf)
        pieces = t / "pieces.json"
        extra = t / "extra.json"
        ppu_extra = t / "ppu.json"
        funcs = t / "funcs.json"
        manifest = t / "manifest.json"
        before = [{"ea": hx(LISTED), "size": 0x40, "entry": "0x10"}, {"ea": hx(0x1F000), "size": 0x20, "entry": "0x0"}]
        pieces.write_text(json.dumps(before))
        extra.write_text(json.dumps({}))
        funcs.write_text(json.dumps([{"start": hx(PPU), "end": hx(PPU + 0x40), "toc": "0x83F2B0"}]))
        manifest.write_text(json.dumps([{"name": "jobmod_x", "base": 0x4000, "ea": MODULE, "eas": [MODULE],
                                         "size": 0x100, "code_end": 0x40C0}]))
        profile = t / "profile.txt"
        common = ["--profile", str(profile), "--elf", str(t / "elf"), "--pic-list", str(pieces),
                  "--jobmods-manifest", str(manifest), "--jobmods-extra", str(extra),
                  "--functions", str(funcs), "--ppu-extra", str(ppu_extra)]
        rows = [missed_code.MAGIC,
                spu(seg, PIECE, 0x10, 0x80, 2, 0x5010, 5000),              # a new piece
                spu(seg, PIECE, 0x20, 0x80, 2, 0x5020, 1500),              # and a second entry of it
                spu(seg, BRA_PIECE, 0x10, 0x40, 2, 0x6010, 9000),          # tied to an address
                spu(seg, PIECE + 0x400, 0, 0x20, 2, 0x7000, 10),           # too few steps
                spu(seg, MODULE, 0x80, 0x100, 2, 0x4080, 3000),            # a module, by where it came from
                spu(seg, MODULE, 0xC4, 0x100, 200, 0x40C4, 300),           # past what was lifted of it, in its image
                spu(seg, MODULE, 0xF8, 0x100, 200, 0x40F8, 200),           # its 16 bytes not all in the image
                spu(seg, LISTED, 0x10, 0x40, 400, 0x8010, 800),            # listed, and interpreted still
                spu(seg, LISTED, 0x20, 0x40, 400, 0x8020, 700),            # a further entry of a listed piece
                spu(seg, LISTED, 0x10, 0x40, 2, 0x9010, 0),                # given straight back
                spu(seg, PIECE, 0x10, 0, -1, 0xA000, 40, get=False),       # no GET known
                "ppu %08X lr=00015000 word=60000000 calls=2" % (PPU + 0x10),   # inside a known function
                "ppu %08X lr=00015000 word=12345678 calls=2" % (PPU + 0x20),   # not what is there
                "ppu %08X lr=00015000 word=60000000 calls=2" % PPU]            # known already
        profile.write_text("\n".join(rows) + "\n")

        out = tool(common).stdout
        assert json.loads(pieces.read_text()) == before and not ppu_extra.exists(), "wrote without --apply"
        assert profile.exists()
        for want in ("new piece 0x11000, 128 bytes, entry 0x10 and 0x20",
                     "jobmod_x: new entries 0x4080 0x40C4; 1 past its code_end, which moves out to them",
                     "piece 0x14000: new entries 0x20", "new PPU function 0x00015010", "absolute branch",
                     "too few steps", "jobmod_x 0x40F8: outside the module (its image ends at 0x4100)", "no GET known",
                     "piece 0x14000 entry 0x10", "the instruction differs"):
            assert want in out, (want, out)
        print("PASS: every kind of row goes where it belongs")

        out = tool(common + ["--apply"]).stdout
        now = json.loads(pieces.read_text())
        assert [p["ea"] for p in now] == [hx(PIECE), hx(LISTED), hx(0x1F000)], now
        assert now[0] == {"ea": hx(PIECE), "size": 0x80, "entry": "0x10", "entries": ["0x20"]}, now[0]
        assert now[1]["entries"] == ["0x20"], now[1]
        assert json.loads(extra.read_text()) == {"jobmod_x": ["0x4080", "0x40C4"]}
        assert [e["addr"] for e in json.loads(ppu_extra.read_text())] == ["0x00015010"]
        assert not profile.exists() and (t / "profile.txt.applied").exists()
        print("PASS: --apply writes the lists and sets the profile aside")

        (t / "profile.txt.applied").rename(profile)
        out = tool(common).stdout
        assert "0 change(s)" in out, out
        print("PASS: a profile already taken in changes nothing")

        profile.write_text(missed_code.MAGIC + "\nspu " + "AB" * 16 + " ea=00011000 size=128 off=10 pc=05010 "
                           "owner=2 calls=1 steps=9000\n")
        r = tool(common, check=False)
        assert r.returncode and "another build" in (r.stdout + r.stderr), r
        print("PASS: a profile from another build is refused")

        profile.write_text(missed_code.MAGIC + "\nspu nonsense\n")
        r = tool(common, check=False)
        assert r.returncode and "malformed" in (r.stdout + r.stderr), r
        print("PASS: a damaged profile is refused")


def hx(v):
    return "0x%X" % v


def test_merge_functions():
    funcs = [{"start": "0x100", "end": "0x140", "toc": "0xA", "opd": "0xB"}, {"start": "0x200", "end": "0x240", "toc": "0xA"}]
    out = missed_code.merge_ppu_functions(funcs, [0x120, 0x180, 0x200])
    assert [(f["start"], f["end"]) for f in out] == [
        ("0x100", "0x120"), ("0x120", "0x140"), ("0x180", "0x200"), ("0x200", "0x240")], out
    assert all(f.get("toc") == "0xA" for f in out), out
    assert "opd" not in out[1] and funcs[0]["end"] == "0x140", "the split changed its input or kept the opd"
    print("PASS: a missed PPU address splits the function around it, or fills the gap")


def test_recorder():
    cc = os.environ.get("CC")
    if os.name == "nt" and not cc:
        print("SKIP: the recorder fixture needs a C compiler in CC here")
        return
    with tempfile.TemporaryDirectory(prefix="missed-rec-") as tmp:
        exe = Path(tmp) / "fixture"
        subprocess.run([cc or "cc", "-std=gnu17", "-O1", "-UNDEBUG", *shlex.split(os.environ.get("CFLAGS", "")),
                        "-I" + str(root / "include"), str(root / "runtime/diag/tests/test_missed_code.c"),
                        str(root / "runtime/diag/missed_code.c"), "-o", str(exe)], check=True)
        path = Path(tmp) / "m.txt"
        for run in (1, 2):
            r = subprocess.run([str(exe), str(path)], capture_output=True, text=True, timeout=30)
            assert r.returncode == 0, (r.returncode, r.stderr)
            spu_rows, ppu_rows = missed_code.read_profile(str(path))
            assert len(spu_rows) == 2 and len(ppu_rows) == 1, path.read_text()
            a = next(x for x in spu_rows if x["ea"])
            assert (a["ea"], a["size"], a["off"], a["pc"], a["owner"]) == (0x755080, 2816, 0x18, 0x11018, 2), a
            assert (a["calls"], a["steps"]) == (run, 100 * run), a
            b = next(x for x in spu_rows if not x["ea"])
            assert (b["pc"], b["owner"], b["steps"]) == (0x20000, -1, 7 * run), b
            assert ppu_rows[0]["calls"] == run and ppu_rows[0]["addr"] == 0x123450, ppu_rows
        assert re.match(re.escape(missed_code.MAGIC) + "\n", path.read_text())
        assert not Path(str(path) + ".tmp").exists()
        print("PASS: the recorder names the GET, adds to what earlier runs left, and leaves out zero-step entries")


test_tool()
test_merge_functions()
test_recorder()
print("ok")
