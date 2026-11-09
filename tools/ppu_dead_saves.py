"""Dead stack saves for lifted PPU functions (ppu_lifter.py --dead-saves).

More than half of the stores in a lifted program go to the guest stack, and most of those save a
register no code reads back. A lifted function restores the non-volatile GPRs from a copy it took at
entry (_cs_N, made in the lifter's prologue pass), and r2 from a literal, yet `std rN,off(r1)` still
pays for a guest store: the range compare, the reserved-line test, a byte swap. Every call pays them
again.

This pass, which changes guest memory only and leaves ctx as it was:
  - drops the saves of r14-r31 and of r2 that nothing reads;
  - gives f14-f31 and LR the same treatment as the GPRs: a copy at entry in place of the reload.
    The FPR's save goes as well. LR's stays, with the back chain: the runtime's guest stack walks read
    them, and with those two the frame can still be walked;
  - writes the back chain and LR without the range compare and the reserved-line test. They are the
    frame's own header, which no SPU or other thread holds, and r1 is always mapped.

A save goes when, in its function:
  - it is the function's own: it comes before any write of the register, and the lifter's restore
    no longer reads the slot;
  - no other store touches the slot, no load reads it and no address is taken of it;
  - no callee reads it. A piece of a function lifted as a function of its own reaches into the frame
    of the piece that jumped to it: what a function reads above its entry r1 is settled over the
    program, along its calls and tail calls (Program.solve).
Offsets are compared against r1 at entry, tracked along the control flow. Where r1 cannot be tracked
or is copied, the function keeps everything.

Quadword accesses at r1 plus a register are not looked at: compilers keep the VMX save area apart
from the others and nothing reads a GPR with lvx.

--dead-saves=poison keeps the stores and writes a poison value in place of each register that would
go, so a reader this pass did not foresee gets garbage: run that build first when the title or the
compiler is new.
"""
import re
from collections import Counter

POISON = "0xBADC0FFEE0DDF00DULL"

_OFF = r"(-?0x[0-9A-Fa-f]+|-?\d+)"
_SAVE = re.compile(r"^\s*vm_write64\(ctx->gpr\[1\] \+ " + _OFF + r", ctx->gpr\[(\d+)\]\);\s*$")
_FSAVE = re.compile(r"^\s*\{ uint64_t tmp; memcpy\(&tmp, &ctx->fpr\[(1[4-9]|2[0-9]|3[01])\], 8\); "
                    r"vm_write64\(ctx->gpr\[1\] \+ " + _OFF + r", tmp\); \}\s*$")
_FREST = re.compile(r"^\s*\{ uint64_t tmp = vm_read64\(ctx->gpr\[1\] \+ " + _OFF + r"\); "
                    r"memcpy\(&ctx->fpr\[(1[4-9]|2[0-9]|3[01])\], &tmp, 8\); \}\s*$")
_LRLOAD = re.compile(r"^\s*ctx->gpr\[(\d+)\] = vm_read64\(ctx->gpr\[1\] \+ " + _OFF + r"\);\s*$")
_MFLR = re.compile(r"^\s*ctx->gpr\[(\d+)\] = ctx->lr;\s*$")
_BACKCHAIN = re.compile(r"vm_write64\(ctx->gpr\[1\] \+ " + _OFF + r", ctx->gpr\[1\]\);")
_READ = re.compile(r"vm_read(8|16|32|64)\w*\(ctx->gpr\[1\] \+ " + _OFF + r"\)")
_WRITE = re.compile(r"vm_write(8|16|32|64)\w*\(ctx->gpr\[1\] \+ " + _OFF + r",")
_ADDR = re.compile(r"ctx->gpr\[\d+\] = ctx->gpr\[1\] \+ \(int64_t\)\(" + _OFF + r"\);")
_ADDIC = re.compile(r"\{ uint64_t _a = ctx->gpr\[1\]; uint64_t _r = _a \+ \(uint64_t\)\(int64_t\)\(" + _OFF + r"\); "
                    r"ctx->gpr\[\d+\] = _r; [^}]*\}")
_R1_ADD = re.compile(r"ctx->gpr\[1\] \+= " + _OFF + r";")
_R1_ADD2 = re.compile(r"ctx->gpr\[1\] = ctx->gpr\[1\] \+ \(int64_t\)\(" + _OFF + r"\);")
_R1_DEF = re.compile(r"ctx->gpr\[1\]\s*(?:[-+*/|&^%]|<<|>>)?=(?!=)")
_ANY_DEF = re.compile(r"ctx->gpr\[(\d+)\]\s*(?:[-+*/|&^%]|<<|>>)?=(?!=)")
_FPR_DEF = re.compile(r"ctx->fpr\[(\d+)\]\s*(?:[-+*/])?=(?!=)|&ctx->fpr\[(\d+)\]")
_VMX_R1 = re.compile(r"\(\(?ctx->gpr\[\d+\] \+ ctx->gpr\[\d+\]\)?\)?(?: & ~0xFULL)?; (?:vm_raw_(?:ld|st)16|vmx_lv[lr]x)|"
                     r"VM_RAW_ST")
_CS_DECL = re.compile(r"^\s*uint64_t _cs_(\d+) = ctx->gpr\[\1\];")
_LABEL = re.compile(r"^\s*(loc_[0-9A-Fa-f]+):")
_GOTO = re.compile(r"goto (loc_[0-9A-Fa-f]+);")
_RETURN = re.compile(r"\breturn;")
_TRAMP = re.compile(r"\{ g_trampoline_fn = \(void\(\*\)\(void\*\)\)(\w+); return; \}")
_DIRECT = re.compile(r"\b(\w*func_[0-9A-Fa-f]+)\(ctx\)")
SIZE = {"8": 1, "16": 2, "32": 4, "64": 8}
LR_SLOT = 0x10          # the caller's frame header holds the saved LR there, from the entry r1

# In the generated header: a store to the frame's own header.
HEADER = """\
/* --dead-saves: the back chain and the saved LR, the frame's own header. No SPU or other thread
 * holds those lines and r1 is always mapped, so neither the range compare nor the reserved-line
 * test applies. */
#ifdef PPU_MM_LEAN
#define vm_write64_frame(a, v) (*(volatile vm_u64u*)(_mm.vb + (uint32_t)(a)) = __builtin_bswap64(v))
#else
#define vm_write64_frame(a, v) vm_write64(a, v)
#endif
"""


def _num(s):
    return int(s, 0)


class Frame:
    """What one function does with its stack: r1 along the control flow, and every use of it."""

    def __init__(self, name, lines):
        self.name = name
        self.lines = lines
        self.saves = []             # (line, canonical offset, register)
        self.fsaves = []            # (line, canonical offset, register)
        self.frest = {}             # canonical offset -> [(line, register)]
        self.reads = []             # (line, canonical offset, size)
        self.writes = []            # (line, canonical offset, size)
        self.addrs = set()          # canonical offsets whose address is taken
        self.calls = []             # (delta of r1 at the call, callee)
        self.unsafe = None          # why this function keeps everything
        self.above = set()          # (offset, size) read at or above the entry r1, callees included
        self.cs = {int(m.group(1)) for m in map(_CS_DECL.match, lines) if m}
        self.first_def = {}
        self.first_fdef = {}
        self.lr_save = None         # (line, canonical offset) of the saved LR
        for i, l in enumerate(lines):
            if _CS_DECL.match(l):
                continue
            for m in _ANY_DEF.finditer(l):
                self.first_def.setdefault(int(m.group(1)), i)
            if not _FSAVE.match(l):
                for m in _FPR_DEF.finditer(l):
                    self.first_fdef.setdefault(int(m.group(1) or m.group(2)), i)
        self._flow()
        if self.unsafe is None:
            self._uses()
        if self.unsafe is None:
            self._find_lr()

    def _flow(self):
        lines = self.lines
        n = len(lines)
        labels = {}
        for i, l in enumerate(lines):
            m = _LABEL.match(l)
            if m:
                labels[m.group(1)] = i
        succ = [[] for _ in range(n)]
        for i, l in enumerate(lines):
            s = l.strip()
            for g in _GOTO.findall(s):
                if g in labels:
                    succ[i].append(labels[g])
            tramp = _TRAMP.search(s) is not None
            cond = s.startswith(("if", "switch"))
            ends = s.startswith("goto ") or (not cond and (_RETURN.search(s) is not None or tramp))
            if s.startswith("switch"):
                ends = True
            if not ends and i + 1 < n:
                succ[i].append(i + 1)
        self.delta = delta = [None] * n
        if not n:
            return
        delta[0] = 0
        work = [0]
        while work:
            i = work.pop()
            d = delta[i]
            s = lines[i]
            add = _R1_ADD.search(s)
            add2 = _R1_ADD2.search(s)
            if add:
                d = d + _num(add.group(1))
            elif add2:
                d = d + _num(add2.group(1))
            elif _R1_DEF.search(s):
                self.unsafe = "r1 set some other way"
                return
            for j in succ[i]:
                if delta[j] is None:
                    delta[j] = d
                    work.append(j)
                elif delta[j] != d:
                    self.unsafe = "r1 differs where paths meet"
                    return

    def _uses(self):
        for i, l in enumerate(self.lines):
            if "ctx->gpr[1]" not in l and "func_" not in l:
                continue
            d = self.delta[i]
            if d is None:
                continue                # unreachable text
            t = l
            sm = _SAVE.match(t)
            if sm:
                reg = int(sm.group(2))
                if reg in self.cs or reg == 2:
                    self.saves.append((i, _num(sm.group(1)) + d, reg))
            fs = _FSAVE.match(t)
            if fs:
                self.fsaves.append((i, _num(fs.group(2)) + d, int(fs.group(1))))
            fr = _FREST.match(t)
            if fr:
                self.frest.setdefault(_num(fr.group(1)) + d, []).append((i, int(fr.group(2))))
            for m in _READ.finditer(t):
                self.reads.append((i, _num(m.group(2)) + d, SIZE[m.group(1)]))
            for m in _WRITE.finditer(t):
                self.writes.append((i, _num(m.group(2)) + d, SIZE[m.group(1)]))
            for m in _ADDR.finditer(t):
                self.addrs.add(_num(m.group(1)) + d)
            for m in _ADDIC.finditer(t):
                self.addrs.add(_num(m.group(1)) + d)
            for m in _TRAMP.finditer(t):
                self.calls.append((d, m.group(1)))
            for m in _DIRECT.finditer(t):
                self.calls.append((d, m.group(1)))
            # What is left of r1 in this line must be a form understood above.
            r = _BACKCHAIN.sub("", t)
            r = _READ.sub("", r)
            r = _WRITE.sub("", r)
            r = _ADDR.sub("", r)
            r = _ADDIC.sub("", r)
            r = _R1_ADD.sub("", r)
            r = _R1_ADD2.sub("", r)
            if "ctx->gpr[1]" in r and not _VMX_R1.search(r):
                self.unsafe = "r1 used in another form"
                self.saves = []
                self.fsaves = []
                return

    def _find_lr(self):
        """The prologue's mflr and its save, in the entry block, before anything writes LR."""
        reg = None
        for i, l in enumerate(self.lines):
            if _LABEL.match(l) or "ctx->lr =" in l or _DIRECT.search(l) or "goto " in l or "return;" in l:
                if "ctx->lr =" in l or _LABEL.match(l) or reg is None or "return;" in l or "goto " in l:
                    return
            m = _MFLR.match(l)
            if m and reg is None:
                reg = int(m.group(1))
                continue
            if reg is None:
                continue
            sm = _SAVE.match(l)
            if sm and int(sm.group(2)) == reg:
                c = _num(sm.group(1)) + self.delta[i]
                if c == LR_SLOT:
                    self.lr_save = (i, c)
                return
            for m in _ANY_DEF.finditer(l):
                if int(m.group(1)) == reg:
                    return


class Program:
    def __init__(self, funcs):
        self.frames = {name: Frame(name, lines) for name, lines in funcs}
        for f in self.frames.values():
            f.above = {(o, s) for _i, o, s in f.reads if o >= 0}

    def solve(self):
        callers = {}
        for name, f in self.frames.items():
            for _d, t in f.calls:
                callers.setdefault(t, set()).add(name)
        work = list(self.frames)
        pending = set(work)
        while work:
            n = work.pop()
            pending.discard(n)
            f = self.frames[n]
            add = set()
            for d, t in f.calls:
                g = self.frames.get(t)
                if g is None:
                    continue
                for o, s in g.above:
                    c = d + o
                    if c >= 0 and (c, s) not in f.above:
                        add.add((c, s))
            if add:
                f.above |= add
                for c in callers.get(n, ()):
                    if c not in pending:
                        pending.add(c)
                        work.append(c)

    def _clear(self, f, i, c, allowed_reads=(), allowed_writes=()):
        """None if the slot at c is the save's alone, else why not."""
        if f.unsafe:
            return f.unsafe
        lo, hi = c, c + 8
        for j, o, s in f.reads:
            if j not in allowed_reads and o < hi and lo < o + s:
                return "the slot is read"
        for j, o, s in f.writes:
            if j != i and j not in allowed_writes and o < hi and lo < o + s:
                return "the slot is written again"
        for o in f.addrs:
            if lo - 7 <= o < hi:
                return "the slot's address is taken"
        for d, t in f.calls:
            g = self.frames.get(t)
            if g is None:
                continue
            for o, s in g.above:
                if d + o < hi and lo < d + o + s:
                    return "a callee reads it"
        return None

    def gpr_saves(self, name, why=None):
        """The lines of `name` holding a save of r14-r31 or r2 that can go."""
        f = self.frames[name]
        out = []
        toc = {i for i, _c, reg in f.saves if reg == 2}      # r2 is saved again before each indirect call
        for i, c, reg in f.saves:
            r = None
            if reg != 2 and i > f.first_def.get(reg, 1 << 30):
                r = "register written before the save"
            r = r or self._clear(f, i, c, allowed_writes=toc if reg == 2 else ())
            if r is None:
                out.append(i)
            elif why is not None:
                why[r] += 1
        return out

    def fpr_saves(self, name, why=None):
        """(save line, register, restore lines) for each f14-f31 save whose reloads can take a copy."""
        f = self.frames[name]
        out = []
        for i, c, reg in f.fsaves:
            rest = [j for j, r in f.frest.get(c, []) if r == reg]
            r = None
            if not rest:
                r = "fpr: no reload of it here"
            elif i > f.first_fdef.get(reg, 1 << 30):
                r = "fpr: written before the save"
            elif any(r2 != reg for _j, r2 in f.frest.get(c, [])):
                r = "fpr: reloaded into another register"
            r = r or self._clear(f, i, c, allowed_reads=set(rest))
            if r is None:
                out.append((i, reg, rest))
            elif why is not None:
                why[r] += 1
        return out

    def lr_reloads(self, name):
        """The lines that reload the saved LR, if every read of the slot is such a reload."""
        f = self.frames[name]
        if f.unsafe or f.lr_save is None:
            return []
        i, c = f.lr_save
        out = []
        for j, o, s in f.reads:
            if o < c + 8 and c < o + s:
                if s != 8 or o != c or not _LRLOAD.match(f.lines[j]):
                    return []
                out.append(j)
        for j, o, s in f.writes:
            if j != i and o < c + 8 and c < o + s:
                return []
        return out


def apply(funcs, mode, tails=None):
    """funcs: objects with .name and .body_lines; mode "on" or "poison"; tails: name -> the line a body
    without a terminator falls through to, which is a tail call too. Rewrites .body_lines.
    Returns (counts, why it kept the others)."""
    tails = tails or {}
    p = Program([(f.name, f.body_lines + ([tails[f.name]] if f.name in tails else [])) for f in funcs])
    p.solve()
    why = Counter()
    n = Counter()
    for f in funcs:
        fr = p.frames[f.name]
        lines = f.body_lines
        decls = []
        for i in p.gpr_saves(f.name, why):
            m = _SAVE.match(lines[i])
            lines[i] = f"    vm_write64(ctx->gpr[1] + {m.group(1)}, {POISON});" if mode == "poison" else "    /* dead save */;"
            n["gpr"] += 1
        for i, reg, rest in p.fpr_saves(f.name, why):
            m = _FSAVE.match(lines[i])
            lines[i] = (f"    vm_write64(ctx->gpr[1] + {m.group(2)}, {POISON});" if mode == "poison"
                        else "    /* dead save */;")
            for j in rest:
                lines[j] = f"    memcpy(&ctx->fpr[{reg}], &_fs_{reg}, 8);"
            decl = f"    uint64_t _fs_{reg}; memcpy(&_fs_{reg}, &ctx->fpr[{reg}], 8);"
            if decl not in decls:
                decls.append(decl)
            n["fpr"] += 1
        reloads = p.lr_reloads(f.name)
        if reloads:
            for j in reloads:
                m = _LRLOAD.match(lines[j])
                lines[j] = f"    ctx->gpr[{m.group(1)}] = _cs_lr;"
            decls.append("    uint64_t _cs_lr = ctx->lr;")
            n["lr reloads"] += len(reloads)
        if fr.lr_save is not None and fr.unsafe is None:
            i, _c = fr.lr_save
            lines[i] = lines[i].replace("vm_write64(", "vm_write64_frame(", 1)
            n["lr store"] += 1
        for i, l in enumerate(lines):
            if _BACKCHAIN.search(l):
                lines[i] = l.replace("vm_write64(", "vm_write64_frame(", 1)
                n["back chain"] += 1
        if decls:
            f.body_lines = decls + lines
    return n, why
