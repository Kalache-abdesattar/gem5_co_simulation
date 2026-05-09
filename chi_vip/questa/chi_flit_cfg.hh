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

// SNP flit address field width = CHIE_SNP_ADDR_WIDTH_PARAM in the RTL.
// The CHI spec and RTL store byte_addr >> 3 (cache-line granularity) in this
// field.  pack_snp() shifts addr right by 3; rx_control.sv reconstructs the
// byte address with {rxsnp_addr_r[width-6:0], 3'b000}.
// With 44-bit field: covers up to 2^47 = 128 TB byte address space.
static constexpr int CHI_SNP_ADDR_W   = CHI_ADDR_W;   // = CHIE_SNP_ADDR_WIDTH_PARAM

// Logical processor ID width per node (typically 4).
static constexpr int CHI_LPID_W       = 4;

// Data bus width in bytes (16 / 32 / 64 for 128 / 256 / 512-bit buses).
static constexpr int CHI_DATA_W_BYTES = 64;
static constexpr int CHI_DATA_W_BITS  = CHI_DATA_W_BYTES * 8;

// Initial L-credits issued on each TX channel at link activation.
// Questa DUT must expect exactly this many credits before sending flits.
static constexpr int CHI_INIT_LCRD    = 15;
