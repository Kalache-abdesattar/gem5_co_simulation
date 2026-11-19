/*
 * etrace.cpp -- Pin tool: emit an Elastic-Traces-shaped dependency stream.
 *
 * Produces, per thread, the graph half of gem5's elastic trace:
 *   seq_num, pc, type (LOAD/STORE/COMP), vaddr, size, register dependencies
 *   (RAW, by producer seq_num) and order dependencies (store->access on the
 *   same line).
 *
 * It does NOT produce comp_delay. That is derived in gem5 from O3 pipeline
 * timestamps (executeTick / toCommitTick / commitTick); Pin has no pipeline
 * and no cycles, so there is nothing to derive it from. The converter
 * synthesises it, and the result must be described as a synthetic in-order
 * timing model -- not as Elastic Traces.
 *
 * Output is text; etrace_to_elastic.py turns it into the two protobuf files,
 * because a pintool cannot link libprotobuf (Pin CRT is -nostdlib).
 *
 * Record: S <seq> <pc> <type> <vaddr> <size> <nreg> [regdeps...] <norder> [orderdeps...]
 */

#include "pin.H"
#if defined(SDE_INIT)
/* Built with makefile.sde to run under `sde64 -t64 ... -replay`: SDE's
 * driver passes its own switches (-work-dir, -replay:*) to the tool, which
 * only parse once sde_pin_init() has registered them. */
#  include "sde-init.H"
#  include "sde-pinplay-supp.H"
#endif
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <vector>

using std::string;
using std::vector;

/* SDE already owns -o (its own output file), so the SDE build renames it. */
#if defined(SDE_INIT)
#  define ETRACE_OUT_KNOB "etrace_out"
#else
#  define ETRACE_OUT_KNOB "o"
#endif
KNOB<string> KnobOut(KNOB_MODE_WRITEONCE, "pintool", ETRACE_OUT_KNOB, "etrace",
                     "output prefix; files are <prefix>.tidN.txt");
KNOB<UINT64> KnobMax(KNOB_MODE_WRITEONCE, "pintool", "max", "2000000",
                     "stop after N emitted RECORDS per thread (0 = unlimited). "
                     "A memory instruction emits one record per operand, so "
                     "records != instructions; use -maxinsts to bound by "
                     "instructions when comparing against a gem5 capture.");
KNOB<UINT64> KnobMaxInsts(KNOB_MODE_WRITEONCE, "pintool", "maxinsts", "0",
                     "stop after N instructions per thread (0 = unlimited)");
KNOB<UINT32> KnobLine(KNOB_MODE_WRITEONCE, "pintool", "line", "64",
                      "line size for order dependencies");
KNOB<BOOL>   KnobCrack(KNOB_MODE_WRITEONCE, "pintool", "crack", "1",
                     "split each x86 instruction into gem5-shaped micro-ops. "
                     "gem5's elastic traces record micro-ops (1.44 per "
                     "instruction measured), so without this the dependency "
                     "graph has ~1.44x too few nodes and replay runs ~50% "
                     "fast no matter what comp_delay is used. 0 = one record "
                     "per instruction, the original behaviour.");
KNOB<BOOL>   KnobRoi(KNOB_MODE_WRITEONCE, "pintool", "roi", "0",
                     "only record inside the ROI");
KNOB<string> KnobRoiBeg(KNOB_MODE_WRITEONCE, "pintool", "roi_begin",
                        "__parsec_roi_begin", "ROI open symbol");
KNOB<string> KnobRoiEnd(KNOB_MODE_WRITEONCE, "pintool", "roi_end",
                        "__parsec_roi_end", "ROI close symbol");
/* gem5's m5_work_begin emits opcode 0F 04, which is an illegal instruction on
 * real x86 -- a gem5-marked binary would SIGILL under Pin. The Pin path marks
 * regions with ordinary no-op functions hooked by name instead. */
KNOB<string> KnobSyncBeg(KNOB_MODE_WRITEONCE, "pintool", "sync_begin",
                         "__trace_sync_begin",
                         "symbol marking a region begin; arg0 = type "
                         "(0 parallel, 1 critical, 2 barrier)");
KNOB<string> KnobSyncEnd(KNOB_MODE_WRITEONCE, "pintool", "sync_end",
                         "__trace_sync_end", "symbol marking a region end");
/* Derive sync regions from the pthread calls themselves, so an UNMODIFIED
 * binary (PARSEC, say) yields a synchronisation trace:
 *   pthread_mutex_lock/unlock  -> critical section begin/end
 *   pthread_barrier_wait       -> barrier, bracketed before and after
 * This is the faithful mapping: a mutex region IS a critical section and a
 * barrier wait IS a barrier. */
KNOB<BOOL> KnobAutoSync(KNOB_MODE_WRITEONCE, "pintool", "autosync", "0",
                        "derive sync markers from pthread calls");
/* Barriers a program implements itself (PARSEC's parsec_barrier_wait: a
 * mutex + condition variable, optionally spinning first). Hooked like
 * pthread_barrier_wait. Inside one, a thread's instructions and mutex calls
 * are not recorded: the spin and the barrier's own lock are waiting, which
 * the replay's barrier models, not work. Comma-separated symbol names as the
 * image lists them (mangled for C++). Needs -autosync. */
KNOB<string> KnobBarrierFn(KNOB_MODE_WRITEONCE, "pintool", "barrier_fn", "",
                           "functions to treat as barrier waits");
/* Pin lists RIP among the registers a branch writes and a branch or a
 * RIP-relative access reads, so tracking it makes every branch depend on the
 * previous one. In a loop that is a carried chain through the branch's three
 * cracked micro-ops: measured on synth l1, 3 cycles per 12-micro-op iteration
 * (dependency bound 4.04 micro-ops/cycle) against gem5's own trace at 12.2,
 * whose bound is the loop counter. gem5 does not rename RIP: rdip/limm
 * micro-ops read it with no register dependency. */
/* gem5's x86 micro-op dependency shape for memory instructions:
 *   add ecx,[rdx]  ->  ld t1,[rdx] (rdx)   add ecx,ecx,t1 (ecx, ld)
 *   mov [rdx],ecx  ->  st ecx,[rdx] (rdx, ecx)              one micro-op
 *   add [rdx],ecx  ->  ld (rdx)  add (ecx, ld)  st (rdx, add)
 * The original cracking gave the data registers to nobody: the STORE took
 * only the address, so it issued before its value existed, no load ever
 * overtook it, and O3's store-set predictor (which holds O3 to 3.0
 * micro-ops/cycle on synth l1) could never train in a replay. */
KNOB<BOOL> KnobGem5Uops(KNOB_MODE_WRITEONCE, "pintool", "gem5uops", "1",
                        "crack memory instructions with gem5's dependency "
                        "shape (loads, operation, stores; data registers "
                        "on the operation or store). 0 = original cracking.");
/* gem5's own x86 micro-op expansion, generated from its microcode by
 * gen_gem5_uops.py. On PARSEC blackscholes the plain cracking emitted 33%
 * fewer compute micro-ops than O3 commits, and gave every one the latency of
 * an integer add. With a table, each instruction whose gem5 macro-op name can
 * be formed from XED (mnemonic + operand forms R/M/P/I/XMM, e.g.
 * ADDSD_XMM_P) is emitted exactly as gem5 expands it, with each compute
 * micro-op's op class; the rest fall back to -gem5uops. */
KNOB<string> KnobUopTable(KNOB_MODE_WRITEONCE, "pintool", "uoptable", "",
                          "gem5_uops.tbl from gen_gem5_uops.py (empty = off)");
KNOB<BOOL> KnobRipDeps(KNOB_MODE_WRITEONCE, "pintool", "ripdeps", "0",
                       "track dependencies through RIP (the original "
                       "behaviour). 0 = ignore RIP, as gem5 does.");

enum RType { COMP = 3, LOAD = 1, STORE = 2 };

/* Per-instruction static descriptor, built once at JIT time. */
struct InstDesc
{
    bool    isFirst;      /* first record emitted for this instruction */
    ADDRINT pc;
    UINT32  type;          /* RType */
    UINT32  size;
    UINT32  opcode;        /* Pin OPCODE enum; see <prefix>.opcodes.txt */
    bool    isLast;        /* last micro-op of this instruction */
    bool    chainPrev;     /* depends on the previous micro-op of this inst */
    bool    takeSrcs;      /* reads the instruction's source registers */
    /* -gem5uops 1 only: which register sets this micro-op reads, and
     * whether it is the one that produces the destination registers. */
    bool    useAddr;
    bool    useData;
    bool    writesDst;
    /* -uoptable only */
    bool    tableUop;      /* emitted from a gem5 macro-op template */
    bool    rdLo, rdHi;    /* reads XMM low / high halves (tables only) */
    /* gem5 micro-op reads its own destination's old value (merge or
     * conditional keep): GPR, XMM low half, XMM high half. */
    bool    rdDst, rdDstLo, rdDstHi;
    bool    rmw;           /* load of a read-modify-write (ldst, ldstl) */
    bool    locked;        /* locked load (ldstl): non-speculative in O3 */
    /* -uoptable: the destination registers this micro-op itself writes;
     * published when the instruction ends (see the isLast block). */
    vector<REG> wrRegs;
    bool    rdZ, rdC, rdE, wrZ, wrC, wrE;   /* gem5 flag groups */
    bool    lastLo, lastHi;/* last micro-op writing XMM low / high */
    UINT8   uopIdx;        /* position within the macro-op */
    UINT8   ndep;          /* earlier micro-ops of the macro-op it reads */
    UINT8   deps[8];
    UINT32  memOff;        /* added to the operand address (split access) */
    UINT8   opClass;       /* etrace op-class code (gen_gem5_uops.py) */
    /* Control flow, for branch prediction at replay. The converter derives
     * taken/not-taken from the next instruction's PC and the length. */
    UINT8   brKind;        /* 0 none, 1 cond, 2 jmp, 3 call, 4 indirect
                              call, 5 return, 6 indirect jmp */
    UINT8   insLen;        /* instruction length in bytes */
    UINT64  execs;         /* first micro-op only: dynamic count, coverage */
    std::string g5name;    /* first micro-op only: gem5 macro-op name */
    bool    g5ok;
    vector<REG> srcRegs;   /* data inputs: consumed by the COMP micro-op */
    vector<REG> addrRegs;  /* base/index: consumed by the LOAD/STORE micro-op */
    vector<REG> dstRegs;
};

struct TState
{
    UINT64 prevUop;        /* seq of the previous micro-op of this instruction */
    UINT64 dstSeq;         /* seq of this instruction's destination writer */
    UINT64 uopSeq[32];     /* seqs of this macro-op's micro-ops (-uoptable) */
    /* (register, writer seq) of this macro-op so far, published at its end */
    std::vector<std::pair<REG, UINT64>> macroWrites;
    /* gem5 flag groups: last writer, and this macro-op's pending writes */
    UINT64 lastZ, lastC, lastE, pendZ, pendC, pendE;
    /* gem5 renames each XMM register as two 64-bit halves; the high half
     * has its own producer. lastWriter holds the low half (and every other
     * register), lastWriterHi the high half. */
    std::vector<UINT64> lastWriterHi;
    UINT64 dstSeqLo, dstSeqHi;
    std::ofstream out;
    UINT64 seq;
    UINT64 emitted;
    UINT64 syncs;
    UINT64 insts;
    /* REG is a small dense enum, so a flat vector beats a map by a wide
     * margin on the hot path. */
    std::vector<UINT64>       lastWriter;   /* RAW: reg -> producer seq */
    std::map<ADDRINT, UINT64> lastAccess;   /* order: line -> last seq */
    UINT64 lastLoad;    /* seq of the most recent LOAD  (commit-order rules) */
    UINT64 lastStore;   /* seq of the most recent STORE (commit-order rules) */
    UINT64 lastAny;     /* seq of the previous record   (issue-order rule)   */
    ADDRINT pendingCreate; /* pthread_t* of an in-flight pthread_create */
    UINT32 lastJoinChild;  /* dedupe: pthread_join is hooked in libc and */
    UINT64 lastJoinAt;     /* in the libpthread forwarder, both run      */
    UINT32 inBarrier;      /* inside a -barrier_fn call: record nothing  */
    char                      buf[512];     /* formatted record staging */
    TState() : seq(0), emitted(0), syncs(0), insts(0), inBarrier(0)
    {
        lastWriter.assign(REG_LAST, 0);
        lastWriterHi.assign(REG_LAST, 0);
        dstSeqLo = dstSeqHi = 0;
        lastLoad = lastStore = lastAny = 0;
        prevUop = dstSeq = 0;
        lastZ = lastC = lastE = pendZ = pendC = pendE = 0;
        pendingCreate = 0;
        lastJoinChild = 0;
        lastJoinAt = ~0ULL;
    }
};

static TLS_KEY       g_tls;
static PIN_LOCK      g_lock;
static volatile BOOL g_roiActive = false;
static vector<InstDesc*> g_descs;           /* owned; freed at exit */

static inline TState* TS(THREADID tid)
{
    return static_cast<TState*>(PIN_GetThreadData(g_tls, tid));
}

/* ------------------------------------------------------------ analysis */

static inline bool IsVecReg(REG r)
{
    return REG_is_xmm(r) || REG_is_ymm(r) || REG_is_zmm(r);
}

static VOID OnInst(THREADID tid, InstDesc* d, ADDRINT ea)
{
    if (KnobRoi.Value() && !g_roiActive) return;
    TState* s = TS(tid);
    if (!s) return;
    if (s->inBarrier) return;     /* waiting in a -barrier_fn, not work */
    if (d->isFirst) d->execs++;   /* coverage counts even unrecorded runs */
    if (KnobMax.Value() && s->emitted >= KnobMax.Value()) return;
    if (KnobMaxInsts.Value() && s->insts >= KnobMaxInsts.Value()) return;
    if (d->isFirst) s->insts++;
    if (d->type != COMP) ea += d->memOff;

    const UINT64 seq = ++s->seq;

    /* RAW register dependencies.
     *
     * With cracking, an instruction becomes a chain of micro-ops and the
     * dependency structure has to follow gem5's, which looks like this for
     * `add (%rdx),%ecx`:
     *     LOAD                        reg=[producer of %rdx]
     *     COMP  reg=[producer of %ecx, the LOAD]
     * So the source registers are claimed by the first micro-op, each later
     * micro-op depends on its predecessor, and only the final micro-op
     * becomes the last writer of the destination registers. */
    UINT64 rdeps[16];
    UINT32 nrd = 0;
    /* A memory micro-op depends on the registers forming the address; the
     * COMP micro-op depends on the data inputs.  Mixing them puts the wrong
     * producer distance on the load: for `add (%rdx),%ecx` the load must
     * depend on %rdx, not on the accumulator %ecx that the COMP consumes. */
    auto addDep = [&](UINT64 w) {
        if (w == 0 || w == seq || nrd >= 16) return;
        for (UINT32 k = 0; k < nrd; k++)
            if (rdeps[k] == w) return;
        rdeps[nrd++] = w;
    };
    if (d->tableUop) {
        if (d->useAddr)
            for (size_t i = 0; i < d->addrRegs.size(); i++)
                addDep(s->lastWriter[d->addrRegs[i]]);
        if (d->rdZ) addDep(s->lastZ);
        if (d->rdC) addDep(s->lastC);
        if (d->rdE) addDep(s->lastE);
        for (size_t i = 0; i < d->srcRegs.size(); i++) {
            const REG r = d->srcRegs[i];
            if (r == LEVEL_BASE::REG_RFLAGS)
                continue;           /* flag groups above */
            if (!IsVecReg(r)) {
                if (d->useData) addDep(s->lastWriter[r]);
            } else {
                if (d->rdLo) addDep(s->lastWriter[r]);
                if (d->rdHi) addDep(s->lastWriterHi[r]);
            }
        }
        if (d->rdDst || d->rdDstLo || d->rdDstHi)
            for (size_t i = 0; i < d->dstRegs.size(); i++) {
                const REG r = d->dstRegs[i];
                if (!IsVecReg(r)) {
                    if (d->rdDst) addDep(s->lastWriter[r]);
                } else {
                    if (d->rdDstLo) addDep(s->lastWriter[r]);
                    if (d->rdDstHi) addDep(s->lastWriterHi[r]);
                }
            }
    } else if (KnobGem5Uops.Value()) {
        if (d->useAddr)
            for (size_t i = 0; i < d->addrRegs.size(); i++)
                addDep(s->lastWriter[d->addrRegs[i]]);
        if (d->useData)
            for (size_t i = 0; i < d->srcRegs.size(); i++) {
                addDep(s->lastWriter[d->srcRegs[i]]);
                if (IsVecReg(d->srcRegs[i]))
                    addDep(s->lastWriterHi[d->srcRegs[i]]);
            }
    } else if (d->takeSrcs) {
        const vector<REG>& srcs =
            (d->type == COMP) ? d->srcRegs : d->addrRegs;
        for (size_t i = 0; i < srcs.size(); i++)
            addDep(s->lastWriter[srcs[i]]);
    }
    if (d->chainPrev)
        addDep(s->prevUop);
    for (UINT32 k = 0; k < d->ndep; k++)
        if (d->deps[k] < 32)
            addDep(s->uopSeq[d->deps[k]]);

    /* Order (rob) dependencies, following what ElasticTrace itself records.
     * Matching the *count* matters as much as the delays: gem5 averages 0.52
     * rob_deps per record against 0.17 for a same-line-only rule, and those
     * extra edges are what let a load's pre-issue delay overlap with other
     * work instead of landing on the critical path.
     *
     * gem5's rules (elastic_trace.cc):
     *   - a store gets an order dep on the last store committed earlier and
     *     on the last load completed earlier (updateCommitOrderDep, run twice)
     *   - a record left with no dependency at all gets an issue-order dep on
     *     a recently completed record, so it is not treated as free to issue
     *     at time zero (updateIssueOrderDep)
     * plus the same-line ordering this tool already tracked. */
    UINT64 odeps[4];
    UINT32 nod = 0;
    const UINT64 selfSeq = seq;
    #define PUSH_ODEP(v) do {                                        \
        const UINT64 _v = (v);                                       \
        if (_v != 0 && _v != selfSeq && nod < 4) {                   \
            bool dup = false;                                        \
            for (UINT32 _k = 0; _k < nod; _k++)                      \
                if (odeps[_k] == _v) { dup = true; break; }          \
            if (!dup) odeps[nod++] = _v;                             \
        }                                                            \
    } while (0)

    if (d->type != COMP) {
        const ADDRINT line = ea & ~(ADDRINT)(KnobLine.Value() - 1);
        std::map<ADDRINT, UINT64>::iterator it = s->lastAccess.find(line);
        if (it != s->lastAccess.end())
            PUSH_ODEP(it->second);
        s->lastAccess[line] = seq;
    }
    if (d->type == STORE) {
        PUSH_ODEP(s->lastStore);
        PUSH_ODEP(s->lastLoad);
    }
    if (nrd == 0 && nod == 0)
        PUSH_ODEP(s->lastAny);          /* issue-order dep for a free node */

    if (d->type == LOAD)  s->lastLoad  = seq;
    if (d->type == STORE) s->lastStore = seq;
    s->lastAny = seq;

    int n = snprintf(s->buf, sizeof(s->buf), "S %llu %llu %u %llu %u %u",
                     (unsigned long long)seq, (unsigned long long)d->pc,
                     d->type, (unsigned long long)ea, d->size, nrd);
    for (UINT32 i = 0; i < nrd; i++)
        n += snprintf(s->buf + n, sizeof(s->buf) - n, " %llu",
                      (unsigned long long)rdeps[i]);
    n += snprintf(s->buf + n, sizeof(s->buf) - n, " %u", nod);
    for (UINT32 i = 0; i < nod; i++)
        n += snprintf(s->buf + n, sizeof(s->buf) - n, " %llu",
                      (unsigned long long)odeps[i]);
    /* opcode, then the micro-op position flag (1 = last micro-op of this
     * instruction).  The converter needs it because gem5 charges the
     * intermediate micro-ops of a cracked instruction no delay and bills only
     * the last one. */
    n += snprintf(s->buf + n, sizeof(s->buf) - n, " %u %u %u %u %u",
                  d->opcode, d->isLast ? 1u : 0u,
                  /* loads: 1 = read-modify-write, 2 = locked RMW */
                  d->type == LOAD ? (d->locked ? 2u : d->rmw ? 1u : 0u)
                                  : (UINT32)d->opClass,
                  (UINT32)d->brKind, (UINT32)d->insLen);
    s->buf[n++] = '\n';
    s->out.write(s->buf, n);

    if (d->tableUop && d->uopIdx < 32)
        s->uopSeq[d->uopIdx] = seq;
    for (size_t i = 0; i < d->wrRegs.size(); i++)
        s->macroWrites.push_back(std::make_pair(d->wrRegs[i], seq));
    if (d->wrZ) s->pendZ = seq;
    if (d->wrC) s->pendC = seq;
    if (d->wrE) s->pendE = seq;
    if (d->tableUop && d->lastLo) s->dstSeqLo = seq;
    if (d->tableUop && d->lastHi) s->dstSeqHi = seq;
    if (KnobGem5Uops.Value() && d->writesDst)
        s->dstSeq = seq;
    if (d->isLast) {
        /* Destinations are published only after the whole instruction, so
         * its own micro-ops read the old values (push reads the old rsp).
         * gem5 mode credits them to the micro-op that computes them, not to
         * a trailing store: gem5 has no STORE->COMP register edges. */
        const UINT64 w = (KnobGem5Uops.Value() && s->dstSeq) ? s->dstSeq
                                                             : seq;
        for (size_t i = 0; i < d->dstRegs.size(); i++) {
            const REG r = d->dstRegs[i];
            if (IsVecReg(r) && d->tableUop &&
                (s->dstSeqLo || s->dstSeqHi)) {
                if (s->dstSeqLo) s->lastWriter[r] = s->dstSeqLo;
                if (s->dstSeqHi) s->lastWriterHi[r] = s->dstSeqHi;
            } else {
                UINT64 ww = w;
                for (size_t j = s->macroWrites.size(); j-- > 0;)
                    if (s->macroWrites[j].first == r) {
                        ww = s->macroWrites[j].second;
                        break;
                    }
                s->lastWriter[r] = ww;
                if (IsVecReg(r)) s->lastWriterHi[r] = ww;
            }
        }
        if (d->tableUop) {
            if (s->pendZ) s->lastZ = s->pendZ;
            if (s->pendC) s->lastC = s->pendC;
            if (s->pendE) s->lastE = s->pendE;
        } else {
            /* untabled instruction writing the flags: all groups */
            for (size_t i = 0; i < d->dstRegs.size(); i++)
                if (d->dstRegs[i] == LEVEL_BASE::REG_RFLAGS)
                    s->lastZ = s->lastC = s->lastE =
                        s->lastWriter[LEVEL_BASE::REG_RFLAGS];
        }
        s->pendZ = s->pendC = s->pendE = 0;
        s->macroWrites.clear();
        s->prevUop = 0;
        s->dstSeq = 0;
        s->dstSeqLo = s->dstSeqHi = 0;
    } else {
        s->prevUop = seq;
    }

    s->emitted++;
}

/* Sync marker. Records the running record count, which is the unit TraceCPU
 * counts in at replay, so the arbiter compares like with like. */
static VOID OnSync(THREADID tid, ADDRINT type, UINT32 isEnd)
{
    if (KnobRoi.Value() && !g_roiActive) return;
    TState* s = TS(tid);
    if (!s) return;
    s->out << "Y " << (UINT32)type << ' ' << isEnd << ' '
           << s->emitted << '\n';
    s->syncs++;
}

/* type: 0 parallel, 1 critical, 2 barrier. isEnd: 0 begin, 1 end. */
static VOID OnAutoSync(THREADID tid, UINT32 type, UINT32 isEnd)
{
    if (KnobRoi.Value() && !g_roiActive) return;
    TState* s = TS(tid);
    if (!s) return;
    if (type == 1 && s->inBarrier) return;   /* the barrier's own mutex */
    s->out << "Y " << type << ' ' << isEnd << ' ' << s->emitted << '\n';
    s->syncs++;
}

/* trylock acquires only when it returns 0. Hooking it at IPOINT_BEFORE like
 * pthread_mutex_lock would record acquisitions that never happened; a
 * trylock-heavy workload (streamcluster: 780k trylock vs 91k lock) then emits
 * far more unlocks than locks. */
/* Thread creation and join (-autosync). Without them a replay starts every
 * thread at time zero, so a main thread's serial setup overlaps the workers
 * it has not created yet, and it never waits for them at join.
 *   Y 3 0 <records> <child>   parent, after a successful pthread_create
 *   Y 4 0 <records> <child>   parent, before pthread_join(child)
 * <child> is the child's trace number. Pin numbers application threads in
 * the order they start, main = 0, so the k-th successful create (counted
 * across all creators) is thread k; a program whose libraries start threads
 * of their own breaks that assumption. pthread_join names its child by
 * pthread_t, which pthread_create stored through its first argument. */
static PIN_LOCK g_spawnLock;
static UINT32 g_created = 0;
static std::map<ADDRINT, UINT32> g_handleToTid;

static VOID OnCreateBefore(THREADID tid, ADDRINT threadPtr)
{
    TState* s = TS(tid);
    if (s) s->pendingCreate = threadPtr;
}

static VOID OnCreateAfter(THREADID tid, ADDRINT ret)
{
    TState* s = TS(tid);
    if (!s) return;
    const ADDRINT ptr = s->pendingCreate;
    s->pendingCreate = 0;
    if (ret != 0 || !ptr) return;          /* create failed */
    ADDRINT handle = 0;
    PIN_SafeCopy(&handle, (VOID*)ptr, sizeof(handle));
    PIN_GetLock(&g_spawnLock, tid + 1);
    const UINT32 child = ++g_created;
    g_handleToTid[handle] = child;
    PIN_ReleaseLock(&g_spawnLock);
    if (KnobRoi.Value() && !g_roiActive) return;
    s->out << "Y 3 0 " << s->emitted << ' ' << child << '\n';
    s->syncs++;
}

static VOID OnJoinBefore(THREADID tid, ADDRINT handle)
{
    TState* s = TS(tid);
    if (!s) return;
    PIN_GetLock(&g_spawnLock, tid + 1);
    auto it = g_handleToTid.find(handle);
    const bool known = it != g_handleToTid.end();
    const UINT32 child = known ? it->second : 0;
    PIN_ReleaseLock(&g_spawnLock);
    if (!known) return;
    if (KnobRoi.Value() && !g_roiActive) return;
    if (child == s->lastJoinChild && s->emitted == s->lastJoinAt) return;
    s->lastJoinChild = child;
    s->lastJoinAt = s->emitted;
    s->out << "Y 4 0 " << s->emitted << ' ' << child << '\n';
    s->syncs++;
}

static VOID OnTryLock(THREADID tid, ADDRINT ret)
{
    if (ret != 0) return;                 /* did not acquire */
    OnAutoSync(tid, 1, 0);
}

static VOID OnBarrierFnEnter(THREADID tid)
{
    OnAutoSync(tid, 2, 0);
    TState* s = TS(tid);
    if (s) s->inBarrier++;
}

static VOID OnBarrierFnLeave(THREADID tid)
{
    TState* s = TS(tid);
    if (!s || !s->inBarrier) return;
    s->inBarrier--;
    OnAutoSync(tid, 2, 1);
}

static VOID RoiBegin() { g_roiActive = true;  }
static VOID RoiEnd()   { g_roiActive = false; }

/* ----------------------------------------------------- instrumentation */

/* ---- gem5 micro-op table (-uoptable) ------------------------------------ */
struct UopTpl
{
    UINT32 type;          /* LOAD/STORE/COMP */
    UINT8 opClass;
    UINT8 ndep;
    UINT8 deps[8];
    bool useAddr, useData, writes;
    bool rdLo, rdHi, wrLo, wrHi;
    bool rdDst, rdDstLo, rdDstHi;
    bool rmw, locked;
    bool wflags;          /* writes the flags */
    bool rdZ, rdC, rdE;   /* reads gem5 flag groups Zaps / Cfof / Ecf-Ezf */
    bool wrZ, wrC, wrE;   /* writes them */
    std::string wname;    /* arch register written: explicit, reg, regm */
    UINT8 memop;          /* index among the macro-op's loads (or stores) */
    UINT32 off;
    UINT32 size;
};
static std::map<std::string, std::vector<UopTpl>> g_uopTab;

static void LoadUopTable(const std::string& path)
{
    std::ifstream in(path.c_str());
    if (!in) { std::cerr << "[etrace] cannot open " << path << "\n"; return; }
    std::string name;
    UINT32 n;
    while (in >> name >> n) {
        std::vector<UopTpl> v;
        for (UINT32 i = 0; i < n; i++) {
            std::string f;
            in >> f;
            /* kind,opclass,deps,flags,memop,offset,size */
            std::vector<std::string> p;
            size_t a = 0, b;
            while ((b = f.find(',', a)) != std::string::npos) {
                p.push_back(f.substr(a, b - a)); a = b + 1;
            }
            p.push_back(f.substr(a));
            if (p.size() != 7) continue;
            UopTpl u;
            u.type = p[0] == "L" ? LOAD : p[0] == "S" ? STORE : COMP;
            u.opClass = (UINT8)atoi(p[1].c_str());
            u.ndep = 0;
            if (p[2] != "-") {
                size_t c = 0, e;
                std::string ds = p[2] + ".";
                while ((e = ds.find('.', c)) != std::string::npos && u.ndep < 8) {
                    u.deps[u.ndep++] = (UINT8)atoi(ds.substr(c, e - c).c_str());
                    c = e + 1;
                }
            }
            {
                /* "flags=name": the name is the register it writes */
                const size_t eq = p[3].find('=');
                u.wname = eq == std::string::npos ? "" : p[3].substr(eq + 1);
                if (eq != std::string::npos) p[3] = p[3].substr(0, eq);
            }
            u.wflags = p[3].find('f') != std::string::npos;
            u.rdZ = p[3].find('z') != std::string::npos;
            u.rdC = p[3].find('c') != std::string::npos;
            u.rdE = p[3].find('e') != std::string::npos;
            u.wrZ = p[3].find('Z') != std::string::npos;
            u.wrC = p[3].find('C') != std::string::npos;
            u.wrE = p[3].find('E') != std::string::npos;
            u.useAddr = p[3].find('a') != std::string::npos;
            u.useData = p[3].find('d') != std::string::npos;
            u.writes  = p[3].find('w') != std::string::npos;
            u.rdLo = p[3].find('r') != std::string::npos;
            u.rdHi = p[3].find('R') != std::string::npos;
            u.wrLo = p[3].find('x') != std::string::npos;
            u.wrHi = p[3].find('X') != std::string::npos;
            u.rdDst = p[3].find('D') != std::string::npos;
            u.rdDstLo = p[3].find('y') != std::string::npos;
            u.rdDstHi = p[3].find('Y') != std::string::npos;
            u.rmw = p[3].find('m') != std::string::npos;
            u.locked = p[3].find('M') != std::string::npos;
            u.memop = (UINT8)atoi(p[4].c_str());
            u.off = atoi(p[5].c_str());
            u.size = atoi(p[6].c_str());
            v.push_back(u);
        }
        g_uopTab[name] = v;
    }
    std::cerr << "[etrace] uoptable: " << g_uopTab.size() << " macro-ops\n";
}

/* gem5's macro-op name for this instruction: XED mnemonic plus one form per
 * explicit operand (R register, XMM, MMX, M memory, P rip-relative memory,
 * I immediate/branch target), e.g. ADDSD_XMM_P, JNZ_I, CALL_NEAR_I. */
static std::string Gem5Name(INS ins)
{
    std::string m = INS_Mnemonic(ins);
    if (m.size() > 4 && m.compare(m.size() - 4, 4, "_XMM") == 0)
        m.resize(m.size() - 4);          /* XED MOVSD_XMM is gem5 MOVSD */
    if (m == "SHL") m = "SAL";           /* same opcode, gem5 spells it SAL */
    std::string forms;
    for (UINT32 i = 0; i < INS_OperandCount(ins); i++) {
        if (INS_OperandIsImplicit(ins, i)) continue;
        const char* f = 0;
        if (INS_OperandIsReg(ins, i)) {
            REG r = INS_OperandReg(ins, i);
            if (REG_is_xmm(r)) f = "XMM";
            else if (REG_is_mm(r)) f = "MMX";
            else if (REG_is_gr(REG_FullRegName(r))) f = "R";
        } else if (INS_OperandIsMemory(ins, i) ||
                   INS_OperandIsAddressGenerator(ins, i)) {
            f = INS_OperandMemoryBaseReg(ins, i) == REG_INST_PTR ? "P" : "M";
        } else if (INS_OperandIsImmediate(ins, i) ||
                   INS_OperandIsBranchDisplacement(ins, i)) {
            f = "I";
        }
        if (f) { forms += '_'; forms += f; }
    }
    return m + forms;
}

/* gem5 microcode register name -> Pin full register (GPRs only). */
static REG Gem5NameReg(const std::string& n)
{
    static const std::map<std::string, REG> m = {
        {"rax", LEVEL_BASE::REG_RAX}, {"eax", LEVEL_BASE::REG_RAX}, {"ax", LEVEL_BASE::REG_RAX}, {"al", LEVEL_BASE::REG_RAX},
        {"ah", LEVEL_BASE::REG_RAX}, {"rbx", LEVEL_BASE::REG_RBX}, {"ebx", LEVEL_BASE::REG_RBX}, {"bx", LEVEL_BASE::REG_RBX},
        {"bl", LEVEL_BASE::REG_RBX}, {"rcx", LEVEL_BASE::REG_RCX}, {"ecx", LEVEL_BASE::REG_RCX}, {"cx", LEVEL_BASE::REG_RCX},
        {"cl", LEVEL_BASE::REG_RCX}, {"rdx", LEVEL_BASE::REG_RDX}, {"edx", LEVEL_BASE::REG_RDX}, {"dx", LEVEL_BASE::REG_RDX},
        {"dl", LEVEL_BASE::REG_RDX}, {"rsi", LEVEL_BASE::REG_RSI}, {"esi", LEVEL_BASE::REG_RSI}, {"si", LEVEL_BASE::REG_RSI},
        {"rdi", LEVEL_BASE::REG_RDI}, {"edi", LEVEL_BASE::REG_RDI}, {"di", LEVEL_BASE::REG_RDI},
        {"rsp", LEVEL_BASE::REG_RSP}, {"esp", LEVEL_BASE::REG_RSP}, {"sp", LEVEL_BASE::REG_RSP},
        {"rbp", LEVEL_BASE::REG_RBP}, {"ebp", LEVEL_BASE::REG_RBP}, {"bp", LEVEL_BASE::REG_RBP},
        {"r8", LEVEL_BASE::REG_R8}, {"r9", LEVEL_BASE::REG_R9}, {"r10", LEVEL_BASE::REG_R10}, {"r11", LEVEL_BASE::REG_R11},
        {"r12", LEVEL_BASE::REG_R12}, {"r13", LEVEL_BASE::REG_R13}, {"r14", LEVEL_BASE::REG_R14},
        {"r15", LEVEL_BASE::REG_R15}};
    auto it = m.find(n);
    return it == m.end() ? REG_INVALID() : it->second;
}

static VOID Instruction(INS ins, VOID* v)
{
    InstDesc* d = new InstDesc();
    d->isFirst = true;
    d->isLast = true;
    d->chainPrev = false;
    d->takeSrcs = true;
    d->useAddr = d->useData = d->writesDst = false;
    d->tableUop = false;
    d->rdLo = d->rdHi = d->lastLo = d->lastHi = false;
    d->rdDst = d->rdDstLo = d->rdDstHi = false;
    d->rmw = d->locked = false;
    d->rdZ = d->rdC = d->rdE = d->wrZ = d->wrC = d->wrE = false;
    d->uopIdx = d->ndep = d->opClass = 0;
    d->memOff = 0;
    d->execs = 0;
    d->g5ok = false;
    d->pc   = INS_Address(ins);
    d->type = COMP;
    d->size = 0;
    d->opcode = INS_Opcode(ins);
    d->insLen = (UINT8)INS_Size(ins);
    d->brKind = 0;
    if (INS_IsRet(ins))
        d->brKind = 5;
    else if (INS_IsCall(ins))
        d->brKind = INS_IsDirectControlFlow(ins) ? 3 : 4;
    else if (INS_IsBranch(ins))
        d->brKind = INS_HasFallThrough(ins) ? 1
                  : INS_IsDirectControlFlow(ins) ? 2 : 6;

    /* Which registers form a memory address?  Those belong to the memory
     * micro-op; everything else is a data input to the COMP micro-op. */
    const bool ripDeps = KnobRipDeps.Value();
    auto tracked = [ripDeps](REG r) {
        return REG_valid(r) && (ripDeps || r != REG_INST_PTR);
    };
    vector<REG> addr;
    /* Memory operands and address generators (lea): Pin does not count
     * lea's operand as a memory operand, which left lea with no inputs. */
    for (UINT32 op = 0; op < INS_OperandCount(ins); op++) {
        if (!INS_OperandIsMemory(ins, op) &&
            !INS_OperandIsAddressGenerator(ins, op))
            continue;
        REG b_ = INS_OperandMemoryBaseReg(ins, op);
        REG x_ = INS_OperandMemoryIndexReg(ins, op);
        if (tracked(REG_FullRegName(b_))) addr.push_back(REG_FullRegName(b_));
        if (tracked(REG_FullRegName(x_))) addr.push_back(REG_FullRegName(x_));
    }
    for (UINT32 i = 0; i < INS_MaxNumRRegs(ins); i++) {
        REG r = REG_FullRegName(INS_RegR(ins, i));
        if (!tracked(r)) continue;
        bool isAddr = false;
        for (size_t j = 0; j < addr.size(); j++)
            if (addr[j] == r) { isAddr = true; break; }
        if (isAddr) d->addrRegs.push_back(r);
        else        d->srcRegs.push_back(r);
    }
    for (UINT32 i = 0; i < INS_MaxNumWRegs(ins); i++) {
        REG r = REG_FullRegName(INS_RegW(ins, i));
        if (tracked(r)) d->dstRegs.push_back(r);
    }

    PIN_GetLock(&g_lock, 1);
    g_descs.push_back(d);
    PIN_ReleaseLock(&g_lock);

    const UINT32 nOps = INS_MemoryOperandCount(ins);

    /* How many COMP micro-ops gem5 emits beyond the memory accesses.
     * Calibrated against gem5's own elastic traces for the same binary:
     *   add (%rdx),%ecx -> LOAD + COMP                 (2 uops)
     *   jne <target>    -> COMP + COMP + COMP          (3 uops)
     *   call            -> ~3.9 records per instruction
     *   ret             -> ~2.2
     *   cmovcc          -> ~4.0
     * Everything else gets one COMP for the operation itself, which is what
     * the uncracked tool emitted for every instruction. */
    UINT32 nComp = 1;
    if (!KnobCrack.Value()) {
        nComp = (nOps == 0) ? 1 : 0;        /* original behaviour */
    } else if (INS_IsCall(ins)) {
        nComp = 3;
    } else if (INS_IsRet(ins)) {
        nComp = 1;
    } else if (INS_IsBranch(ins)) {
        nComp = INS_HasFallThrough(ins) ? 3 : 2;   /* conditional : jmp */
    } else if (INS_Opcode(ins) >= XED_ICLASS_CMOVB &&
               INS_Opcode(ins) <= XED_ICLASS_CMOVZ) {
        nComp = 3;
    }

    if (!g_uopTab.empty() && KnobCrack.Value()) {
        d->g5name = Gem5Name(ins);
        auto it = g_uopTab.find(d->g5name);
        if (it == g_uopTab.end()) {
            /* Names XED forms differently from gem5's decoder:
             *  - the accumulator short forms (cmp eax, imm) mark the
             *    register implicit: gem5 still calls it CMP_R_I
             *  - MOVZX/MOVSX carry the source size: MOVZX_B_R_M
             *  - operand-less spellings: NOP */
            const std::string mn = INS_Mnemonic(ins);
            const size_t us = d->g5name.find('_', mn.size());
            const std::string forms = (us == std::string::npos) ? ""
                                      : d->g5name.substr(mn.size());
            std::vector<std::string> alt;
            if (forms.compare(0, 2, "_I") == 0) alt.push_back(mn + "_R" + forms);
            if (mn == "MOVZX" || mn == "MOVSX") {
                UINT32 w = 0;
                for (UINT32 i = 0; i < INS_OperandCount(ins); i++)
                    if (i > 0 && !INS_OperandIsImplicit(ins, i))
                        w = INS_OperandWidth(ins, i);
                alt.push_back(mn + (w == 8 ? "_B" : "_W") + forms);
            }
            /*  - MOVQ between XMM and GPR is gem5's MOVD (REX.W inside)
             *  - shift by one: SHR_1_R; implicit-register forms: CDQE_R
             *  - lock prefix: CMPXCHG_LOCK is gem5's CMPXCHG_LOCKED */
            if (mn == "MOVQ") alt.push_back("MOVD" + forms);
            if ((mn == "SHR" || mn == "SAR" || mn == "SAL" || mn == "SHL" ||
                 mn == "ROL" || mn == "ROR" || mn == "RCL" || mn == "RCR") &&
                (forms == "_R" || forms == "_M" || forms == "_P"))
                alt.push_back((mn == "SHL" ? std::string("SAL") : mn) +
                              "_1" + forms);
            if (mn.size() > 5 && mn.compare(mn.size() - 5, 5, "_LOCK") == 0)
                alt.push_back(mn.substr(0, mn.size() - 5) + "_LOCKED" + forms);
            alt.push_back(mn + "_R");
            alt.push_back(mn + "_R_R");
            alt.push_back(mn);
            for (const std::string& a : alt) {
                it = g_uopTab.find(a);
                if (it != g_uopTab.end()) { d->g5name = a; break; }
            }
        }
        vector<UINT32> rdOps, wrOps;
        for (UINT32 op = 0; op < nOps; op++) {
            if (INS_MemoryOperandIsRead(ins, op))    rdOps.push_back(op);
            if (INS_MemoryOperandIsWritten(ins, op)) wrOps.push_back(op);
        }
        if (it != g_uopTab.end() && it->second.size() <= 32) {
            d->g5ok = true;
            const std::vector<UopTpl>& t = it->second;
            int lastW = -1, lastLo = -1, lastHi = -1;
            for (size_t k = 0; k < t.size(); k++) {
                if (t[k].writes) lastW = (int)k;
                if (t[k].wrLo) lastLo = (int)k;
                if (t[k].wrHi) lastHi = (int)k;
            }
            /* Which destination register each micro-op writes: explicit
             * names first, then reg/regm take the remaining GPR
             * destinations, flags go to the flag writer. */
            std::vector<std::vector<REG>> wr(t.size());
            {
                std::vector<REG> left;
                for (size_t i = 0; i < d->dstRegs.size(); i++) {
                    const REG r = d->dstRegs[i];
                    if (!IsVecReg(r) && r != LEVEL_BASE::REG_RFLAGS) left.push_back(r);
                }
                auto take = [&](REG r) {
                    for (size_t i = 0; i < left.size(); i++)
                        if (left[i] == r) { left.erase(left.begin() + i);
                                            return true; }
                    return false;
                };
                for (size_t k = 0; k < t.size(); k++) {
                    const REG r = Gem5NameReg(t[k].wname);
                    if (REG_valid(r) && take(r)) wr[k].push_back(r);
                }
                for (size_t k = 0; k < t.size(); k++) {
                    if ((t[k].wname == "reg" || t[k].wname == "regm") &&
                        !left.empty()) {
                        wr[k].push_back(left.front());
                        left.erase(left.begin());
                    }
                }
                for (size_t k = 0; k < t.size(); k++)
                    if (t[k].wflags) wr[k].push_back(LEVEL_BASE::REG_RFLAGS);
            }
            for (size_t k = 0; k < t.size(); k++) {
                const UopTpl& u = t[k];
                InstDesc* x = (k == 0) ? d : new InstDesc(*d);
                x->wrRegs = wr[k];
                x->rdZ = u.rdZ; x->rdC = u.rdC; x->rdE = u.rdE;
                x->wrZ = u.wrZ; x->wrC = u.wrC; x->wrE = u.wrE;
                x->tableUop = true;
                x->uopIdx = (UINT8)k;
                x->isFirst = (k == 0);
                x->isLast = (k + 1 == t.size());
                x->takeSrcs = false;
                x->chainPrev = false;
                x->useAddr = u.useAddr;
                x->useData = u.useData;
                x->writesDst = ((int)k == lastW);
                x->rdLo = u.rdLo;
                x->rdHi = u.rdHi;
                x->rdDst = u.rdDst;
                x->rdDstLo = u.rdDstLo;
                x->rdDstHi = u.rdDstHi;
                x->rmw = u.rmw;
                x->locked = u.locked;
                x->lastLo = ((int)k == lastLo);
                x->lastHi = ((int)k == lastHi);
                x->ndep = u.ndep;
                for (UINT32 j = 0; j < u.ndep; j++) x->deps[j] = u.deps[j];
                x->opClass = (u.type == COMP) ? u.opClass : 0;
                x->memOff = u.off;
                if (k != 0) x->g5name.clear();
                INT32 op = -1;
                if (u.type == LOAD && !rdOps.empty())
                    op = rdOps[u.memop < rdOps.size() ? u.memop : rdOps.size() - 1];
                else if (u.type == STORE && !wrOps.empty())
                    op = wrOps[u.memop < wrOps.size() ? u.memop : wrOps.size() - 1];
                /* a memory micro-op Pin has no operand for becomes compute */
                x->type = (op >= 0) ? u.type : COMP;
                x->size = (op >= 0) ? (u.size ? u.size
                                      : (UINT32)INS_MemoryOperandSize(ins, op))
                                    : 0;
                if (k != 0) {
                    PIN_GetLock(&g_lock, 1);
                    g_descs.push_back(x);
                    PIN_ReleaseLock(&g_lock);
                }
                if (op >= 0)
                    INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)OnInst,
                                             IARG_THREAD_ID, IARG_PTR, x,
                                             IARG_MEMORYOP_EA, (UINT32)op,
                                             IARG_END);
                else
                    INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)OnInst,
                                             IARG_THREAD_ID, IARG_PTR, x,
                                             IARG_ADDRINT, (ADDRINT)0, IARG_END);
            }
            return;
        }
    }

    if (KnobGem5Uops.Value() && KnobCrack.Value()) {
        /* Loads, then the operation, then stores (see KnobGem5Uops). A
         * read-modify-write operand appears in both lists. */
        vector<UINT32> rdOps, wrOps;
        for (UINT32 op = 0; op < nOps; op++) {
            if (INS_MemoryOperandIsRead(ins, op))    rdOps.push_back(op);
            if (INS_MemoryOperandIsWritten(ins, op)) wrOps.push_back(op);
        }
        /* mov to or from memory is a single ld or st in gem5. */
        const UINT32 nC = (nOps && INS_IsMov(ins)) ? 0 : nComp;
        const UINT32 total = (UINT32)rdOps.size() + nC + (UINT32)wrOps.size();
        UINT32 k = 0;
        auto emit = [&](UINT32 type, INT32 op, bool useAddr, bool useData,
                        bool chain, bool writesDst) {
            InstDesc* u = new InstDesc(*d);
            u->type      = type;
            u->size      = (op >= 0) ? (UINT32)INS_MemoryOperandSize(ins, op)
                                     : 0;
            u->isFirst   = (k == 0);
            u->isLast    = (++k == total);
            u->takeSrcs  = false;
            u->useAddr   = useAddr;
            u->useData   = useData;
            u->chainPrev = chain;
            u->writesDst = writesDst;
            PIN_GetLock(&g_lock, 1);
            g_descs.push_back(u);
            PIN_ReleaseLock(&g_lock);
            if (op >= 0)
                INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)OnInst,
                                         IARG_THREAD_ID, IARG_PTR, u,
                                         IARG_MEMORYOP_EA, (UINT32)op,
                                         IARG_END);
            else
                INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)OnInst,
                                         IARG_THREAD_ID, IARG_PTR, u,
                                         IARG_ADDRINT, (ADDRINT)0, IARG_END);
        };
        for (size_t i = 0; i < rdOps.size(); i++)
            /* a pure load (no operation, no store) writes the destination */
            emit(LOAD, (INT32)rdOps[i], true, false, false,
                 nC == 0 && wrOps.empty() && i + 1 == rdOps.size());
        for (UINT32 c = 0; c < nC; c++)
            emit(COMP, -1, false, c == 0, c > 0 || !rdOps.empty(),
                 c + 1 == nC);
        for (size_t i = 0; i < wrOps.size(); i++) {
            /* the value comes from the operation or a load if there is one,
             * otherwise straight from the data registers (mov [m], r) */
            const bool produced = nC > 0 || !rdOps.empty();
            emit(STORE, (INT32)wrOps[i], true, !produced, produced || i > 0,
                 false);
        }
        return;
    }

    const UINT32 nUops = nOps + nComp;
    UINT32 emitted = 0;

    /* Memory micro-ops first: the access, then the operation that consumes it. */
    for (UINT32 op = 0; op < nOps; op++) {
        InstDesc* md = new InstDesc(*d);
        md->isFirst   = (emitted == 0);
        md->size      = (UINT32)INS_MemoryOperandSize(ins, op);
        md->type      = INS_MemoryOperandIsWritten(ins, op) ? STORE : LOAD;
        md->takeSrcs  = (emitted == 0);
        md->chainPrev = (emitted != 0);
        md->isLast    = (++emitted == nUops);
        PIN_GetLock(&g_lock, 1);
        g_descs.push_back(md);
        PIN_ReleaseLock(&g_lock);
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)OnInst,
                                 IARG_THREAD_ID, IARG_PTR, md,
                                 IARG_MEMORYOP_EA, op, IARG_END);
    }
    for (UINT32 c = 0; c < nComp; c++) {
        InstDesc* cd = new InstDesc(*d);
        cd->isFirst   = (emitted == 0);
        cd->type      = COMP;
        cd->size      = 0;
        cd->takeSrcs  = (emitted == 0);
        cd->chainPrev = (emitted != 0);
        cd->isLast    = (++emitted == nUops);
        PIN_GetLock(&g_lock, 1);
        g_descs.push_back(cd);
        PIN_ReleaseLock(&g_lock);
        INS_InsertPredicatedCall(ins, IPOINT_BEFORE, (AFUNPTR)OnInst,
                                 IARG_THREAD_ID, IARG_PTR, cd,
                                 IARG_ADDRINT, (ADDRINT)0, IARG_END);
    }
}

static VOID Image(IMG img, VOID* v)
{
    RTN b = RTN_FindByName(img, KnobRoiBeg.Value().c_str());
    if (RTN_Valid(b)) { RTN_Open(b);
        RTN_InsertCall(b, IPOINT_BEFORE, (AFUNPTR)RoiBegin, IARG_END);
        RTN_Close(b); std::cerr << "[etrace] ROI begin hooked\n"; }
    RTN e = RTN_FindByName(img, KnobRoiEnd.Value().c_str());
    if (RTN_Valid(e)) { RTN_Open(e);
        RTN_InsertCall(e, IPOINT_BEFORE, (AFUNPTR)RoiEnd, IARG_END);
        RTN_Close(e); std::cerr << "[etrace] ROI end hooked\n"; }

    RTN sb = RTN_FindByName(img, KnobSyncBeg.Value().c_str());
    if (RTN_Valid(sb)) { RTN_Open(sb);
        RTN_InsertCall(sb, IPOINT_BEFORE, (AFUNPTR)OnSync,
                       IARG_THREAD_ID,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                       IARG_UINT32, 0, IARG_END);
        RTN_Close(sb); std::cerr << "[etrace] sync begin hooked\n"; }
    if (KnobAutoSync.Value()) {
        struct { const char* sym; UINT32 type; UINT32 isEnd; BOOL after; } H[] = {
            { "pthread_mutex_lock",   1, 0, false },  /* enter critical  */
            { "pthread_mutex_unlock", 1, 1, false },  /* leave critical  */
            { "pthread_barrier_wait", 2, 0, false },  /* arrive barrier  */
            { "pthread_barrier_wait", 2, 1, true  },  /* leave  barrier  */
        };
        RTN cr = RTN_FindByName(img, "pthread_create");
        if (RTN_Valid(cr)) {
            RTN_Open(cr);
            RTN_InsertCall(cr, IPOINT_BEFORE, (AFUNPTR)OnCreateBefore,
                           IARG_THREAD_ID, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                           IARG_END);
            RTN_InsertCall(cr, IPOINT_AFTER, (AFUNPTR)OnCreateAfter,
                           IARG_THREAD_ID, IARG_FUNCRET_EXITPOINT_VALUE,
                           IARG_END);
            RTN_Close(cr);
            std::cerr << "[etrace] autosync hooked pthread_create (spawn)\n";
        }
        RTN jn = RTN_FindByName(img, "pthread_join");
        if (RTN_Valid(jn)) {
            RTN_Open(jn);
            RTN_InsertCall(jn, IPOINT_BEFORE, (AFUNPTR)OnJoinBefore,
                           IARG_THREAD_ID, IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                           IARG_END);
            RTN_Close(jn);
            std::cerr << "[etrace] autosync hooked pthread_join (join)\n";
        }
        /* trylock needs its return value, so it is hooked separately. */
        RTN tl = RTN_FindByName(img, "pthread_mutex_trylock");
        if (RTN_Valid(tl)) {
            RTN_Open(tl);
            RTN_InsertCall(tl, IPOINT_AFTER, (AFUNPTR)OnTryLock,
                           IARG_THREAD_ID, IARG_FUNCRET_EXITPOINT_VALUE,
                           IARG_END);
            RTN_Close(tl);
            std::cerr << "[etrace] autosync hooked pthread_mutex_trylock"
                      << " (after, success only)\n";
        }
        std::string fns = KnobBarrierFn.Value();
        for (size_t p = 0; p < fns.size();) {
            size_t q = fns.find(',', p);
            if (q == std::string::npos) q = fns.size();
            const std::string sym = fns.substr(p, q - p);
            p = q + 1;
            RTN r = sym.empty() ? RTN_Invalid() : RTN_FindByName(img, sym.c_str());
            if (!RTN_Valid(r)) continue;
            RTN_Open(r);
            RTN_InsertCall(r, IPOINT_BEFORE, (AFUNPTR)OnBarrierFnEnter,
                           IARG_THREAD_ID, IARG_END);
            RTN_InsertCall(r, IPOINT_AFTER, (AFUNPTR)OnBarrierFnLeave,
                           IARG_THREAD_ID, IARG_END);
            RTN_Close(r);
            std::cerr << "[etrace] autosync hooked barrier function " << sym
                      << "\n";
        }
        for (UINT32 k = 0; k < 4; k++) {
            RTN r = RTN_FindByName(img, H[k].sym);
            if (!RTN_Valid(r)) continue;
            RTN_Open(r);
            RTN_InsertCall(r, H[k].after ? IPOINT_AFTER : IPOINT_BEFORE,
                           (AFUNPTR)OnAutoSync, IARG_THREAD_ID,
                           IARG_UINT32, H[k].type, IARG_UINT32, H[k].isEnd,
                           IARG_END);
            RTN_Close(r);
            std::cerr << "[etrace] autosync hooked " << H[k].sym
                      << (H[k].after ? " (after)" : " (before)") << "\n";
        }
    }

    RTN se = RTN_FindByName(img, KnobSyncEnd.Value().c_str());
    if (RTN_Valid(se)) { RTN_Open(se);
        RTN_InsertCall(se, IPOINT_BEFORE, (AFUNPTR)OnSync,
                       IARG_THREAD_ID,
                       IARG_FUNCARG_ENTRYPOINT_VALUE, 0,
                       IARG_UINT32, 1, IARG_END);
        RTN_Close(se); std::cerr << "[etrace] sync end hooked\n"; }
}

/* The opcode ids in the records are Pin-build specific, so emit a legend
 * mapping id -> mnemonic.  Written once; the converter reads it to key its
 * delay model on the mnemonic rather than a bare number. */
static VOID WriteOpcodeLegend()
{
    char buf[512];
    snprintf(buf, sizeof(buf), "%s.opcodes.txt", KnobOut.Value().c_str());
    std::ofstream f(buf);
    if (!f) return;
    for (UINT32 op = 0; op < XED_ICLASS_LAST; op++) {
        const std::string m = OPCODE_StringShort(op);
        if (!m.empty()) f << op << " " << m << "\n";
    }
    f.close();
    std::cerr << "[etrace] opcode legend -> " << buf << "\n";
}


static VOID ThreadStart(THREADID tid, CONTEXT* c, INT32 f, VOID* v)
{
    TState* s = new TState();
    char buf[512];
    snprintf(buf, sizeof(buf), "%s.tid%u.txt", KnobOut.Value().c_str(),
             (unsigned)tid);
    s->out.open(buf);
    PIN_SetThreadData(g_tls, s, tid);
    std::cerr << "[etrace] thread " << tid << " -> " << buf << "\n";
}

static VOID ThreadFini(THREADID tid, const CONTEXT* c, INT32 code, VOID* v)
{
    TState* s = TS(tid);
    if (s) { s->out.flush(); s->out.close();
             std::cerr << "[etrace] thread " << tid << ": "
                       << s->emitted << " records, "
                       << s->insts << " instructions, "
                       << s->syncs << " sync markers\n"; }
}

static VOID Fini(INT32 code, VOID* v)
{
    WriteOpcodeLegend();
    if (!g_uopTab.empty()) {
        UINT64 ok = 0, miss = 0;
        std::map<std::string, UINT64> missed;
        for (InstDesc* d : g_descs) {
            if (!d->isFirst || !d->execs) continue;
            if (d->g5ok) ok += d->execs;
            else { miss += d->execs; missed[d->g5name] += d->execs; }
        }
        std::vector<std::pair<UINT64, std::string>> top;
        for (auto& kv : missed) top.push_back({kv.second, kv.first});
        std::sort(top.rbegin(), top.rend());
        std::cerr << "[etrace] uoptable coverage: "
                  << (ok + miss ? 100.0 * ok / (ok + miss) : 0.0)
                  << "% of dynamic instructions; top unmatched:";
        for (size_t i = 0; i < top.size() && i < 12; i++)
            std::cerr << ' ' << top[i].second << '=' << top[i].first;
        std::cerr << '\n';
    }
    std::cerr << "[etrace] done\n";
}

[[maybe_unused]] static INT32 Usage()   // unused under SDE_INIT
{
    std::cerr << "Emit an elastic-trace-shaped dependency stream.\n"
              << KNOB_BASE::StringKnobSummary() << "\n";
    return -1;
}

int main(int argc, char* argv[])
{
    PIN_InitSymbols();
#if defined(SDE_INIT)
    // Same sequence as SDE's example-replay.cpp. Without the two tracing
    // calls the -replay switches parse but the replayer never activates:
    // nullapp runs natively (~136K instructions of loader) and exits 1.
    sde_pin_init(argc, argv);
    sde_tracing_flags(argc, argv);
    sde_init();
    sde_tracing_activate(argc, argv);
#else
    if (PIN_Init(argc, argv)) return Usage();
#endif
    g_tls = PIN_CreateThreadDataKey(NULL);
    PIN_InitLock(&g_lock);
    PIN_InitLock(&g_spawnLock);
    if (!KnobUopTable.Value().empty())
        LoadUopTable(KnobUopTable.Value());
    INS_AddInstrumentFunction(Instruction, 0);
    IMG_AddInstrumentFunction(Image, 0);
    PIN_AddThreadStartFunction(ThreadStart, 0);
    PIN_AddThreadFiniFunction(ThreadFini, 0);
    PIN_AddFiniFunction(Fini, 0);
    PIN_StartProgram();
    return 0;
}
