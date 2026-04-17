// questa_top_ipc.cc — IPC variant of questa_top for Questa vsim co-simulation
//
// This module runs inside the Questa vsim process.  It does NOT contain a
// CHI_VIP_Controller or any gem5 objects.  Communication with gem5 is
// entirely through the POSIX shared-memory ring buffers defined in chi_ipc.hh.
//
// Compilation (standalone):
//   See Makefile target 'questa_top_ipc.o'.
// Questa integration:
//   sccom -work work questa_top_ipc.cc [include flags] -lrt
//   (link with -lrt for POSIX shm on Linux)

#include "questa_top_ipc.hh"
#include <unistd.h>   // usleep
#include <cstring>

namespace gem5 { namespace ruby {

using namespace sc_core;
using namespace chi_flit;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
questa_top_ipc::questa_top_ipc(sc_module_name n,
                                const std::string& shm_name,
                                uint64_t cycle_ps)
    : sc_module(n)
    , shm_(shm_name, /*create=*/false)  // questa side attaches; gem5 creates
    , cycle_ps_(cycle_ps)
{
    SC_METHOD(clk_posedge);
    sensitive << clk.pos();
}

// ─────────────────────────────────────────────────────────────────────────────
// start_of_simulation: spin until gem5 has created the shm segment
// ─────────────────────────────────────────────────────────────────────────────
void questa_top_ipc::start_of_simulation()
{
    fprintf(stderr, "[questa_top_ipc] waiting for gem5 to create shm '%s'...\n",
            shm_.name().c_str());
    int retries = 0;
    while (!shm_.tryOpen()) {
        usleep(10000);  // 10 ms
        if (++retries % 100 == 0)
            fprintf(stderr, "[questa_top_ipc] still waiting (%ds)...\n",
                    retries / 100);
    }
    fprintf(stderr, "[questa_top_ipc] attached to shm, starting simulation\n");
}

// ─────────────────────────────────────────────────────────────────────────────
// Posedge handler
// ─────────────────────────────────────────────────────────────────────────────
void questa_top_ipc::clk_posedge()
{
    if (!shm_.valid()) {
        // Shouldn't happen after start_of_simulation, but be safe.
        return;
    }
    auto* shm = shm_.layout();

    // ── Synchronous reset ────────────────────────────────────────────────────
    if (!rstn.read()) {
        txreq_lcrdv.write(false);
        txrsp_lcrdv.write(false);
        txdat_lcrdv.write(false);
        rxrsp_flitv.write(false);   rxrsp_flitpend.write(false);
        rxdat_flitv.write(false);   rxdat_flitpend.write(false);
        rxsnp_flitv.write(false);   rxsnp_flitpend.write(false);
        rxrsp_flit.write(RspFlit(0));
        rxdat_flit.write(DatFlit(0));
        rxsnp_flit.write(SnpFlit(0));
        while (!rsp_out_q_.empty()) rsp_out_q_.pop();
        while (!dat_out_q_.empty()) dat_out_q_.pop();
        while (!snp_out_q_.empty()) snp_out_q_.pop();
        rsp_lcrd_ = dat_lcrd_ = snp_lcrd_ = 0;
        req_lcrd_init_ = rsp_lcrd_init_ = dat_lcrd_init_ = CHI_INIT_LCRD;
        questa_ps_ = 0;
        return;
    }

    questa_ps_ += cycle_ps_;
    shm->questa_grant_ps.store(questa_ps_,
                                std::memory_order_release);

    // ── Phase 1: init L-credits ───────────────────────────────────────────────
    bool send_req_crd = (req_lcrd_init_ > 0);
    bool send_rsp_crd = (rsp_lcrd_init_ > 0);
    bool send_dat_crd = (dat_lcrd_init_ > 0);
    if (req_lcrd_init_ > 0) --req_lcrd_init_;
    if (rsp_lcrd_init_ > 0) --rsp_lcrd_init_;
    if (dat_lcrd_init_ > 0) --dat_lcrd_init_;

    // ── Phase 2: consume DUT-granted credits ──────────────────────────────────
    if (rxrsp_lcrdv.read()) ++rsp_lcrd_;
    if (rxdat_lcrdv.read()) ++dat_lcrd_;
    if (rxsnp_lcrdv.read()) ++snp_lcrd_;

    // ── Phase 3: sample DUT TX flits → push into q2g ring buffers ───────────

    if (txreq_flitv.read()) {
        chi_ipc::ChiIpcReq msg = req_flit_to_ipc(txreq_flit.read());
        if (!shm->q2g_req.push(msg))
            fprintf(stderr, "[questa_top_ipc] q2g_req full — dropped REQ!\n");
        send_req_crd = true;
    }

    if (txrsp_flitv.read()) {
        chi_ipc::ChiIpcRsp msg = rsp_flit_to_ipc(txrsp_flit.read());
        if (!shm->q2g_rsp.push(msg))
            fprintf(stderr, "[questa_top_ipc] q2g_rsp full — dropped RSP!\n");
        send_rsp_crd = true;
    }

    if (txdat_flitv.read()) {
        chi_ipc::ChiIpcDat msg = dat_flit_to_ipc(txdat_flit.read());
        if (!shm->q2g_dat.push(msg))
            fprintf(stderr, "[questa_top_ipc] q2g_dat full — dropped DAT!\n");
        send_dat_crd = true;
    }

    txreq_lcrdv.write(send_req_crd);
    txrsp_lcrdv.write(send_rsp_crd);
    txdat_lcrdv.write(send_dat_crd);

    // ── Phase 4: drain g2q ring buffers → flit output queues ─────────────────
    {
        chi_ipc::ChiIpcRsp rsp;
        while (shm->g2q_rsp.pop(rsp))
            rsp_out_q_.push(ipc_to_rsp_flit(rsp));
    }
    {
        chi_ipc::ChiIpcDat dat;
        while (shm->g2q_dat.pop(dat))
            dat_out_q_.push(ipc_to_dat_flit(dat));
    }
    {
        chi_ipc::ChiIpcSnp snp;
        while (shm->g2q_snp.pop(snp))
            snp_out_q_.push(ipc_to_snp_flit(snp));
    }

    // ── Phase 5: drive outbound RX flits to DUT ──────────────────────────────

    // RXRSP
    if (!rsp_out_q_.empty() && rsp_lcrd_ > 0) {
        rxrsp_flitv.write(true);
        rxrsp_flit.write(rsp_out_q_.front());
        rsp_out_q_.pop();
        --rsp_lcrd_;
        rxrsp_flitpend.write(!rsp_out_q_.empty() && rsp_lcrd_ > 0);
    } else {
        rxrsp_flitv.write(false);
        rxrsp_flitpend.write(!rsp_out_q_.empty() && rsp_lcrd_ > 0);
    }

    // RXDAT
    if (!dat_out_q_.empty() && dat_lcrd_ > 0) {
        rxdat_flitv.write(true);
        rxdat_flit.write(dat_out_q_.front());
        dat_out_q_.pop();
        --dat_lcrd_;
        rxdat_flitpend.write(!dat_out_q_.empty() && dat_lcrd_ > 0);
    } else {
        rxdat_flitv.write(false);
        rxdat_flitpend.write(!dat_out_q_.empty() && dat_lcrd_ > 0);
    }

    // RXSNP
    if (!snp_out_q_.empty() && snp_lcrd_ > 0) {
        rxsnp_flitv.write(true);
        rxsnp_flit.write(snp_out_q_.front());
        snp_out_q_.pop();
        --snp_lcrd_;
        rxsnp_flitpend.write(!snp_out_q_.empty() && snp_lcrd_ > 0);
    } else {
        rxsnp_flitv.write(false);
        rxsnp_flitpend.write(!snp_out_q_.empty() && snp_lcrd_ > 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IPC → flit converters  (g2q direction: gem5 → DUT)
// ─────────────────────────────────────────────────────────────────────────────

RspFlit questa_top_ipc::ipc_to_rsp_flit(const chi_ipc::ChiIpcRsp& m)
{
    chi::chi_ctrl_extension e;
    e.cmn.set_qos(m.qos);
    e.cmn.set_src_id(m.src_id);
    e.cmn.set_txn_id(m.txn_id);
    e.resp.set_tgt_id(m.tgt_id);
    e.resp.set_opcode(static_cast<chi::rsp_optype_e>(m.opcode));
    e.resp.set_resp(  static_cast<chi::rsp_resptype_e>(m.resp));
    e.resp.set_resp_err(static_cast<chi::rsp_resperrtype_e>(m.resp_err));
    e.resp.set_pcrd_type(m.pcrd_type);
    e.resp.set_db_id(m.db_id);
    RspFlit f;
    pack_rsp(e, f);
    return f;
}

DatFlit questa_top_ipc::ipc_to_dat_flit(const chi_ipc::ChiIpcDat& m)
{
    chi::chi_data_extension e;
    e.cmn.set_qos(m.qos);
    e.cmn.set_src_id(m.src_id);
    e.cmn.set_txn_id(m.txn_id);
    e.dat.set_tgt_id(m.tgt_id);
    e.dat.set_home_n_id(m.home_n_id);
    e.dat.set_opcode(static_cast<chi::dat_optype_e>(m.opcode));
    e.dat.set_resp( static_cast<chi::dat_resptype_e>(m.resp));
    e.dat.set_resp_err(static_cast<chi::rsp_resperrtype_e>(m.resp_err));
    e.dat.set_db_id(m.db_id);
    e.dat.set_data_id(m.data_id);
    int beat_bytes = (m.data_len > 0 && m.data_len <= CHI_DATA_W_BYTES)
                   ? static_cast<int>(m.data_len)
                   : CHI_DATA_W_BYTES;
    DatFlit f;
    pack_dat(e, m.data, beat_bytes, f);
    // Override BE bits with the actual per-byte enables from the IPC struct.
    // pack_dat synthesises all-ones BE for active bytes; m.be[] carries the
    // real byte-enable mask from the gem5 memory operation.
    using F = chi_flit::dat_f;
    for (int i = 0; i < CHI_DATA_W_BYTES; i++)
        bf_setb(f, F::BE_LO + i, m.be[i] != 0);
    return f;
}

SnpFlit questa_top_ipc::ipc_to_snp_flit(const chi_ipc::ChiIpcSnp& m)
{
    chi::chi_snp_extension e;
    e.cmn.set_qos(m.qos);
    e.cmn.set_src_id(m.src_id);
    e.cmn.set_txn_id(m.txn_id);
    e.req.set_opcode(static_cast<chi::snp_optype_e>(m.opcode));
    e.req.set_fwd_n_id(m.fwd_n_id);
    e.req.set_fwd_txn_id(m.fwd_txn_id);
    e.req.set_non_secure(static_cast<bool>(m.ns));
    e.req.set_ret_to_src(static_cast<bool>(m.ret_to_src));
    e.req.set_do_not_goto_sd(static_cast<bool>(m.do_not_goto_sd));
    e.req.set_do_not_data_pull(static_cast<bool>(m.do_not_data_pull));
    SnpFlit f;
    pack_snp(e, m.addr, f);
    return f;
}

// ─────────────────────────────────────────────────────────────────────────────
// Flit → IPC converters  (q2g direction: DUT → gem5)
// ─────────────────────────────────────────────────────────────────────────────

chi_ipc::ChiIpcReq questa_top_ipc::req_flit_to_ipc(const ReqFlit& f)
{
    using F = chi_flit::req_f;
    chi_ipc::ChiIpcReq m{};
    m.addr        = static_cast<uint64_t>(bf_get(f, F::ADDR_LO, F::ADDR_W));
    m.opcode      = static_cast<uint8_t>( bf_get(f, F::OPCODE_LO, F::OPCODE_W));
    m.txn_id      = static_cast<uint8_t>( bf_get(f, F::TXNID_LO, F::TXNID_W));
    m.src_id      = static_cast<uint16_t>(bf_get(f, F::SRCID_LO, F::SRCID_W));
    m.tgt_id      = static_cast<uint16_t>(bf_get(f, F::TGTID_LO, F::TGTID_W));
    m.qos         = static_cast<uint8_t>( bf_get(f, F::QOS_LO, F::QOS_W));
    m.ns          = static_cast<uint8_t>( bf_getb(f, F::NS_LO));
    m.allow_retry = static_cast<uint8_t>( bf_getb(f, F::ALLOWRETRY_LO));
    m.order       = static_cast<uint8_t>( bf_get(f, F::ORDER_LO, F::ORDER_W));
    m.mem_attr    = static_cast<uint8_t>( bf_get(f, F::MEMATTR_LO, F::MEMATTR_W));
    m.size        = static_cast<uint8_t>( bf_get(f, F::SIZE_LO, F::SIZE_W));
    // EXCL_LO == SNOOPME_LO: same flit bit, alternative semantics by opcode
    m.exc_l       = static_cast<uint8_t>( bf_getb(f, F::EXCL_LO));
    m.snoop_me    = static_cast<uint8_t>( bf_getb(f, F::SNOOPME_LO));
    m.pcrd_type   = static_cast<uint8_t>( bf_get(f, F::PCRDTYPE_LO, F::PCRDTYPE_W));
    return m;
}

chi_ipc::ChiIpcRsp questa_top_ipc::rsp_flit_to_ipc(const RspFlit& f)
{
    using F = chi_flit::rsp_f;
    chi_ipc::ChiIpcRsp m{};
    m.opcode    = static_cast<uint8_t>( bf_get(f, F::OPCODE_LO, F::OPCODE_W));
    m.txn_id    = static_cast<uint8_t>( bf_get(f, F::TXNID_LO, F::TXNID_W));
    m.src_id    = static_cast<uint16_t>(bf_get(f, F::SRCID_LO, F::SRCID_W));
    m.tgt_id    = static_cast<uint16_t>(bf_get(f, F::TGTID_LO, F::TGTID_W));
    m.qos       = static_cast<uint8_t>( bf_get(f, F::QOS_LO, F::QOS_W));
    m.resp      = static_cast<uint8_t>( bf_get(f, F::RESP_LO, F::RESP_W));
    m.resp_err  = static_cast<uint8_t>( bf_get(f, F::RESPERR_LO, F::RESPERR_W));
    m.pcrd_type = static_cast<uint8_t>( bf_get(f, F::PCRDTYPE_LO, F::PCRDTYPE_W));
    m.db_id     = static_cast<uint16_t>(bf_get(f, F::DBID_LO, F::DBID_W));
    return m;
}

chi_ipc::ChiIpcDat questa_top_ipc::dat_flit_to_ipc(const DatFlit& f)
{
    using F = chi_flit::dat_f;
    chi_ipc::ChiIpcDat m{};
    // DAT flit has no ADDR field; gem5 recovers the address from the
    // outstanding TBE that matches the TxnID.  Set addr=0 here.
    m.addr      = 0;
    m.opcode    = static_cast<uint8_t>( bf_get(f, F::OPCODE_LO, F::OPCODE_W));
    m.txn_id    = static_cast<uint8_t>( bf_get(f, F::TXNID_LO, F::TXNID_W));
    m.src_id    = static_cast<uint16_t>(bf_get(f, F::SRCID_LO, F::SRCID_W));
    m.tgt_id    = static_cast<uint16_t>(bf_get(f, F::TGTID_LO, F::TGTID_W));
    m.home_n_id = static_cast<uint16_t>(bf_get(f, F::HOMEID_LO, F::HOMEID_W));
    m.db_id     = static_cast<uint16_t>(bf_get(f, F::DBID_LO, F::DBID_W));
    m.qos       = static_cast<uint8_t>( bf_get(f, F::QOS_LO, F::QOS_W));
    m.resp      = static_cast<uint8_t>( bf_get(f, F::RESP_LO, F::RESP_W));
    m.resp_err  = static_cast<uint8_t>( bf_get(f, F::RESPERR_LO, F::RESPERR_W));
    m.data_id   = static_cast<uint8_t>( bf_get(f, F::DATAID_LO, F::DATAID_W));
    m.data_len  = CHI_DATA_W_BYTES;
    for (int i = 0; i < CHI_DATA_W_BYTES; i++) {
        m.data[i] = static_cast<uint8_t>(
            f.range(F::DATA_LO + i*8 + 7, F::DATA_LO + i*8).to_uint());
        m.be[i]   = static_cast<uint8_t>(bf_getb(f, F::BE_LO + i) ? 0xff : 0x00);
    }
    return m;
}

} } // namespace gem5::ruby
