#!/usr/bin/env python3
"""Extract gem5's x86 macro-op -> micro-op expansion for etrace.

    gen_gem5_uops.py <gem5 src/arch/x86/isa> > gem5_uops.tbl

gem5's x86 front end expands each instruction according to the microcode in
src/arch/x86/isa/insts/**.py ("def macroop ADDSD_XMM_M { ldfp ...; maddf ...
};"). etrace normally emits one LOAD/STORE per memory operand plus a fixed
number of COMPs, which on PARSEC blackscholes is 33% fewer compute micro-ops
than O3 commits. This table lets etrace emit gem5's micro-ops instead.

One line per macro-op:
    NAME n  u1 u2 ... un
each micro-op is  kind,opclass,deps,flags,memop,offset,size  where
    kind     L load | S store | C compute
    opclass  etrace op-class code (see OPCLASS below)
    deps     '.'-separated indices of earlier micro-ops in this macro-op whose
             temporaries (t*, ufp*) it reads, or '-'
    flags    a = reads the instruction's address registers
             d = reads architectural non-XMM data registers (GPR, flags)
             w = writes architectural non-XMM state (GPR or flags)
             r / R = reads XMM low / high halves (xmml*, xmmh*)
             x / X = writes XMM low / high halves
             f = writes the flags (any group)
             z / c / e = reads gem5's Zaps (ZF AF PF SF) / Cfof (CF OF) /
                 Ecf-Ezf flag register from before this macro-op: a
                 condition testing it, or a partial write that keeps bits
             Z / C / E = writes that flag register (flag dependences inside
                 a macro-op are in deps)
             =name (last) = the architectural register this micro-op writes:
                 an explicit name (rsp, rax, ...) or reg / regm, the
                 instruction's ModRM register / r-m operand
             M = locked load (ldstl): non-speculative in O3 (executes at
                 the ROB head once older stores are written back)
             m = load of a read-modify-write (ldst/ldstl): gem5 sends it
                 with READ_MODIFY_WRITE, which Ruby issues as a store-type
                 (RMW_Read) request
             D = reads its own destination GPR (old value)
             y / Y = reads its own destination XMM low / high half
             (gem5 merge/insert or conditional-keep semantics: O3 waits for
             the destination's previous writer. Real x86 does not --
             movd %xmm0,%eax zero-extends -- so Pin reports no such read;
             without it a blackscholes worker's movd/movaps waited 7 cycles
             in replay against 80 in O3.)
             gem5 renames the two 64-bit halves of an XMM register
             separately: a scalar op or a MOVSD load leaves the high half's
             producer untouched, so its consumers do not wait on the load.
    memop    index of this load/store among the macro-op's loads (or stores)
    offset   byte offset added to the operand address (split accesses)
    size     access size in bytes (0 = the operand's own size)
Micro-code loops (br/label, e.g. DIV) are expanded statically, once.
"""
import glob
import os
import re
import sys

# etrace op-class codes, mirrored in IntervalCPU (and its O3 x86 FU pool).
OPCLASS = {
    "IntAluOp": 0, "IntMultOp": 1, "IntDivOp": 2,
    "FloatAddOp": 3, "FloatCmpOp": 4, "FloatCvtOp": 5, "FloatMultOp": 6,
    "FloatMultAccOp": 7, "FloatMiscOp": 8, "FloatDivOp": 9, "FloatSqrtOp": 10,
}
SIMD = 11            # every Simd*Op: latency 1 on O3's x86 SIMD_Unit
BARRIER = 12         # mfence/lfence/sfence: O3 executes it non-speculatively
                     # at the ROB head; younger memory ops wait for it


def opclass_code(name):
    if name in OPCLASS:
        return OPCLASS[name]
    if name.startswith("Simd"):
        return SIMD
    return 0


def dest_readers(isa):
    """micro-op mnemonic -> 'D' (reads int dest) and/or 'F' (reads fp dest).

    gem5 micro-ops whose code reads the destination they write (merge or
    insert semantics, or a conditional else-path that keeps it), with no
    4/8-byte variant (big_code) or invalidate_srcs that drops it, depend in
    O3 on the destination's previous writer.
    """
    classes = {}
    for f in ("regop.isa", "mediaop.isa", "fpop.isa"):
        p = os.path.join(isa, "microops", f)
        if not os.path.exists(p):
            continue
        s = open(p).read()
        for m in re.finditer(r"^\s*class (\w+)\(([^)]*)\):(.*?)"
                             r"(?=^\s*class |\Z)", s, re.S | re.M):
            name, bases, body = m.groups()

            def grab(attr, body=body):
                q3 = "'" * 3
                pat = (attr + r"\s*=\s*(" + q3 + r"(.*?)" + q3 +
                       r"|'([^'\n]*)'|" + '"' + r'([^"\n]*)' + '")')
                mm = re.search(pat, body, re.S)
                if not mm:
                    return None
                return next(g for g in mm.groups()[1:] if g is not None)
            classes[name] = {"bases": [b.strip() for b in bases.split(",")],
                             "code": grab("code"), "big": grab("big_code"),
                             "else": grab("else_code"),
                             "inv": grab("invalidate_srcs")}

    def attr(n, a, d=0):
        c = classes.get(n)
        if not c or d > 10:
            return None
        if c[a] is not None:
            return c[a]
        for b in c["bases"]:
            v = attr(b, a, d + 1)
            if v is not None:
                return v
        return None
    tok = re.compile(r"\b(FpDestReg\w*|DestReg)\b(\s*=(?!=))?")
    res = {}
    for n in classes:
        code = (attr(n, "code") or "") + "\n" + (attr(n, "else") or "")
        ri = rf = False
        for m in tok.finditer(code):
            if m.group(2):
                continue                    # assignment target
            if m.group(1).startswith("Fp"):
                rf = True
            else:
                ri = True
        if attr(n, "big") or attr(n, "inv"):
            ri = False                      # 4/8-byte form does not read it
        if ri or rf:
            res[n.lower()] = ("D" if ri else "") + ("F" if rf else "")
    return res


def microop_classes(isa):
    """micro-op mnemonic -> op_class, following Python class inheritance."""
    base, oc = {}, {}
    for f in ("fpop.isa", "mediaop.isa", "regop.isa", "limmop.isa",
              "seqop.isa", "specop.isa", "debug.isa"):
        p = os.path.join(isa, "microops", f)
        if not os.path.exists(p):
            continue
        s = open(p).read()
        for m in re.finditer(r"^\s*class (\w+)\((\w+)[^)]*\):(.*?)"
                             r"(?=^\s*class |\Z)", s, re.S | re.M):
            name, b, body = m.groups()
            base[name] = b
            o = re.search(r"op_class\s*=\s*['\"](\w+)['\"]", body)
            if o:
                oc[name] = o.group(1)

    def resolve(n, d=0):
        if n in oc:
            return oc[n]
        if n in base and d < 12:
            return resolve(base[n], d + 1)
        return "IntAluOp"
    table = {n.lower(): resolve(n) for n in base}
    # Built from InstObjParams without an explicit class, so gem5's ISA
    # parser infers one from the operands: a floating-point destination
    # gives FloatAddOp (O3 counts lfpimm, in every MOVSD load, as FloatAdd).
    table["lfpimm"] = "FloatAddOp"
    return table


# micro-ops whose operands are all sources (no register destination)
NODEST = {"wrip", "wripi", "wrflags", "wruflags", "wrdl", "wrdh", "wrbase",
          "wrlimit", "wrattr", "wrsel", "wrcr", "wrdr", "wrval", "wrtsc",
          # first operand is a source (operand_types start with a Src):
          # compares that write the flags in their code
          "mcmpf2rf", "compfp", "chks", "wrxftw"}
# gem5 micro-ops that pass values through implicit registers (ProdLow/ProdHi,
# Quotient/Remainder/Divisor): writers and readers of those pseudo-temps.
IMPLICIT_W = {"mul1s": "__prod", "mul1u": "__prod", "div1": "__div",
              "div2": "__div", "div2i": "__div"}
IMPLICIT_R = {"mulel": "__prod", "muleh": "__prod", "div2": "__div",
              "div2i": "__div", "divq": "__div", "divr": "__div"}


def wrregop_subclasses(isa):
    """Micro-ops derived from WrRegOp: no register destination, the first
    operand is a source (mul1s, div1, wrip, wrbase, ...)."""
    base = {}
    for f in ("regop.isa",):
        s = open(os.path.join(isa, "microops", f)).read()
        for m in re.finditer(r"^\s*class (\w+)\(([^)]*)\):", s, re.M):
            base[m.group(1)] = [b.strip() for b in m.group(2).split(",")]

    def isw(n, d=0):
        if n == "WrRegOp":
            return True
        return d < 10 and any(isw(b, d + 1) for b in base.get(n, []))
    return {n.lower() for n in base if n != "WrRegOp" and isw(n)}


# micro-ops whose code writes the flags without a flags=() list
# (ccFlagBits / cfofBits assigned in mediaop/fpop/regop.isa)
WRREGOPS = set()
IMPLICIT_FLAGW = {"mcmpf2rf", "compfp", "wrflags", "wruflags"}
# Conditional micro-ops (gem5's CondRegOp: Mov, its immediate form movi,
# Wrip/Wripi; Movfp; Br): for them "flags=(...)" is the condition they READ,
# not flags they write. Treating it as a write gave every conditional branch
# no dependence on the compare that sets its condition, so replay resolved
# branches ~100 cycles early (blackscholes worker, aligned O3PipeView).
CONDOPS = {"wrip", "wripi", "br", "mov", "movi", "movfp"}
# operand keywords that look like register names to the ARCH pattern
KEYWORDS = {"seg", "sib", "riprel", "disp", "ss", "ds", "cs", "es", "fs",
            "gs", "imm", "dsz", "asz", "ssz", "osz", "dataSize"}
TEMP = re.compile(r"^(t\d+|ufp\d+|t\d+[bwdq]?)$")
ARCH = re.compile(r"^(reg|regm|xmml|xmmh|xmmlm|xmmhm|mmx|mmxm|"
                  r"r[a-z0-9]+|e[a-z]+|[a-d][xlh]|[sd]il?|[sb]pl?|"
                  r"st\d*|fpr\d+)$")


def is_load(m):
    return m.startswith("ld") or m in ("ldst", "ldstl")


def is_store(m):
    return (m.startswith("st") and m not in ("sti",)) or m == "stupd"


def parse_ops(body):
    out = []
    for raw in body.splitlines():
        line = raw.split("#")[0].strip()
        # directives (.adjust_env, .function_call, ...) are not micro-ops
        # fault "NoFault" is NOP's one micro-op; it commits like any other
        if not line or line.startswith((".", "label", "br ", "panic")):
            continue
        parts = line.split(None, 1)
        mnem = parts[0].lower()
        args = parts[1] if len(parts) > 1 else ""
        out.append((mnem, args))
    return out


def tokens(args):
    # positional args only (drop key=value), split on commas outside quotes
    toks, depth, cur, q = [], 0, "", None
    for ch in args:
        if q:
            cur += ch
            if ch == q:
                q = None
            continue
        if ch in "\"'":
            q = ch
            cur += ch
        elif ch in "([":
            depth += 1
            cur += ch
        elif ch in ")]":
            depth -= 1
            cur += ch
        elif ch == "," and depth == 0:
            toks.append(cur.strip())
            cur = ""
        else:
            cur += ch
    if cur.strip():
        toks.append(cur.strip())
    return [t for t in toks if "=" not in t]


# gem5 x86 condition-code register groups (operands.isa CCRegPred)
ZAPS = {"ZF", "AF", "PF", "SF"}
CFOF = {"CF", "OF"}
EGRP = {"ECF", "EZF", "DF"}
# micro-ops that read flags outside a flags=() condition
FLAGREAD_CF = {"adc", "sbb", "rcl", "rcr", "adci", "sbbi", "rcli", "rcri"}
FLAGREAD_ALL = {"ruflag", "ruflags", "rflag", "rflags", "rdflags"}


def flag_groups(args, mnem):
    """(reads, writes, partial_reads) sets over {'z','c','e'}."""
    m = re.search(r"flags=\(([^)]*)\)", args)
    names = [n.strip() for n in m.group(1).split(",") if n.strip()] if m \
        else []
    reads, writes = set(), set()
    if mnem in CONDOPS:
        # gem5 condition tests (ConditionTests in x86 isa) -> flag groups
        cond_groups = {
            "True": set(), "ECF": {"e"}, "EZF": {"e"}, "SZnZF": {"z"},
            "MSTRZ": {"z", "e"}, "STRZ": {"z", "e"}, "MSTRC": {"c", "e"},
            "STRZnEZF": {"z", "e"}, "OF": {"c"}, "CF": {"c"}, "ZF": {"z"},
            "CvZF": {"c", "z"}, "SF": {"z"}, "PF": {"z"}, "SxOF": {"z", "c"},
            "SxOvZF": {"z", "c"}, "STRnZnEZF": {"z", "e"},
            "MSTRnC": {"c", "e"}, "STRnZ": {"z", "e"}, "MSTRnZ": {"z", "e"},
            "nCEZF": {"c", "e"}, "CEZF": {"c", "e"}, "CECF": {"c", "e"},
        }
        for n in names:
            c = n[1:] if n.startswith("n") else n
            c = c[1:] if c.startswith("C") else c
            if c not in cond_groups and ("n" + c) in cond_groups:
                c = "n" + c
            g = cond_groups.get(c)
            if g is None:
                print(f"[gen_gem5_uops] unknown condition {n} in {mnem}",
                      file=sys.stderr)
                g = {"z", "c", "e"}
            reads |= g
    else:
        got = set(names)
        if got & ZAPS:
            writes.add("z")
            if not ZAPS <= got:
                reads.add("z")          # partial: keeps the other bits
        if got & CFOF:
            writes.add("c")
            if not CFOF <= got:
                reads.add("c")
        if got & EGRP:
            writes.add("e")
    if mnem in IMPLICIT_FLAGW:
        writes |= {"z", "c"}
    if mnem in FLAGREAD_CF:
        reads.add("c")
    if mnem in FLAGREAD_ALL:
        reads |= {"z", "c", "e"}
    return reads, writes


def describe(ops, classes, dreads=None):
    dreads = dreads or {}
    writer = {}            # temp name -> micro-op index
    fwriter = {}           # flag group -> micro-op index (this macro-op)
    nload = nstore = 0
    res = []
    for i, (mnem, args) in enumerate(ops):
        toks = tokens(args)
        flagsw = ("flags=" in args and mnem not in CONDOPS) or \
            mnem in IMPLICIT_FLAGW
        flagsr = "flags=" in args and mnem in CONDOPS
        mem = is_load(mnem) or is_store(mnem)
        kind = "L" if is_load(mnem) else "S" if is_store(mnem) else "C"
        # lea reads the address registers too, without touching memory
        uses_addr = any(k in args for k in ("sib", "riprel", "["))
        # registers: for loads the first operand is the destination; for
        # stores it is the data source; for compute the first is the dest
        regs = [t for t in toks
                if t not in KEYWORDS and (TEMP.match(t) or ARCH.match(t))]
        if kind == "S":
            dest, srcs = None, regs[:1]
        elif mnem in NODEST or mnem in WRREGOPS:
            dest, srcs = None, regs
        elif regs:
            dest, srcs = regs[0], regs[1:]
        else:
            dest, srcs = None, []
        if kind == "C" and mnem in ("rdip", "limm", "lfpimm"):
            srcs = []
        if mnem in IMPLICIT_R:
            srcs = srcs + [IMPLICIT_R[mnem]]
        # riprel addressing reads t7, which the macro-op's rdip wrote
        if "riprel" in args and "t7" in writer:
            srcs = srcs + ["t7"]
        # gem5 reads the destination too (merge / conditional keep)
        dr = dreads.get(mnem, "")
        rd_dst_gpr = rd_dst_lo = rd_dst_hi = False
        if dest is not None and dr:
            if TEMP.match(dest):
                srcs = srcs + [dest]
            elif dest in ("xmml", "xmmlm"):
                rd_dst_lo = "F" in dr
            elif dest in ("xmmh", "xmmhm"):
                rd_dst_hi = "F" in dr
            elif ARCH.match(dest):
                rd_dst_gpr = "D" in dr
        deps = sorted({writer[s] for s in srcs if s in writer})
        frd, fwr = flag_groups(args, mnem)
        ext_frd = set()
        for g in frd:
            if g in fwriter:
                deps = sorted(set(deps) | {fwriter[g]})
            else:
                ext_frd.add(g)
        for g in fwr:
            fwriter[g] = i
        XLO, XHI = ("xmml", "xmmlm"), ("xmmh", "xmmhm")
        reads_arch = flagsr or any(ARCH.match(s) and s not in XLO + XHI
                                   for s in srcs)
        reads_lo = any(s in XLO for s in srcs)
        reads_hi = any(s in XHI for s in srcs)
        writes_lo = dest in XLO
        writes_hi = dest in XHI
        writes_arch = flagsw or (dest is not None and bool(ARCH.match(dest))
                                 and dest not in XLO + XHI)
        wname = ""
        if dest is not None and ARCH.match(dest) and \
                dest not in XLO + XHI and kind != "S":
            wname = dest.lower()
        if dest is not None and TEMP.match(dest):
            writer[dest] = i
        if mnem in IMPLICIT_W:
            writer[IMPLICIT_W[mnem]] = i
        off = 0
        mo = re.search(r"\+\s*(\d+)", args)
        if mem and mo:
            off = int(mo.group(1))
        size = 0
        ms = re.search(r"dataSize\s*=\s*(\d+)", args)
        if mem and ms:
            size = int(ms.group(1))
        memop = nload if kind == "L" else nstore if kind == "S" else 0
        if kind == "L":
            nload += 1
        elif kind == "S":
            nstore += 1
        fl = ("a" if uses_addr else "") + ("d" if reads_arch else "") + \
             ("w" if writes_arch else "") + ("r" if reads_lo else "") + \
             ("R" if reads_hi else "") + ("x" if writes_lo else "") + \
             ("X" if writes_hi else "") + ("D" if rd_dst_gpr else "") + \
             ("y" if rd_dst_lo else "") + ("Y" if rd_dst_hi else "") + \
             ("m" if mnem in ("ldst", "ldstl") else "") + \
             ("M" if mnem == "ldstl" else "") + \
             ("f" if flagsw else "") + \
             "".join(sorted(ext_frd)) + "".join(sorted(g.upper()
                                                     for g in fwr)) + \
             (("=" + wname) if wname else "")
        oc = 0 if mem else opclass_code(classes.get(mnem, "IntAluOp"))
        if mnem in ("mfence", "lfence", "sfence"):
            oc = BARRIER
        res.append(f"{kind},{oc},{'.'.join(map(str, deps)) or '-'},"
                   f"{fl or '-'},{memop},{off},{size}")
    return res


def main():
    isa = sys.argv[1]
    classes = microop_classes(isa)
    dreads = dest_readers(isa)
    global WRREGOPS
    WRREGOPS = wrregop_subclasses(isa)
    n = 0
    for f in sorted(glob.glob(os.path.join(isa, "insts", "**", "*.py"),
                              recursive=True)):
        s = open(f).read()
        for m in re.finditer(r"def macroop (\w+)\s*\{(.*?)\};", s, re.S):
            name, body = m.groups()
            ops = parse_ops(body)
            if not ops:
                continue
            u = describe(ops, classes, dreads)
            print(name, len(u), " ".join(u))
            n += 1
    # Integer DIV/IDIV are generated from a %-template with a data-dependent
    # microcode loop (div1, div2i, div2, then 4x div2 + br per iteration until
    # the dividend is consumed, then divq, divr), so they are not in the
    # parsed text. Emit them with one loop iteration: 10 micro-ops, which is
    # the measured average on synth fp (O3 commits 8.9 more micro-ops per
    # divide than the plain one-COMP cracking).
    def div_entry(name, mem, rip):
        u = []
        if rip:
            u.append("C,0,-,-,0,0,0")                      # rdip t7
        if mem:
            u.append(f"L,0,{len(u) - 1 if rip else '-'},a,0,0,0")
        first = len(u)
        dep0 = str(first - 1) if mem else "-"
        u.append(f"C,2,{dep0},d,0,0,0")                     # div1
        for _ in range(6):                                   # div2i + 5 div2
            u.append(f"C,2,{len(u) - 1},-,0,0,0")
        u.append(f"C,0,{len(u) - 1},-,0,0,0")              # br
        u.append(f"C,2,{len(u) - 1},w,0,0,0")              # divq
        u.append(f"C,2,{len(u) - 2},w,0,0,0")              # divr
        print(name, len(u), " ".join(u))
    for op in ("DIV", "IDIV"):
        for b in ("", "_B"):
            for form, mem, rip in (("R", False, False), ("M", True, False),
                                   ("P", True, True)):
                div_entry(f"{op}{b}_{form}", mem, rip)
                n += 1
    print(f"[gen_gem5_uops] {n} macro-ops", file=sys.stderr)


if __name__ == "__main__":
    main()
