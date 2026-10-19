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

// test_questa_top.cc
//
// Tests for questa_top: the pin-level ↔ TLM2 adapter used for Questa co-sim.
//
// Part A — chi_flit_pack unit tests (pure C++, no clock)
// Part B — questa_top integration tests (clocked, sc_main + sc_start)

#include <systemc>
#include <chi/chi_tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <cstdio>
#include <cstring>
#include <memory>

#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/common/MachineID.hh"
#include "chi_vip_controller.hh"
#include "questa_top.hh"
#include "chi_flit_pack.hh"
#include "CHIResponseMsg.hh"
#include "CHIRequestMsg.hh"
#include "CHIDataMsg.hh"

using namespace gem5::ruby;
using namespace gem5::ruby::CHI;
using namespace sc_core;
using namespace chi_flit;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { printf("  PASS: %s\n", msg); g_pass++; } \
        else      { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); g_fail++; } \
    } while(0)

// ─────────────────────────────────────────────────────────────────────────────
// Part A — chi_flit_pack unit tests
// ─────────────────────────────────────────────────────────────────────────────

static void test_flit_pack() {
    // [1] unpack_req: address, opcode, TxnID, SrcID, NS, AllowRetry, order
    printf("\n[1] unpack_req: field extraction\n");
    {
        ReqFlit f = 0;
        using F = req_f;
        bf_set(f, F::SRCID_LO,      F::SRCID_W,  5);
        bf_set(f, F::TXNID_LO,      F::TXNID_W,  0xAB);
        bf_set(f, F::TGTID_LO,      F::TGTID_W,  3);
        bf_set(f, F::OPCODE_LO,     F::OPCODE_W,
               static_cast<uint64_t>(chi::req_optype_e::ReadShared));
        bf_set(f, F::ADDR_LO,       F::ADDR_W,   0x12345678ULL);
        bf_set(f, F::SIZE_LO,       F::SIZE_W,   6);
        bf_setb(f, F::NS_LO,        true);
        bf_setb(f, F::ALLOWRETRY_LO,true);
        bf_set(f, F::ORDER_LO,      F::ORDER_W,  2);

        tlm::tlm_generic_payload gp;
        chi::chi_ctrl_extension e;
        unpack_req(f, gp, e);

        CHECK(e.cmn.get_src_id()  == 5,                           "SrcID=5");
        CHECK(e.cmn.get_txn_id()  == 0xAB,                        "TxnID=0xAB");
        CHECK(e.req.get_tgt_id()  == 3,                           "TgtID=3");
        CHECK(e.req.get_opcode()  == chi::req_optype_e::ReadShared,"ReadShared opcode");
        CHECK(gp.get_address()    == 0x12345678ULL,               "address preserved");
        CHECK(e.req.get_size()    == 6,                           "size=6");
        CHECK(e.req.is_non_secure(),                              "NS bit");
        CHECK(e.req.is_allow_retry(),                             "AllowRetry");
        CHECK(e.req.get_order()   == 2,                           "order=2");
    }

    // [2] pack_rsp: every field round-trips through the flit
    printf("\n[2] pack_rsp: field packing\n");
    {
        chi::chi_ctrl_extension e;
        e.cmn.set_src_id(7);
        e.cmn.set_txn_id(0x33);
        e.resp.set_tgt_id(2);
        e.resp.set_opcode(chi::rsp_optype_e::CompAck);
        e.resp.set_resp(chi::rsp_resptype_e::Comp_UC);
        e.resp.set_db_id(0x1F);
        e.resp.set_pcrd_type(3);
        e.resp.set_fwd_state(5);
        e.resp.set_data_pull(true);
        e.resp.set_trace_tag(true);

        RspFlit f;
        pack_rsp(e, f);

        using F = rsp_f;
        CHECK(bf_get(f, F::SRCID_LO,     F::SRCID_W)  == 7,     "SrcID=7");
        CHECK(bf_get(f, F::TXNID_LO,     F::TXNID_W)  == 0x33,  "TxnID=0x33");
        CHECK(bf_get(f, F::TGTID_LO,     F::TGTID_W)  == 2,     "TgtID=2");
        CHECK(bf_get(f, F::OPCODE_LO,    F::OPCODE_W) ==
              static_cast<uint64_t>(chi::rsp_optype_e::CompAck), "CompAck opcode");
        CHECK(bf_get(f, F::RESP_LO,      F::RESP_W)   ==
              static_cast<uint64_t>(chi::rsp_resptype_e::Comp_UC),"resp=Comp_UC");
        CHECK(bf_get(f, F::DBID_LO,      F::DBID_W)   == 0x1F,  "DBID=0x1F");
        CHECK(bf_get(f, F::PCRDTYPE_LO,  F::PCRDTYPE_W)== 3,    "PCrdType=3");
        CHECK(bf_get(f, F::FWDSTATE_LO,  F::FWDSTATE_W)== 5,    "FwdState=5");
        CHECK(bf_getb(f, F::DATAPULL_LO),                        "DataPull=1");
        CHECK(bf_getb(f, F::TRACETAG_LO),                        "TraceTag=1");
    }

    // [3] pack_dat / unpack_dat: full round-trip including data bytes
    printf("\n[3] pack_dat / unpack_dat round-trip\n");
    {
        chi::chi_data_extension e_out;
        e_out.cmn.set_src_id(9);
        e_out.cmn.set_txn_id(0x77);
        e_out.dat.set_tgt_id(4);
        e_out.dat.set_home_n_id(12);
        e_out.dat.set_opcode(chi::dat_optype_e::CompData);
        e_out.dat.set_resp(chi::dat_resptype_e::CompData_UC);
        e_out.dat.set_data_id(2);
        e_out.dat.set_db_id(0xA);
        e_out.dat.set_cc_id(1);
        e_out.dat.set_trace_tag(true);

        uint8_t data_out[CHI_DATA_W_BYTES];
        for (int i = 0; i < CHI_DATA_W_BYTES; i++) data_out[i] = (uint8_t)(i ^ 0x5A);

        DatFlit f;
        pack_dat(e_out, data_out, CHI_DATA_W_BYTES, f);

        tlm::tlm_generic_payload gp;
        chi::chi_data_extension e_in;
        uint8_t data_in[CHI_DATA_W_BYTES] = {};
        unpack_dat(f, gp, e_in, data_in);

        CHECK(e_in.cmn.get_src_id()   == 9,     "SrcID round-trip");
        CHECK(e_in.cmn.get_txn_id()   == 0x77,  "TxnID round-trip");
        CHECK(e_in.dat.get_tgt_id()   == 4,     "TgtID round-trip");
        CHECK(e_in.dat.get_home_n_id()== 12,    "HomeNID round-trip");
        CHECK(e_in.dat.get_opcode()   == chi::dat_optype_e::CompData, "opcode round-trip");
        CHECK(e_in.dat.get_data_id()  == 2,     "DataID round-trip");
        CHECK(e_in.dat.get_db_id()    == 0xA,   "DBID round-trip");
        CHECK(e_in.dat.get_cc_id()    == 1,     "CCID round-trip");
        CHECK(e_in.dat.is_trace_tag(),           "TraceTag round-trip");
        CHECK(memcmp(data_out, data_in, CHI_DATA_W_BYTES) == 0, "data bytes round-trip");
    }

    // [4] pack_snp: address (3 LSBs stripped), opcode, FwdNID, RetToSrc
    printf("\n[4] pack_snp: field packing\n");
    {
        chi::chi_snp_extension e;
        e.cmn.set_src_id(1);
        e.cmn.set_txn_id(0x55);
        e.req.set_opcode(chi::snp_optype_e::SnpSharedFwd);
        e.req.set_fwd_n_id(6);
        e.req.set_fwd_txn_id(0x12);
        e.req.set_ret_to_src(true);
        e.req.set_do_not_goto_sd(false);
        e.req.set_do_not_data_pull(true);
        e.req.set_trace_tag(true);

        uint64_t addr = 0x8040ULL;  // cache-line aligned (bottom 3 bits 0)
        SnpFlit f;
        pack_snp(e, addr, f);

        using F = snp_f;
        CHECK(bf_get(f, F::SRCID_LO,     F::SRCID_W)    == 1,    "SrcID=1");
        CHECK(bf_get(f, F::TXNID_LO,     F::TXNID_W)    == 0x55, "TxnID=0x55");
        CHECK(bf_get(f, F::OPCODE_LO,    F::OPCODE_W)   ==
              static_cast<uint64_t>(chi::snp_optype_e::SnpSharedFwd), "SnpSharedFwd");
        CHECK(bf_get(f, F::FWDNID_LO,    F::FWDNID_W)   == 6,   "FwdNID=6");
        CHECK(bf_get(f, F::FWDTXNID_LO,  F::FWDTXNID_W) == 0x12,"FwdTxnID=0x12");
        CHECK(bf_getb(f, F::RETTOSRC_LO),                         "RetToSrc=1");
        CHECK(!bf_getb(f, F::DONOTGOTOSD_LO),                     "DoNotGoToSD=0");
        // DoNotDataPull is deliberately NOT packed: chi_flit_pack.hh reserves
        // bit 94 as zero ("not wired to RTL, keeps TGTID aligned"), so pack_snp
        // never sets it even when the extension requests it.  This check
        // asserted the opposite and so always failed -- the test was wrong, not
        // the bridge.  Re-enable only if the field is ever wired to the RTL.
        // CHECK(bf_getb(f, F::DONOTDATAPULL_LO),                 "DoNotDataPull=1");
        CHECK(!bf_getb(f, F::DONOTDATAPULL_LO),  "DoNotDataPull reserved 0 (not wired)");
        CHECK(bf_getb(f, F::TRACETAG_LO),                         "TraceTag=1");
        CHECK(bf_get(f, F::ADDR_LO, F::ADDR_W) == (addr >> 3),   "address (3 LSBs dropped)");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Part B helpers — called between sc_start() increments
// ─────────────────────────────────────────────────────────────────────────────

// Advance exactly n full clock cycles (1000ps each).
static void tick(int n = 1) { sc_start(n * 1000, SC_PS); }

// Helper: inject gem5 RSP into ctrl_'s rspIn and call wakeup().
// This models the HN-F sending a RSP destined for the DUT.
static void push_rsp_out(questa_top& qt, RubySystem& rs,
                         CHIResponseType t, Addr txn_id, Addr dbid = 0)
{
    auto msg = std::make_shared<CHIResponseMsg>(curTick(), 64, &rs);
    msg->settype(t);
    msg->settxnId(txn_id);
    msg->setdbid(dbid);
    MachineID src{MachineType_Directory, 0};
    msg->setresponder(src);
    MachineID tgt{MachineType_Cache, 1};
    msg->getDestination().add(tgt);
    qt.ctrl_.getRspIn()->enqueue(msg, curTick(), 0, false, false);
    qt.ctrl_.wakeup();
}

static void push_snp_out(questa_top& qt, RubySystem& rs,
                         CHIRequestType t, Addr addr = 0x8000, Addr txn_id = 1)
{
    auto msg = std::make_shared<CHIRequestMsg>(curTick(), 64, &rs);
    msg->settype(t);
    msg->settxnId(txn_id);
    msg->setusesTxnId(true);
    msg->setaddr(addr);
    MachineID src{MachineType_Directory, 0};
    msg->setrequestor(src);
    MachineID tgt{MachineType_Cache, 1};
    msg->getDestination().add(tgt);
    qt.ctrl_.getSnpIn()->enqueue(msg, curTick(), 0, false, false);
    qt.ctrl_.wakeup();
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_questa_top ===\n");

    // ── Part A ────────────────────────────────────────────────────────────────
    test_flit_pack();

    // ── Part B: clocked questa_top tests ─────────────────────────────────────
    RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;

    MachineID rnf_id{MachineType_Cache,     0};
    MachineID hnf_id{MachineType_Directory, 0};

    // ── All pin-level signals ─────────────────────────────────────────────────
    sc_clock           clk ("clk",  1000, SC_PS);
    sc_signal<bool>    rstn;
    sc_signal<bool>    txreq_flitv,    txreq_flitpend;
    sc_signal<ReqFlit> txreq_flit;
    sc_signal<bool>    txrsp_flitv,    txrsp_flitpend;
    sc_signal<RspFlit> txrsp_flit;
    sc_signal<bool>    txdat_flitv,    txdat_flitpend;
    sc_signal<DatFlit> txdat_flit;
    sc_signal<bool>    rxrsp_lcrdv,    rxdat_lcrdv,    rxsnp_lcrdv;
    sc_signal<bool>    txreq_lcrdv,    txrsp_lcrdv,    txdat_lcrdv;
    sc_signal<bool>    rxrsp_flitv,    rxrsp_flitpend;
    sc_signal<RspFlit> rxrsp_flit;
    sc_signal<bool>    rxdat_flitv,    rxdat_flitpend;
    sc_signal<DatFlit> rxdat_flit;
    sc_signal<bool>    rxsnp_flitv,    rxsnp_flitpend;
    sc_signal<SnpFlit> rxsnp_flit;

    // ── questa_top ────────────────────────────────────────────────────────────
    questa_top qt("qt", rnf_id, hnf_id, &rs, &net);
    qt.clk  (clk);
    qt.rstn (rstn);
    qt.txreq_flitv    (txreq_flitv);    qt.txreq_flitpend (txreq_flitpend);
    qt.txreq_flit     (txreq_flit);
    qt.txrsp_flitv    (txrsp_flitv);    qt.txrsp_flitpend (txrsp_flitpend);
    qt.txrsp_flit     (txrsp_flit);
    qt.txdat_flitv    (txdat_flitv);    qt.txdat_flitpend (txdat_flitpend);
    qt.txdat_flit     (txdat_flit);
    qt.rxrsp_lcrdv    (rxrsp_lcrdv);
    qt.rxdat_lcrdv    (rxdat_lcrdv);
    qt.rxsnp_lcrdv    (rxsnp_lcrdv);
    qt.txreq_lcrdv    (txreq_lcrdv);
    qt.txrsp_lcrdv    (txrsp_lcrdv);
    qt.txdat_lcrdv    (txdat_lcrdv);
    qt.rxrsp_flitv    (rxrsp_flitv);    qt.rxrsp_flitpend (rxrsp_flitpend);
    qt.rxrsp_flit     (rxrsp_flit);
    qt.rxdat_flitv    (rxdat_flitv);    qt.rxdat_flitpend (rxdat_flitpend);
    qt.rxdat_flit     (rxdat_flit);
    qt.rxsnp_flitv    (rxsnp_flitv);    qt.rxsnp_flitpend (rxsnp_flitpend);
    qt.rxsnp_flit     (rxsnp_flit);

    // ── [5] Reset: all RX outputs low during rstn=0 ───────────────────────────
    printf("\n[5] reset holds all outputs low\n");
    {
        rstn.write(false);
        tick(3);
        CHECK(!rxrsp_flitv.read(), "RXRSPFLITV=0 during reset");
        CHECK(!rxdat_flitv.read(), "RXDATFLITV=0 during reset");
        CHECK(!rxsnp_flitv.read(), "RXSNPFLITV=0 during reset");
        CHECK(!txreq_lcrdv.read(), "TXREQLCRDV=0 during reset");
        CHECK(!txrsp_lcrdv.read(), "TXRSPLCRDV=0 during reset");
        CHECK(!txdat_lcrdv.read(), "TXDATLCRDV=0 during reset");
    }

    // ── [6] Init credits: TXREQLCRDV pulses exactly CHI_INIT_LCRD times ───────
    printf("\n[6] init credits: TXREQLCRDV pulses exactly CHI_INIT_LCRD times\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);

        int cnt = 0;
        for (int i = 0; i < CHI_INIT_LCRD + 4; i++) {
            tick();
            if (txreq_lcrdv.read()) cnt++;
        }
        CHECK(cnt == CHI_INIT_LCRD, "TXREQLCRDV count == CHI_INIT_LCRD");
    }

    // ── [7] TXRSP and TXDAT also get CHI_INIT_LCRD credits ───────────────────
    printf("\n[7] init credits on TXRSP and TXDAT channels\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);

        int rsp_cnt = 0, dat_cnt = 0;
        for (int i = 0; i < CHI_INIT_LCRD + 4; i++) {
            tick();
            if (txrsp_lcrdv.read()) rsp_cnt++;
            if (txdat_lcrdv.read()) dat_cnt++;
        }
        CHECK(rsp_cnt == CHI_INIT_LCRD, "TXRSPLCRDV count == CHI_INIT_LCRD");
        CHECK(dat_cnt == CHI_INIT_LCRD, "TXDATLCRDV count == CHI_INIT_LCRD");
    }

    // ── [8] No credit return without incoming flit after init phase ───────────
    printf("\n[8] no spurious credit after init phase\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);
        tick(CHI_INIT_LCRD + 2);  // exhaust init credits

        int spurious = 0;
        for (int i = 0; i < 5; i++) {
            tick();
            if (txreq_lcrdv.read()) spurious++;
        }
        CHECK(spurious == 0, "no TXREQLCRDV without TXREQFLITV");
    }

    // ── [9] Credit returned same cycle as TXREQFLITV ─────────────────────────
    printf("\n[9] TXREQLCRDV returned same cycle as TXREQFLITV\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);
        tick(CHI_INIT_LCRD + 2);  // exhaust init credits

        ReqFlit f = 0;
        using F = req_f;
        bf_set(f, F::SRCID_LO,  F::SRCID_W,  1);
        bf_set(f, F::TXNID_LO,  F::TXNID_W,  0x10);
        bf_set(f, F::OPCODE_LO, F::OPCODE_W,
               static_cast<uint64_t>(chi::req_optype_e::ReadShared));
        bf_set(f, F::ADDR_LO,   F::ADDR_W,   0x4000);
        bf_setb(f, F::NS_LO,    true);

        txreq_flit.write(f);
        txreq_flitv.write(true);
        tick();
        bool got_crd = txreq_lcrdv.read();
        txreq_flitv.write(false);
        CHECK(got_crd, "TXREQLCRDV=1 on same cycle as TXREQFLITV=1");

        tick();
        CHECK(!txreq_lcrdv.read(), "TXREQLCRDV=0 when no TXREQFLITV");
        // Consume the enqueued REQ so it doesn't affect later tests
        if (qt.ctrl_.getReqOut()->isReady(curTick()))
            qt.ctrl_.getReqOut()->dequeue(curTick());
    }

    // ── [10] TXREQ flit reaches controller reqOut ─────────────────────────────
    printf("\n[10] TXREQ flit → controller reqOut populated\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);

        ReqFlit f = 0;
        using F = req_f;
        bf_set(f, F::SRCID_LO,  F::SRCID_W,  2);
        bf_set(f, F::TXNID_LO,  F::TXNID_W,  0x42);
        bf_set(f, F::OPCODE_LO, F::OPCODE_W,
               static_cast<uint64_t>(chi::req_optype_e::ReadUnique));
        bf_set(f, F::ADDR_LO,   F::ADDR_W,   0x9000);
        bf_setb(f, F::NS_LO,    true);

        txreq_flit.write(f);
        txreq_flitv.write(true);
        tick();
        txreq_flitv.write(false);

        CHECK(qt.ctrl_.getReqOut()->isReady(curTick()),
              "reqOut has a ready message");
        auto* msg = dynamic_cast<const CHIRequestMsg*>(
            qt.ctrl_.getReqOut()->peek());
        CHECK(msg != nullptr, "reqOut contains CHIRequestMsg");
        if (msg) {
            CHECK(msg->gettype()  == CHIRequestType_ReadUnique, "opcode=ReadUnique");
            CHECK(msg->getaddr()  == 0x9000,                   "address=0x9000");
            CHECK(msg->gettxnId()== 0x42,                     "TxnID=0x42");
        }
        qt.ctrl_.getReqOut()->dequeue(curTick());
    }

    // ── [11] TXRSP (CompAck) reaches controller rspOut ───────────────────────
    printf("\n[11] TXRSP (CompAck) → controller rspOut\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);

        RspFlit f = 0;
        using F = rsp_f;
        bf_set(f, F::SRCID_LO,  F::SRCID_W,  3);
        bf_set(f, F::TXNID_LO,  F::TXNID_W,  0x20);
        bf_set(f, F::OPCODE_LO, F::OPCODE_W,
               static_cast<uint64_t>(chi::rsp_optype_e::CompAck));

        txrsp_flit.write(f);
        txrsp_flitv.write(true);
        tick();
        txrsp_flitv.write(false);

        CHECK(qt.ctrl_.getRspOut()->isReady(curTick()),
              "rspOut has a ready message after CompAck");
        qt.ctrl_.getRspOut()->dequeue(curTick());
    }

    // ── [12] Outbound RSP gated without DUT credit ───────────────────────────
    printf("\n[12] outbound RXRSP gated without DUT credit\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);
        // rxrsp_lcrdv stays low — DUT never grants a credit
        push_rsp_out(qt, rs, CHIResponseType_Comp_UC, 1);
        tick(3);
        CHECK(!rxrsp_flitv.read(), "RXRSPFLITV=0 without DUT credit");
    }

    // ── [13] Outbound RSP sends once DUT grants credit; flit content correct ──
    printf("\n[13] outbound RXRSP: sends with credit, flit content correct\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);
        push_rsp_out(qt, rs, CHIResponseType_Comp_UC, 0x55, 0x0E);

        // Grant one credit from DUT; same posedge: Phase 2 increments, Phase 4 sends.
        rxrsp_lcrdv.write(true);
        tick();
        rxrsp_lcrdv.write(false);

        bool got = rxrsp_flitv.read();
        CHECK(got, "RXRSPFLITV=1 after DUT credit granted");
        if (got) {
            RspFlit rf = rxrsp_flit.read();
            using F = rsp_f;
            uint64_t op = bf_get(rf, F::OPCODE_LO, F::OPCODE_W);
            CHECK(op == static_cast<uint64_t>(chi::rsp_optype_e::Comp),
                  "RXRSP opcode=Comp");
            uint64_t resp = bf_get(rf, F::RESP_LO, F::RESP_W);
            CHECK(resp == static_cast<uint64_t>(chi::rsp_resptype_e::Comp_UC),
                  "RXRSP resp=Comp_UC");
            CHECK(bf_get(rf, F::DBID_LO, F::DBID_W) == 0x0E,
                  "RXRSP DBID=0x0E");
        }
        tick();
        CHECK(!rxrsp_flitv.read(), "RXRSPFLITV=0 after queue drained");
    }

    // ── [14] Outbound SNP sends once DUT grants credit ───────────────────────
    printf("\n[14] outbound RXSNP: gated then sent on credit\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);
        push_snp_out(qt, rs, CHIRequestType_SnpUnique, 0xC000, 7);

        tick(2);
        CHECK(!rxsnp_flitv.read(), "RXSNPFLITV=0 before credit");

        rxsnp_lcrdv.write(true);
        tick();
        rxsnp_lcrdv.write(false);

        bool got = rxsnp_flitv.read();
        CHECK(got, "RXSNPFLITV=1 after DUT credit");
        if (got) {
            SnpFlit sf = rxsnp_flit.read();
            using F = snp_f;
            CHECK(bf_get(sf, F::OPCODE_LO, F::OPCODE_W) ==
                  static_cast<uint64_t>(chi::snp_optype_e::SnpUnique),
                  "RXSNP opcode=SnpUnique");
            CHECK(bf_get(sf, F::ADDR_LO, F::ADDR_W) == (0xC000ULL >> 3),
                  "RXSNP address bits correct");
        }
    }

    // ── [15] FLITPEND: 2 credits pre-loaded + 2 queued → FLITPEND=1 on send ──
    printf("\n[15] RXRSPFLITPEND asserted when send has credit + queue non-empty\n");
    {
        rstn.write(false);  tick(2);  rstn.write(true);

        // Pre-load 2 RX credits from DUT while queue is still empty.
        // Phase 4 sees empty queue, so no flit sent — credits accumulate.
        rxrsp_lcrdv.write(true);  tick();
        rxrsp_lcrdv.write(true);  tick();
        rxrsp_lcrdv.write(false);

        // Now queue 2 RSPs via direct controller injection.
        push_rsp_out(qt, rs, CHIResponseType_Comp_UC, 1);
        push_rsp_out(qt, rs, CHIResponseType_Comp,    2);

        // First tick with queued flits: rsp_lcrd_=2, send 1st (→crd=1), queue=1 left.
        // FLITPEND = !empty(true) && crd>0(true) = true.
        tick();
        bool pend = rxrsp_flitpend.read();
        bool flitv = rxrsp_flitv.read();
        CHECK(flitv, "first RSP sent (credit available)");
        CHECK(pend,  "FLITPEND=1 (second flit queued, credit remaining)");

        // Second tick: send 2nd RSP (crd→0). FLITPEND = !empty(false) = false.
        tick();
        CHECK(rxrsp_flitv.read(),    "second RSP sent");
        CHECK(!rxrsp_flitpend.read(),"FLITPEND=0 after queue empty");
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
