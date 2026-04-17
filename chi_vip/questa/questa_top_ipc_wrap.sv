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
//     rxdat_flitv/pend/flit(636:0)/lcrdv; rxsnp_flitv/pend/flit(84:0)/lcrdv);

`timescale 1ps/1ps

package chi_ipc_pkg;
    // Flit widths — must match chi_flit_cfg.hh
    // NODEID_W=7  TXNID_W=8  ADDR_W=44  DATA_W_BYTES=64
    localparam int CHI_REQ_FLIT_W = 113;
    localparam int CHI_RSP_FLIT_W =  56;
    localparam int CHI_DAT_FLIT_W = 637;
    localparam int CHI_SNP_FLIT_W =  85;
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

    // ── SC module instantiation ─────────────────────────────────────────────
    // questa_top_ipc_vsim is the SC_MODULE_EXPORT-registered flat wrapper.
    // Questa's elaboration connects the SystemC module's ports to these
    // SV signals automatically by name matching.
    //
    // Port list confirmed by vgencomp:
    //   component questa_top_ipc_vsim port (
    //     clk : in std_logic; rstn : in std_logic;
    //     txreq_flitv : in std_logic; txreq_flitpend : in std_logic;
    //     txreq_flit : in std_logic_vector(112 downto 0);
    //     txreq_lcrdv : out std_logic;
    //     txrsp_flitv : in std_logic; txrsp_flitpend : in std_logic;
    //     txrsp_flit : in std_logic_vector(55 downto 0);
    //     txrsp_lcrdv : out std_logic;
    //     txdat_flitv : in std_logic; txdat_flitpend : in std_logic;
    //     txdat_flit : in std_logic_vector(636 downto 0);
    //     txdat_lcrdv : out std_logic;
    //     rxrsp_flitv : out std_logic; rxrsp_flitpend : out std_logic;
    //     rxrsp_flit : out std_logic_vector(55 downto 0);
    //     rxrsp_lcrdv : in std_logic;
    //     rxdat_flitv : out std_logic; rxdat_flitpend : out std_logic;
    //     rxdat_flit : out std_logic_vector(636 downto 0);
    //     rxdat_lcrdv : in std_logic;
    //     rxsnp_flitv : out std_logic; rxsnp_flitpend : out std_logic;
    //     rxsnp_flit : out std_logic_vector(84 downto 0);
    //     rxsnp_lcrdv : in std_logic);
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
        .rxdat_flit      (rxdat_flit),
        .rxdat_lcrdv     (rxdat_lcrdv),
        .rxsnp_flitv     (rxsnp_flitv),
        .rxsnp_flitpend  (rxsnp_flitpend),
        .rxsnp_flit      (rxsnp_flit),
        .rxsnp_lcrdv     (rxsnp_lcrdv)
    );

endmodule : questa_top_ipc_wrap
