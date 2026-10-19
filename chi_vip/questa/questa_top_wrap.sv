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

// questa_top_wrap.sv
//
// SystemVerilog wrapper for the questa_top SystemC module.
//
// Questa co-sim flow:
//   1. sccom -work work -g \
//         -I$CHI_VIP/chi_shim -I$CHI_VIP/gem5_src \
//         -I$SCC/include -I$SYSTEMC/src \
//         -I$CHI_VIP/sc -I$CHI_VIP/questa \
//         $CHI_VIP/questa/questa_top.cc \
//         $CHI_VIP/sc/chi_vip_controller.cc \
//         $CHI_VIP/gem5_src/CHIGenericController.cc \
//         $CHI_VIP/gem5_src/utils.cc
//   2. sclink -work work -g  (link the SC objects)
//   3. vlog -work work -sv questa_top_wrap.sv  (this file)
//   4. vsim -sc work.questa_top_wrap <other tops>
//
// The DUT instantiates questa_top_wrap and drives/reads the CHI channel
// signals exactly as it would drive a real CHI HN-F pin interface.
//
// Signal naming uses the RN-F perspective (same as questa_top.hh):
//   TX*   = RN-F transmits = questo_top receives (inputs here)
//   RX*   = RN-F receives  = questa_top drives (outputs here)
//
// Parameters:
//   These must match chi_flit_cfg.hh in the SC compilation.
//   Default values are for the standard 7-bit node-ID, 8-bit TxnID,
//   44-bit address, 512-bit data bus profile.

`timescale 1ps/1ps

package chi_vip_pkg;
    // ── Flit widths — must match chi_flit_cfg.hh ────────────────────────────
    // NODEID_W=7  TXNID_W=8  ADDR_W=44  DATA_W_BYTES=64
    localparam int CHI_REQ_FLIT_W = 113;
    localparam int CHI_RSP_FLIT_W =  56;
    localparam int CHI_DAT_FLIT_W = 637;
    localparam int CHI_SNP_FLIT_W =  85;
endpackage : chi_vip_pkg

// ─────────────────────────────────────────────────────────────────────────────
// CHI channel interfaces
// ─────────────────────────────────────────────────────────────────────────────

// TX REQ channel  (RN-F → HN-F proxy, i.e. questa_top receives)
interface chi_txreq_if #(int FLIT_W = chi_vip_pkg::CHI_REQ_FLIT_W) (input logic clk);
    logic                flitv;
    logic                flitpend;
    logic [FLIT_W-1:0]   flit;
    logic                lcrdv;   // HN-F grants credit to RN-F
    modport rnf (output flitv, output flitpend, output flit, input  lcrdv);
    modport hnf (input  flitv, input  flitpend, input  flit, output lcrdv);
endinterface

// TX RSP channel  (RN-F → HN-F proxy)
interface chi_txrsp_if #(int FLIT_W = chi_vip_pkg::CHI_RSP_FLIT_W) (input logic clk);
    logic                flitv;
    logic                flitpend;
    logic [FLIT_W-1:0]   flit;
    logic                lcrdv;
    modport rnf (output flitv, output flitpend, output flit, input  lcrdv);
    modport hnf (input  flitv, input  flitpend, input  flit, output lcrdv);
endinterface

// TX DAT channel  (RN-F → HN-F proxy)
interface chi_txdat_if #(int FLIT_W = chi_vip_pkg::CHI_DAT_FLIT_W) (input logic clk);
    logic                flitv;
    logic                flitpend;
    logic [FLIT_W-1:0]   flit;
    logic                lcrdv;
    modport rnf (output flitv, output flitpend, output flit, input  lcrdv);
    modport hnf (input  flitv, input  flitpend, input  flit, output lcrdv);
endinterface

// RX RSP channel  (HN-F proxy → RN-F)
interface chi_rxrsp_if #(int FLIT_W = chi_vip_pkg::CHI_RSP_FLIT_W) (input logic clk);
    logic                flitv;
    logic                flitpend;
    logic [FLIT_W-1:0]   flit;
    logic                lcrdv;   // RN-F grants credit to HN-F
    modport rnf (input  flitv, input  flitpend, input  flit, output lcrdv);
    modport hnf (output flitv, output flitpend, output flit, input  lcrdv);
endinterface

// RX DAT channel  (HN-F proxy → RN-F)
interface chi_rxdat_if #(int FLIT_W = chi_vip_pkg::CHI_DAT_FLIT_W) (input logic clk);
    logic                flitv;
    logic                flitpend;
    logic [FLIT_W-1:0]   flit;
    logic                lcrdv;
    modport rnf (input  flitv, input  flitpend, input  flit, output lcrdv);
    modport hnf (output flitv, output flitpend, output flit, input  lcrdv);
endinterface

// RX SNP channel  (HN-F proxy → RN-F)
interface chi_rxsnp_if #(int FLIT_W = chi_vip_pkg::CHI_SNP_FLIT_W) (input logic clk);
    logic                flitv;
    logic                flitpend;
    logic [FLIT_W-1:0]   flit;
    logic                lcrdv;
    modport rnf (input  flitv, input  flitpend, input  flit, output lcrdv);
    modport hnf (output flitv, output flitpend, output flit, input  lcrdv);
endinterface

// ─────────────────────────────────────────────────────────────────────────────
// questa_top_wrap: flat-port SV module that wraps the SC questa_top.
//
// Questa's scgencomp generates a module stub named after the fully qualified
// SC module class.  We wrap it here so the DUT sees a clean, interface-free
// flat port list (some tools/DUTs don't support interface ports at the top).
// ─────────────────────────────────────────────────────────────────────────────
module questa_top_wrap
    import chi_vip_pkg::*;
(
    input  logic                    clk,
    input  logic                    rstn,

    // ── TXREQ (RN-F → HN-F proxy) ──────────────────────────────────────────
    input  logic                    txreq_flitv,
    input  logic                    txreq_flitpend,
    input  logic [CHI_REQ_FLIT_W-1:0] txreq_flit,
    output logic                    txreq_lcrdv,

    // ── TXRSP (RN-F → HN-F proxy) ──────────────────────────────────────────
    input  logic                    txrsp_flitv,
    input  logic                    txrsp_flitpend,
    input  logic [CHI_RSP_FLIT_W-1:0] txrsp_flit,
    output logic                    txrsp_lcrdv,

    // ── TXDAT (RN-F → HN-F proxy) ──────────────────────────────────────────
    input  logic                    txdat_flitv,
    input  logic                    txdat_flitpend,
    input  logic [CHI_DAT_FLIT_W-1:0] txdat_flit,
    output logic                    txdat_lcrdv,

    // ── RXRSP (HN-F proxy → RN-F) ──────────────────────────────────────────
    output logic                    rxrsp_flitv,
    output logic                    rxrsp_flitpend,
    output logic [CHI_RSP_FLIT_W-1:0] rxrsp_flit,
    input  logic                    rxrsp_lcrdv,

    // ── RXDAT (HN-F proxy → RN-F) ──────────────────────────────────────────
    output logic                    rxdat_flitv,
    output logic                    rxdat_flitpend,
    output logic [CHI_DAT_FLIT_W-1:0] rxdat_flit,
    input  logic                    rxdat_lcrdv,

    // ── RXSNP (HN-F proxy → RN-F) ──────────────────────────────────────────
    output logic                    rxsnp_flitv,
    output logic                    rxsnp_flitpend,
    output logic [CHI_SNP_FLIT_W-1:0] rxsnp_flit,
    input  logic                    rxsnp_lcrdv
);

    // ── Questa SystemC co-sim instantiation ──────────────────────────────────
    // The SC module class is gem5::ruby::questa_top.
    // scgencomp creates a SV stub named with mangled scope separators;
    // Questa internally uses the SC elaboration name passed to the constructor
    // ("qt" here).  Adjust if your top-level name differs.
    //
    // NOTE: The Questa scgencomp stub uses 'sc_logic' for boolean ports and
    //       'sc_bv' for wide ports.  All signal widths below come from the
    //       package localparams above.  Replace the stub instantiation comment
    //       with the actual scgencomp output once you run scgencomp.
    //
    // --- BEGIN scgencomp-generated stub (replace with actual output) ---
    //
    //  gem5__ruby__questa_top qt (
    //      .clk            (clk),
    //      .rstn           (rstn),
    //      .txreq_flitv    (txreq_flitv),
    //      .txreq_flitpend (txreq_flitpend),
    //      .txreq_flit     (txreq_flit),
    //      .txreq_lcrdv    (txreq_lcrdv),
    //      .txrsp_flitv    (txrsp_flitv),
    //      .txrsp_flitpend (txrsp_flitpend),
    //      .txrsp_flit     (txrsp_flit),
    //      .txrsp_lcrdv    (txrsp_lcrdv),
    //      .txdat_flitv    (txdat_flitv),
    //      .txdat_flitpend (txdat_flitpend),
    //      .txdat_flit     (txdat_flit),
    //      .txdat_lcrdv    (txdat_lcrdv),
    //      .rxrsp_flitv    (rxrsp_flitv),
    //      .rxrsp_flitpend (rxrsp_flitpend),
    //      .rxrsp_flit     (rxrsp_flit),
    //      .rxrsp_lcrdv    (rxrsp_lcrdv),
    //      .rxdat_flitv    (rxdat_flitv),
    //      .rxdat_flitpend (rxdat_flitpend),
    //      .rxdat_flit     (rxdat_flit),
    //      .rxdat_lcrdv    (rxdat_lcrdv),
    //      .rxsnp_flitv    (rxsnp_flitv),
    //      .rxsnp_flitpend (rxsnp_flitpend),
    //      .rxsnp_flit     (rxsnp_flit),
    //      .rxsnp_lcrdv    (rxsnp_lcrdv)
    //  );
    //
    // --- END scgencomp-generated stub ---

endmodule : questa_top_wrap

// ─────────────────────────────────────────────────────────────────────────────
// chi_vip_bind: convenience module that wraps questa_top_wrap using the
// interface types defined above.  Instantiate this at the top level and
// connect the interface ports to your DUT's CHI port groups.
// ─────────────────────────────────────────────────────────────────────────────
module chi_vip_bind
    import chi_vip_pkg::*;
(
    input  logic clk,
    input  logic rstn,

    chi_txreq_if.hnf  txreq,
    chi_txrsp_if.hnf  txrsp,
    chi_txdat_if.hnf  txdat,
    chi_rxrsp_if.hnf  rxrsp,
    chi_rxdat_if.hnf  rxdat,
    chi_rxsnp_if.hnf  rxsnp
);
    questa_top_wrap u_qt (
        .clk             (clk),
        .rstn            (rstn),
        .txreq_flitv     (txreq.flitv),
        .txreq_flitpend  (txreq.flitpend),
        .txreq_flit      (txreq.flit),
        .txreq_lcrdv     (txreq.lcrdv),
        .txrsp_flitv     (txrsp.flitv),
        .txrsp_flitpend  (txrsp.flitpend),
        .txrsp_flit      (txrsp.flit),
        .txrsp_lcrdv     (txrsp.lcrdv),
        .txdat_flitv     (txdat.flitv),
        .txdat_flitpend  (txdat.flitpend),
        .txdat_flit      (txdat.flit),
        .txdat_lcrdv     (txdat.lcrdv),
        .rxrsp_flitv     (rxrsp.flitv),
        .rxrsp_flitpend  (rxrsp.flitpend),
        .rxrsp_flit      (rxrsp.flit),
        .rxrsp_lcrdv     (rxrsp.lcrdv),
        .rxdat_flitv     (rxdat.flitv),
        .rxdat_flitpend  (rxdat.flitpend),
        .rxdat_flit      (rxdat.flit),
        .rxdat_lcrdv     (rxdat.lcrdv),
        .rxsnp_flitv     (rxsnp.flitv),
        .rxsnp_flitpend  (rxsnp.flitpend),
        .rxsnp_flit      (rxsnp.flit),
        .rxsnp_lcrdv     (rxsnp.lcrdv)
    );
endmodule : chi_vip_bind
