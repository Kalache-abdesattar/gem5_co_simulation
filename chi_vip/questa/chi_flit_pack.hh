#pragma once
// ── CHI flit pack / unpack ────────────────────────────────────────────────────
//
// Defines bit-field positions for each CHI channel flit and provides
// inline helpers to convert between sc_bv<N> wire values and the SCC
// chi_ctrl_extension / chi_data_extension / chi_snp_extension structs.
//
// Field positions follow IHI0050E (CHI-B) Table 13-x for the profile
// selected in chi_flit_cfg.hh.  If your DUT uses a different layout, edit
// the offset structs below — only the integer constants need to change.
//
// NOTE: fields that overlap in the CHI spec (e.g. ReturnNID/StashNID) are
// mapped to the same bit range; the correct semantic is determined by the
// opcode at the gem5 layer, not here.

#include <systemc>
#include <chi/chi_tlm.h>
#include <cstring>
#include "chi_flit_cfg.hh"

namespace chi_flit {

// ─────────────────────────────────────────────────────────────────────────────
// Bit-field helpers for sc_bv<N>
// ─────────────────────────────────────────────────────────────────────────────
// Generic bit-field helpers — work with both sc_bv<N> and sc_lv<N>.
template<typename V>
inline uint64_t bf_get(const V& v, int lo, int width) {
    return v.range(lo + width - 1, lo).to_uint64();
}

template<typename V>
inline void bf_set(V& v, int lo, int width, uint64_t val) {
    v.range(lo + width - 1, lo) = val & ((1ULL << width) - 1);
}

// Single-bit helpers.
template<typename V>
inline bool bf_getb(const V& v, int bit) {
    return v.get_bit(bit) != 0;
}
template<typename V>
inline void bf_setb(V& v, int bit, bool val) {
    v.set_bit(bit, val ? sc_dt::Log_1 : sc_dt::Log_0);
}

// ─────────────────────────────────────────────────────────────────────────────
// REQ flit field offsets  (IHI0050E Table 13-1, CHI-B standard profile)
// ─────────────────────────────────────────────────────────────────────────────
struct req_f {
    static constexpr int QOS_LO        = 0;
    static constexpr int QOS_W         = 4;
    static constexpr int TGTID_LO      = QOS_LO      + QOS_W;            //  4
    static constexpr int TGTID_W       = CHI_NODEID_W;
    static constexpr int SRCID_LO      = TGTID_LO    + TGTID_W;          // 11
    static constexpr int SRCID_W       = CHI_NODEID_W;
    static constexpr int TXNID_LO      = SRCID_LO    + SRCID_W;          // 18
    static constexpr int TXNID_W       = CHI_TXNID_W;
    // ReturnNID / StashNID share the same position (opcode determines semantics)
    static constexpr int RTNID_LO      = TXNID_LO    + TXNID_W;          // 30
    static constexpr int RTNID_W       = CHI_NODEID_W;
    static constexpr int STASHNIDV_LO  = RTNID_LO    + RTNID_W;          // 37
    // ReturnTxnID / StashLPID share the same 4-bit slot
    static constexpr int RTNTXNID_LO   = STASHNIDV_LO + 1;               // 38
    static constexpr int RTNTXNID_W    = CHI_LPID_W;                      //  4
    static constexpr int STASHLPIDV_LO = RTNTXNID_LO  + RTNTXNID_W;     // 42
    static constexpr int OPCODE_LO     = STASHLPIDV_LO + 1;              // 43
    static constexpr int OPCODE_W      = 7;
    static constexpr int SIZE_LO       = OPCODE_LO    + OPCODE_W;        // 50
    static constexpr int SIZE_W        = 3;
    static constexpr int ADDR_LO       = SIZE_LO      + SIZE_W;          // 53
    static constexpr int ADDR_W        = CHI_ADDR_W;
    static constexpr int NS_LO         = ADDR_LO      + ADDR_W;          // 97
    static constexpr int LIKELYSH_LO   = NS_LO        + 1;               // 98
    static constexpr int ALLOWRETRY_LO = LIKELYSH_LO  + 1;               // 99
    static constexpr int ORDER_LO      = ALLOWRETRY_LO + 1;              // 100
    static constexpr int ORDER_W       = 2;
    static constexpr int PCRDTYPE_LO   = ORDER_LO     + ORDER_W;         // 102
    static constexpr int PCRDTYPE_W    = 4;
    static constexpr int MEMATTR_LO    = PCRDTYPE_LO   + PCRDTYPE_W;    // 106
    static constexpr int MEMATTR_W     = 4;
    static constexpr int SNPATTR_LO    = MEMATTR_LO    + MEMATTR_W;     // 110
    static constexpr int DWT_LO        = SNPATTR_LO    + 1;              // 111
    // Excl and SnoopMe are alternative names for the same flit bit (IHI0050E
    // Table 13-1): Excl applies to ReadClean/ReadUnique/CleanUnique/MakeUnique
    // etc.; SnoopMe applies to WriteUnique*.  Both must be set on unpack.
    static constexpr int EXCL_LO       = DWT_LO        + 1;              // 112
    static constexpr int SNOOPME_LO    = EXCL_LO;                        // 112 (alias)
    static constexpr int EXPCOMPACK_LO = SNOOPME_LO    + 1;              // 113
    static constexpr int TAGOP_LO      = EXPCOMPACK_LO + 1;              // 114
    static constexpr int TAGOP_W       = 2;
    static constexpr int TRACETAG_LO   = TAGOP_LO      + TAGOP_W;       // 116
    static constexpr int TOTAL_W       = TRACETAG_LO   + 1;              // 117
};

// ─────────────────────────────────────────────────────────────────────────────
// RSP flit field offsets  (IHI0050E Table 13-3)
// ─────────────────────────────────────────────────────────────────────────────
struct rsp_f {
    static constexpr int QOS_LO       = 0;
    static constexpr int QOS_W        = 4;
    static constexpr int TGTID_LO     = QOS_LO     + QOS_W;             //  4
    static constexpr int TGTID_W      = CHI_NODEID_W;
    static constexpr int SRCID_LO     = TGTID_LO   + TGTID_W;           // 11
    static constexpr int SRCID_W      = CHI_NODEID_W;
    static constexpr int TXNID_LO     = SRCID_LO   + SRCID_W;           // 18
    static constexpr int TXNID_W      = CHI_TXNID_W;
    static constexpr int OPCODE_LO    = TXNID_LO   + TXNID_W;           // 30
    static constexpr int OPCODE_W     = 6;
    static constexpr int RESPERR_LO   = OPCODE_LO  + OPCODE_W;          // 36
    static constexpr int RESPERR_W    = 2;
    static constexpr int RESP_LO      = RESPERR_LO + RESPERR_W;         // 38
    static constexpr int RESP_W       = 3;
    static constexpr int FWDSTATE_LO  = RESP_LO    + RESP_W;            // 41
    static constexpr int FWDSTATE_W   = 3;
    static constexpr int DATAPULL_LO  = FWDSTATE_LO + FWDSTATE_W;      // 44
    static constexpr int DBID_LO      = DATAPULL_LO + 1;                // 45
    static constexpr int DBID_W       = CHI_TXNID_W;
    static constexpr int PCRDTYPE_LO  = DBID_LO    + DBID_W;            // 57
    static constexpr int PCRDTYPE_W   = 4;
    static constexpr int TAGOP_LO     = PCRDTYPE_LO + PCRDTYPE_W;       // 61
    static constexpr int TAGOP_W      = 2;
    static constexpr int TRACETAG_LO  = TAGOP_LO   + TAGOP_W;           // 63
    static constexpr int TOTAL_W      = TRACETAG_LO + 1;                 // 64
};

// ─────────────────────────────────────────────────────────────────────────────
// DAT flit field offsets  (IHI0050E Table 13-4)
// ─────────────────────────────────────────────────────────────────────────────
struct dat_f {
    static constexpr int QOS_LO       = 0;
    static constexpr int QOS_W        = 4;
    static constexpr int TGTID_LO     = QOS_LO     + QOS_W;             //  4
    static constexpr int TGTID_W      = CHI_NODEID_W;
    static constexpr int HOMEID_LO    = TGTID_LO   + TGTID_W;           // 11
    static constexpr int HOMEID_W     = CHI_NODEID_W;
    static constexpr int OPCODE_LO    = HOMEID_LO  + HOMEID_W;          // 18
    static constexpr int OPCODE_W     = 4;
    static constexpr int RESPERR_LO   = OPCODE_LO  + OPCODE_W;          // 22
    static constexpr int RESPERR_W    = 2;
    static constexpr int RESP_LO      = RESPERR_LO + RESPERR_W;         // 24
    static constexpr int RESP_W       = 3;
    static constexpr int FWDSTATE_LO  = RESP_LO    + RESP_W;            // 27
    static constexpr int FWDSTATE_W   = 3;
    static constexpr int DATAPULL_LO  = FWDSTATE_LO + FWDSTATE_W;      // 30
    static constexpr int DBID_LO      = DATAPULL_LO + 1;                // 31
    static constexpr int DBID_W       = CHI_TXNID_W;
    static constexpr int CCID_LO      = DBID_LO    + DBID_W;            // 43
    static constexpr int CCID_W       = 2;
    static constexpr int DATAID_LO    = CCID_LO    + CCID_W;            // 45
    static constexpr int DATAID_W     = 2;
    static constexpr int TAGOP_LO     = DATAID_LO  + DATAID_W;          // 47
    static constexpr int TAGOP_W      = 2;
    static constexpr int TXNID_LO     = TAGOP_LO   + TAGOP_W;           // 49
    static constexpr int TXNID_W      = CHI_TXNID_W;
    static constexpr int SRCID_LO     = TXNID_LO   + TXNID_W;           // 61
    static constexpr int SRCID_W      = CHI_NODEID_W;
    static constexpr int TRACETAG_LO  = SRCID_LO   + SRCID_W;           // 68
    // Byte-enable and data follow the header
    static constexpr int BE_LO        = TRACETAG_LO + 1;                // 69
    static constexpr int BE_W         = CHI_DATA_W_BYTES;
    static constexpr int DATA_LO      = BE_LO       + BE_W;             // 69+64=133 (default)
    static constexpr int DATA_W       = CHI_DATA_W_BITS;
    static constexpr int TOTAL_W      = DATA_LO     + DATA_W;           // 645 (default)
};

// ─────────────────────────────────────────────────────────────────────────────
// SNP flit field offsets  (IHI0050E Table 13-2 / CHIE_SNP_FLIT_* macros)
//
// Field order matches the RTL chie_defines.v layout exactly so the IPC
// wire can be forwarded to the DUT without per-field remapping:
//   [46:0]  — QOS/SRCID/TXNID/FWDNID/FWDTXNID/OPCODE  (bit-exact with RTL)
//   [90:47] — ADDR (CHI_SNP_ADDR_W bits = CHIE_SNP_ADDR_WIDTH_PARAM)
//             byte_addr >> 3 (cache-line address); rx_control.sv reconstructs
//             byte addr as {rxsnp_addr_r[width-6:0], 3'b000}
//   [93:91] — NS / DONOTGOTOSD / RETTOSRC
//   [94]    — TRACETAG
//   [101:95]— TGTID (identifies the target RN-F)
//
// Total: CHI_SNP_ADDR_W + CHI_NODEID_W*3 + 37 = 102 bits (default config)
// ─────────────────────────────────────────────────────────────────────────────
struct snp_f {
    static constexpr int QOS_LO          = 0;
    static constexpr int QOS_W           = 4;
    static constexpr int SRCID_LO        = QOS_LO        + QOS_W;        //  4
    static constexpr int SRCID_W         = CHI_NODEID_W;
    static constexpr int TXNID_LO        = SRCID_LO      + SRCID_W;      // 11
    static constexpr int TXNID_W         = CHI_TXNID_W;
    static constexpr int FWDNID_LO       = TXNID_LO      + TXNID_W;      // 23
    static constexpr int FWDNID_W        = CHI_NODEID_W;
    static constexpr int FWDTXNID_LO     = FWDNID_LO     + FWDNID_W;    // 30
    static constexpr int FWDTXNID_W      = CHI_TXNID_W;
    static constexpr int OPCODE_LO       = FWDTXNID_LO   + FWDTXNID_W;  // 42
    static constexpr int OPCODE_W        = 5;
    static constexpr int ADDR_LO         = OPCODE_LO      + OPCODE_W;    // 47
    static constexpr int ADDR_W          = CHI_SNP_ADDR_W;               // 44
    static constexpr int NS_LO           = ADDR_LO        + ADDR_W;      // 91
    static constexpr int DONOTGOTOSD_LO  = NS_LO           + 1;          // 92
    static constexpr int RETTOSRC_LO     = DONOTGOTOSD_LO  + 1;          // 93
    static constexpr int TRACETAG_LO     = RETTOSRC_LO     + 1;          // 94
    static constexpr int TGTID_LO        = TRACETAG_LO     + 1;          // 95
    static constexpr int TGTID_W         = CHI_NODEID_W;
    static constexpr int TOTAL_W         = TGTID_LO        + TGTID_W;    // 102
};

// ─────────────────────────────────────────────────────────────────────────────
// Flit-width constants (used as sc_bv<N> template arguments)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int CHI_REQ_FLIT_W = req_f::TOTAL_W;  // 117 (default config)
static constexpr int CHI_RSP_FLIT_W = rsp_f::TOTAL_W;  //  64
static constexpr int CHI_DAT_FLIT_W = dat_f::TOTAL_W;  // 645
static constexpr int CHI_SNP_FLIT_W = snp_f::TOTAL_W;  // 102

// ─────────────────────────────────────────────────────────────────────────────
// REQ flit ↔ chi_ctrl_extension (inbound: DUT RN-F → HN-F proxy)
// ─────────────────────────────────────────────────────────────────────────────
inline void unpack_req(const sc_dt::sc_bv<CHI_REQ_FLIT_W>& f,
                       tlm::tlm_generic_payload& gp,
                       chi::chi_ctrl_extension& e)
{
    using F = req_f;
    e.cmn.set_qos(   static_cast<uint8_t>( bf_get(f, F::QOS_LO,        F::QOS_W)));
    e.cmn.set_src_id(static_cast<unsigned>(bf_get(f, F::SRCID_LO,     F::SRCID_W)));
    e.cmn.set_txn_id(static_cast<unsigned>(bf_get(f, F::TXNID_LO,     F::TXNID_W)));
    e.req.set_tgt_id(static_cast<uint8_t>( bf_get(f, F::TGTID_LO,     F::TGTID_W)));
    e.req.set_opcode(static_cast<chi::req_optype_e>(bf_get(f, F::OPCODE_LO, F::OPCODE_W)));
    e.req.set_size(  static_cast<uint8_t>( bf_get(f, F::SIZE_LO,      F::SIZE_W)));
    e.req.set_non_secure(   bf_getb(f, F::NS_LO));
    e.req.set_likely_shared(bf_getb(f, F::LIKELYSH_LO));
    e.req.set_allow_retry(  bf_getb(f, F::ALLOWRETRY_LO));
    e.req.set_order(static_cast<uint8_t>( bf_get(f, F::ORDER_LO,      F::ORDER_W)));
    e.req.set_pcrd_type(static_cast<uint8_t>(bf_get(f, F::PCRDTYPE_LO, F::PCRDTYPE_W)));
    e.req.set_mem_attr(static_cast<uint8_t>(bf_get(f, F::MEMATTR_LO,   F::MEMATTR_W)));
    e.req.set_snp_attr(static_cast<bool>(   bf_get(f, F::SNPATTR_LO,   1)));
    e.req.set_exp_comp_ack( bf_getb(f, F::EXPCOMPACK_LO));
    // Excl and SnoopMe share the same flit bit; set both so gem5 sees the
    // correct value regardless of which field it queries for the given opcode.
    e.req.set_excl(         bf_getb(f, F::EXCL_LO));
    e.req.set_snoop_me(     bf_getb(f, F::SNOOPME_LO));
    e.req.set_trace_tag(    bf_getb(f, F::TRACETAG_LO));
    e.req.set_return_n_id(static_cast<uint16_t>(bf_get(f, F::RTNID_LO,    F::RTNID_W)));
    e.req.set_return_txn_id(static_cast<uint8_t>( bf_get(f, F::RTNTXNID_LO, F::RTNTXNID_W)));
    gp.set_address(static_cast<uint64_t>(bf_get(f, F::ADDR_LO, F::ADDR_W)));
    gp.set_command(tlm::TLM_IGNORE_COMMAND);
    gp.set_data_length(0);
    gp.set_data_ptr(nullptr);
    // NOTE: does NOT call gp.set_extension() — caller must do that if the
    // GP will be passed to b_transport (using a heap-allocated extension).
}

// ─────────────────────────────────────────────────────────────────────────────
// RSP flit ↔ chi_ctrl_extension (inbound: DUT → HN-F proxy)
// ─────────────────────────────────────────────────────────────────────────────
inline void unpack_rsp(const sc_dt::sc_bv<CHI_RSP_FLIT_W>& f,
                       tlm::tlm_generic_payload& gp,
                       chi::chi_ctrl_extension& e)
{
    using F = rsp_f;
    e.cmn.set_qos(    static_cast<uint8_t>( bf_get(f, F::QOS_LO,       F::QOS_W)));
    e.cmn.set_src_id(static_cast<unsigned>(bf_get(f, F::SRCID_LO,  F::SRCID_W)));
    e.cmn.set_txn_id(static_cast<unsigned>(bf_get(f, F::TXNID_LO,  F::TXNID_W)));
    e.resp.set_tgt_id(static_cast<uint16_t>(bf_get(f, F::TGTID_LO, F::TGTID_W)));
    e.resp.set_opcode(static_cast<chi::rsp_optype_e>(bf_get(f, F::OPCODE_LO, F::OPCODE_W)));
    e.resp.set_resp_err(static_cast<chi::rsp_resperrtype_e>(bf_get(f, F::RESPERR_LO, F::RESPERR_W)));
    e.resp.set_resp(  static_cast<chi::rsp_resptype_e>(bf_get(f, F::RESP_LO, F::RESP_W)));
    e.resp.set_fwd_state(static_cast<uint8_t>(bf_get(f, F::FWDSTATE_LO, F::FWDSTATE_W)));
    e.resp.set_data_pull(bf_getb(f, F::DATAPULL_LO));
    e.resp.set_db_id( static_cast<uint16_t>(bf_get(f, F::DBID_LO,    F::DBID_W)));
    e.resp.set_pcrd_type(static_cast<uint8_t>(bf_get(f, F::PCRDTYPE_LO, F::PCRDTYPE_W)));
    e.resp.set_trace_tag(bf_getb(f, F::TRACETAG_LO));
    gp.set_command(tlm::TLM_IGNORE_COMMAND);
    gp.set_data_length(0);
    gp.set_data_ptr(nullptr);
    // NOTE: does NOT call gp.set_extension() — see unpack_req comment above.
}

// ─────────────────────────────────────────────────────────────────────────────
// DAT flit ↔ chi_data_extension (inbound: DUT → HN-F proxy)
// ─────────────────────────────────────────────────────────────────────────────
inline void unpack_dat(const sc_dt::sc_lv<CHI_DAT_FLIT_W>& f,
                       tlm::tlm_generic_payload& gp,
                       chi::chi_data_extension& e,
                       uint8_t* data_buf /* CHI_DATA_W_BYTES bytes */)
{
    using F = dat_f;
    e.cmn.set_qos(    static_cast<uint8_t>( bf_get(f, F::QOS_LO,       F::QOS_W)));
    e.cmn.set_src_id(static_cast<unsigned>(bf_get(f, F::SRCID_LO, F::SRCID_W)));
    e.cmn.set_txn_id(static_cast<unsigned>(bf_get(f, F::TXNID_LO, F::TXNID_W)));
    e.dat.set_tgt_id( static_cast<uint16_t>(bf_get(f, F::TGTID_LO,   F::TGTID_W)));
    e.dat.set_home_n_id(static_cast<uint16_t>(bf_get(f, F::HOMEID_LO, F::HOMEID_W)));
    e.dat.set_opcode(static_cast<chi::dat_optype_e>(bf_get(f, F::OPCODE_LO, F::OPCODE_W)));
    e.dat.set_resp_err(static_cast<chi::rsp_resperrtype_e>(bf_get(f, F::RESPERR_LO, F::RESPERR_W)));
    e.dat.set_resp(  static_cast<chi::dat_resptype_e>(bf_get(f, F::RESP_LO,  F::RESP_W)));
    e.dat.set_fwd_state(static_cast<uint8_t>(bf_get(f, F::FWDSTATE_LO, F::FWDSTATE_W)));
    e.dat.set_data_pull(static_cast<uint8_t>(bf_getb(f, F::DATAPULL_LO)));
    e.dat.set_db_id( static_cast<uint16_t>(bf_get(f, F::DBID_LO,   F::DBID_W)));
    e.dat.set_cc_id( static_cast<uint8_t>( bf_get(f, F::CCID_LO,   F::CCID_W)));
    e.dat.set_data_id(static_cast<uint8_t>(bf_get(f, F::DATAID_LO, F::DATAID_W)));
    e.dat.set_trace_tag(bf_getb(f, F::TRACETAG_LO));
    // Extract byte-enables into data_buf's BE slot (we reuse data_buf for BE here)
    // and extract data bytes.
    for (int i = 0; i < CHI_DATA_W_BYTES; i++) {
        data_buf[i] = static_cast<uint8_t>(
            f.range(F::DATA_LO + i*8 + 7, F::DATA_LO + i*8).to_uint());
    }
    gp.set_command(tlm::TLM_WRITE_COMMAND);
    gp.set_data_ptr(data_buf);
    gp.set_data_length(static_cast<unsigned>(CHI_DATA_W_BYTES));
    // NOTE: does NOT call gp.set_extension() — see unpack_req comment above.
}

// ─────────────────────────────────────────────────────────────────────────────
// RSP flit pack  (outbound: HN-F proxy → DUT)
// ─────────────────────────────────────────────────────────────────────────────
inline void pack_rsp(const chi::chi_ctrl_extension& e,
                     sc_dt::sc_bv<CHI_RSP_FLIT_W>& f)
{
    using F = rsp_f;
    f = 0;
    bf_set(f, F::QOS_LO,     F::QOS_W,     e.cmn.get_qos());
    bf_set(f, F::SRCID_LO,   F::SRCID_W,   e.cmn.get_src_id());
    bf_set(f, F::TXNID_LO,   F::TXNID_W,   e.cmn.get_txn_id());
    bf_set(f, F::TGTID_LO,   F::TGTID_W,   e.resp.get_tgt_id());
    bf_set(f, F::OPCODE_LO,  F::OPCODE_W,  static_cast<uint64_t>(e.resp.get_opcode()));
    bf_set(f, F::RESPERR_LO, F::RESPERR_W, static_cast<uint64_t>(e.resp.get_resp_err()));
    bf_set(f, F::RESP_LO,    F::RESP_W,    static_cast<uint64_t>(e.resp.get_resp()));
    bf_set(f, F::FWDSTATE_LO,F::FWDSTATE_W,e.resp.get_fwd_state());
    bf_setb(f, F::DATAPULL_LO,             e.resp.get_data_pull());
    bf_set(f, F::DBID_LO,    F::DBID_W,   e.resp.get_db_id());
    bf_set(f, F::PCRDTYPE_LO,F::PCRDTYPE_W,e.resp.get_pcrd_type());
    bf_setb(f, F::TRACETAG_LO,             e.resp.is_trace_tag());
}

// ─────────────────────────────────────────────────────────────────────────────
// DAT flit pack  (outbound: HN-F proxy → DUT)
// ─────────────────────────────────────────────────────────────────────────────
inline void pack_dat(const chi::chi_data_extension& e,
                     const uint8_t* data_bytes,
                     int beat_bytes,
                     sc_dt::sc_lv<CHI_DAT_FLIT_W>& f)
{
    using F = dat_f;
    // Explicitly zero all bits in 32-bit chunks.  sc_lv<637> operator=(int 0)
    // is implementation-defined in Questa's SC runtime and may only clear the
    // first word, leaving DATA field bits (125-636) as 4-state 'x'.
    for (int lo = 0; lo < CHI_DAT_FLIT_W; lo += 32) {
        int hi = std::min(lo + 31, CHI_DAT_FLIT_W - 1);
        f.range(hi, lo) = 0;
    }
    bf_set(f, F::QOS_LO,      F::QOS_W,    e.cmn.get_qos());
    bf_set(f, F::SRCID_LO,    F::SRCID_W,  e.cmn.get_src_id());
    bf_set(f, F::TXNID_LO,    F::TXNID_W,  e.cmn.get_txn_id());
    bf_set(f, F::TGTID_LO,    F::TGTID_W,  e.dat.get_tgt_id());
    bf_set(f, F::HOMEID_LO,   F::HOMEID_W, e.dat.get_home_n_id());
    bf_set(f, F::OPCODE_LO,   F::OPCODE_W, static_cast<uint64_t>(e.dat.get_opcode()));
    bf_set(f, F::RESPERR_LO,  F::RESPERR_W,static_cast<uint64_t>(e.dat.get_resp_err()));
    bf_set(f, F::RESP_LO,     F::RESP_W,   static_cast<uint64_t>(e.dat.get_resp()));
    bf_set(f, F::FWDSTATE_LO, F::FWDSTATE_W, e.dat.get_fwd_state());
    bf_setb(f, F::DATAPULL_LO,              e.dat.get_data_pull() != 0);
    bf_set(f, F::DBID_LO,     F::DBID_W,   e.dat.get_db_id());
    bf_set(f, F::CCID_LO,     F::CCID_W,   e.dat.get_cc_id());
    bf_set(f, F::DATAID_LO,   F::DATAID_W, e.dat.get_data_id());
    bf_setb(f, F::TRACETAG_LO,             e.dat.is_trace_tag());
    // Byte enables: set all-ones for the active beat, zeros for padding
    for (int i = 0; i < CHI_DATA_W_BYTES; i++) {
        bf_setb(f, F::BE_LO + i, i < beat_bytes);
    }
    // Data bytes (zero-padded beyond beat_bytes)
    for (int i = 0; i < beat_bytes && i < CHI_DATA_W_BYTES; i++) {
        bf_set(f, F::DATA_LO + i*8, 8, data_bytes[i]);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SNP flit pack  (outbound: HN-F proxy → DUT)
// ─────────────────────────────────────────────────────────────────────────────
inline void pack_snp(const chi::chi_snp_extension& e,
                     uint64_t addr,
                     sc_dt::sc_bv<CHI_SNP_FLIT_W>& f,
                     uint16_t tgt_id = 0)
{
    using F = snp_f;
    f = 0;
    bf_set(f, F::QOS_LO,          F::QOS_W,      e.cmn.get_qos());
    bf_set(f, F::SRCID_LO,        F::SRCID_W,    e.cmn.get_src_id());
    bf_set(f, F::TXNID_LO,        F::TXNID_W,    e.cmn.get_txn_id());
    bf_set(f, F::FWDNID_LO,       F::FWDNID_W,   e.req.get_fwd_n_id());
    bf_set(f, F::FWDTXNID_LO,     F::FWDTXNID_W, e.req.get_fwd_txn_id());
    bf_set(f, F::OPCODE_LO,       F::OPCODE_W,    static_cast<uint64_t>(e.req.get_opcode()));
    bf_set(f, F::ADDR_LO,         F::ADDR_W,      addr >> 3);
    bf_setb(f, F::NS_LO,          e.req.is_non_secure());
    bf_setb(f, F::DONOTGOTOSD_LO, e.req.is_do_not_goto_sd());
    bf_setb(f, F::RETTOSRC_LO,    e.req.is_ret_to_src());
    bf_setb(f, F::TRACETAG_LO,    e.req.is_trace_tag());
    bf_set(f, F::TGTID_LO,        F::TGTID_W,    static_cast<uint64_t>(tgt_id));
}

} // namespace chi_flit
