#pragma once
// ── questa_top_ipc ──────────────────────────────────────────────────────────
// IPC-bridge variant of questa_top for the Questa vsim co-simulation side.
//
// Architecture:
//
//   Questa DUT (RN-F) ←pin signals→ questa_top_ipc ←shm ring buffers→ gem5
//                                      (chi_ipc)
//
// questa_top_ipc does NOT embed a CHI_VIP_Controller.  Instead, every flit
// received from the DUT is serialised into the q2g_* SPSC ring buffers in the
// shared-memory segment.  Conversely, it drains the g2q_* buffers and converts
// the messages back into pin-level output flits for the DUT.
//
// Timing:
//   Each posedge of clk corresponds to one CHI link cycle.  questa_top_ipc
//   advances questa_grant_ps by one cycle_ps each posedge and reads
//   gem5_grant_ps to decide whether gem5 has issued responses the DUT may
//   need in the same cycle.  There is no tight barrier synchronisation; both
//   sides run as fast as they can and the ring buffers buffer any imbalance.
//   This is a LOOSE co-simulation model — suitable for functional validation;
//   for cycle-accurate timing replace with a barrier-based protocol.
//
// Usage:
//   1. Construct questa_top_ipc before sc_start(), providing the shared-memory
//      segment name (same string given to VIPController on the gem5 side) and
//      the cycle period in picoseconds.
//   2. Bind the 22 pin-level ports to your DUT just like questa_top.
//   3. Call sc_start().  questa_top_ipc will spin in tryOpen() until gem5
//      creates the segment, then run normally.

#include <systemc>
#include <queue>
#include <string>
#include <cstdint>

#include "chi_flit_cfg.hh"
#include "chi_flit_pack.hh"
#include "../ipc/chi_ipc.hh"

namespace gem5 { namespace ruby {

using ReqFlit = sc_dt::sc_bv<chi_flit::CHI_REQ_FLIT_W>;
using RspFlit = sc_dt::sc_bv<chi_flit::CHI_RSP_FLIT_W>;
// sc_lv (4-state) is required for DatFlit: Questa's SC→SV bridge only
// propagates all words of a wide port when the type is sc_lv; sc_bv only
// propagates word 0 (bits 31:0), leaving DATA bits [636:125] as 'x' in SV.
using DatFlit = sc_dt::sc_lv<chi_flit::CHI_DAT_FLIT_W>;
using SnpFlit = sc_dt::sc_bv<chi_flit::CHI_SNP_FLIT_W>;

SC_MODULE(questa_top_ipc)
{
    // ── Clock & reset ─────────────────────────────────────────────────────────
    sc_core::sc_in<bool>  clk  {"clk"};
    sc_core::sc_in<bool>  rstn {"rstn"};

    // ── TXREQ  (DUT → HN-F) ──────────────────────────────────────────────────
    sc_core::sc_in<bool>     txreq_flitv    {"txreq_flitv"};
    sc_core::sc_in<bool>     txreq_flitpend {"txreq_flitpend"};
    sc_core::sc_in<ReqFlit>  txreq_flit     {"txreq_flit"};
    sc_core::sc_out<bool>    txreq_lcrdv    {"txreq_lcrdv"};

    // ── TXRSP  (DUT → HN-F) ──────────────────────────────────────────────────
    sc_core::sc_in<bool>     txrsp_flitv    {"txrsp_flitv"};
    sc_core::sc_in<bool>     txrsp_flitpend {"txrsp_flitpend"};
    sc_core::sc_in<RspFlit>  txrsp_flit     {"txrsp_flit"};
    sc_core::sc_out<bool>    txrsp_lcrdv    {"txrsp_lcrdv"};

    // ── TXDAT  (DUT → HN-F) ──────────────────────────────────────────────────
    sc_core::sc_in<bool>     txdat_flitv    {"txdat_flitv"};
    sc_core::sc_in<bool>     txdat_flitpend {"txdat_flitpend"};
    sc_core::sc_in<DatFlit>  txdat_flit     {"txdat_flit"};
    sc_core::sc_out<bool>    txdat_lcrdv    {"txdat_lcrdv"};

    // ── RXRSP  (HN-F → DUT) ──────────────────────────────────────────────────
    sc_core::sc_out<bool>    rxrsp_flitv    {"rxrsp_flitv"};
    sc_core::sc_out<bool>    rxrsp_flitpend {"rxrsp_flitpend"};
    sc_core::sc_out<RspFlit> rxrsp_flit     {"rxrsp_flit"};
    sc_core::sc_in<bool>     rxrsp_lcrdv    {"rxrsp_lcrdv"};

    // ── RXDAT  (HN-F → DUT) ──────────────────────────────────────────────────
    sc_core::sc_out<bool>    rxdat_flitv    {"rxdat_flitv"};
    sc_core::sc_out<bool>    rxdat_flitpend {"rxdat_flitpend"};
    sc_core::sc_out<DatFlit> rxdat_flit     {"rxdat_flit"};
    sc_core::sc_in<bool>     rxdat_lcrdv    {"rxdat_lcrdv"};

    // ── RXSNP  (HN-F → DUT) ──────────────────────────────────────────────────
    sc_core::sc_out<bool>    rxsnp_flitv    {"rxsnp_flitv"};
    sc_core::sc_out<bool>    rxsnp_flitpend {"rxsnp_flitpend"};
    sc_core::sc_out<SnpFlit> rxsnp_flit     {"rxsnp_flit"};
    sc_core::sc_in<bool>     rxsnp_lcrdv    {"rxsnp_lcrdv"};

    SC_HAS_PROCESS(questa_top_ipc);

    // shm_name : e.g. "/chi_vip_0"  — must match gem5 VIPController shm_name
    // cycle_ps : link clock period in picoseconds (default 1000 = 1 ns)
    questa_top_ipc(sc_core::sc_module_name n,
                   const std::string& shm_name,
                   uint64_t           cycle_ps = 1000);
    ~questa_top_ipc() = default;

    void start_of_simulation() override;
    void clk_posedge();

  private:
    chi_ipc::ChiShmHandle shm_;
    uint64_t              cycle_ps_;
    uint64_t              questa_ps_{0};  // running questa time in ps

    // Outbound flit queues (g2q channel → DUT)
    std::queue<RspFlit> rsp_out_q_;
    std::queue<DatFlit> dat_out_q_;
    std::queue<SnpFlit> snp_out_q_;

    // L-credit accounting (credits held for sending to DUT)
    int rsp_lcrd_{0};
    int dat_lcrd_{0};
    int snp_lcrd_{0};

    // Cycles to suppress rxdat_flitv after a send: dat_rx_sm (always_ff) needs
    // 3 SC cycles to return to RX_IDLE (latch + FLIT0 + FLIT1), so back-to-back
    // sends would drop the second flit because the SM is in a non-IDLE state.
    int dat_tx_cooldown_{0};

    // Init-phase: pulse TXREQ/TXRSP/TXDATLCRDV for CHI_INIT_LCRD cycles
    int req_lcrd_init_{CHI_INIT_LCRD};
    int rsp_lcrd_init_{CHI_INIT_LCRD};
    int dat_lcrd_init_{CHI_INIT_LCRD};

    // SNP credits flow from CVA6 (RN-F) to questa (HN-F).  If CVA6 does not
    // assert hnf_txsnplcrdv during link init, snp_lcrd_ stays 0 and snoops
    // can never be sent.  Pre-load snp_lcrd_ to CHI_INIT_LCRD on the first
    // active cycle so snoops can proceed without waiting for CVA6 credits.
    bool snp_lcrd_preloaded_{false};

    // RSP credits (rxrsp_lcrdv) are asserted by the testbench only while a
    // wait_rsp task is active.  For autonomously-generated HN-F responses
    // (e.g. CompDBIDResp for a CVA6-hardware-initiated WriteBackPtl), no
    // wait_rsp is in flight, so rsp_lcrd_ stays 0 and the flit is never sent.
    // Pre-load rsp_lcrd_ to CHI_INIT_LCRD on the first active cycle so the
    // HN-F can send RSP flits at any time.
    bool rsp_lcrd_preloaded_{false};

    // The ICN spends one credit per 256-bit RTL beat forwarded to hnf_rxdatflitv.
    // Each assembled 512-bit IPC DAT consumed two beats, so we must return two
    // credits.  pending_extra_dat_crd_ holds credits deferred to the next cycle.
    int pending_extra_dat_crd_{0};

    // Convert IPC structs → flit bits
    RspFlit ipc_to_rsp_flit(const chi_ipc::ChiIpcRsp& m);
    DatFlit ipc_to_dat_flit(const chi_ipc::ChiIpcDat& m);
    SnpFlit ipc_to_snp_flit(const chi_ipc::ChiIpcSnp& m);

    // Convert flit bits → IPC structs
    chi_ipc::ChiIpcReq req_flit_to_ipc(const ReqFlit& f);
    chi_ipc::ChiIpcRsp rsp_flit_to_ipc(const RspFlit& f);
    chi_ipc::ChiIpcDat dat_flit_to_ipc(const DatFlit& f);
};

} } // namespace gem5::ruby
