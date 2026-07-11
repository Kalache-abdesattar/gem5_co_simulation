// chi_ipc.hh — POSIX shared-memory IPC protocol for gem5 ↔ Questa co-simulation
//
// Layout (ChiShmLayout placed at the start of a named shared-memory segment):
//
//   Q2G ring buffers (Questa vsim → gem5):
//       q2g_req  — REQ channel (DUT → HN-F)
//       q2g_rsp  — RSP channel (DUT → HN-F, e.g. CompAck / SnpResp)
//       q2g_dat  — DAT channel (DUT → HN-F, e.g. CopyBackWrData)
//
//   G2Q ring buffers (gem5 → Questa vsim):
//       g2q_rsp  — RSP channel (HN-F → DUT, e.g. Comp / DBIDResp)
//       g2q_dat  — DAT channel (HN-F → DUT, e.g. CompData)
//       g2q_snp  — SNP channel (HN-F → DUT)
//
//   Time sync counters:
//       gem5_grant_ps   — gem5 has processed up to this ps mark
//       questa_grant_ps — questa has processed up to this ps mark
//
// Ring buffers are SPSC (single-producer / single-consumer) with power-of-2
// capacity.  Indices are monotonically increasing 32-bit values; modular
// arithmetic via masking gives the array slot.
//
// NOTE: std::atomic<> in mmap'd shared memory is an extension (not strictly
// standard C++), but works reliably on x86/ARM Linux with glibc because
// atomic operations map to hardware instructions.

#pragma once
#include <atomic>
#include <cstddef>   // offsetof
#include <cstdint>
#include <cstring>
#include <string>

namespace gem5 { namespace ruby { namespace chi_ipc {

// ─────────────────────────────────────────────────────────────────────────────
// Wire-format structs.  All opcode fields carry raw SCC / CHI-spec integers.
// Sizes chosen to be naturally aligned (no hidden padding).
// ─────────────────────────────────────────────────────────────────────────────

// REQ channel  (q2g: DUT → gem5 HN-F)
struct ChiIpcReq {
    // ── CHI Issue D fields (offsets 0–23) ──────────────────────────────────
    uint64_t addr;          // cache-line address
    uint8_t  opcode;        // chi::req_optype_e integer
    uint8_t  _pad0;
    uint16_t txn_id;
    uint16_t src_id;
    uint16_t tgt_id;
    uint8_t  qos;
    uint8_t  ns;            // 1 = non-secure
    uint8_t  allow_retry;   // 1 = AllowRetry
    uint8_t  order;         // 2-bit Order field
    uint8_t  mem_attr;      // 4-bit MemAttr
    uint8_t  size;          // 3-bit Size (log2 bytes)
    uint8_t  snoop_me;      // 1 = SnoopMe
    uint8_t  exc_l;         // 1 = Exclusive
    uint8_t  pcrd_type;     // 4-bit PCrdType
    uint8_t  exp_comp_ack;  // 1 = ExpCompAck (RN-F will send CompAck)
    // ── CHI Issue E / E.b fields (offsets 24–35) — scaffolding, always 0 ──
    uint8_t  tag_op;        // [E]   TagOp[1:0]: 0=Invalid 1=Transfer 2=Update 3=Match
    uint8_t  pbha;          // [E]   Page-Based Hardware Attributes [3:0]
    uint16_t mpam;          // [E]   {MPAMNS[0], MPAMPartID[8:0], MPAMPerfMon[0]}
    uint16_t mecid;         // [E.b] Memory Encryption Context ID [15:0]
    uint8_t  cah;           // [E.b] Cache Allocation Hint
    uint8_t  trace_tag;     // [E.b] TraceTag: flag for system-level trace
    uint8_t  c_busy;        // [E.b] Component Busy [2:0]: pipeline pressure hint
    uint8_t  p_group_id;    // [E]   Persistence Group ID [7:0]
    uint8_t  stash_group_id;// [E]   Stash Group ID [7:0]
    uint8_t  tag_group_id;  // [E]   Tag Group ID [7:0]
    uint8_t  _pad[2];       // explicit padding to sizeof=40
};
static_assert(sizeof(ChiIpcReq) == 40, "ChiIpcReq layout changed — update protocol version");

// RSP channel  (q2g or g2q)
struct ChiIpcRsp {
    // ── CHI Issue D fields (offsets 0–13) ──────────────────────────────────
    uint8_t  opcode;        // chi::rsp_optype_e integer
    uint8_t  _pad0;
    uint16_t txn_id;
    uint16_t src_id;
    uint16_t tgt_id;
    uint8_t  qos;
    uint8_t  resp;          // chi::rsp_resptype_e integer
    uint8_t  resp_err;      // chi::rsp_resperrtype_e integer
    uint8_t  pcrd_type;
    uint16_t db_id;
    uint8_t  fwd_state;     // 3-bit FwdState (forwarding snoops)
    uint8_t  data_pull;     // 1 = DataPull request included
    // ── CHI Issue E / E.b fields (offsets 16–18) — scaffolding, always 0 ──
    uint8_t  tag_op;        // [E]   TagOp[1:0]: echoed tag operation
    uint8_t  trace_tag;     // [E.b] TraceTag: propagated trace marker
    uint8_t  c_busy;        // [E.b] Component Busy [2:0]
    uint8_t  _pad[1];       // explicit padding to sizeof=20
};
static_assert(sizeof(ChiIpcRsp) == 20, "ChiIpcRsp layout changed — update protocol version");

// DAT channel  (q2g or g2q)
struct ChiIpcDat {
    // ── CHI Issue D fields (offsets 0–24) ──────────────────────────────────
    uint64_t addr;
    uint8_t  opcode;        // chi::dat_optype_e integer
    uint8_t  _pad0;
    uint16_t txn_id;
    uint16_t src_id;
    uint16_t tgt_id;
    uint16_t home_n_id;
    uint16_t db_id;
    uint8_t  qos;
    uint8_t  resp;          // chi::dat_resptype_e integer
    uint8_t  resp_err;      // chi::rsp_resperrtype_e integer
    uint8_t  data_id;
    uint16_t data_len;      // number of valid data bytes (0 = no data)
    uint8_t  cc_id;         // 2-bit Critical Chunk ID
    // ── CHI Issue E / E.b fields (offsets 27–35) — scaffolding, always 0 ──
    uint8_t  tag_op;        // [E]   TagOp[1:0]: tag operation for this data flit
    uint8_t  trace_tag;     // [E.b] TraceTag: propagated trace marker
    uint8_t  c_busy;        // [E.b] Component Busy [2:0]
    uint8_t  data_source;   // [E]   DataSource[3:0]: 0=normal 1=prefetch 2=DRAM …
    uint8_t  tu;            // [E]   Tag Update mask [3:0]: 1 bit per 16B granule
    uint16_t mecid;         // [E.b] Memory Encryption Context ID [15:0]
    uint16_t tag;           // [E]   Memory tags: 4 bits per 16B granule (16 bits for 64B line)
    // ── Payload (data[] aligned at offset 36) ──────────────────────────────
    uint8_t  data[64];      // full cache-line payload (zero-padded)
    uint8_t  be[64];        // byte enables (0x00 or 0xff per byte)
};
static_assert(sizeof(ChiIpcDat) == 168, "ChiIpcDat layout changed — update protocol version");

// SNP channel  (g2q: gem5 HN-F → DUT)
struct ChiIpcSnp {
    // ── CHI Issue D fields (offsets 0–19) ──────────────────────────────────
    uint64_t addr;
    uint8_t  opcode;        // chi::snp_optype_e integer
    uint8_t  _pad0;
    uint16_t txn_id;
    uint16_t src_id;
    uint16_t fwd_n_id;
    uint16_t fwd_txn_id;
    uint8_t  qos;
    uint8_t  ns;
    uint8_t  ret_to_src;
    uint8_t  do_not_goto_sd;
    uint8_t  do_not_data_pull;
    // ── CHI Issue E / E.b fields (offsets 20–24) — scaffolding, always 0 ──
    uint8_t  tag_op;        // [E]   TagOp[1:0]: tag operation for snoop with tag transfer
    uint8_t  trace_tag;     // [E.b] TraceTag: propagated trace marker
    uint8_t  c_busy;        // [E.b] Component Busy [2:0]
    uint16_t mpam;          // [E]   MPAM[10:0]: forwarded partitioning context
    uint16_t tgt_id;        // target RN-F NodeID (used by questa_top_ipc to route g2q_snp)
    uint8_t  _pad2[2];      // explicit padding to sizeof=32
};
static_assert(sizeof(ChiIpcSnp) == 32, "ChiIpcSnp layout changed — update protocol version");

// ─────────────────────────────────────────────────────────────────────────────
// Single-producer / single-consumer ring buffer
// ─────────────────────────────────────────────────────────────────────────────
template<typename T, uint32_t N>
struct alignas(64) ChiRingBuf {
    static_assert((N & (N - 1)) == 0, "N must be a power of 2");
    static constexpr uint32_t MASK = N - 1;

    // head: monotonically increasing producer index (next write slot)
    alignas(64) std::atomic<uint32_t> head{0};
    // tail: monotonically increasing consumer index (next read slot)
    alignas(64) std::atomic<uint32_t> tail{0};
    T entries[N];

    // Producer: returns false if full.
    bool push(const T& v) {
        uint32_t h = head.load(std::memory_order_relaxed);
        if (h - tail.load(std::memory_order_acquire) == N)
            return false;  // full
        entries[h & MASK] = v;
        head.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer: returns false if empty.
    bool pop(T& v) {
        uint32_t t = tail.load(std::memory_order_relaxed);
        if (t == head.load(std::memory_order_acquire))
            return false;  // empty
        v = entries[t & MASK];
        tail.store(t + 1, std::memory_order_release);
        return true;
    }

    bool empty() const {
        return tail.load(std::memory_order_acquire) ==
               head.load(std::memory_order_acquire);
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// Shared memory layout — placed at offset 0 of the named shm segment
// ─────────────────────────────────────────────────────────────────────────────
struct ChiShmLayout {
    static constexpr int MAX_RNF = 8;

    // Advisory progress markers.
    alignas(64) std::atomic<uint64_t> gem5_grant_ps{0};
    alignas(64) std::atomic<uint64_t> questa_grant_ps{0};

    // RN-F configuration (written by gem5 VIPControllers during init).
    // num_rnf: number of active RN-Fs (0 until gem5 has written it).
    // rnf_nid[i]: RTL CHI NodeID for queue slot i.
    // questa_top_ipc reads num_rnf/rnf_nid to route incoming flits by src_id.
    alignas(64) std::atomic<uint32_t> num_rnf{0};
    uint16_t rnf_nid[MAX_RNF];
    uint8_t  _rnf_pad[64 - 4 - MAX_RNF * 2];  // pad header block to 64 bytes

    // Q2G: DUT → gem5 HN-F, one ring per RN-F indexed by rnf_index(src_id).
    ChiRingBuf<ChiIpcReq, 64> q2g_req[MAX_RNF];
    ChiRingBuf<ChiIpcRsp, 64> q2g_rsp[MAX_RNF];
    ChiRingBuf<ChiIpcDat, 32> q2g_dat[MAX_RNF];

    // G2Q: gem5 HN-F → DUT (shared; VIPController sets tgt_id per message).
    ChiRingBuf<ChiIpcRsp, 64> g2q_rsp;
    ChiRingBuf<ChiIpcDat, 32> g2q_dat;
    ChiRingBuf<ChiIpcSnp, 64> g2q_snp;

    // Test-completion signal.  Set by gem5's VIPController when the DUT
    // stores TOHOST_PASS (=1) or a FAIL sentinel to the tohost address
    // (0x80040000).  Questa polls this in clk_posedge and calls sc_stop()
    // when non-zero so both sides terminate on a single tohost event
    // without out-of-band process signalling.
    //
    // IMPORTANT: placed at the very END of the struct and WITHOUT
    // `alignas(64)`.  Any field added ABOVE this line (or any alignment
    // change to an existing field above) shifts the offsets of every
    // ring buffer, silently corrupting shm-mapped memory on the side that
    // wasn't fully rebuilt.  The static_asserts below pin the ring buffer
    // offsets so the compiler catches such drift.
    //   0                    = simulation still running
    //   TOHOST_PASS  (1)     = DUT signalled PASS
    //   0xFA11xxxxxxxxxxxx   = DUT signalled FAIL
    std::atomic<uint64_t> sim_done{0};

    // Map RTL SrcID → q2g queue index.  Returns 0 if not found (safe fallback).
    int rnf_index(uint16_t src_id) const {
        uint32_t n = num_rnf.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n && i < (uint32_t)MAX_RNF; ++i)
            if (rnf_nid[i] == src_id) return static_cast<int>(i);
        return 0;
    }
};

// Pin ring-buffer offsets so any future struct change that shifts them
// (accidental alignas, inserted field, changed padding) fails at compile time
// on either side.  The two sides *must* agree byte-for-byte on these offsets.
static_assert(offsetof(ChiShmLayout, q2g_req) == 192,
              "ChiShmLayout::q2g_req offset drifted — gem5 and Questa will "
              "read/write different bytes.  Add new fields at the END of the "
              "struct, after sim_done.");
static_assert(offsetof(ChiShmLayout, g2q_rsp) > offsetof(ChiShmLayout, q2g_dat),
              "ring buffer ordering changed — see comment above.");

// ─────────────────────────────────────────────────────────────────────────────
// Shared memory handle — RAII wrapper for shm_open / mmap
// ─────────────────────────────────────────────────────────────────────────────
class ChiShmHandle {
  public:
    // create=true  → shm_open(O_CREAT|O_RDWR) + ftruncate + mmap
    //                 (used by gem5 which starts first; resets layout to zero)
    // create=false → shm_open(O_RDWR) + mmap
    //                 (used by questa_top_ipc; retries until segment exists)
    ChiShmHandle(const std::string& name, bool create);
    ~ChiShmHandle();

    ChiShmLayout*       layout()       { return layout_; }
    const ChiShmLayout* layout() const { return layout_; }
    bool valid() const { return layout_ != nullptr; }

    // Try to open an existing segment (for the questa side).
    // Returns true on success; call this in a retry loop if needed.
    bool tryOpen();

    const std::string& name() const { return name_; }

  private:
    int           fd_     = -1;
    ChiShmLayout* layout_ = nullptr;
    std::string   name_;
    bool          owner_  = false;  // true if we created the segment

    ChiShmHandle(const ChiShmHandle&)            = delete;
    ChiShmHandle& operator=(const ChiShmHandle&) = delete;
};

}}} // namespace gem5::ruby::chi_ipc
