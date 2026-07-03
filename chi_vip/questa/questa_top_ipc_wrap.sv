// questa_top_ipc_wrap.sv
//
// SystemVerilog wrapper for the questa_top_ipc IPC co-simulation module.
//
// Questa co-sim build flow (from chi_vip/ directory):
//
//   QUESTA=/path/to/questa_24/questasim
//   LIBSOC=/path/to/scc_install_questa
//   CHIDIR=$(pwd)
//
//   vlib questa_build/work
//   cd questa_build
//
//   # Compile all SC sources with sccom
//   sccom -suppress sccom-94 -O2 -std=c++17 \
//       -I $CHIDIR/questa -I $CHIDIR/ipc -I $LIBSOC/include \
//       $CHIDIR/ipc/chi_ipc.cc \
//       $CHIDIR/questa/questa_top_ipc.cc \
//       $CHIDIR/questa/questa_top_ipc_vsim.cc \
//       $CHIDIR/questa/questa_cosim_top_ipc.cc
//
//   # Link with system g++ (Questa's bundled ld 10.3.0 cannot handle
//   # modern glibc RELR sections on Fedora 40+ / Ubuntu 24+):
//   OBJDIR=work/_sc/linux_x86_64_gcc-10.3.0
//   g++ -std=c++17 -shared -fPIC -o work/systemc.so \
//       $OBJDIR/chi_ipc.o $OBJDIR/questa_top_ipc.o \
//       $OBJDIR/questa_top_ipc_vsim.o \
//       $OBJDIR/questa_cosim_top_ipc.o \
//       $QUESTA/linux_x86_64/libsystemc_cpp17.a -lrt
//
//   # Compile SV wrapper
//   vlog -work work -sv $CHIDIR/questa/questa_top_ipc_wrap.sv
//
//   # Run (gem5 must be started first so it creates the shm segment):
//   vsim -sc work.questa_top_ipc_wrap top [+other_vsim_args]
//
// NOTE: The vgencomp-confirmed port list (all 22 ports, widths from
//       chi_flit_cfg.hh defaults: NODEID_W=7 TXNID_W=8 ADDR_W=44 DATA=512):
//   component questa_top_ipc_vsim port (
//     clk : in std_logic; rstn : in std_logic;
//     txreq_flitv/pend/flit(112:0)/lcrdv; txrsp_flitv/pend/flit(55:0)/lcrdv;
//     txdat_flitv/pend/flit(636:0)/lcrdv; rxrsp_flitv/pend/flit(55:0)/lcrdv;
//     rxdat_flitv/pend/flit(636:0)/lcrdv; rxsnp_flitv/pend/flit(102:0)/lcrdv);

`timescale 1ps/1ps

// DPI-C import: questa_get_rxdat_data is exported from systemc.so (questa_top_ipc.cc).
// Returns DATA (512 bits) and BE (64 bits) of the pre-staged DAT flit.  Questa's
// SC→SV bridge for sc_lv<637> only propagates bits 0-31; DATA/BE (bits 636:61)
// are transferred via this DPI bypass instead.
// svBitVecVal arrays: data_w[16] = 512 bits (lsw first), be_w[2] = 64 bits.
import "DPI-C" function void questa_get_rxdat_data(
    output bit [511:0] data_bits,   // DATA field: 512 bits
    output bit  [63:0] be_bits      // BE field:    64 bits
);

package chi_ipc_pkg;
    // Flit widths — must match chi_flit_cfg.hh
    // NODEID_W=7  TXNID_W=12  ADDR_W=44  DATA_W_BYTES=64
    localparam int CHI_REQ_FLIT_W = 117;
    localparam int CHI_RSP_FLIT_W =  64;
    localparam int CHI_DAT_FLIT_W = 645;
    localparam int CHI_SNP_FLIT_W = 103;
endpackage : chi_ipc_pkg

// ─────────────────────────────────────────────────────────────────────────────
// questa_top_ipc_wrap: flat-port SV module that instantiates questa_top_ipc_vsim.
//
// questa_top_ipc_vsim is the SC_MODULE_EXPORT-registered flat wrapper that
// Questa's vsim elaborates as the SystemC co-simulation module.
// Port directions are from the DUT (RN-F) perspective:
//   TX* = DUT transmits to HN-F proxy (inputs here)
//   RX* = DUT receives from HN-F proxy (outputs here)
// ─────────────────────────────────────────────────────────────────────────────
module questa_top_ipc_wrap
    import chi_ipc_pkg::*;
(
    input  logic                       clk,
    input  logic                       rstn,

    // ── TXREQ (DUT → HN-F proxy) ───────────────────────────────────────────
    input  logic                       txreq_flitv,
    input  logic                       txreq_flitpend,
    input  logic [CHI_REQ_FLIT_W-1:0] txreq_flit,
    output logic                       txreq_lcrdv,

    // ── TXRSP (DUT → HN-F proxy) ───────────────────────────────────────────
    input  logic                       txrsp_flitv,
    input  logic                       txrsp_flitpend,
    input  logic [CHI_RSP_FLIT_W-1:0] txrsp_flit,
    output logic                       txrsp_lcrdv,

    // ── TXDAT (DUT → HN-F proxy) ───────────────────────────────────────────
    input  logic                       txdat_flitv,
    input  logic                       txdat_flitpend,
    input  logic [CHI_DAT_FLIT_W-1:0] txdat_flit,
    output logic                       txdat_lcrdv,

    // ── RXRSP (HN-F proxy → DUT) ───────────────────────────────────────────
    output logic                       rxrsp_flitv,
    output logic                       rxrsp_flitpend,
    output logic [CHI_RSP_FLIT_W-1:0] rxrsp_flit,
    input  logic                       rxrsp_lcrdv,

    // ── RXDAT (HN-F proxy → DUT) ───────────────────────────────────────────
    output logic                       rxdat_flitv,
    output logic                       rxdat_flitpend,
    output logic [CHI_DAT_FLIT_W-1:0] rxdat_flit,
    input  logic                       rxdat_lcrdv,

    // ── RXSNP (HN-F proxy → DUT) ───────────────────────────────────────────
    output logic                       rxsnp_flitv,
    output logic                       rxsnp_flitpend,
    output logic [CHI_SNP_FLIT_W-1:0] rxsnp_flit,
    input  logic                       rxsnp_lcrdv
);

    // ── Internal: SC module drives rxdat_flit_sc; we reconstruct rxdat_flit ──
    // Questa's SC→SV bridge for sc_lv<645> only propagates bits 0-31 to SV.
    // DATA (bits 644:133) and BE (bits 132:69) are transferred via the DPI
    // bypass function questa_get_rxdat_data() exported from systemc.so.
    // The always block reassembles the full 645-bit flit from:
    //   - rxdat_flit_sc[68:0]  : header (bits 68:0), propagated correctly
    //   - be_bits[63:0]        : BE field (64 bits), via DPI
    //   - data_bits[511:0]     : DATA field (512 bits), via DPI
    //
    // IMPORTANT: intermediate DPI result variables must be declared as 'bit'
    // (2-state) to match the DPI function's 'output bit' parameter types.
    // Using 'logic' (4-state) causes a type mismatch: Questa writes svBitVecVal*
    // (uint32_t array) but the variable expects svLogicVecVal* (aval/bval pairs),
    // so the write-back silently fails and the variables remain as 'x'.
    logic [CHI_DAT_FLIT_W-1:0] rxdat_flit_sc;
    bit   [511:0] rxdat_dpi_data;
    bit    [63:0] rxdat_dpi_be;

    always @(rxdat_flit_sc, rxdat_flitv) begin
        questa_get_rxdat_data(rxdat_dpi_data, rxdat_dpi_be);
        rxdat_flit = {rxdat_dpi_data, rxdat_dpi_be, rxdat_flit_sc[68:0]};
    end

    questa_top_ipc_vsim u_chi_ipc (
        .clk             (clk),
        .rstn            (rstn),
        .txreq_flitv     (txreq_flitv),
        .txreq_flitpend  (txreq_flitpend),
        .txreq_flit      (txreq_flit),
        .txreq_lcrdv     (txreq_lcrdv),
        .txrsp_flitv     (txrsp_flitv),
        .txrsp_flitpend  (txrsp_flitpend),
        .txrsp_flit      (txrsp_flit),
        .txrsp_lcrdv     (txrsp_lcrdv),
        .txdat_flitv     (txdat_flitv),
        .txdat_flitpend  (txdat_flitpend),
        .txdat_flit      (txdat_flit),
        .txdat_lcrdv     (txdat_lcrdv),
        .rxrsp_flitv     (rxrsp_flitv),
        .rxrsp_flitpend  (rxrsp_flitpend),
        .rxrsp_flit      (rxrsp_flit),
        .rxrsp_lcrdv     (rxrsp_lcrdv),
        .rxdat_flitv     (rxdat_flitv),
        .rxdat_flitpend  (rxdat_flitpend),
        .rxdat_flit      (rxdat_flit_sc),   // SC drives internal signal
        .rxdat_lcrdv     (rxdat_lcrdv),
        .rxsnp_flitv     (rxsnp_flitv),
        .rxsnp_flitpend  (rxsnp_flitpend),
        .rxsnp_flit      (rxsnp_flit),
        .rxsnp_lcrdv     (rxsnp_lcrdv)
    );

endmodule : questa_top_ipc_wrap
