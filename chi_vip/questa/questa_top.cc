// questa_top.cc — SystemC pin-level ↔ TLM2 adapter for Questa co-simulation
//
// See questa_top.hh for the architecture description.
//
// Compilation (standalone, for functional verification without vsim):
//   See Makefile target 'questa_top.o' in chi_vip/Makefile.
//
// Questa integration:
//   1. sccom -work work questa_top.cc [include flags]
//   2. vlog -work work <DUT RTL files>
//   3. Bind questa_top ports to SV interface in a SV wrapper module.
//   4. vsim -sc <top> -scgenerics ...

#include "questa_top.hh"
#include <cstring>
#include <cstdio>

namespace gem5 { namespace ruby {

using namespace sc_core;
using namespace chi_flit;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
questa_top::questa_top(sc_module_name n,
                       MachineID      id,
                       MachineID      downstream_id,
                       RubySystem*    rs,
                       Network*       net)
    : sc_module(n)
    , downstream_id_(downstream_id)
    , ctrl_("ctrl_", id, rs, net)
{
    // ── Bind TLM2 sockets ────────────────────────────────────────────────────
    // ctrl_'s initiator sockets (outbound to DUT) → our catch targets
    ctrl_.txrsp_socket.bind(rxrsp_catch_);
    ctrl_.txdat_socket.bind(rxdat_catch_);
    ctrl_.txsnp_socket.bind(rxsnp_catch_);

    // Our inject initiators → ctrl_'s target sockets (inbound from DUT)
    txreq_inject_.bind(ctrl_.rxreq_socket);
    txrsp_inject_.bind(ctrl_.rxrsp_socket);
    txdat_inject_.bind(ctrl_.rxdat_socket);

    // ── Register catch callbacks ─────────────────────────────────────────────
    rxrsp_catch_.register_b_transport(this, &questa_top::on_ctrl_txrsp);
    rxdat_catch_.register_b_transport(this, &questa_top::on_ctrl_txdat);
    rxsnp_catch_.register_b_transport(this, &questa_top::on_ctrl_txsnp);

    // ── SC process ───────────────────────────────────────────────────────────
    SC_METHOD(clk_posedge);
    sensitive << clk.pos();

}

// ─────────────────────────────────────────────────────────────────────────────
// before_end_of_elaboration: called by SystemC after all modules constructed
// and all ports bound, before simulation starts.  This is the earliest safe
// point to call ctrl_.init() (which connects the Ruby message buffers).
// ─────────────────────────────────────────────────────────────────────────────
void questa_top::before_end_of_elaboration()
{
    ctrl_.downstream_ = downstream_id_;
    ctrl_.init();
}

// ─────────────────────────────────────────────────────────────────────────────
// Main clock handler — runs on every posedge of clk
// ─────────────────────────────────────────────────────────────────────────────
void questa_top::clk_posedge()
{
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
        ctrl_.reset_pending_dat();  // discard any partially-assembled inbound DAT
        rsp_lcrd_ = dat_lcrd_ = snp_lcrd_ = 0;
        req_lcrd_init_ = rsp_lcrd_init_ = dat_lcrd_init_ = CHI_INIT_LCRD;
        return;
    }

    // ── Phase 1: issue initial L-credits to DUT on each TX channel ───────────
    // Pulse TXREQLCRDV / TXRSPLCRDV / TXDATLCRDV for CHI_INIT_LCRD cycles
    // immediately after reset de-assertion (link activation).
    bool send_req_crd = (req_lcrd_init_ > 0);
    bool send_rsp_crd = (rsp_lcrd_init_ > 0);
    bool send_dat_crd = (dat_lcrd_init_ > 0);
    if (req_lcrd_init_ > 0) --req_lcrd_init_;
    if (rsp_lcrd_init_ > 0) --rsp_lcrd_init_;
    if (dat_lcrd_init_ > 0) --dat_lcrd_init_;

    // ── Phase 2: consume L-credits granted by the DUT (for our outbound) ─────
    if (rxrsp_lcrdv.read()) ++rsp_lcrd_;
    if (rxdat_lcrdv.read()) ++dat_lcrd_;
    if (rxsnp_lcrdv.read()) ++snp_lcrd_;

    // ── Phase 3: sample inbound TX flits (DUT → HN-F proxy) ─────────────────

    // TXREQ
    if (txreq_flitv.read()) {
        tlm::tlm_generic_payload gp;
        auto* ext = new chi::chi_ctrl_extension();  // heap: GP owns via set_extension
        unpack_req(txreq_flit.read(), gp, *ext);
        gp.set_extension(ext);
        sc_time d = SC_ZERO_TIME;
        txreq_inject_->b_transport(gp, d);
        // Return one L-credit to the DUT for the slot just freed
        send_req_crd = true;
    }

    // TXRSP
    if (txrsp_flitv.read()) {
        tlm::tlm_generic_payload gp;
        auto* ext = new chi::chi_ctrl_extension();  // heap: GP owns via set_extension
        unpack_rsp(txrsp_flit.read(), gp, *ext);
        gp.set_extension(ext);
        sc_time d = SC_ZERO_TIME;
        txrsp_inject_->b_transport(gp, d);
        send_rsp_crd = true;
    }

    // TXDAT
    if (txdat_flitv.read()) {
        // Data buffer lives for the duration of the b_transport call.
        uint8_t data_buf[CHI_DATA_W_BYTES];
        tlm::tlm_generic_payload gp;
        auto* ext = new chi::chi_data_extension();  // heap: GP owns via set_extension
        unpack_dat(txdat_flit.read(), gp, *ext, data_buf);
        gp.set_extension(ext);
        sc_time d = SC_ZERO_TIME;
        txdat_inject_->b_transport(gp, d);
        send_dat_crd = true;
    }

    txreq_lcrdv.write(send_req_crd);
    txrsp_lcrdv.write(send_rsp_crd);
    txdat_lcrdv.write(send_dat_crd);

    // ── Phase 4: drive outbound RX flits (HN-F proxy → DUT) ─────────────────
    // A flit is sent only if we hold a credit from the DUT.
    // FLITPEND is asserted one cycle ahead of FLITV (lookahead).

    // RXRSP
    if (!rsp_out_q_.empty() && rsp_lcrd_ > 0) {
        rxrsp_flitv.write(true);
        rxrsp_flit.write(rsp_out_q_.front());
        rsp_out_q_.pop();
        --rsp_lcrd_;
        // Lookahead: assert FLITPEND now if another flit is waiting
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
// Catch callbacks — invoked by ctrl_ when gem5 has an outbound message ready.
// These can be called at any simulation time (not just posedge); they only
// enqueue the packed flit.  The clock method drains the queues.
// ─────────────────────────────────────────────────────────────────────────────

void questa_top::on_ctrl_txrsp(tlm::tlm_generic_payload& gp, sc_time&)
{
    auto* ext = gp.get_extension<chi::chi_ctrl_extension>();
    if (!ext) {
        fprintf(stderr, "[questa_top] on_ctrl_txrsp: missing chi_ctrl_extension\n");
        gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    RspFlit bits;
    pack_rsp(*ext, bits);
    rsp_out_q_.push(bits);
    gp.set_response_status(tlm::TLM_OK_RESPONSE);
}

void questa_top::on_ctrl_txdat(tlm::tlm_generic_payload& gp, sc_time&)
{
    auto* ext = gp.get_extension<chi::chi_data_extension>();
    if (!ext) {
        fprintf(stderr, "[questa_top] on_ctrl_txdat: missing chi_data_extension\n");
        gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    // gp.get_data_ptr() is valid only during this b_transport call; pack
    // the bytes into the flit immediately.
    const uint8_t* data = gp.get_data_ptr();
    int beat_bytes = data ? static_cast<int>(gp.get_data_length()) : 0;
    DatFlit bits;
    pack_dat(*ext, data, beat_bytes, bits);
    dat_out_q_.push(bits);
    gp.set_response_status(tlm::TLM_OK_RESPONSE);
}

void questa_top::on_ctrl_txsnp(tlm::tlm_generic_payload& gp, sc_time&)
{
    auto* ext = gp.get_extension<chi::chi_snp_extension>();
    if (!ext) {
        fprintf(stderr, "[questa_top] on_ctrl_txsnp: missing chi_snp_extension\n");
        gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    SnpFlit bits;
    pack_snp(*ext, gp.get_address(), bits);
    snp_out_q_.push(bits);
    gp.set_response_status(tlm::TLM_OK_RESPONSE);
}

} } // namespace gem5::ruby
