#!/usr/bin/env python3
"""missed_code.py -- turn what a title's runs missed into the lists the lifters read.

A run started with ps3_missed_code_to() (runtime/diag/missed_code.c) writes down
the SPU code that ran in the interpreter and the PPU calls that found no lifted
function. This reads that profile against the executable and sorts each row:

  - an entry inside a lifted job module: an extra function of that module
    (lift_jobmods.py --extra-funcs). One past the module's code_end but in its
    image too: code reached only through a pointer, which the lift's walk of
    direct branches stopped short of. As an extra function it seeds that walk,
    so the lift's end moves out to it (module_code_end);
  - an entry inside a listed position-independent piece: a further entry of it;
  - code a GET brought out of the executable, in no listed piece: a new piece
    (lift_spu_pic.py --list), the GET being its extent;
  - a PPU call: a function to add to the lift;
  - anything else is only reported.

Every row carries the bytes at its entry, and they have to be the executable's
there: a profile from another build of the title is refused. Rows that are
listed already and still ran interpreted are reported as such, since a further
lift will not change them.

Without --apply nothing is written. With it the lists are updated and the
profile moves to <profile>.applied, so the next run starts a new one and what it
shows is what the lift did not cure.

usage:
  python tools/missed_code.py --profile <file> --elf <PPU ELF> [--pic-list <pieces.json>]
      [--jobmods-manifest <manifest.json> --jobmods-extra <extra.json>]
      [--functions <functions.json> --ppu-extra <extra.json>] [--min-steps N] [--apply]
"""
import argparse
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from lift_jobmods import JOBMOD_IMAGE_BASE, elf_load_segments   # noqa: E402
from lift_spu_pic import pool_start                             # noqa: E402

MAGIC = "ps3recomp-missed-code 1"
SPU_ROW = re.compile(r"spu ([0-9A-F]{32}) ea=([0-9A-F]{8}) size=(\d+) off=([0-9A-F]+) pc=([0-9A-F]+) "
                     r"owner=(-?\d+) calls=(\d+) steps=(\d+)$")
PPU_ROW = re.compile(r"ppu ([0-9A-F]{8}) lr=([0-9A-F]{8}) word=([0-9A-F]{8}) calls=(\d+)$")
# Job code starts here in local store; below it is the module that runs the jobs.
JOB_LS_MIN = 0x4000
# Past a job module's header (lift_jobmods.py).
JOBMOD_CODE_MIN = 0x4030
# bra, brasl: a piece holding one is tied to an address and cannot be lifted to run anywhere.
ABSOLUTE_BRANCHES = (0x060, 0x062)


def read_profile(path):
    """(spu rows, ppu rows). A line that is not a row of this version is an error: the file is
    written whole, so it means the file is not what it should be."""
    spu, ppu = [], []
    with open(path, encoding="ascii") as f:
        lines = f.read().splitlines()
    if not lines or lines[0] != MAGIC:
        sys.exit(f"{path}: not a profile of this version")
    for n, line in enumerate(lines[1:], 2):
        m = SPU_ROW.match(line)
        if m:
            spu.append({"sig": bytes.fromhex(m.group(1)), "ea": int(m.group(2), 16), "size": int(m.group(3)),
                        "off": int(m.group(4), 16), "pc": int(m.group(5), 16), "owner": int(m.group(6)),
                        "calls": int(m.group(7)), "steps": int(m.group(8))})
            continue
        m = PPU_ROW.match(line)
        if m:
            ppu.append({"addr": int(m.group(1), 16), "lr": int(m.group(2), 16), "word": int(m.group(3), 16),
                        "calls": int(m.group(4))})
            continue
        sys.exit(f"{path}:{n}: malformed row: {line[:80]}")
    return spu, ppu


def exec_ranges(elf):
    """(vaddr, end) of each executable PT_LOAD segment of a big-endian ELF64."""
    phoff = struct.unpack_from(">Q", elf, 0x20)[0]
    phentsize, phnum = struct.unpack_from(">HH", elf, 0x36)
    out = []
    for i in range(phnum):
        p_type, p_flags, _, vaddr, _, filesz = struct.unpack_from(">IIQQQQ", elf, phoff + i * phentsize)
        if p_type == 1 and p_flags & 1:
            out.append((vaddr, vaddr + filesz))
    return out


def read_ea(segs, ea, size):
    for va, data in segs:
        if va <= ea and ea + size <= va + len(data):
            return data[ea - va: ea - va + size]
    return None


def has_absolute_branch(code):
    for off in range(0, len(code) - 3, 4):
        if (struct.unpack_from(">I", code, off)[0] >> 23) in ABSOLUTE_BRANCHES:
            return True
    return False


def piece_entries(p):
    return [int(p["entry"], 16)] + [int(e, 16) for e in p.get("entries", [])]


def hx(v):
    return "0x%X" % v


def module_at(manifest, r):
    """(job module, its local-store address for the row's code), or (None, 0). By where the code came
    from: a long module arrives in several GETs, and the context may not count it as resident."""
    if r["ea"]:
        at = r["ea"] + r["off"]
        for m in manifest:
            for e in m.get("eas", [m["ea"]]):
                if e <= at < e + m["size"]:
                    return m, m["base"] + at - e
    k = r["owner"] - JOBMOD_IMAGE_BASE
    return (manifest[k], r["pc"]) if 0 <= k < len(manifest) else (None, 0)


class Plan:
    def __init__(self):
        self.new_pieces = []        # [{"ea", "size", "entry", "entries"?, "steps"}]
        self.piece_entries = {}     # piece index -> {entry offset: steps}
        self.module_entries = {}    # module name -> {LS address: steps}
        self.past_end = set()       # (module name, LS address): an entry past the module's code_end
        self.ppu_new = []           # ppu rows
        self.still = []             # (what, steps): listed, and interpreted all the same
        self.report = []            # (what, steps): left alone, and why


def sort_spu(rows, segs, pieces, manifest, extra, min_steps):
    plan = Plan()
    checked = wrong = 0
    candidates = {}
    # An entry the interpreter gave straight back to lifted code ran nothing of its own.
    rows = [r for r in rows if r["steps"]]
    for r in rows:
        if r["ea"]:
            checked += 1
            if read_ea(segs, r["ea"] + r["off"], 16) != r["sig"]:
                wrong += 1
                r["wrong"] = True
    if wrong and wrong * 2 >= checked:
        sys.exit(f"{wrong} of {checked} rows do not match this executable: the profile is from another build")
    for r in sorted(rows, key=lambda x: -x["steps"]):
        if r.get("wrong"):
            plan.report.append(("entry bytes differ from the executable: pc=0x%05X ea=0x%X+0x%X"
                                % (r["pc"], r["ea"], r["off"]), r["steps"]))
            continue
        at = r["ea"] + r["off"] if r["ea"] else None
        i = None if at is None else next((k for k, p in enumerate(pieces)
                                          if int(p["ea"], 16) <= at < int(p["ea"], 16) + p["size"]), None)
        if i is not None:
            p = pieces[i]
            e = at - int(p["ea"], 16)
            if e in piece_entries(p):
                plan.still.append(("piece %s entry 0x%X" % (p["ea"], e), r["steps"]))
            elif e >= pool_start(read_ea(segs, int(p["ea"], 16), p["size"])):
                plan.report.append(("piece %s: 0x%X is in its constant pool" % (p["ea"], e), r["steps"]))
            else:
                d = plan.piece_entries.setdefault(i, {})
                d[e] = d.get(e, 0) + r["steps"]
            continue
        m, pc = module_at(manifest, r)
        if m:
            # Past code_end is the module's own code still, when its image holds the row's bytes there: the
            # module's code that only a pointer reaches (God of War: Ascension's jobmod_9b03908f93a9_e77C8
            # ran 0xB914-0xF1BC so, interpreted, past a code_end of 0xB90C).
            if not JOBMOD_CODE_MIN <= pc <= m["base"] + m["size"] - 16:
                plan.report.append(("%s 0x%X: outside the module (its image ends at 0x%X)"
                                    % (m["name"], pc, m["base"] + m["size"]), r["steps"]))
            elif read_ea(segs, m["ea"] + pc - m["base"], 16) != r["sig"]:
                plan.report.append(("%s 0x%X: bytes differ from the module" % (m["name"], pc), r["steps"]))
            elif pc in extra.get(m["name"], []):
                plan.still.append(("%s 0x%X" % (m["name"], pc), r["steps"]))
            else:
                d = plan.module_entries.setdefault(m["name"], {})
                d[pc] = d.get(pc, 0) + r["steps"]
                if pc >= m["code_end"]:
                    plan.past_end.add((m["name"], pc))
            continue
        if at is None:
            plan.report.append(("no GET known: pc=0x%05X owner=%d" % (r["pc"], r["owner"]), r["steps"]))
            continue
        if r["pc"] < JOB_LS_MIN:
            plan.report.append(("below the job area: pc=0x%05X owner=%d ea=0x%X" % (r["pc"], r["owner"], r["ea"]),
                                r["steps"]))
            continue
        c = candidates.setdefault((r["ea"], r["size"]), {})
        c[r["off"]] = c.get(r["off"], 0) + r["steps"]
    # The smallest transfer that holds the code is the piece; a larger one around it adds nothing.
    taken = [(int(p["ea"], 16), int(p["ea"], 16) + p["size"]) for p in pieces]
    for (ea, size), ents in sorted(candidates.items(), key=lambda kv: kv[0][1]):
        steps = sum(ents.values())
        what = "GET 0x%X, %d bytes" % (ea, size)
        if steps < min_steps:
            plan.report.append((what + ": too few steps to be worth a lift", steps))
            continue
        if any(ea < e and s < ea + size for s, e in taken):
            plan.report.append((what + ": overlaps a piece", steps))
            continue
        raw = read_ea(segs, ea, size)
        if raw is None:
            plan.report.append((what + ": not all of it is in the executable", steps))
            continue
        end = pool_start(raw)
        good = {e: s for e, s in ents.items() if e < end}
        if not good:
            plan.report.append((what + ": its entries are in the constant pool", steps))
            continue
        if has_absolute_branch(raw[:end]):
            plan.report.append((what + ": has an absolute branch, so it cannot run from anywhere", steps))
            continue
        order = sorted(good, key=lambda e: -good[e])
        piece = {"ea": hx(ea), "size": size, "entry": hx(order[0])}
        if order[1:]:
            piece["entries"] = [hx(e) for e in sorted(order[1:])]
        plan.new_pieces.append(dict(piece, steps=steps))
        taken.append((ea, ea + size))
    return plan


def sort_ppu(rows, segs, ranges, known, plan):
    for r in sorted(rows, key=lambda x: x["addr"]):
        what = "0x%08X from 0x%08X" % (r["addr"], r["lr"])
        if not any(lo <= r["addr"] < hi for lo, hi in ranges):
            plan.report.append(("ppu " + what + ": not in executable code", r["calls"]))
        elif read_ea(segs, r["addr"], 4) != struct.pack(">I", r["word"]):
            plan.report.append(("ppu " + what + ": the instruction differs from the executable", r["calls"]))
        elif r["addr"] in known:
            plan.still.append(("ppu " + what, r["calls"]))
        else:
            plan.ppu_new.append(r)


def insert_piece(pieces, new):
    """Before the first piece at a higher address: the list is in address order, run by run."""
    ea = int(new["ea"], 16)
    for i, p in enumerate(pieces):
        if int(p["ea"], 16) > ea:
            pieces.insert(i, new)
            return
    pieces.append(new)


def write_json(path, data):
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(json.dumps(data, indent=1) + "\n")


def merge_ppu_functions(funcs, addrs):
    """The analysis's function list with each address a function of its own: a function holding it
    ends there, and one in a gap runs to the next function."""
    out = sorted(({**f} for f in funcs), key=lambda f: int(str(f["start"]), 0))
    for a in sorted(set(addrs)):
        starts = [int(str(f["start"]), 0) for f in out]
        if a in starts:
            continue
        below = [i for i, s in enumerate(starts) if s < a]
        i = below[-1] if below else None
        f = out[i] if i is not None else None
        if f is not None and a < int(str(f["end"]), 0):
            new = {"start": hx(a), "end": f["end"]}
            f["end"] = hx(a)
        else:
            above = [s for s in starts if s > a]
            new = {"start": hx(a), "end": hx(above[0] if above else a + 4)}
        near = f or (out[0] if out else None)
        if near is not None and "toc" in near:
            new["toc"] = near["toc"]
        out.insert((i + 1) if i is not None else 0, new)
    return out


def human(n):
    for limit, unit in ((1e9, "G"), (1e6, "M"), (1e3, "k")):
        if n >= limit:
            return "%.1f %s" % (n / limit, unit)
    return str(n)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--profile", required=True)
    ap.add_argument("--elf", required=True, help="the PPU executable the profile's game runs")
    ap.add_argument("--pic-list", help="JSON list of position-independent pieces (lift_spu_pic.py --list)")
    ap.add_argument("--jobmods-manifest", help="jobmods_manifest.json of the last lift")
    ap.add_argument("--jobmods-extra", help="JSON: {module: [LS address, ...]}")
    ap.add_argument("--functions", help="the analysis's PPU function list")
    ap.add_argument("--ppu-extra", help="JSON list of PPU functions to add to the lift")
    ap.add_argument("--min-steps", type=int, default=1000,
                    help="leave out a new piece that ran fewer steps than this (default 1000)")
    ap.add_argument("--apply", action="store_true", help="write the lists, and set the profile aside")
    args = ap.parse_args()

    spu, ppu = read_profile(args.profile)
    with open(args.elf, "rb") as f:
        elf = f.read()
    segs = elf_load_segments(elf)
    pieces = json.load(open(args.pic_list)) if args.pic_list and os.path.exists(args.pic_list) else []
    manifest = (json.load(open(args.jobmods_manifest))
                if args.jobmods_manifest and os.path.exists(args.jobmods_manifest) else [])
    extra = {}
    if args.jobmods_extra and os.path.exists(args.jobmods_extra):
        extra = {k: [int(a, 16) for a in v] for k, v in json.load(open(args.jobmods_extra)).items()}
    ppu_extra = json.load(open(args.ppu_extra)) if args.ppu_extra and os.path.exists(args.ppu_extra) else []
    known = {int(str(e["addr"]), 16) for e in ppu_extra}
    if args.functions and os.path.exists(args.functions):
        known |= {int(str(f["start"]), 0) for f in json.load(open(args.functions))}

    plan = sort_spu(spu, segs, pieces, manifest, extra, args.min_steps)
    sort_ppu(ppu, segs, exec_ranges(elf), known, plan)

    print("%s: %d SPU rows, %s steps interpreted; %d PPU rows" % (
        args.profile, len(spu), human(sum(r["steps"] for r in spu)), len(ppu)))
    for p in sorted(plan.new_pieces, key=lambda x: -x["steps"]):
        print("  new piece %s, %d bytes, entry %s%s (%s steps)" % (
            p["ea"], p["size"], p["entry"],
            " and " + " ".join(p["entries"]) if p.get("entries") else "", human(p["steps"])))
    for i, ents in sorted(plan.piece_entries.items()):
        print("  piece %s: new entries %s" % (pieces[i]["ea"], " ".join(hx(e) for e in sorted(ents))))
    for name, ents in sorted(plan.module_entries.items()):
        past = sorted(a for a in ents if (name, a) in plan.past_end)
        print("  %s: new entries %s%s" % (name, " ".join(hx(a) for a in sorted(ents)),
                                          "; %d past its code_end, which moves out to them" % len(past)
                                          if past else ""))
    for r in plan.ppu_new:
        print("  new PPU function 0x%08X, called from 0x%08X" % (r["addr"], r["lr"]))
    for title, rows in (("listed, and interpreted all the same", plan.still), ("left alone", plan.report)):
        if rows:
            rows = sorted(rows, key=lambda x: -x[1])
            print("  %s: %d" % (title, len(rows)))
            for what, n in rows[:12]:
                print("    %s (%s)" % (what, human(n)))
            if len(rows) > 12:
                print("    ... and %d more" % (len(rows) - 12))

    changes = len(plan.new_pieces) + len(plan.piece_entries) + len(plan.module_entries) + len(plan.ppu_new)
    if not args.apply:
        print("%d change(s) to the lists; --apply writes them" % changes)
        return 0
    # In this order: piece_entries is indexed by the list as it was read.
    for i, ents in plan.piece_entries.items():
        p = pieces[i]
        p["entries"] = [hx(e) for e in sorted(set(int(a, 16) for a in p.get("entries", [])) | set(ents))]
    for p in plan.new_pieces:
        if not args.pic_list:
            sys.exit("--apply with new pieces needs --pic-list")
        insert_piece(pieces, {k: v for k, v in p.items() if k != "steps"})
    if plan.new_pieces or plan.piece_entries:
        write_json(args.pic_list, pieces)
    if plan.module_entries:
        if not args.jobmods_extra:
            sys.exit("--apply with new module entries needs --jobmods-extra")
        for name, ents in plan.module_entries.items():
            extra.setdefault(name, []).extend(ents)
        write_json(args.jobmods_extra, {k: [hx(a) for a in sorted(set(v))] for k, v in sorted(extra.items())})
    if plan.ppu_new:
        if not args.ppu_extra:
            sys.exit("--apply with new PPU functions needs --ppu-extra")
        ppu_extra += [{"addr": "0x%08X" % r["addr"], "lr": "0x%08X" % r["lr"]} for r in plan.ppu_new]
        write_json(args.ppu_extra, sorted(ppu_extra, key=lambda e: int(e["addr"], 16)))
    os.replace(args.profile, args.profile + ".applied")
    print("%d change(s) written; the profile is now %s.applied" % (changes, args.profile))
    return 0


if __name__ == "__main__":
    sys.exit(main())
