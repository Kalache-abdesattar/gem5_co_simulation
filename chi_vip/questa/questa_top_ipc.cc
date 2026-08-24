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
#include <cstddef>    // offsetof
#include <cstring>

// ── DPI-C data bypass ────────────────────────────────────────────────────────
// Questa's SC→SV bridge for sc_lv<637> only propagates bits 0-31 to SV.
// DATA bits (636:125) and BE bits (124:61) never reach the SV side via the
// sc_out port.  We bypass this by storing the pre-staged data in a C global
// and exposing a DPI-C export function that questa_top_ipc_wrap.sv calls to
// reconstruct the full flit in SV.
//
// questa_get_rxdat_data() is called from the SV wrapper's always block when
// rxdat_flit_sc or rxdat_flitv changes.  It returns DATA (512 bits = 16×uint32)
// and BE (64 bits = 2×uint32), both least-significant word first.
static uint8_t  s_rxdat_data[64] = {};
static uint64_t s_rxdat_be       = 0;

extern "C" {
// svBitVecVal = uint32_t; defined in svdpi.h (which sccom finds automatically)
// We declare it manually to avoid requiring the explicit include.
typedef unsigned int BvVal;

void questa_get_rxdat_data(BvVal* data_w, BvVal* be_w)
{
    // 512 bits of DATA → 16 × uint32, little-endian word order (word 0 = bits 31:0)
    for (int w = 0; w < 16; w++) {
        uint32_t v = 0;
        for (int b = 0; b < 4; b++) {
            int idx = w * 4 + b;
            if (idx < 64) v |= (uint32_t)s_rxdat_data[idx] << (b * 8);
        }
        data_w[w] = v;
    }
    // 64 bits of BE → 2 × uint32
    be_w[0] = (uint32_t)(s_rxdat_be & 0xFFFFFFFFu);
    be_w[1] = (uint32_t)(s_rxdat_be >> 32);
}
} // extern "C"

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
    SC_THREAD(clk_thread);
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
    // Read the co-sim time-quantum published by gem5's VIPController-0.  Falls
    // back to the compiled default (10000 ps) if gem5 hasn't written it yet.
    if (auto* shm = shm_.layout()) {
        // gem5 has published quantum_ps by now (written in VIPController-0 init,
        // long before Questa finishes elaboration).  Accept 0 too: 0 = loose
        // mode (barrier disabled).
        quantum_ps_ = shm->quantum_ps.load(std::memory_order_relaxed);
    }
    fprintf(stderr, "[questa_top_ipc] attached to shm, starting simulation"
            " (quantum_ps_=%lu)\n", (unsigned long)quantum_ps_);
    // Layout probe: must match the gem5-side "[VIP:layout]" line.  A mismatch
    // means the two sides disagree on the ChiIpcDat byte layout, and any
    // field beyond the first mismatched offset will be read from the wrong
    // bytes of the shm slot.
    fprintf(stderr,
        "[questa_top_ipc:layout] sizeof(ChiIpcDat)=%zu"
        " off(src_id)=%zu off(tgt_id)=%zu off(home_n_id)=%zu\n",
        sizeof(chi_ipc::ChiIpcDat),
        offsetof(chi_ipc::ChiIpcDat, src_id),
        offsetof(chi_ipc::ChiIpcDat, tgt_id),
        offsetof(chi_ipc::ChiIpcDat, home_n_id));
}

// ─────────────────────────────────────────────────────────────────────────────
// SC_THREAD entry point — runs the per-cycle body on every posedge, then at
// each quantum boundary publishes our progress and spins (yielding to the SC
// kernel, without advancing the RTL clock) until gem5 is within one quantum.
// Deadlock-free: each side only reads the *other* side's counter and advances
// freely within its own quantum.
// ─────────────────────────────────────────────────────────────────────────────
void questa_top_ipc::clk_thread()
{
    while (true) {
        wait(clk.posedge_event());
        clk_posedge_impl();

        if (quantum_ps_ > 0 && questa_ps_ > 0 &&
            (questa_ps_ % quantum_ps_) == 0) {  // quantum_ps_==0 → loose, no barrier
            auto* shm = shm_.layout();
            if (!shm) continue;
            // Pick up a runtime quantum change (gem5 owns quantum_ps).
            uint64_t q = shm->quantum_ps.load(std::memory_order_relaxed);
            if (q > 0) quantum_ps_ = q;

            shm->questa_quantum_end_ps.store(questa_ps_,
                                             std::memory_order_release);
            // Block until gem5's published quantum end is within one quantum of
            // us.  wait(1, SC_PS) yields to the SC kernel without advancing the
            // RTL clock, so DUT state is preserved while we wait.
            while (shm->gem5_quantum_end_ps.load(std::memory_order_acquire)
                       + quantum_ps_ < questa_ps_) {
                wait(1, sc_core::SC_PS);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-cycle body (phases 1-5, reset, credit init) — called from clk_thread
// ─────────────────────────────────────────────────────────────────────────────
void questa_top_ipc::clk_posedge_impl()
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
        memset(s_rxdat_data, 0, sizeof(s_rxdat_data));
        s_rxdat_be = 0;
        rsp_lcrd_ = dat_lcrd_ = snp_lcrd_ = 0;
        dat_tx_cooldown_ = 0;
        pending_extra_dat_crd_ = 0;
        snp_lcrd_preloaded_ = false;
        rsp_lcrd_preloaded_ = false;
        req_lcrd_init_ = rsp_lcrd_init_ = dat_lcrd_init_ = CHI_INIT_LCRD;
        questa_ps_ = 0;
        return;
    }

    questa_ps_ += cycle_ps_;
    shm->questa_grant_ps.store(questa_ps_,
                                std::memory_order_release);

    // Test-completion signal from gem5.  When the DUT stores to tohost
    // (0x80040000), gem5's VIPController publishes the value here and
    // calls exitSimLoop.  Calling sc_stop() ends vsim's `run -all` so
    // Questa exits cleanly in the same cycle.  See chi_ipc.hh for the
    // sim_done placement + static_assert protecting shm layout.
    if (uint64_t done = shm->sim_done.load(std::memory_order_acquire)) {
        fprintf(stderr,
            "[questa_top_ipc] sim_done=0x%016lx observed — sc_stop()\n",
            (unsigned long)done);
        sc_core::sc_stop();
        return;
    }

    if (dat_tx_cooldown_ > 0) --dat_tx_cooldown_;

    // SNP credits come from CVA6 (RN-F); pre-load on first active cycle in case
    // CVA6 does not pulse hnf_txsnplcrdv during link init.
    if (!snp_lcrd_preloaded_) {
        snp_lcrd_ += CHI_INIT_LCRD;
        snp_lcrd_preloaded_ = true;
        fprintf(stderr, "[ipc] SNP credit pre-load: snp_lcrd_=%u @%lupc\n",
                snp_lcrd_, (unsigned long)questa_ps_);
    }

    // RSP credits (rxrsp_lcrdv) are only asserted by the testbench while a
    // wait_rsp task is active.  For HN-F responses to autonomously-generated
    // RN-F transactions (e.g. CVA6 cache-controller WriteBackPtl), no
    // wait_rsp is running, rsp_lcrd_ stays 0, and CompDBIDResp is never sent.
    // Pre-load rsp_lcrd_ at init so the HN-F can issue RSP flits at any time.
    if (!rsp_lcrd_preloaded_) {
        rsp_lcrd_ += CHI_INIT_LCRD;
        rsp_lcrd_preloaded_ = true;
        fprintf(stderr, "[ipc] RSP credit pre-load: rsp_lcrd_=%u @%lupc\n",
                rsp_lcrd_, (unsigned long)questa_ps_);
    }

    // ── Phase 1: init L-credits ───────────────────────────────────────────────
    bool send_req_crd = (req_lcrd_init_ > 0);
    bool send_rsp_crd = (rsp_lcrd_init_ > 0);
    bool send_dat_crd = (dat_lcrd_init_ > 0) || (pending_extra_dat_crd_ > 0);
    if (req_lcrd_init_ > 0) --req_lcrd_init_;
    if (rsp_lcrd_init_ > 0) --rsp_lcrd_init_;
    if (dat_lcrd_init_ > 0) --dat_lcrd_init_;
    if (pending_extra_dat_crd_ > 0) --pending_extra_dat_crd_;

    // ── Phase 2: consume DUT-granted credits ──────────────────────────────────
    if (rxrsp_lcrdv.read()) ++rsp_lcrd_;
    if (rxdat_lcrdv.read()) {
        ++dat_lcrd_;
        fprintf(stderr, "[ipc] Phase2 rxdat_lcrdv=1 → dat_lcrd_=%u @%lupc\n",
                dat_lcrd_, (unsigned long)questa_ps_);
    }
    if (rxsnp_lcrdv.read()) {
        ++snp_lcrd_;
        fprintf(stderr, "[ipc] Phase2 rxsnp_lcrdv=1 → snp_lcrd_=%u @%lupc\n",
                snp_lcrd_, (unsigned long)questa_ps_);
    }

    // ── Phase 3: sample DUT TX flits → push into q2g ring buffers ───────────

    if (txreq_flitv.read()) {
        chi_ipc::ChiIpcReq msg = req_flit_to_ipc(txreq_flit.read());
        if (msg.opcode == 0) {
            // ReqLCrdReturn (opcode 0x00): credit-return flit, not a real
            // request. Do NOT push to ring buffer or return a credit (that
            // would inflate the credit count).
            fprintf(stderr, "[ipc] Phase3 ReqLCrdReturn src=%u @%lupc\n",
                    (unsigned)msg.src_id, (unsigned long)questa_ps_);
        } else {
            int idx = shm->rnf_index(msg.src_id);
            fprintf(stderr, "[ipc] Phase3 TXREQ op=0x%02x txn=%u src=%u addr=0x%lx → q2g_req[%d] @%lupc\n",
                    msg.opcode, msg.txn_id, (unsigned)msg.src_id,
                    (unsigned long)msg.addr, idx, (unsigned long)questa_ps_);
            if (!shm->q2g_req[idx].push(msg))
                fprintf(stderr, "[questa_top_ipc] q2g_req[%d] full — dropped REQ src=%u!\n",
                        idx, (unsigned)msg.src_id);
            send_req_crd = true;
        }
    }

    if (txrsp_flitv.read()) {
        chi_ipc::ChiIpcRsp msg = rsp_flit_to_ipc(txrsp_flit.read());
        int idx = shm->rnf_index(msg.src_id);
        if (!shm->q2g_rsp[idx].push(msg))
            fprintf(stderr, "[questa_top_ipc] q2g_rsp[%d] full — dropped RSP src=%u!\n",
                    idx, (unsigned)msg.src_id);
        send_rsp_crd = true;
    }

    if (txdat_flitv.read()) {
        chi_ipc::ChiIpcDat msg = dat_flit_to_ipc(txdat_flit.read());
        if (msg.opcode == 0) {
            // DatLCrdReturn (opcode 0x00): credit-return flit, not real data —
            // its DATA/BE lines carry stale bus values.  Mirror the REQ-channel
            // handling: do NOT push to the ring buffer (gem5 would misread the
            // stale payload as NCBWrData → addr 0 → panic) and do NOT return a
            // credit or bump the extra-beat counter (that would inflate credits).
            fprintf(stderr, "[ipc] Phase3 DatLCrdReturn src=%u @%lupc\n",
                    (unsigned)msg.src_id, (unsigned long)questa_ps_);
        } else {
            int idx = shm->rnf_index(msg.src_id);
            // DIAGNOSTIC: reveal the DUT DAT beat structure so we can size the
            // reassembly for narrow (128/256-bit) RTL data buses.  be_bytes = how
            // many bytes this flit actually drove; data_id encodes the beat's
            // byte offset (CHI: offset = data_id*16).  For a full-line 512-bit
            // flit be_bytes==64, data_id==0; a 256-bit bus yields be_bytes==32
            // with data_id 0 then 2.
            int be_bytes = 0;
            for (int i = 0; i < CHI_DATA_W_BYTES; i++) be_bytes += (msg.be[i] != 0);
            fprintf(stderr,
                    "[ipc] Phase3 TXDAT op=0x%02x txn=%u src=%u data_id=%u"
                    " cc_id=%u be_bytes=%d resp=0x%02x @%lupc\n",
                    msg.opcode, msg.txn_id, (unsigned)msg.src_id,
                    (unsigned)msg.data_id, (unsigned)msg.cc_id, be_bytes,
                    msg.resp, (unsigned long)questa_ps_);
            if (!shm->q2g_dat[idx].push(msg))
                fprintf(stderr, "[questa_top_ipc] q2g_dat[%d] full — dropped DAT src=%u!\n",
                        idx, (unsigned)msg.src_id);
            send_dat_crd = true;
            // Return one link-credit per RTL beat the ICN spent for this line.
            // A full-line data flit spans CHI_BEATS_PER_LINE beats (= 512/RTL
            // bus width: 512b→1, 256b→2, 128b→4); NCBWrData (opcode 0x3) is a
            // single-beat partial write regardless.  send_dat_crd returns the
            // first credit now; the rest are deferred one-per-cycle.  Hardcoding
            // this to 2 only worked at 256b — it over-credits at 512b (deadlock)
            // and starves at 128b.
            if (msg.opcode != 0x3)
                pending_extra_dat_crd_ += (CHI_BEATS_PER_LINE - 1);
        }
    }

    txreq_lcrdv.write(send_req_crd);
    txrsp_lcrdv.write(send_rsp_crd);
    txdat_lcrdv.write(send_dat_crd);

    // ── Phase 4: drain g2q ring buffers → flit output queues ─────────────────
    {
        chi_ipc::ChiIpcRsp rsp;
        while (shm->g2q_rsp.pop(rsp)) {
            fprintf(stderr, "[ipc] Phase4 g2q_rsp pop: txn=%u tgt=%u op=0x%02x rsp_lcrd_=%u @%lupc\n",
                    rsp.txn_id, rsp.tgt_id, rsp.opcode, rsp_lcrd_,
                    (unsigned long)questa_ps_);
            rsp_out_q_.push(ipc_to_rsp_flit(rsp));
        }
    }
    {
        chi_ipc::ChiIpcDat dat;
        while (shm->g2q_dat.pop(dat)) {
            fprintf(stderr,
                    "[ipc] Phase4 g2q_dat pop: txn=%u src=%u tgt=%u home=%u"
                    " op=%u dat_lcrd_=%u @%lupc\n",
                    dat.txn_id, dat.src_id, dat.tgt_id, dat.home_n_id,
                    dat.opcode, dat_lcrd_,
                    (unsigned long)questa_ps_);
            dat_out_q_.push(ipc_to_dat_flit(dat));
        }
    }
    {
        chi_ipc::ChiIpcSnp snp;
        while (shm->g2q_snp.pop(snp)) {
            fprintf(stderr, "[ipc] Phase4 g2q_snp pop: txn=%u src=%u op=%u addr=0x%lx snp_lcrd_=%u @%lupc\n",
                    snp.txn_id, snp.src_id, snp.opcode,
                    (unsigned long)snp.addr, snp_lcrd_,
                    (unsigned long)questa_ps_);
            snp_out_q_.push(ipc_to_snp_flit(snp));
        }
    }

    // ── Phase 5: drive outbound RX flits to DUT ──────────────────────────────

    // RXRSP
    if (!rsp_out_q_.empty() && rsp_lcrd_ > 0) {
        fprintf(stderr, "[ipc] Phase5 rxrsp_flitv=1 rsp_lcrd_ %u→%u @%lupc\n",
                rsp_lcrd_, rsp_lcrd_ - 1, (unsigned long)questa_ps_);
        rxrsp_flitv.write(true);
        rxrsp_flit.write(rsp_out_q_.front());
        rsp_out_q_.pop();
        --rsp_lcrd_;
        rxrsp_flitpend.write(!rsp_out_q_.empty() && rsp_lcrd_ > 0);
    } else {
        if (!rsp_out_q_.empty()) {
            fprintf(stderr, "[ipc] Phase5 RXRSP STALLED: rsp_out_q_.size=%zu rsp_lcrd_=0 @%lupc\n",
                    rsp_out_q_.size(), (unsigned long)questa_ps_);
        }
        rxrsp_flitv.write(false);
        rxrsp_flitpend.write(!rsp_out_q_.empty() && rsp_lcrd_ > 0);
    }

    // RXDAT — single-cycle.
    // DATA (bits 636:125) and BE (bits 124:61) are transferred to SV via the
    // DPI bypass function questa_get_rxdat_data(), which the SV wrapper calls
    // from its always block when rxdat_flit_sc changes.  The globals must be
    // populated BEFORE rxdat_flit.write() triggers that always block.
    if (!dat_out_q_.empty() && dat_lcrd_ == 0) {
        fprintf(stderr, "[ipc] Phase5 DAT STALLED: dat_out_q_.size=%zu dat_lcrd_=0 @%lupc\n",
                dat_out_q_.size(), (unsigned long)questa_ps_);
    }
    // One IPC rxdat flit is split by the SV adapter into CHI_BEATS_PER_LINE RTL
    // beats, each consuming one DUT rxdat credit — so require (and consume) that
    // many credits, not 1.  Sending on dat_lcrd_>0 with only a 1-credit debit
    // over-sends at >1 beat (512b→1 was fine; 128b→4 drops CompData beats).
    if (!dat_out_q_.empty() && dat_lcrd_ >= CHI_BEATS_PER_LINE && dat_tx_cooldown_ == 0) {
        const DatFlit& nxt = dat_out_q_.front();
        using F2 = chi_flit::dat_f;
        // Populate DPI globals (bypasses SC→SV bridge truncation of sc_lv<637>).
        s_rxdat_be = 0;
        for (int i = 0; i < 64; i++) {
            if (chi_flit::bf_getb(nxt, F2::BE_LO + i))
                s_rxdat_be |= (1ULL << i);
        }
        for (int i = 0; i < 64; i++) {
            s_rxdat_data[i] = static_cast<uint8_t>(
                nxt.range(F2::DATA_LO + i*8 + 7, F2::DATA_LO + i*8).to_uint());
        }
        fprintf(stderr, "[ipc] Phase5 rxdat_flitv=1 dat_lcrd_ %u→%u @%lupc\n",
                dat_lcrd_, dat_lcrd_ - CHI_BEATS_PER_LINE, (unsigned long)questa_ps_);
        rxdat_flitv.write(true);
        rxdat_flit.write(nxt);
        dat_out_q_.pop();
        dat_lcrd_ -= CHI_BEATS_PER_LINE;
        // dat_rx_sm (always_ff) takes latch + CHI_BEATS_PER_LINE emit cycles to
        // drain (512b→2, 256b→3, 128b→5).  Hardcoding 3 let the next IPC flit
        // overwrite dat_rx_latch mid-emission at 4 beats, losing CompData tail.
        dat_tx_cooldown_ = CHI_BEATS_PER_LINE + 1;
        rxdat_flitpend.write(!dat_out_q_.empty() && dat_lcrd_ >= CHI_BEATS_PER_LINE);
    } else {
        rxdat_flitv.write(false);
        rxdat_flitpend.write(!dat_out_q_.empty() && dat_lcrd_ >= CHI_BEATS_PER_LINE && dat_tx_cooldown_ == 0);
    }

    // RXSNP
    if (!snp_out_q_.empty() && snp_lcrd_ == 0) {
        fprintf(stderr, "[ipc] Phase5 SNP STALLED: snp_out_q_.size=%zu snp_lcrd_=0 @%lupc\n",
                snp_out_q_.size(), (unsigned long)questa_ps_);
    }
    if (!snp_out_q_.empty() && snp_lcrd_ > 0) {
        using F = chi_flit::snp_f;
        const SnpFlit& nxt = snp_out_q_.front();
        fprintf(stderr,
            "[ipc] Phase5 rxsnp_flitv=1 snp_lcrd_ %u→%u"
            " addr=0x%lx op=%u txn=%u @%lupc\n",
            snp_lcrd_, snp_lcrd_ - 1,
            (unsigned long)chi_flit::bf_get(nxt, F::ADDR_LO, F::ADDR_W),
            (unsigned)chi_flit::bf_get(nxt, F::OPCODE_LO, F::OPCODE_W),
            (unsigned)chi_flit::bf_get(nxt, F::TXNID_LO,  F::TXNID_W),
            (unsigned long)questa_ps_);
        rxsnp_flitv.write(true);
        rxsnp_flit.write(nxt);
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
    e.resp.set_fwd_state(m.fwd_state);
    e.resp.set_data_pull(static_cast<bool>(m.data_pull));
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
    e.dat.set_cc_id(m.cc_id);
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

    // Debug: verify data bits are packed correctly (remove after fixing)

    return f;
}

// ─────────────────────────────────────────────────────────────────────────────
// SNP opcode translation: gem5 CHI-B/C IPC values → RTL CHI-E (chie_defines.v)
//
// gem5's VIPController::gem5ToSccSnp() was written against an older CHI issue
// whose SNP opcode table differs from CHI Issue E used by the RTL.  This table
// corrects every snoop opcode before the flit is packed and driven to the DUT.
//
// gem5-IPC  gem5 name              CHI-E RTL  RTL name
// ────────  ─────────────────────  ─────────  ─────────────────────
//   0x03    SnpOnce                  0x03      CHIE_SNPONCE          (same)
//   0x04    SnpShared                0x01      CHIE_SNPSHARED
//   0x07    SnpUnique                0x07      CHIE_SNPUNIQUE        (same)
//   0x08    SnpCleanInvalid          0x09      CHIE_SNPCLEANINVALID
//   0x0B    SnpOnceFwd               0x13      CHIE_SNPONCEFWD
//   0x0C    SnpSharedFwd             0x11      CHIE_SNPSHAREDFWD
//   0x0D    SnpUniqueFwd             0x17      CHIE_SNPUNIQUEFWD
//   0x0E    SnpNotSharedDirtyFwd     0x14      CHIE_SNPNOTSHAREDDIRTYFWD
//   0x14    SnpDvmOp (all variants)  0x0D      CHIE_SNPDVMOP
// ─────────────────────────────────────────────────────────────────────────────
static uint8_t translate_snp_opcode(uint8_t gem5_opc)
{
    switch (gem5_opc) {
    case 0x03: return 0x03;  // SnpOnce              → CHIE_SNPONCE          (unchanged)
    case 0x04: return 0x01;  // SnpShared            → CHIE_SNPSHARED
    case 0x07: return 0x07;  // SnpUnique            → CHIE_SNPUNIQUE        (unchanged)
    case 0x08: return 0x09;  // SnpCleanInvalid      → CHIE_SNPCLEANINVALID
    case 0x0B: return 0x13;  // SnpOnceFwd           → CHIE_SNPONCEFWD
    case 0x0C: return 0x11;  // SnpSharedFwd         → CHIE_SNPSHAREDFWD
    case 0x0D: return 0x17;  // SnpUniqueFwd         → CHIE_SNPUNIQUEFWD
    case 0x0E: return 0x14;  // SnpNotSharedDirtyFwd → CHIE_SNPNOTSHAREDDIRTYFWD
    case 0x14: return 0x0D;  // SnpDvmOp (all)       → CHIE_SNPDVMOP
    default:
        fprintf(stderr,
            "[ipc] translate_snp_opcode: unknown gem5 SNP opcode 0x%02x"
            " — forwarding as-is (likely RTL decode error)\n", gem5_opc);
        return gem5_opc;
    }
}

SnpFlit questa_top_ipc::ipc_to_snp_flit(const chi_ipc::ChiIpcSnp& m)
{
    const uint8_t rtl_opc = translate_snp_opcode(m.opcode);

    chi::chi_snp_extension e;
    e.cmn.set_qos(m.qos);
    e.cmn.set_src_id(m.src_id);
    e.cmn.set_txn_id(m.txn_id);
    e.req.set_opcode(static_cast<chi::snp_optype_e>(rtl_opc));
    e.req.set_fwd_n_id(m.fwd_n_id);
    e.req.set_fwd_txn_id(m.fwd_txn_id);
    e.req.set_non_secure(static_cast<bool>(m.ns));
    e.req.set_ret_to_src(static_cast<bool>(m.ret_to_src));
    e.req.set_do_not_goto_sd(static_cast<bool>(m.do_not_goto_sd));
    e.req.set_do_not_data_pull(static_cast<bool>(m.do_not_data_pull));
    SnpFlit f;
    pack_snp(e, m.addr, f, m.tgt_id);

    using F = chi_flit::snp_f;
    uint64_t f_addr = chi_flit::bf_get(f, F::ADDR_LO, F::ADDR_W);
    fprintf(stderr,
        "[ipc] snp_flit pack:"
        " txn=%u src=%u gem5_op=0x%02x rtl_op=0x%02x"
        " ipc_addr=0x%lx flit_addr=0x%lx (bits[%d:%d])"
        " ns=%u rettosrc=%u donotgotosd=%u"
        " fwdnid=%u fwdtxnid=%u"
        " flit_hex[101:64]=0x%08lx [63:0]=0x%016lx\n",
        m.txn_id, m.src_id, m.opcode, rtl_opc,
        (unsigned long)m.addr, (unsigned long)f_addr,
        F::ADDR_LO + F::ADDR_W - 1, F::ADDR_LO,
        (unsigned)chi_flit::bf_getb(f, F::NS_LO),
        (unsigned)chi_flit::bf_getb(f, F::RETTOSRC_LO),
        (unsigned)chi_flit::bf_getb(f, F::DONOTGOTOSD_LO),
        m.fwd_n_id, m.fwd_txn_id,
        (unsigned long)f.range(101, 64).to_uint64(),
        (unsigned long)f.range( 63,  0).to_uint64());

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
    m.txn_id      = static_cast<uint16_t>(bf_get(f, F::TXNID_LO, F::TXNID_W));
    m.src_id      = static_cast<uint16_t>(bf_get(f, F::SRCID_LO, F::SRCID_W));
    m.tgt_id      = static_cast<uint16_t>(bf_get(f, F::TGTID_LO, F::TGTID_W));
    m.qos         = static_cast<uint8_t>( bf_get(f, F::QOS_LO, F::QOS_W));
    m.ns          = static_cast<uint8_t>( bf_getb(f, F::NS_LO));
    m.allow_retry = static_cast<uint8_t>( bf_getb(f, F::ALLOWRETRY_LO));
    m.order       = static_cast<uint8_t>( bf_get(f, F::ORDER_LO, F::ORDER_W));
    m.mem_attr    = static_cast<uint8_t>( bf_get(f, F::MEMATTR_LO, F::MEMATTR_W));
    m.size        = static_cast<uint8_t>( bf_get(f, F::SIZE_LO, F::SIZE_W));
    // EXCL_LO == SNOOPME_LO: same flit bit, alternative semantics by opcode
    m.exc_l         = static_cast<uint8_t>( bf_getb(f, F::EXCL_LO));
    m.snoop_me      = static_cast<uint8_t>( bf_getb(f, F::SNOOPME_LO));
    m.pcrd_type     = static_cast<uint8_t>( bf_get(f, F::PCRDTYPE_LO, F::PCRDTYPE_W));
    m.exp_comp_ack  = static_cast<uint8_t>( bf_getb(f, F::EXPCOMPACK_LO));
    return m;
}

chi_ipc::ChiIpcRsp questa_top_ipc::rsp_flit_to_ipc(const RspFlit& f)
{
    using F = chi_flit::rsp_f;
    chi_ipc::ChiIpcRsp m{};
    m.opcode    = static_cast<uint8_t>( bf_get(f, F::OPCODE_LO, F::OPCODE_W));
    m.txn_id    = static_cast<uint16_t>(bf_get(f, F::TXNID_LO, F::TXNID_W));
    m.src_id    = static_cast<uint16_t>(bf_get(f, F::SRCID_LO, F::SRCID_W));
    m.tgt_id    = static_cast<uint16_t>(bf_get(f, F::TGTID_LO, F::TGTID_W));
    m.qos       = static_cast<uint8_t>( bf_get(f, F::QOS_LO, F::QOS_W));
    m.resp      = static_cast<uint8_t>( bf_get(f, F::RESP_LO, F::RESP_W));
    m.resp_err  = static_cast<uint8_t>( bf_get(f, F::RESPERR_LO, F::RESPERR_W));
    m.pcrd_type = static_cast<uint8_t>( bf_get(f, F::PCRDTYPE_LO, F::PCRDTYPE_W));
    m.db_id     = static_cast<uint16_t>(bf_get(f, F::DBID_LO, F::DBID_W));
    m.fwd_state = static_cast<uint8_t>( bf_get(f, F::FWDSTATE_LO, F::FWDSTATE_W));
    m.data_pull = static_cast<uint8_t>( bf_getb(f, F::DATAPULL_LO));
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
    m.txn_id    = static_cast<uint16_t>(bf_get(f, F::TXNID_LO, F::TXNID_W));
    m.src_id    = static_cast<uint16_t>(bf_get(f, F::SRCID_LO, F::SRCID_W));
    m.tgt_id    = static_cast<uint16_t>(bf_get(f, F::TGTID_LO, F::TGTID_W));
    m.home_n_id = static_cast<uint16_t>(bf_get(f, F::HOMEID_LO, F::HOMEID_W));
    m.db_id     = static_cast<uint16_t>(bf_get(f, F::DBID_LO, F::DBID_W));
    m.qos       = static_cast<uint8_t>( bf_get(f, F::QOS_LO, F::QOS_W));
    m.resp      = static_cast<uint8_t>( bf_get(f, F::RESP_LO, F::RESP_W));
    m.resp_err  = static_cast<uint8_t>( bf_get(f, F::RESPERR_LO, F::RESPERR_W));
    m.data_id   = static_cast<uint8_t>( bf_get(f, F::DATAID_LO, F::DATAID_W));
    m.cc_id     = static_cast<uint8_t>( bf_get(f, F::CCID_LO, F::CCID_W));
    m.data_len  = CHI_DATA_W_BYTES;
    for (int i = 0; i < CHI_DATA_W_BYTES; i++) {
        m.data[i] = static_cast<uint8_t>(
            f.range(F::DATA_LO + i*8 + 7, F::DATA_LO + i*8).to_uint());
        m.be[i]   = static_cast<uint8_t>(bf_getb(f, F::BE_LO + i) ? 0xff : 0x00);
    }
    return m;
}

} } // namespace gem5::ruby
