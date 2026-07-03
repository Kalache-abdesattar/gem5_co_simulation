#pragma once
// ── CHI interface configuration ───────────────────────────────────────────────
// Adjust these constants to match your DUT's CHI interface parameters.
// They must be consistent with the RTL CHI VIP or DUT configuration.
//
// Reference: ARM AMBA CHI Architecture Specification IHI0050E.

// Node ID width [7..11].  7 → up to 128 nodes; 11 → up to 2048 nodes.
static constexpr int CHI_NODEID_W     = 7;

// Transaction ID width [8..12].  8 → 256 outstanding; 12 → 4096.
static constexpr int CHI_TXNID_W      = 12;

// Physical address width (bits).
static constexpr int CHI_ADDR_W       = 44;

// SNP flit address field width, in the SCC/IPC packing (chi_flit_pack.hh).
// The IPC layer stores byte_addr >> 3 in a 44-bit field [90:47]; the gem5 TB
// remap (riscv_rnf_gem5_tb.sv snp_ipc2rtl) forwards only the lower 41 bits
// [87:47] to the RTL's CHIE_SNP_ADDR_WIDTH_PARAM=41 field and zero-fills the
// top.  Keep this at 44 so the IPC field positions above ADDR match the TB's
// IPC_SNP_W=103 layout (NS[91] .. TGTID[102:96]).
static constexpr int CHI_SNP_ADDR_W   = CHI_ADDR_W;   // 44-bit IPC ADDR field

// Logical processor ID width per node (typically 4).
static constexpr int CHI_LPID_W       = 4;

// Data bus width in bytes (16 / 32 / 64 for 128 / 256 / 512-bit buses).
static constexpr int CHI_DATA_W_BYTES = 64;
static constexpr int CHI_DATA_W_BITS  = CHI_DATA_W_BYTES * 8;

// Initial L-credits issued on each TX channel at link activation.
// Questa DUT must expect exactly this many credits before sending flits.
static constexpr int CHI_INIT_LCRD    = 15;
