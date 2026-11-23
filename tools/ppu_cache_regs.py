"""Guest registers in C locals for lifted PPU functions (ppu_lifter.py --cache-regs).

Every lifted statement reads and writes the guest registers in the ppu_context the function was handed
(`ctx->gpr[3] = ...`). Unless the context is __restrict (PPU_CTX_RESTRICT), the compiler has to keep that
memory up to date after every statement and read a register back after every guest store, which may point
anywhere, ctx included; with it, LLVM still gives up on a pointer with many uses unless its capture limit is
raised. This pass gives each GPR, FPR and CTR a function uses a C local (`_rg3`, `_rf1`, `_rctr`) and runs
the statements on those, so the compiler sees plain values:

  wt  written through: every statement that writes a register also stores it to ctx (at each way out of the
      statement), so ctx holds its current value at all times, as before. What goes away is reading it
      back: a local is loaded from ctx only where the function first needs it after its entry or after a
      statement that hands ctx out (lazily, at the statement that reads it, or on the edge into a label
      whose code reads it).
  wb  written back: a register lives in its local and goes to ctx only where it may have changed, right
      before a statement that hands ctx out, before every `return` (a trampoline hand-off returns too) and
      at the end of the body. ctx is exact at every call, return and hand-off, which is all a callee, the
      caller, the runtime and the guest ABI see.

A statement that hands ctx itself to anything (a lifted function, ps3_indirect_call, ps3_hle_call,
lv2_syscall, DRAIN_TRAMPOLINE, an insn hook, PPU_RESV_ADDR) runs verbatim, against an up-to-date context,
and every local counts as stale after it. CR and XER stay where --lazy-flags puts them: its macros read and
write ctx->cr and ctx->xer out of this pass's sight. VRs, VSCR, FPSCR and LR stay in ctx.

The states come from a forward analysis over the body (labels and every `goto` give the edges, a line is a
statement: the lifter writes no block over more than one line) and a backward liveness analysis over the
same graph. A body with a goto to a label it does not have is left as it was.

After the register caching of GoW-Ascension-Recomp's lifter (github.com/bc160201983/GoW-Ascension-Recomp,
ps3kit/tools/ppu_lifter.py, _cache_regs; MIT), cut to the registers --lazy-flags leaves.
"""
import re

REF_RE = re.compile(r'\bctx->(?:(gpr|fpr)\[(\d+)\]|(ctr)\b)')
BARE_RE = re.compile(r'\bctx\b(?!\s*->)')
LABEL_RE = re.compile(r'^\s*(loc_[0-9A-Fa-f]+)\s*:(?!:)')
GOTO_RE = re.compile(r'\bgoto\s+(loc_[0-9A-Fa-f]+)\s*;')
RETURN_RE = re.compile(r'\breturn\s*;')
ASSIGN_RE = re.compile(r'\s*(?:[-+*/|&^%]|<<|>>)?=(?!=)|\s*(?:\+\+|--)')
COMMENT_RE = re.compile(r'/\*.*?\*/')
KILL_RE = re.compile(r'^ctx->(?:(gpr|fpr)\[(\d+)\]|(ctr)) = ([^;{}]*);$')
BLOCK_RETURN_RE = re.compile(r'^\{[^{}]*\breturn;\s*\}$')        # { PPU_LF_OUT(0); g_trampoline_fn = f; return; }
ENDS_RETURN_RE = re.compile(r'^(?:ps3_hle_call\([^;]*\);|switch .*\}) return;$')

CTR = 66                                # slot bits: GPR n -> n, FPR n -> 32 + n, CTR 66
ALL = ((1 << 64) - 1) | (1 << CTR)
MODES = ("wt", "wb")


def _slot(m) -> int:
    if m.group(1) == "gpr":
        return int(m.group(2))
    if m.group(1) == "fpr":
        return 32 + int(m.group(2))
    return CTR


def _names(slot: int) -> tuple[str, str, str]:
    """(local name, ctx field, C type) of a slot."""
    if slot < 32:
        return f"_rg{slot}", f"ctx->gpr[{slot}]", "uint64_t"
    if slot < 64:
        return f"_rf{slot - 32}", f"ctx->fpr[{slot - 32}]", "double"
    return "_rctr", "ctx->ctr", "uint64_t"


def _slots(mask: int):
    while mask:
        low = mask & -mask
        yield low.bit_length() - 1
        mask ^= low


def _store(mask: int) -> str:
    return " ".join(f"{_names(s)[1]} = {_names(s)[0]};" for s in _slots(mask))


def _load(mask: int) -> str:
    return " ".join(f"{_names(s)[0]} = {_names(s)[1]};" for s in _slots(mask))


def rewrite(body: list[str], mode: str) -> list[str]:
    """The register-caching rewrite of one function body: the lines between its braces, one statement or
    label each. Returns the new list (the same lines when there is nothing to do).

    Per slot the rewrite tracks a state: L, the local holds the register's value, or C, only ctx does (at
    entry, after a statement that hands ctx out). A line first loads the slots it mentions that are in C,
    then runs on the locals. wt stores what it writes back to ctx at every way out of the line; wb keeps
    what may have changed (dirty) and stores it before a statement that hands ctx out, before every return
    and at the end. A label keeps a slot in L when a predecessor has it in L and the code after the label
    reads it (or, wb, it may be dirty); predecessors that have it in C load it on their edge."""
    if mode not in MODES:
        raise ValueError("cache-regs mode %r" % mode)
    wt_mask, flush_mask = (ALL, 0) if mode == "wt" else (0, ALL)
    n = len(body)
    kind = [""] * n          # "label" | "sync" | "plain"
    uses = [0] * n           # slots read
    refs = [0] * n           # slots mentioned (read or written)
    writes = [0] * n         # slots possibly written (plain lines)
    kills = [0] * n          # slots certainly overwritten before any read
    has_ret = [False] * n
    falls = [True] * n
    gotos: list[list[str]] = [[] for _ in range(n)]
    labels: dict[str, int] = {}
    referenced = 0
    for i, line in enumerate(body):
        code = COMMENT_RE.sub("", line).strip()
        lm = LABEL_RE.match(line)
        if lm:
            kind[i] = "label"
            labels[lm.group(1)] = i
            continue
        gotos[i] = GOTO_RE.findall(code)
        if (code == "return;" or BLOCK_RETURN_RE.match(code) or ENDS_RETURN_RE.match(code)
                or (code.startswith("goto ") and code.count(";") == 1)):
            falls[i] = False
        u = w = 0
        for m in REF_RE.finditer(code):
            s = 1 << _slot(m)
            am = ASSIGN_RE.match(code, m.end())
            if am and am.group(0).strip() == "=":
                w |= s                      # plain assignment target: written, not read
                continue
            u |= s
            if (am or code[m.start() - 1:m.start()] == "&"
                    or code[:m.start()].rstrip().endswith(("++", "--"))):
                w |= s                      # compound / ++ / -- / address taken
        uses[i], writes[i], refs[i] = u, w, u | w
        referenced |= u | w
        km = KILL_RE.match(code)
        if km and not any(_slot(m2) == _slot(km) for m2 in REF_RE.finditer(km.group(4))):
            kills[i] = 1 << _slot(km)
        if BARE_RE.search(code):
            kind[i] = "sync"
        else:
            kind[i] = "plain"
            has_ret[i] = bool(RETURN_RE.search(code))
    if not referenced:
        return list(body)

    # Control-flow edges; -1 is the function's entry. A goto to a label this body does not have would not
    # compile; such a body is left alone.
    preds: list[list[int]] = [[] for _ in range(n)]
    succs: list[list[int]] = [[] for _ in range(n)]
    if n:
        preds[0].append(-1)
    for i in range(n):
        if falls[i] and i + 1 < n:
            succs[i].append(i + 1)
        for g in gotos[i]:
            if g not in labels:
                return list(body)
            succs[i].append(labels[g])
        for t in succs[i]:
            preds[t].append(i)
    end_falls = n == 0 or falls[n - 1]
    flushable = flush_mask & ~wt_mask

    # Backward liveness of the locals: read before being overwritten. A statement that hands ctx out ends
    # every live range (what is read after it is loaded from ctx again).
    live_in = [0] * n
    work = list(range(n))
    inq = [True] * n
    while work:
        i = work.pop()
        inq[i] = False
        out = 0
        for t in succs[i]:
            out |= live_in[t]
        if kind[i] == "label":
            v = out
        elif kind[i] == "sync":
            v = 0
        else:
            v = uses[i] | (out & ~kills[i])
        if v != live_in[i]:
            live_in[i] = v
            for p in preds[i]:
                if p >= 0 and not inq[p]:
                    inq[p] = True
                    work.append(p)

    # Forward, from nothing upwards: the slots whose local is valid (L) and, wb, the ones that may differ
    # from ctx (dirty), on entry to each line.
    l_in = [0] * n
    d_in = [0] * n

    def l_out(i):
        return 0 if i < 0 or kind[i] == "sync" else l_in[i] | refs[i]

    def d_out(i):
        return 0 if i < 0 or kind[i] == "sync" else d_in[i] | (writes[i] & flushable)
    work = list(range(n - 1, -1, -1))
    inq = [True] * n
    while work:
        i = work.pop()
        inq[i] = False
        lu = du = 0
        for p in preds[i]:
            lu |= l_out(p)
            du |= d_out(p)
        lv = (lu & (live_in[i] | du)) if kind[i] == "label" else lu
        if lv != l_in[i] or du != d_in[i]:
            l_in[i], d_in[i] = lv, du
            for t in succs[i]:
                if not inq[t]:
                    inq[t] = True
                    work.append(t)
    end_flush = d_out(n - 1) if n and end_falls else 0

    def rename(m):
        return _names(_slot(m))[0]

    def edge_loads(p, t):
        return _load(l_in[t] & ~l_out(p))

    out_lines = []
    for s in _slots(referenced):
        name, field, ctype = _names(s)
        out_lines.append(f"{ctype} {name} = {field};")
    for i, line in enumerate(body):
        ind = line[:len(line) - len(line.lstrip())]
        if kind[i] == "label":
            if i > 0 and (i - 1) in preds[i]:
                ld = edge_loads(i - 1, i)
                if ld:
                    out_lines.append(f"{ind}{ld}")
            out_lines.append(line)
            continue
        if kind[i] == "sync":
            fl = d_in[i] & flushable
            if fl:
                out_lines.append(f"{ind}{_store(fl)}")
            text = line
            if gotos[i]:
                def _goto_sync(m, i=i):
                    t = labels.get(m.group(1))
                    r = edge_loads(i, t) if t is not None else ""
                    return f"{{ {r} goto {m.group(1)}; }}" if r else m.group(0)
                text = GOTO_RE.sub(_goto_sync, text)
            out_lines.append(text)
            continue
        pre = refs[i] & ~l_in[i]
        if pre:
            out_lines.append(f"{ind}{_load(pre)}")
        wt = _store(writes[i] & wt_mask)
        text = REF_RE.sub(rename, line)
        if gotos[i]:
            def _goto(m, i=i, wt=wt):
                t = labels.get(m.group(1))
                r = (wt + " " + edge_loads(i, t)).strip() if t is not None else wt
                return f"{{ {r} goto {m.group(1)}; }}" if r else m.group(0)
            text = GOTO_RE.sub(_goto, text)
        if has_ret[i]:
            r = (wt + " " + _store((d_in[i] | writes[i]) & flushable)).strip()
            if r:
                text = RETURN_RE.sub(lambda m, r=r: f"{{ {r} return; }}", text)
        out_lines.append(text)
        if falls[i] and wt:
            out_lines.append(f"{ind}{wt}")
    if end_falls and end_flush:
        out_lines.append(_store(end_flush))
    return out_lines
