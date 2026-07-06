#!/usr/bin/env python3
# sw/litmus/port.py — translate a .litmus file to a baremetal C test.
#
# Usage:
#   port.py <input.litmus> <output.c>
#   port.py --batch <dir>      # port every .litmus under <dir> into ported/
#
# Generated tests #include "../harness.h" and provide _init(), so they
# build with the existing sw/ baremetal toolchain.

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path


# ─── .litmus parsing ─────────────────────────────────────────────────────────

class LitmusTest:
    def __init__(self, name: str, nthreads: int,
                 reg_inits: dict[int, dict[str, str]],
                 mem_inits: dict[str, str],
                 locations: list[str],
                 bodies: list[list[str]],
                 predicate: str,
                 predicate_kind: str):
        self.name = name
        self.nthreads = nthreads
        self.reg_inits = reg_inits     # {thread: {reg: literal_str}}
        self.mem_inits = mem_inits     # {loc_name: literal_str}
        self.locations = locations     # ordered, deduped
        self.bodies = bodies           # bodies[t] = [instr, instr, ...]
        self.predicate = predicate
        self.predicate_kind = predicate_kind  # "exists" or "forall"


_NUM_RE = re.compile(r"^-?\d+$|^0x[0-9a-fA-F]+$")


def is_literal(s: str) -> bool:
    return bool(_NUM_RE.match(s.strip()))


# Expand litmus pseudo-mnemonics that aren't real RISC-V instructions.
# Release-store / acquire-load are expressed as fence + plain ld/st.
_PSEUDO_RE = re.compile(
    r"^\s*(l[bhwd]|s[bhwd])\.(aq|rl|aq\.rl|aqrl)\s+(.*)$"
)


def expand_pseudo(instr: str) -> list[str]:
    m = _PSEUDO_RE.match(instr)
    if not m:
        return [instr]
    op, suffix, operands = m.group(1), m.group(2), m.group(3)
    out: list[str] = []
    # Release semantics: previous rw must precede the store
    if op.startswith("s") and "rl" in suffix:
        out.append("fence rw,w")
    out.append(f"{op} {operands}")
    # Acquire semantics: load must precede subsequent rw
    if op.startswith("l") and "aq" in suffix:
        out.append("fence r,rw")
    return out


def parse_litmus(path: Path) -> LitmusTest:
    text = path.read_text()

    # Strip OCaml-style comments `(* ... *)`.  These can be embedded inside
    # thread-table cells and even span multiple lines (mixed-size HAND tests),
    # which would otherwise corrupt the `|` column split.
    text = re.sub(r"\(\*.*?\*\)", "", text, flags=re.DOTALL)

    lines = text.splitlines()

    # 1) Name
    m = re.match(r"\s*(RISCV|RISCV64|RISCV32)\s+(\S+)", lines[0])
    if not m:
        # Non-RISCV file (e.g. tests/to-AArch64/*.litmus).  Use a sentinel
        # ValueError that port_one() can quietly skip.
        raise ValueError(f"{path}: non-RISCV ISA (first line: {lines[0]!r})")
    name = m.group(2)

    # 2) Initial-state block: between '{' and '}'
    try:
        i_open = next(i for i, l in enumerate(lines) if l.strip().startswith("{"))
        i_close = next(i for i, l in enumerate(lines) if i > i_open and l.strip().endswith("}"))
    except StopIteration:
        raise ValueError(f"{path}: missing '{{ ... }}' initial-state block")

    init_text = "\n".join(lines[i_open:i_close + 1])
    init_text = init_text.strip().lstrip("{").rstrip("}")
    reg_inits: dict[int, dict[str, str]] = {}
    mem_inits: dict[str, str] = {}
    locations: list[str] = []

    for stmt in init_text.split(";"):
        stmt = stmt.strip()
        if not stmt:
            continue
        # Skip C-style type declarations (e.g. "uint64_t x", "uint64_t 1:x5")
        # that some SF_THESIS tests include alongside the value bindings.
        if "=" not in stmt:
            continue
        # forms:  i:rN=value     (thread-local register init)
        #         name=value     (memory location init)
        if ":" in stmt and stmt.split(":")[0].strip().isdigit():
            tid_s, rest = stmt.split(":", 1)
            tid = int(tid_s)
            reg, val = [x.strip() for x in rest.split("=", 1)]
            reg_inits.setdefault(tid, {})[reg] = val
            if not is_literal(val):
                if val not in locations:
                    locations.append(val)
        else:
            loc, val = [x.strip() for x in stmt.split("=", 1)]
            mem_inits[loc] = val
            if loc not in locations:
                locations.append(loc)

    # Also discover any location referenced only in mem_inits but not above
    for loc in mem_inits:
        if loc not in locations:
            locations.append(loc)

    # 3) Thread table: from the line after '}' until the predicate keyword
    # (`exists`, `forall`, `final`, or negated `~exists`).  `filter` and
    # `locations` directives sit between the table and the predicate; we
    # capture `filter` (ANDed into the assertion) and ignore `locations`.
    body_lines = []
    filter_expr = ""
    j = i_close + 1
    while j < len(lines):
        l = lines[j].rstrip()
        if re.match(r"\s*(~?\s*exists|forall|final)\b", l):
            break
        if re.match(r"\s*filter\b", l):
            # filter (expr)  — may span multiple lines.
            buf = re.sub(r"^\s*filter\b", "", l).strip()
            while not buf.count("(") <= buf.count(")") or buf == "":
                j += 1
                if j >= len(lines):
                    break
                buf += " " + lines[j].strip()
            filter_expr = buf.strip()
            j += 1
            continue
        if re.match(r"\s*locations\b", l):
            # Skip — possibly multi-line `locations [ ... ]`
            while "]" not in l:
                j += 1
                if j >= len(lines):
                    break
                l = lines[j]
            j += 1
            continue
        if l.strip():
            body_lines.append(l)
        j += 1

    if not body_lines:
        raise ValueError(f"{path}: empty thread body")

    # First non-blank row is the header (P0 | P1 | ...)
    header = body_lines[0]
    headers = [h.strip().rstrip(";").strip() for h in header.split("|")]
    nthreads = len(headers)
    for k, h in enumerate(headers):
        if not re.match(rf"P{k}$", h):
            raise ValueError(f"{path}: unexpected thread header {h!r} (want P{k})")

    bodies: list[list[str]] = [[] for _ in range(nthreads)]
    for row in body_lines[1:]:
        if not row.strip():
            continue
        cols = [c.strip().rstrip(";").strip() for c in row.split("|")]
        if len(cols) != nthreads:
            raise ValueError(f"{path}: row has {len(cols)} cols, want {nthreads}: {row!r}")
        for t, instr in enumerate(cols):
            if instr:
                bodies[t].append(instr)

    # 4) Predicate
    pred_lines = lines[j:]
    if not pred_lines:
        raise ValueError(f"{path}: missing predicate")
    head = re.match(r"\s*(~?\s*exists|forall|final)\b(.*)$", pred_lines[0])
    if not head:
        raise ValueError(f"{path}: predicate header parse failed")
    predicate_kind = re.sub(r"\s+", "", head.group(1))  # "~exists" or "exists"
    # The predicate body may be on the same line as `exists` (most SF_THESIS
    # tests) or on subsequent lines (most diy-generated tests); concatenate.
    predicate = (head.group(2) + " " +
                 " ".join(l.strip() for l in pred_lines[1:])).strip()
    if filter_expr:
        # `filter F` followed by `exists P` becomes the combined predicate
        # `(F) /\ (P)` — failure requires the run to pass the filter AND
        # satisfy the existence/forbidden clause.
        predicate = f"({filter_expr}) /\\ ({predicate})"

    return LitmusTest(name=name, nthreads=nthreads, reg_inits=reg_inits,
                      mem_inits=mem_inits, locations=locations,
                      bodies=bodies, predicate=predicate,
                      predicate_kind=predicate_kind)


# ─── Predicate → C expression ────────────────────────────────────────────────

# Map register references (0:x5) and location references (x) to C lvalues.
def translate_predicate(p: LitmusTest) -> tuple[str, set[tuple[int, str]], set[str]]:
    expr = p.predicate
    needed_regs: set[tuple[int, str]] = set()
    needed_locs: set[str] = set()

    # Replace `i:rN` with `out[i].rN` and record needed regs.
    def reg_sub(m: re.Match) -> str:
        tid, reg = int(m.group(1)), m.group(2)
        needed_regs.add((tid, reg))
        return f"out_p{tid}.{reg}"
    expr = re.sub(r"(\d+)\s*:\s*([xX]\d+|[a-z][a-z0-9]*)", reg_sub, expr)

    # Replace bare location names with ctx.<name>.
    for loc in p.locations:
        # Word boundary; only when not preceded by '.' (which would mean part of
        # something like out_p0.x5 that we already translated).
        pattern = rf"(?<![.\w]){re.escape(loc)}\b"

        def loc_sub(m: re.Match, _loc=loc) -> str:
            needed_locs.add(_loc)
            return f"ctx.{_loc}"
        expr = re.sub(pattern, loc_sub, expr)

    # Logical operators.
    expr = expr.replace("/\\", "&&").replace("\\/", "||")
    expr = re.sub(r"(?<![!<>=])=(?!=)", "==", expr)  # single = → ==
    expr = expr.replace("~", "!")
    # `not` keyword (used in SF_THESIS predicates) → `!`.  Word-boundaried so
    # we don't mangle identifiers that happen to start with "not".
    expr = re.sub(r"\bnot\b", "!", expr)

    return expr.strip(), needed_regs, needed_locs


# ─── Code generation ─────────────────────────────────────────────────────────

C_TEMPLATE = """\
// sw/litmus/ported/{cname}.c — generated by port.py from {src}
//
// Litmus test: {name}
// Predicate ({kind}): {predicate_raw}

#define LITMUS_NTHREADS {nthreads}
#define LITMUS_NRUNS    16

#include "../harness.h"

struct litmus_ctx {{
{ctx_fields}}};

static struct litmus_ctx ctx;

{out_structs}
{thread_fns}
static int check_result(void)
{{
    // Litmus semantics: `exists P` and `~exists P` both flag the predicate P
    // as a *witness* (typically an RVWMO reordering).  For hardware validation
    // on well-behaved memory, we treat any observed witness as a failure — a
    // healthy hardware run produces zero witnesses.  `forall P` is inverted:
    // any run that violates P is the failure.
    return ({predicate_c}) {invert}0;  /* {kind}: {witness_desc} */
}}

static litmus_barrier_t bar;
static volatile int     failures = 0;

int main(int argc, char **argv)
{{
    (void)argc; (void)argv;
    uint64_t hart;
    asm volatile("csrr %0, mhartid" : "=r"(hart));

    if (hart == 0)
        litmus_barrier_init(&bar);

    int64_t sense = 0;
    for (int i = 0; i < LITMUS_NRUNS; ++i) {{
        if (hart == 0) {{
{ctx_reset}        }}
        sense ^= 1;
        litmus_barrier_wait(&bar, sense);

        switch (hart) {{
{dispatch}        }}

        sense ^= 1;
        litmus_barrier_wait(&bar, sense);

        if (hart == 0 && check_result())
            failures++;
    }}

    if (hart == 0)
        litmus_report(failures > 0);
    for (;;)
        asm volatile("wfi");
    return 0;
}}
"""


def gen_thread_fn(p: LitmusTest, tid: int,
                  out_regs: set[str]) -> str:
    inits = p.reg_inits.get(tid, {})
    body = p.bodies[tid]
    name_to_init: list[tuple[str, str]] = []   # (reg, c_init_expr)

    for reg, val in inits.items():
        if is_literal(val):
            init_c = val
        else:
            init_c = f"(int64_t)&ctx.{val}"
        name_to_init.append((reg, init_c))

    # The registers we need to output to the predicate evaluator
    # come from out_regs (set of reg names for this thread).
    declared = {r for r, _ in name_to_init}
    for r in out_regs:
        if r not in declared:
            name_to_init.append((r, "0"))

    bindings = "\n".join(
        f'    register int64_t reg_{r} asm("{r}") = {init};'
        for r, init in name_to_init
    )

    expanded: list[str] = []
    for instr in body:
        expanded.extend(expand_pseudo(instr.strip()))
    asm_body = "\n".join(f'        "{ins}\\n\\t"' for ins in expanded)

    constraints_io = ", ".join(f'"+r"(reg_{r})' for r, _ in name_to_init)
    if not constraints_io:
        constraints_io = '"r"(0)'  # avoid empty constraint list (degenerate)

    saves = "\n".join(f"    out->{r} = reg_{r};" for r in out_regs)

    return f"""
static void p{tid}(p{tid}_out_t *out)
{{
{bindings}
    asm volatile(
{asm_body}
        : {constraints_io}
        :
        : "memory"
    );
{saves}
}}
"""


def loc_type(_name: str) -> str:
    return "volatile int64_t"


def gen_c(p: LitmusTest, src: Path) -> str:
    cname = p.name.replace("+", "_").replace(".", "_").replace("-", "_")

    predicate_c, needed_regs, _needed_locs = translate_predicate(p)

    # Per-thread output register sets (only what the predicate needs)
    per_thread_out: dict[int, set[str]] = {}
    for tid, reg in needed_regs:
        per_thread_out.setdefault(tid, set()).add(reg)

    # Always emit a struct per thread (even if empty) for uniformity
    out_structs_parts = []
    out_decls_parts = []
    for tid in range(p.nthreads):
        regs = per_thread_out.get(tid, set())
        fields = "\n".join(f"    int64_t {r};" for r in sorted(regs)) or "    int _unused;"
        out_structs_parts.append(f"typedef struct {{\n{fields}\n}} p{tid}_out_t;\n")
        out_decls_parts.append(f"static p{tid}_out_t out_p{tid};")
    out_structs = "\n".join(out_structs_parts + out_decls_parts) + "\n"

    # Context fields
    ctx_fields = "".join(f"    {loc_type(loc)} {loc};\n" for loc in p.locations)

    # Reset locations each iteration (mem_inits or 0)
    ctx_reset = ""
    for loc in p.locations:
        v = p.mem_inits.get(loc, "0")
        if not is_literal(v):
            v = "0"
        ctx_reset += f"            ctx.{loc} = {v};\n"
    for tid in range(p.nthreads):
        regs = sorted(per_thread_out.get(tid, set()))
        for r in regs:
            ctx_reset += f"            out_p{tid}.{r} = 0;\n"

    # Per-thread functions
    thread_fns = ""
    for tid in range(p.nthreads):
        thread_fns += gen_thread_fn(p, tid, per_thread_out.get(tid, set()))

    # Dispatch
    dispatch = ""
    for tid in range(p.nthreads):
        dispatch += f"        case {tid}: p{tid}(&out_p{tid}); break;\n"

    # For `exists` / `~exists`: witness-observed → fail (return P != 0).
    # For `forall` / `final`:   witness-violated → fail (return P == 0).
    is_forall = p.predicate_kind in ("forall", "final")
    invert = "==" if is_forall else "!="
    witness_desc = ("fail if any run violates predicate"
                    if is_forall
                    else "fail if predicate observed on any run")

    return C_TEMPLATE.format(
        cname=cname,
        src=src,
        name=p.name,
        nthreads=p.nthreads,
        kind=p.predicate_kind,
        predicate_raw=p.predicate,
        ctx_fields=ctx_fields,
        out_structs=out_structs,
        thread_fns=thread_fns,
        ctx_reset=ctx_reset,
        dispatch=dispatch,
        predicate_c=predicate_c,
        invert=invert,
        witness_desc=witness_desc,
    )


# ─── CLI ─────────────────────────────────────────────────────────────────────

def safe_cname(litmus_name: str) -> str:
    return re.sub(r"[^A-Za-z0-9_]", "_", litmus_name)


def port_one(src: Path, dst: Path) -> None:
    try:
        p = parse_litmus(src)
    except ValueError as e:
        # Non-RISCV files (to-AArch64/) are expected — silently skip them.
        if "non-RISCV ISA" in str(e):
            return
        print(f"SKIP {src}: {e}", file=sys.stderr)
        return
    except Exception as e:
        print(f"SKIP {src}: {e}", file=sys.stderr)
        return
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(gen_c(p, src))
    print(f"PORT {src.name} → {dst}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Port .litmus tests to baremetal C.")
    ap.add_argument("src", help="path to a .litmus file or a directory (with --batch)")
    ap.add_argument("dst", nargs="?", help="output .c file (single-file mode)")
    ap.add_argument("--batch", action="store_true",
                    help="recursively port all .litmus files under src into <port.py-dir>/ported/")
    args = ap.parse_args()

    here = Path(__file__).resolve().parent

    if args.batch:
        src_root = Path(args.src).resolve()
        if not src_root.is_dir():
            ap.error(f"--batch requires a directory, got {src_root}")
        out_dir = here / "ported"
        n = 0
        for f in sorted(src_root.rglob("*.litmus")):
            cname = safe_cname(f.stem)
            port_one(f, out_dir / f"{cname}.c")
            n += 1
        print(f"Ported {n} tests to {out_dir}/")
        return 0

    src = Path(args.src).resolve()
    if not args.dst:
        ap.error("dst required in single-file mode (or pass --batch)")
    port_one(src, Path(args.dst).resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
