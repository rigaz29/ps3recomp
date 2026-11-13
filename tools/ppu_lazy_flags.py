"""Lazy CR and XER[CA] for lifted PPU functions (ppu_lifter.py --lazy-flags).

The lifter writes every compare as a read-modify-write of ctx->cr and every carry as one of ctx->xer:
one statement in four of a lifted function, and nearly all of it has no reader. ctx is shared with
every callee, so the compiler cannot drop any of it. This pass moves what the PPC ABI makes volatile
into locals of the function, where it can:
  - CR0, CR1, CR5, CR6 and CR7;
  - XER[CA], in a local that holds it at bit 29.
A compare whose result only a branch reads becomes a compare and a branch.

CR2, CR3 and CR4 are non-volatile: a callee saves them with mfcr, so they stay in ctx->cr.

What it takes for granted is the ABI: at a call, a return and an indirect tail call the volatile
fields and CA are dead. A direct tail call can go to a fragment of the same function, so what the
target reads before writing it goes back to ctx first, along every such exit (Program.solve). A
function that holds a form this pass does not know stays as it was.

Over God of War: Ascension's 22,320 lifted functions: no form unknown, no CA read at entry or after a
call, and the volatile CR reads at entry are data decoded as branches (a float at a function's start).

--lazy-flags=check keeps every ctx->cr and ctx->xer access as it was, so the game runs as it would
without this pass, and keeps the locals beside them, as they would be. Each read compares the two
(PPU_LF_CK and friends in the generated code): what differs is something the pass would break.
"""
import re

VOLATILE_FIELDS = (0, 1, 5, 6, 7)
VOL_MASK = 0                # the volatile fields' bits of the 32-bit CR word
for _f in VOLATILE_FIELDS:
    VOL_MASK |= 0xF << (28 - 4 * _f)
NV_MASK = 0xFFFFFFFF & ~VOL_MASK
CA_BIT = 1 << 32            # beside them in a uses or defs mask
FLAGS = VOL_MASK | CA_BIT


def field_of(k):
    """Word bit k's CR field, and its bit in the field (3 LT, 2 GT, 1 EQ, 0 SO)."""
    return 7 - k // 4, k % 4


def is_vol(k):
    return bool(VOL_MASK >> k & 1)


# ---- the forms the lifter writes ----------------------------------------------------------------
_W_FIELD = re.compile(r"ctx->cr = \(ctx->cr & ~\(0xFu << (\d+)\)\) \| \((cr_val|_f|field) << \1\);")
_W_FIXED = (
    # (form, field, value)
    (re.compile(r"ctx->cr = \(ctx->cr & ~\(0xFu << 28\)\) \| \(_sc \? \(2u << 28\) : 0u\) \| "
                r"\(\(ctx->xer >> 31\) << 28\);"), 0, "(_sc ? 2u : 0u) | (ctx->xer >> 31)"),
    (re.compile(r"ctx->cr = \(ctx->cr & 0x0FFFFFFFu\) \| \(_c << 28\);"), 0, "_c"),
    (re.compile(r"ctx->cr = \(ctx->cr & ~0x0F000000u\) \| \(\(ctx->fpscr >> 4\) & 0x0F000000u\);"),
     1, "(ctx->fpscr >> 28) & 0xF"),
    (re.compile(r"ctx->cr = \(ctx->cr & ~0xF0u\) \| \(_c6 << 4\);"), 6, "_c6"),
    (re.compile(r"ctx->cr=\(ctx->cr & ~\(0xFu<<4\)\)\|\(c6<<4\);"), 6, "c6"),
    (re.compile(r"ctx->cr=\(ctx->cr & ~\(0xFu<<4\)\)\|\(\(\(d\[0\]\|d\[1\]\|d\[2\]\|d\[3\]\)==0\?2u:0u\)<<4\);"),
     6, "((d[0]|d[1]|d[2]|d[3])==0?2u:0u)"),
)
_W_BIT = re.compile(r"ctx->cr = \(ctx->cr & ~\(1u << \(31 - (\d+)\)\)\) \| \((.+?) << \(31 - \1\)\);")
_W_MTCRF = re.compile(r"ctx->cr = \(ctx->cr & 0x([0-9A-F]{8})u\) \| \(\(uint32_t\)ctx->gpr\[(\d+)\] & 0x([0-9A-F]{8})u\);")
_W_MTCR = re.compile(r"ctx->cr = \(uint32_t\)ctx->gpr\[(\d+)\];")

_R_BIT = re.compile(r"\(ctx->cr >> (\d+)\) & 1\b")
_R_BIT31 = re.compile(r"\(ctx->cr >> \(31 - (\d+)\)\) & 1\b")
_R_MASK = re.compile(r"\(ctx->cr >> (\d+)\) & ([248])\b")
_R_FIELD = re.compile(r"\(ctx->cr >> (\d+)\) & 0xF\b")
_R_MFCR = re.compile(r"= ctx->cr( & 0x[0-9A-F]{8}u)?;")
_MASK_BIT = {2: 1, 4: 2, 8: 3}

_CA_W = re.compile(r"ctx->xer = \(ctx->xer & ~\(1u << 29\)\) \| (.+?);")
_CA_SHIFT = re.compile(r"ctx->gpr\[\d+\] = (?:\(uint64_t\))?ppc_(sraw|srad)\(&ctx->xer, (.*?\))\s*;")
_CA_R = re.compile(r"\(ctx->xer >> 29\) & 1\b")
_XER_MT = re.compile(r"ctx->xer = \(uint32_t\)ctx->gpr\[\d+\] & 0xE000007Fu;")
_XER_MF = re.compile(r"= ctx->xer;")

TRAMP = re.compile(r"\{ g_trampoline_fn = \(void\(\*\)\(void\*\)\)(\w+); return; \}")
_LABEL = re.compile(r"^\s*(loc_[0-9A-Fa-f]+):")
_GOTO = re.compile(r"goto (loc_[0-9A-Fa-f]+);")
_CALL = re.compile(r"\b(?:\w*func_[0-9A-Fa-f]+\(ctx\)|ps3_indirect_call(?:_to)?\(|lv2_syscall\(|ps3_hle_call\()")
_RETURN = re.compile(r"\breturn;")


def line_flags(s):
    """(uses, defs) of one line as masks, or None for a form this pass does not know."""
    if "ctx->cr" not in s and "ctx->xer" not in s:
        return 0, 0                 # every form below names one of them; four lines in five name neither
    uses = defs = 0
    t = s
    for m in _W_FIELD.finditer(t):
        defs |= 0xF << int(m.group(1))
    t = _W_FIELD.sub("", t)
    for rx, f, _v in _W_FIXED:
        if rx.search(t):
            defs |= 0xF << (28 - 4 * f)
            t = rx.sub("", t)
    for m in _W_BIT.finditer(t):
        defs |= 1 << (31 - int(m.group(1)))
    t = _W_BIT.sub("", t)
    for m in _W_MTCRF.finditer(t):
        defs |= int(m.group(3), 16)
    t = _W_MTCRF.sub("", t)
    if _W_MTCR.search(t):
        defs |= 0xFFFFFFFF
        t = _W_MTCR.sub("", t)
    for m in _R_BIT31.finditer(t):
        uses |= 1 << (31 - int(m.group(1)))
    t = _R_BIT31.sub("", t)
    for m in _R_MASK.finditer(t):
        uses |= 1 << (int(m.group(1)) + _MASK_BIT[int(m.group(2))])
    t = _R_MASK.sub("", t)
    for m in _R_BIT.finditer(t):
        uses |= 1 << int(m.group(1))
    t = _R_BIT.sub("", t)
    for m in _R_FIELD.finditer(t):
        uses |= 0xF << int(m.group(1))
    t = _R_FIELD.sub("", t)
    m = _R_MFCR.search(t)
    if m:
        uses |= int(m.group(1)[5:-1], 16) if m.group(1) else 0xFFFFFFFF
        t = _R_MFCR.sub("", t)
    if "ctx->cr" in t:
        return None
    if _CA_R.search(t):
        uses |= CA_BIT
    if _CA_W.search(t) or _CA_SHIFT.search(t) or _XER_MT.search(t):
        defs |= CA_BIT
    if _XER_MF.search(t):
        uses |= CA_BIT
    if "&ctx->xer" in _CA_SHIFT.sub("", t):
        return None
    return uses, defs


class Flow:
    """One function's lines as a control-flow graph, for the flags."""

    def __init__(self, name, lines):
        self.name = name
        self.lines = lines
        n = len(lines)
        self.ok = True
        self.use = [0] * n
        self.dfn = [0] * n
        self.call = [False] * n
        self.exits = {}             # line -> the lifted functions a tail call on it can reach
        self.succ = [[] for _ in range(n)]
        labels = {}
        for i, l in enumerate(lines):
            m = _LABEL.match(l)
            if m:
                labels[m.group(1)] = i
        for i, l in enumerate(lines):
            s = l.strip()
            f = line_flags(s)
            if f is None:
                self.ok = False
                return
            self.use[i], self.dfn[i] = f
            self.call[i] = _CALL.search(s) is not None
            for g in _GOTO.findall(s):
                if g in labels:
                    self.succ[i].append(labels[g])
            ts = TRAMP.findall(s)
            if ts:
                self.exits[i] = ts
            cond = s.startswith(("if", "switch"))
            ends = s.startswith("goto ") or (not cond and (_RETURN.search(s) is not None or bool(ts)))
            if s.startswith("switch"):
                ends = True         # its arms and `return` after the braces
            if not ends and i + 1 < n:
                self.succ[i].append(i + 1)
        self.entry = 0

    def live(self, exit_use):
        n = len(self.lines)
        live = [0] * n
        changed = True
        while changed:
            changed = False
            for i in range(n - 1, -1, -1):
                out = 0
                for j in self.succ[i]:
                    out |= live[j]
                for t in self.exits.get(i, ()):
                    out |= exit_use(t)
                x = (out & ~self.dfn[i]) | self.use[i]
                if x != live[i]:
                    live[i] = x
                    changed = True
        self.live_in = live
        return live[0] if n else 0

    def unwritten(self, after_calls=False):
        """Per line, the flags some path reaches it without having written; with after_calls, without
        having written since the last call."""
        n = len(self.lines)
        reach = [None] * n
        if n:
            reach[0] = FLAGS
        work = [0] if n else []
        while work:
            i = work.pop()
            o = reach[i] & ~self.dfn[i]
            if after_calls and self.call[i]:
                o = FLAGS
            for j in self.succ[i]:
                if reach[j] is None or (reach[j] | o) != reach[j]:
                    reach[j] = (reach[j] or 0) | o
                    work.append(j)
        return reach

    def read_after_call(self):
        """Flags read after a call that the function has not written on some path to it: what a
        callee would be handing back. An mfcr is left out: it reads every field."""
        reach = self.unwritten()
        hit = 0
        for i in range(len(self.lines)):
            if self.call[i] and reach[i] is not None:
                out = 0
                for j in self.succ[i]:
                    out |= self.live_in[j]
                hit |= out & reach[i] & FLAGS
        return hit


class Program:
    """Every function's flow. solve() settles what each reads before writing, along tail calls."""

    def __init__(self, funcs):
        self.flows = {name: Flow(name, lines) for name, lines in funcs}
        for f in self.flows.values():
            f.entry = 0

    def exit_use(self, target):
        f = self.flows.get(target)
        if f is None or not f.ok:
            return FLAGS
        return f.entry

    def solve(self):
        callers = {}
        for name, f in self.flows.items():
            if not f.ok:
                continue
            for ts in f.exits.values():
                for t in ts:
                    callers.setdefault(t, set()).add(name)
        work = [n for n, f in self.flows.items() if f.ok]
        seen = set(work)
        while work:
            n = work.pop()
            seen.discard(n)
            f = self.flows[n]
            e = f.live(self.exit_use)
            if e != f.entry:
                f.entry = e
                for c in callers.get(n, ()):
                    if c not in seen:
                        seen.add(c)
                        work.append(c)


# ---- the rewrite --------------------------------------------------------------------------------
def _b(k):
    f, b = field_of(k)
    return "PPU_LF_B(%d, %d)" % (f, b)


def _sub_reads(s):
    s = _R_FIELD.sub(lambda m: "PPU_LF_F(%d)" % (7 - int(m.group(1)) // 4) if is_vol(int(m.group(1))) else m.group(0), s)
    s = _R_MASK.sub(lambda m: "(%s << %d)" % (_b(int(m.group(1)) + _MASK_BIT[int(m.group(2))]), _MASK_BIT[int(m.group(2))])
                    if is_vol(int(m.group(1)) + _MASK_BIT[int(m.group(2))]) else m.group(0), s)
    s = _R_BIT31.sub(lambda m: _b(31 - int(m.group(1))) if is_vol(31 - int(m.group(1))) else m.group(0), s)
    s = _R_BIT.sub(lambda m: _b(int(m.group(1))) if is_vol(int(m.group(1))) else m.group(0), s)
    return s


def _set(f, v):
    return "PPU_LF_SET(%d, %s);" % (f, v)


def _sub_writes(s, keep):
    """The volatile fields' writes go to the locals; with `keep` the exact write stays before them."""
    def both(m, lazy):
        return (m.group(0) + " " + lazy) if keep else lazy

    def w_field(m):
        sh = int(m.group(1))
        return both(m, _set(7 - sh // 4, m.group(2))) if is_vol(sh) else m.group(0)

    def w_bit(m):
        k = 31 - int(m.group(1))
        if not is_vol(k):
            return m.group(0)
        f, b = field_of(k)
        return both(m, "PPU_LF_SETB(%d, %d, %s);" % (f, b, m.group(2)))

    def w_mtcrf(m):
        g, mask = m.group(2), int(m.group(3), 16)
        lazy = " ".join(_set(f, "((uint32_t)ctx->gpr[%s] >> %d) & 0xF" % (g, 28 - 4 * f))
                        for f in VOLATILE_FIELDS if mask >> (28 - 4 * f) & 0xF)
        if keep:
            return (m.group(0) + " " + lazy) if lazy else m.group(0)
        nv = mask & NV_MASK
        cr = ("ctx->cr = (ctx->cr & 0x%08Xu) | ((uint32_t)ctx->gpr[%s] & 0x%08Xu);" % (~nv & 0xFFFFFFFF, g, nv)) if nv else ""
        return (cr + " " + lazy).strip()

    def w_mtcr(m):
        g = m.group(1)
        lazy = " ".join(_set(f, "((uint32_t)ctx->gpr[%s] >> %d) & 0xF" % (g, 28 - 4 * f)) for f in VOLATILE_FIELDS)
        if keep:
            return m.group(0) + " " + lazy
        return "ctx->cr = (ctx->cr & 0x%08Xu) | ((uint32_t)ctx->gpr[%s] & 0x%08Xu); %s" % (VOL_MASK, g, NV_MASK, lazy)

    s = _W_FIELD.sub(w_field, s)
    for rx, f, v in _W_FIXED:
        s = rx.sub(lambda m: both(m, _set(f, v)), s)
    s = _W_BIT.sub(w_bit, s)
    s = _W_MTCRF.sub(w_mtcrf, s)
    s = _W_MTCR.sub(w_mtcr, s)
    return s


def exit_text(target, program):
    """What a tail call to `target` leaves in ctx first."""
    return out_text(program.exit_use(target))


def out_text(mask):
    parts = ["PPU_LF_OUT(%d);" % f for f in VOLATILE_FIELDS if mask >> (28 - 4 * f) & 0xF]
    if mask & CA_BIT:
        parts.append("PPU_LF_OUT_CA();")
    return " ".join(parts)


def _sub_exits(s, program):
    def tramp(m):
        t = exit_text(m.group(1), program)
        return ("{ %s g_trampoline_fn = (void(*)(void*))%s; return; }" % (t, m.group(1))) if t else m.group(0)
    return TRAMP.sub(tramp, s)


def _lazy_line(s, program):
    if "ctx->cr" not in s and "ctx->xer" not in s:
        # nothing below but the tail call matches a line without them, as in line_flags
        return _sub_exits(s, program) if "g_trampoline_fn" in s else s
    s = _sub_reads(s)
    s = _sub_writes(s, False)
    s = _R_MFCR.sub(lambda m: "= PPU_LF_CR()%s;" % (m.group(1) or ""), s)
    s = _CA_W.sub(lambda m: "_cax = %s;" % m.group(1), s)
    s = _CA_R.sub("(_cax >> 29) & 1", s)
    s = s.replace("(&ctx->xer, ", "(&_cax, ")
    if _XER_MT.search(s):
        s += " _cax = ctx->xer;"
    s = _XER_MF.sub("= (ctx->xer & ~(1u << 29)) | (_cax & (1u << 29));", s)
    return _sub_exits(s, program)


def _check_line(s, flow, i, reach, program):
    """The exact line with the locals beside it, and what compares them before it."""
    ind = s[:len(s) - len(s.lstrip())]
    pre = []
    read = flow.use[i]
    for k in range(32):
        if read >> k & 1 and is_vol(k) and not _R_MFCR.search(s):
            f, b = field_of(k)
            pre.append("%sPPU_LF_CK(%d, %d, %d);" % (ind, k, f, b))
    m = _R_MFCR.search(s)
    if m:
        # what the function has written since its last call: the rest of the word is junk by the ABI
        mask = (int(m.group(1)[5:-1], 16) if m.group(1) else 0xFFFFFFFF) & VOL_MASK & ~(reach[i] or 0)
        if mask:
            pre.append("%sPPU_LF_CKM(0x%08Xu);" % (ind, mask))
    if read & CA_BIT:
        pre.append("%sPPU_LF_CK_CA();" % ind)
    s = _sub_writes(s, True)
    s = _CA_W.sub(lambda m: m.group(0) + " _cax = %s;" % m.group(1), s)
    s = _CA_SHIFT.sub(lambda m: "(void)ppc_%s(&_cax, %s; %s" % (m.group(1), m.group(2), m.group(0)), s)
    if _XER_MT.search(s):
        s += " _cax = ctx->xer;"
    return pre, _sub_exits(s, program)


def rewrite(name, program, check):
    """(the function's lines with the flags in locals, True), or (None, False) to leave it alone."""
    flow = program.flows.get(name)
    if flow is None or not flow.ok:
        return None, False
    lines = flow.lines
    if not any(flow.use) and not any(flow.dfn):
        return lines, True
    decl = []
    for f in VOLATILE_FIELDS:
        decl.append(("PPU_LF_IN(%d);" if flow.entry >> (28 - 4 * f) & 0xF else "PPU_LF_ZERO(%d);") % f)
    decl.append("PPU_LF_IN_CA();" if flow.entry & CA_BIT else "PPU_LF_ZERO_CA();")
    out = ["    " + " ".join(decl)]
    if check:
        reach = flow.unwritten(True)
        for i, l in enumerate(lines):
            pre, s = _check_line(l, flow, i, reach, program)
            out += pre
            out.append(s)
    else:
        out += [_lazy_line(l, program) for l in lines]
    return out, True


# The generated code's side of it, for the header's preamble.
HEADER = r"""
/* --lazy-flags (ppu_lazy_flags.py): the volatile CR fields and XER[CA] in locals of each lifted
 * function. PPU_LF_CHECK, --lazy-flags=check: the exact accesses stay, the locals follow them through
 * ctx->cr_lz and ctx->xer_lz, and every read compares the two. */
#ifdef PPU_LF_CHECK
#define PPU_LF_CTX_CR  ctx->cr_lz
#define PPU_LF_CTX_XER ctx->xer_lz
#ifdef __cplusplus
extern "C"
#endif
void ppu_lf_report(const char* fn, int what);
#define PPU_LF_CK(k, f, b) do { if (((ctx->cr >> (k)) & 1) != PPU_LF_B(f, b)) ppu_lf_report(__func__, k); } while (0)
#define PPU_LF_CKM(m) do { if ((ctx->cr ^ PPU_LF_CR()) & (m)) ppu_lf_report(__func__, 40); } while (0)
#define PPU_LF_CK_CA() do { if (((ctx->xer ^ _cax) >> 29) & 1) ppu_lf_report(__func__, 32); } while (0)
#else
#define PPU_LF_CTX_CR  ctx->cr
#define PPU_LF_CTX_XER ctx->xer
#endif
#define PPU_LF_B(f, b) ((_crf##f >> (b)) & 1u)
#define PPU_LF_F(f) _crf##f
#define PPU_LF_SET(f, v) _crf##f = (uint32_t)(v)
#define PPU_LF_SETB(f, b, v) _crf##f = (_crf##f & ~(1u << (b))) | ((uint32_t)(v) << (b))
#define PPU_LF_ZERO(f) uint32_t _crf##f = 0
#define PPU_LF_IN(f) uint32_t _crf##f = (PPU_LF_CTX_CR >> (28 - 4 * (f))) & 0xFu
#define PPU_LF_OUT(f) PPU_LF_CTX_CR = (PPU_LF_CTX_CR & ~(0xFu << (28 - 4 * (f)))) | (_crf##f << (28 - 4 * (f)))
#define PPU_LF_ZERO_CA() uint32_t _cax = 0
#define PPU_LF_IN_CA() uint32_t _cax = PPU_LF_CTX_XER
#define PPU_LF_OUT_CA() PPU_LF_CTX_XER = (PPU_LF_CTX_XER & ~(1u << 29)) | (_cax & (1u << 29))
#define PPU_LF_CR() ((ctx->cr & 0x00FFF000u) | (_crf0 << 28) | (_crf1 << 24) | (_crf5 << 8) | (_crf6 << 4) | _crf7)
"""
