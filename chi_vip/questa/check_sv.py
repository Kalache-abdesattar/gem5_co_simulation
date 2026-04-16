#!/usr/bin/env python3
"""
check_sv.py  —  lightweight structural validator for questa_top_wrap.sv and
                tb_chi_vip.sv.

Checks:
  1. Flit-width constants in the SV match chi_flit_cfg.hh (parsed via regex).
  2. Port list of questa_top_wrap declares all required CHI signals.
  3. questa_top_wrap instantiation inside chi_vip_bind connects all ports.
  4. tb_chi_vip instantiates questa_top_wrap and connects all its ports.
  5. No duplicate port declarations in questa_top_wrap.
"""

import re
import sys
from pathlib import Path

QUESTA_DIR = Path(__file__).parent
CFG_H      = QUESTA_DIR / "chi_flit_cfg.hh"
WRAP_SV    = QUESTA_DIR / "questa_top_wrap.sv"
TB_SV      = QUESTA_DIR / "tb_chi_vip.sv"

pass_count = 0
fail_count = 0

def check(cond, msg):
    global pass_count, fail_count
    if cond:
        print(f"  PASS: {msg}")
        pass_count += 1
    else:
        print(f"  FAIL: {msg}")
        fail_count += 1

# ── 1. Parse flit widths from chi_flit_cfg.hh ────────────────────────────────
def parse_cfg(path):
    cfg = {}
    for line in path.read_text().splitlines():
        m = re.match(r'\s*static constexpr int\s+(\w+)\s*=\s*(\d+)', line)
        if m:
            cfg[m.group(1)] = int(m.group(2))
    return cfg

def expected_flit_widths(cfg):
    """Re-derive flit widths from chi_flit_cfg.hh constants.
    These must match the constexpr values in chi_flit_pack.hh."""
    NID  = cfg["CHI_NODEID_W"]     # 7
    TXN  = cfg["CHI_TXNID_W"]      # 8
    ADDR = cfg["CHI_ADDR_W"]       # 44
    DATA = cfg["CHI_DATA_W_BYTES"] # 64

    # req_f::TOTAL_W = TRACETAG_LO + 1
    # Walk the exact field chain from chi_flit_pack.hh:
    #   QOS(4) TGTID(NID) SRCID(NID) TXNID(TXN) RTNID(NID) STASHNIDV(1)
    #   RTNTXNID(LPID=4) STASHLPIDV(1) OPCODE(7) SIZE(3) ADDR(ADDR)
    #   NS(1) LIKELYSH(1) ALLOWRETRY(1) ORDER(2) PCRDTYPE(4) MEMATTR(4)
    #   SNPATTR(1) DWT(1) SNOOPME(1) EXPCOMPACK(1) TAGOP(2) TRACETAG(1)
    LPID = cfg.get("CHI_LPID_W", 4)
    req_tracetag = (4 + NID + NID + TXN + NID + 1 + LPID + 1 + 7 + 3 + ADDR
                    + 1 + 1 + 1 + 2 + 4 + 4 + 1 + 1 + 1 + 1 + 2)
    req_w = req_tracetag + 1

    # rsp_f::TOTAL_W = TRACETAG_LO + 1 = 55 + 1 = 56
    rsp_tracetag = 4 + NID + NID + TXN + 6 + 2 + 3 + 3 + 1 + TXN + 4 + 2
    rsp_w = rsp_tracetag + 1

    # dat_f::TOTAL_W = DATA_LO + DATA_W
    dat_tracetag = 4 + NID + NID + 4 + 2 + 3 + 3 + 1 + TXN + 2 + 2 + 2 + TXN + NID
    dat_be_lo    = dat_tracetag + 1
    dat_data_lo  = dat_be_lo + DATA
    dat_w        = dat_data_lo + DATA * 8

    # snp_f::TOTAL_W = TRACETAG_LO + 1
    snp_tracetag = 4 + NID + TXN + NID + TXN + 5 + 1 + 1 + 1 + 1 + (ADDR - 3)
    snp_w = snp_tracetag + 1

    return {"REQ": req_w, "RSP": rsp_w, "DAT": dat_w, "SNP": snp_w}

print("\n=== check_sv: structural validation ===")

print("\n[1] flit widths in SV match chi_flit_cfg.hh")
cfg = parse_cfg(CFG_H)
expected = expected_flit_widths(cfg)

wrap_text = WRAP_SV.read_text()
# Extract localparam values from the SV package
sv_widths = {}
for name, pat in [("REQ","CHI_REQ_FLIT_W"), ("RSP","CHI_RSP_FLIT_W"),
                  ("DAT","CHI_DAT_FLIT_W"), ("SNP","CHI_SNP_FLIT_W")]:
    m = re.search(rf'localparam\s+int\s+{pat}\s*=\s*(\d+)', wrap_text)
    sv_widths[name] = int(m.group(1)) if m else None

for ch in ["REQ","RSP","DAT","SNP"]:
    sv  = sv_widths[ch]
    exp = expected[ch]
    check(sv == exp,
          f"CHI_{ch}_FLIT_W: SV={sv} expected={exp} (from cfg: {cfg})")

# ── 2. questa_top_wrap port completeness ─────────────────────────────────────
print("\n[2] questa_top_wrap declares all required CHI ports")
required_ports = [
    "clk", "rstn",
    "txreq_flitv", "txreq_flitpend", "txreq_flit", "txreq_lcrdv",
    "txrsp_flitv", "txrsp_flitpend", "txrsp_flit", "txrsp_lcrdv",
    "txdat_flitv", "txdat_flitpend", "txdat_flit", "txdat_lcrdv",
    "rxrsp_flitv", "rxrsp_flitpend", "rxrsp_flit", "rxrsp_lcrdv",
    "rxdat_flitv", "rxdat_flitpend", "rxdat_flit", "rxdat_lcrdv",
    "rxsnp_flitv", "rxsnp_flitpend", "rxsnp_flit", "rxsnp_lcrdv",
]
# Extract the port section of questa_top_wrap (between first '(' and ');')
m = re.search(r'module\s+questa_top_wrap\b.*?\((.*?)\);',
              wrap_text, re.DOTALL)
if m:
    port_block = m.group(1)
else:
    port_block = wrap_text

for p in required_ports:
    check(re.search(rf'\b{p}\b', port_block) is not None,
          f"port '{p}' present in questa_top_wrap")

# ── 3. No duplicate port names in questa_top_wrap ────────────────────────────
print("\n[3] no duplicate port declarations in questa_top_wrap")
if m:
    port_names = re.findall(r'\b(tx\w+|rx\w+|clk|rstn)\b\s*[,)]',
                            port_block)
    seen, dups = set(), set()
    for p in port_names:
        if p in seen:
            dups.add(p)
        seen.add(p)
    check(len(dups) == 0,
          f"no duplicate ports (dups={dups if dups else 'none'})")
else:
    check(False, "could not parse questa_top_wrap port block")

# ── 4. chi_vip_bind connects all ports of questa_top_wrap ────────────────────
print("\n[4] chi_vip_bind instantiates questa_top_wrap with all ports connected")
bind_m = re.search(r'module\s+chi_vip_bind\b.*?endmodule\s*:\s*chi_vip_bind',
                   wrap_text, re.DOTALL)
if bind_m:
    bind_block = bind_m.group(0)
    for p in required_ports:
        check(re.search(rf'\.{p}\s*\(', bind_block) is not None,
              f"chi_vip_bind connects .{p}()")
else:
    check(False, "chi_vip_bind module not found in questa_top_wrap.sv")

# ── 5. tb_chi_vip instantiates questa_top_wrap and connects all ports ─────────
print("\n[5] tb_chi_vip instantiates questa_top_wrap and connects all ports")
if not TB_SV.exists():
    check(False, "tb_chi_vip.sv not found")
else:
    tb_text = TB_SV.read_text()
    check("questa_top_wrap" in tb_text,
          "tb_chi_vip.sv references questa_top_wrap")
    tb_inst = re.search(r'questa_top_wrap\s+\w+\s*\((.*?)\);',
                        tb_text, re.DOTALL)
    if tb_inst:
        conn_block = tb_inst.group(1)
        for p in required_ports:
            check(re.search(rf'\.{p}\s*\(', conn_block) is not None,
                  f"tb instantiation connects .{p}()")
    else:
        check(False, "could not find questa_top_wrap instantiation in tb_chi_vip.sv")

# ── 6. tb uses correct field offsets matching chi_flit_pack.hh ───────────────
print("\n[6] tb field offsets consistent with cfg-derived values")
# REQ opcode LO = 4+NID+NID+TXN+NID+1+4+1 = 4+7+7+8+7+1+4+1 = 39
NID  = cfg["CHI_NODEID_W"]
TXN  = cfg["CHI_TXNID_W"]
req_opcode_lo = 4 + NID + NID + TXN + NID + 1 + 4 + 1  # = 39
rsp_opcode_lo = 4 + NID + NID + TXN                    # = 26

m_req = re.search(r'REQF_OPCODE_LO\s*=\s*(\d+)', tb_text)
m_rsp = re.search(r'RSPF_OPCODE_LO\s*=\s*(\d+)', tb_text)
if m_req:
    check(int(m_req.group(1)) == req_opcode_lo,
          f"REQF_OPCODE_LO={m_req.group(1)} (expected {req_opcode_lo})")
else:
    check(False, "REQF_OPCODE_LO not found in tb_chi_vip.sv")
if m_rsp:
    check(int(m_rsp.group(1)) == rsp_opcode_lo,
          f"RSPF_OPCODE_LO={m_rsp.group(1)} (expected {rsp_opcode_lo})")
else:
    check(False, "RSPF_OPCODE_LO not found in tb_chi_vip.sv")

# ── Summary ───────────────────────────────────────────────────────────────────
print(f"\n=== RESULTS: {pass_count} passed, {fail_count} failed ===")
sys.exit(1 if fail_count else 0)
