// Copyright (c) 2025-2026 Abdesattar Kalache <abdesattar.kalache@tuni.fi>
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#pragma once
// ── questa_top ────────────────────────────────────────────────────────────────
// SystemC top-level module for Questa co-simulation.
//
// questa_top sits between the RTL DUT (RN-F) and the CHI_VIP_Controller
// (gem5 HN-F proxy).  It adapts pin-level CHI signals to TLM2 sockets.
//
//   Questa DUT (RN-F) ←pin signals→ questa_top ←TLM2→ CHI_VIP_Controller
//                                                              ↕
//                                                        gem5 Ruby HN-F
//
// Signal naming convention (DUT's perspective):
//   TX* = DUT transmits  (inbound to questa_top / CHI_VIP_Controller)
//   RX* = DUT receives   (outbound from questa_top / CHI_VIP_Controller)
//
// Link activation:
//   On de-assertion of rstn, questa_top pulses TXREQLCRDV / TXRSPLCRDV /
//   TXDATLCRDV for CHI_INIT_LCRD consecutive cycles to grant the DUT its
//   initial L-credit budget.  Thereafter each received TX flit returns one
//   credit.  Outbound RX flits are held until the DUT grants credits via
//   RXRSPLCRDV / RXDATLCRDV / RXSNPLCRDV.

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <chi/chi_tlm.h>
#include <queue>
#include <cstdint>

#include "chi_flit_cfg.hh"
#include "chi_flit_pack.hh"
#include "chi_vip_controller.hh"

namespace gem5 { namespace ruby {

// Convenience type aliases driven by chi_flit_pack.hh constexpr widths.
using ReqFlit = sc_dt::sc_bv<chi_flit::CHI_REQ_FLIT_W>;
using RspFlit = sc_dt::sc_bv<chi_flit::CHI_RSP_FLIT_W>;
using DatFlit = sc_dt::sc_lv<chi_flit::CHI_DAT_FLIT_W>;
using SnpFlit = sc_dt::sc_bv<chi_flit::CHI_SNP_FLIT_W>;

SC_MODULE(questa_top)
{
    // ── Clock & reset ─────────────────────────────────────────────────────────
    sc_core::sc_in<bool>  clk  {"clk"};   // posedge-sampling clock
    sc_core::sc_in<bool>  rstn {"rstn"};  // active-low synchronous reset

    // ── TXREQ  (DUT RN-F → HN-F proxy) ──────────────────────────────────────
    sc_core::sc_in<bool>     txreq_flitv    {"txreq_flitv"};
    sc_core::sc_in<bool>     txreq_flitpend {"txreq_flitpend"};
    sc_core::sc_in<ReqFlit>  txreq_flit     {"txreq_flit"};
    sc_core::sc_out<bool>    txreq_lcrdv    {"txreq_lcrdv"};  // credit to DUT

    // ── TXRSP  (DUT → HN-F proxy) ────────────────────────────────────────────
    sc_core::sc_in<bool>     txrsp_flitv    {"txrsp_flitv"};
    sc_core::sc_in<bool>     txrsp_flitpend {"txrsp_flitpend"};
    sc_core::sc_in<RspFlit>  txrsp_flit     {"txrsp_flit"};
    sc_core::sc_out<bool>    txrsp_lcrdv    {"txrsp_lcrdv"};

    // ── TXDAT  (DUT → HN-F proxy) ────────────────────────────────────────────
    sc_core::sc_in<bool>     txdat_flitv    {"txdat_flitv"};
    sc_core::sc_in<bool>     txdat_flitpend {"txdat_flitpend"};
    sc_core::sc_in<DatFlit>  txdat_flit     {"txdat_flit"};
    sc_core::sc_out<bool>    txdat_lcrdv    {"txdat_lcrdv"};

    // ── RXRSP  (HN-F proxy → DUT) ────────────────────────────────────────────
    sc_core::sc_out<bool>    rxrsp_flitv    {"rxrsp_flitv"};
    sc_core::sc_out<bool>    rxrsp_flitpend {"rxrsp_flitpend"};
    sc_core::sc_out<RspFlit> rxrsp_flit     {"rxrsp_flit"};
    sc_core::sc_in<bool>     rxrsp_lcrdv    {"rxrsp_lcrdv"};  // credit from DUT

    // ── RXDAT  (HN-F proxy → DUT) ────────────────────────────────────────────
    sc_core::sc_out<bool>    rxdat_flitv    {"rxdat_flitv"};
    sc_core::sc_out<bool>    rxdat_flitpend {"rxdat_flitpend"};
    sc_core::sc_out<DatFlit> rxdat_flit     {"rxdat_flit"};
    sc_core::sc_in<bool>     rxdat_lcrdv    {"rxdat_lcrdv"};

    // ── RXSNP  (HN-F proxy → DUT) ────────────────────────────────────────────
    sc_core::sc_out<bool>    rxsnp_flitv    {"rxsnp_flitv"};
    sc_core::sc_out<bool>    rxsnp_flitpend {"rxsnp_flitpend"};
    sc_core::sc_out<SnpFlit> rxsnp_flit     {"rxsnp_flit"};
    sc_core::sc_in<bool>     rxsnp_lcrdv    {"rxsnp_lcrdv"};

    SC_HAS_PROCESS(questa_top);

    // gem5 objects (RubySystem, Network, node ID) must already exist and
    // outlive questa_top.  In a full co-sim they are created by the gem5
    // initialisation code before sc_start().
    // downstream_id: the MachineID of the HN-F this proxy connects to.
    // Default {MachineType_Directory, 0} matches the standard gem5 single-HNF
    // configuration.  ctrl_.init() is called inside the constructor once
    // downstream_ is set — do not call it again externally.
    questa_top(sc_core::sc_module_name n,
               MachineID              id,
               MachineID              downstream_id,
               RubySystem*            rs,
               Network*               net);
    ~questa_top() = default;

    // Called by SystemC just before simulation starts (after all modules are
    // constructed and bound).  Users must NOT call ctrl_.init() manually.
    void before_end_of_elaboration() override;

    // downstream_id_ stored so before_end_of_elaboration() can use it.
    // May be overridden via ctrl_.downstream_ before sc_start().
    MachineID downstream_id_;

public:
    // ── TLM2 bridge (public for test access and advanced integration) ─────────
    CHI_VIP_Controller ctrl_;

    // Target sockets: catch outbound TLM2 transactions from ctrl_
    tlm_utils::simple_target_socket<questa_top> rxrsp_catch_ {"rxrsp_catch_"};
    tlm_utils::simple_target_socket<questa_top> rxdat_catch_ {"rxdat_catch_"};
    tlm_utils::simple_target_socket<questa_top> rxsnp_catch_ {"rxsnp_catch_"};

    // Initiator sockets: inject inbound TLM2 transactions into ctrl_
    tlm_utils::simple_initiator_socket<questa_top> txreq_inject_ {"txreq_inject_"};
    tlm_utils::simple_initiator_socket<questa_top> txrsp_inject_ {"txrsp_inject_"};
    tlm_utils::simple_initiator_socket<questa_top> txdat_inject_ {"txdat_inject_"};

    // Outbound flit queues — populated by catch callbacks, drained on posedge
    std::queue<RspFlit> rsp_out_q_;
    std::queue<DatFlit> dat_out_q_;
    std::queue<SnpFlit> snp_out_q_;

    // Outbound L-credit counters (how many flits we may send to the DUT)
    int rsp_lcrd_ {0};
    int dat_lcrd_ {0};
    int snp_lcrd_ {0};

    // Init-phase credit counters: decremented each cycle until 0
    int req_lcrd_init_ {CHI_INIT_LCRD};
    int rsp_lcrd_init_ {CHI_INIT_LCRD};
    int dat_lcrd_init_ {CHI_INIT_LCRD};

    // SC_METHOD — all channel work is done on posedge
    void clk_posedge();

    // Catch callbacks from ctrl_'s initiator sockets
    void on_ctrl_txrsp(tlm::tlm_generic_payload&, sc_core::sc_time&);
    void on_ctrl_txdat(tlm::tlm_generic_payload&, sc_core::sc_time&);
    void on_ctrl_txsnp(tlm::tlm_generic_payload&, sc_core::sc_time&);
};

} } // namespace gem5::ruby
