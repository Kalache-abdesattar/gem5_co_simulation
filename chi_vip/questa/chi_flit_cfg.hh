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

// IPC-side data width (bytes).  The SV adapter always assembles a full 64-byte
// (512-bit) cache line into one IPC flit, so this stays 64 regardless of the RTL
// beat width.
static constexpr int CHI_DATA_W_BYTES = 64;
static constexpr int CHI_DATA_W_BITS  = CHI_DATA_W_BYTES * 8;

// RTL-side DAT bus width in BITS (128 / 256 / 512).  This is the *beat* width the
// ICN speaks — it sets how many link-credits the ICN spends per 512-bit line and
// therefore how many the bridge must return.  Set at build time to match the RTL
// (-DCHI_DATA_BEAT_BITS=…); defaults to the 256-bit baseline (2 beats/line).
#ifndef CHI_DATA_BEAT_BITS
#  define CHI_DATA_BEAT_BITS 256
#endif
static constexpr int CHI_BEATS_PER_LINE = CHI_DATA_W_BITS / CHI_DATA_BEAT_BITS;

// Initial L-credits issued on each TX channel at link activation.
// Questa DUT must expect exactly this many credits before sending flits.
static constexpr int CHI_INIT_LCRD    = 15;
