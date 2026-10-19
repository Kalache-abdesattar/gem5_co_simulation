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

// questa_cosim_top.cc — sc_main for Questa vsim co-simulation
//
// DUAL USE:
//
//   1. Questa co-sim (default):
//      Compiled with sccom/sclink; vsim drives sc_start().
//      sc_main creates questa_top and returns immediately — Questa's
//      SystemC kernel takes over.  Port binding happens through
//      scgencomp-generated stubs that vsim maps automatically.
//      Build: see chi_vip/questa/ Makefile comments.
//
//   2. Standalone smoke test (-DQUESTA_STANDALONE_TEST):
//      Compiled with g++; sc_main drives clock manually for a few cycles
//      to verify the C++ side compiles and initialises cleanly.
//      Build: see Makefile target 'build/questa_cosim_test'.
//
// Node-ID configuration (tune to your topology):
//   VIP_NODE_NUM — MachineID.num for the questa_top (Cache type), default 0
//   HNF_NODE_NUM — MachineID.num for the downstream HN-F (Directory), default 0
//   VIP_CACHE_LINE — cache-line width in bytes, default 64 (512-bit bus)
//
// Override at compile time:
//   -DVIP_NODE_NUM=2 -DHNF_NODE_NUM=0 -DVIP_CACHE_LINE=64

#include <systemc>
#include <cstdio>

#include "mem/ruby/network/Network.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/common/MachineID.hh"
#include "questa_top.hh"

#ifndef VIP_NODE_NUM
#  define VIP_NODE_NUM 0
#endif
#ifndef HNF_NODE_NUM
#  define HNF_NODE_NUM 0
#endif
#ifndef VIP_CACHE_LINE
#  define VIP_CACHE_LINE 64
#endif

// ─────────────────────────────────────────────────────────────────────────────
// Global shim objects
// These must outlive sc_main's return because questa_top holds pointers to them.
// In a full gem5+SC flow, replace these with pointers to gem5's RubySystem and
// Network objects obtained after gem5_initialize().
// ─────────────────────────────────────────────────────────────────────────────
static gem5::ruby::RubySystem g_rs;
static Network                g_net;

// ─────────────────────────────────────────────────────────────────────────────
// sc_main
// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int argc, char* argv[])
{
    using namespace sc_core;
    using namespace gem5::ruby;   // brings ReqFlit/RspFlit/DatFlit/SnpFlit

    // ── Shim configuration ───────────────────────────────────────────────────
    g_rs.m_cache_line_size = VIP_CACHE_LINE;

    const MachineID vip_id  {MachineType_Cache,     VIP_NODE_NUM};
    const MachineID hnf_id  {MachineType_Directory, HNF_NODE_NUM};

    printf("[questa_cosim_top] VIP  node: %s\n", vip_id.to_string().c_str());
    printf("[questa_cosim_top] HN-F node: %s\n", hnf_id.to_string().c_str());
    printf("[questa_cosim_top] Cache line: %d bytes\n", VIP_CACHE_LINE);

    // ── questa_top instantiation ─────────────────────────────────────────────
    // In vsim co-sim mode sc_main returns immediately after elaboration;
    // Questa's runtime then calls sc_start() and drives simulation.
    // The questa_top instance MUST outlive sc_main's return frame, so it is
    // declared static.  Questa manages the SC kernel lifecycle externally.
    static questa_top qt("qt", vip_id, hnf_id, &g_rs, &g_net);

#ifndef QUESTA_STANDALONE_TEST
    // ── vsim co-sim path ─────────────────────────────────────────────────────
    // Port binding is handled automatically by Questa's scgencomp mechanism.
    printf("[questa_cosim_top] SC elaboration done, vsim takes over.\n");
    return 0;

#else
    // ── Standalone smoke path ─────────────────────────────────────────────────
    // Bind signals manually and drive clock for 100 cycles.
    // This verifies the C++ side compiles and ctrl_.init() works.

    sc_signal<bool>   clk_s   {"clk_s",   false};
    sc_signal<bool>   rstn_s  {"rstn_s",  false};

    sc_signal<bool>     txreq_flitv_s    {"txreq_flitv_s",    false};
    sc_signal<bool>     txreq_flitpend_s {"txreq_flitpend_s", false};
    sc_signal<ReqFlit>  txreq_flit_s    {"txreq_flit_s"};

    sc_signal<bool>     txrsp_flitv_s    {"txrsp_flitv_s",    false};
    sc_signal<bool>     txrsp_flitpend_s {"txrsp_flitpend_s", false};
    sc_signal<RspFlit>  txrsp_flit_s    {"txrsp_flit_s"};

    sc_signal<bool>     txdat_flitv_s    {"txdat_flitv_s",    false};
    sc_signal<bool>     txdat_flitpend_s {"txdat_flitpend_s", false};
    sc_signal<DatFlit>  txdat_flit_s    {"txdat_flit_s"};

    sc_signal<bool>     txreq_lcrdv_s    {"txreq_lcrdv_s",    false};
    sc_signal<bool>     txrsp_lcrdv_s    {"txrsp_lcrdv_s",    false};
    sc_signal<bool>     txdat_lcrdv_s    {"txdat_lcrdv_s",    false};

    sc_signal<bool>     rxrsp_flitv_s    {"rxrsp_flitv_s",    false};
    sc_signal<bool>     rxrsp_flitpend_s {"rxrsp_flitpend_s", false};
    sc_signal<RspFlit>  rxrsp_flit_s    {"rxrsp_flit_s"};
    sc_signal<bool>     rxrsp_lcrdv_s    {"rxrsp_lcrdv_s",    false};

    sc_signal<bool>     rxdat_flitv_s    {"rxdat_flitv_s",    false};
    sc_signal<bool>     rxdat_flitpend_s {"rxdat_flitpend_s", false};
    sc_signal<DatFlit>  rxdat_flit_s    {"rxdat_flit_s"};
    sc_signal<bool>     rxdat_lcrdv_s    {"rxdat_lcrdv_s",    false};

    sc_signal<bool>     rxsnp_flitv_s    {"rxsnp_flitv_s",    false};
    sc_signal<bool>     rxsnp_flitpend_s {"rxsnp_flitpend_s", false};
    sc_signal<SnpFlit>  rxsnp_flit_s    {"rxsnp_flit_s"};
    sc_signal<bool>     rxsnp_lcrdv_s    {"rxsnp_lcrdv_s",    false};

    // ── Port binding ──────────────────────────────────────────────────────────
    qt.clk            (clk_s);
    qt.rstn           (rstn_s);

    qt.txreq_flitv    (txreq_flitv_s);
    qt.txreq_flitpend (txreq_flitpend_s);
    qt.txreq_flit     (txreq_flit_s);
    qt.txreq_lcrdv    (txreq_lcrdv_s);

    qt.txrsp_flitv    (txrsp_flitv_s);
    qt.txrsp_flitpend (txrsp_flitpend_s);
    qt.txrsp_flit     (txrsp_flit_s);
    qt.txrsp_lcrdv    (txrsp_lcrdv_s);

    qt.txdat_flitv    (txdat_flitv_s);
    qt.txdat_flitpend (txdat_flitpend_s);
    qt.txdat_flit     (txdat_flit_s);
    qt.txdat_lcrdv    (txdat_lcrdv_s);

    qt.rxrsp_flitv    (rxrsp_flitv_s);
    qt.rxrsp_flitpend (rxrsp_flitpend_s);
    qt.rxrsp_flit     (rxrsp_flit_s);
    qt.rxrsp_lcrdv    (rxrsp_lcrdv_s);

    qt.rxdat_flitv    (rxdat_flitv_s);
    qt.rxdat_flitpend (rxdat_flitpend_s);
    qt.rxdat_flit     (rxdat_flit_s);
    qt.rxdat_lcrdv    (rxdat_lcrdv_s);

    qt.rxsnp_flitv    (rxsnp_flitv_s);
    qt.rxsnp_flitpend (rxsnp_flitpend_s);
    qt.rxsnp_flit     (rxsnp_flit_s);
    qt.rxsnp_lcrdv    (rxsnp_lcrdv_s);

    // ── Clock driver: 20 cycles in reset, then 80 cycles running ─────────────
    // rstn is set BEFORE the posedge so it is stable when the SC_METHOD samples
    // it.  The clock period is 1000 ps.
    auto tick = [&](bool r) {
        rstn_s.write(r);
        clk_s.write(false); sc_start(500, SC_PS);
        clk_s.write(true);  sc_start(500, SC_PS);
    };

    printf("[questa_cosim_top] Standalone: running 20 reset + 80 active cycles\n");

    for (int i = 0; i < 20; i++) tick(false);   // hold in reset
    for (int i = 0; i < 80; i++) tick(true);    // run

    // After CHI_INIT_LCRD (=15) cycles of rstn=1, all init-credit counters
    // should be exhausted (== 0).
    int checks_pass = 0;
    if (qt.req_lcrd_init_ == 0) {
        printf("  PASS: req_lcrd_init_ exhausted after 80 active cycles\n");
        checks_pass++;
    } else {
        printf("  FAIL: req_lcrd_init_ not exhausted (%d)\n", qt.req_lcrd_init_);
    }
    if (qt.rsp_lcrd_init_ == 0) {
        printf("  PASS: rsp_lcrd_init_ exhausted\n");
        checks_pass++;
    } else {
        printf("  FAIL: rsp_lcrd_init_ not exhausted (%d)\n", qt.rsp_lcrd_init_);
    }
    if (qt.dat_lcrd_init_ == 0) {
        printf("  PASS: dat_lcrd_init_ exhausted\n");
        checks_pass++;
    } else {
        printf("  FAIL: dat_lcrd_init_ not exhausted (%d)\n", qt.dat_lcrd_init_);
    }

    printf("[questa_cosim_top] Smoke: %d/3 checks pass\n", checks_pass);
    return (checks_pass == 3) ? 0 : 1;
#endif
}
