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

// test_edge_cases.cc
//
// Tests for edge cases and boundary conditions not covered elsewhere:
//   • AtomicNoReturn outbound: was AtomicStoreAdd, must be AtomicCompare [BUG 16]
//   • wakeup() with empty buffers (no crash)
//   • b_transport_rxdat data_length=0 fallback to cacheLineSize
//   • b_transport_rxdat data_length > cacheLineSize clamps to 64
//   • No-extension error responses on all three inbound sockets
//   • Multiple wakeup() calls drain idempotently (second call after empty)
//   • Very large TxnID values (all bits used)
//   • gem5req_to_scc covers all atomic variants without hitting the default

#include <systemc>
#include <chi/chi_tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <cstdio>
#include <cstring>
#include <vector>

#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/common/MachineID.hh"
#include "chi_vip_controller.hh"
#include "CHIRequestMsg.hh"
#include "CHIResponseMsg.hh"
#include "CHIDataMsg.hh"

using namespace gem5::ruby;
using namespace gem5::ruby::CHI;
using namespace sc_core;

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) \
    do { \
        if (cond) { printf("  PASS: %s\n", msg); g_pass++; } \
        else      { printf("  FAIL: %s  (line %d)\n", msg, __LINE__); g_fail++; } \
    } while(0)

// ── Catch and inject helpers ─────────────────────────────────────────────────

SC_MODULE(CatchAll) {
    tlm_utils::simple_target_socket<CatchAll> rsp_sock;
    tlm_utils::simple_target_socket<CatchAll> dat_sock;
    tlm_utils::simple_target_socket<CatchAll> snp_sock;

    int rsp_count = 0, dat_count = 0, snp_count = 0;
    chi::rsp_optype_e last_rsp_op = chi::rsp_optype_e::Comp;
    chi::dat_optype_e last_dat_op = chi::dat_optype_e::CompData;

    SC_CTOR(CatchAll)
      : rsp_sock("rsp_sock"), dat_sock("dat_sock"), snp_sock("snp_sock")
    {
        rsp_sock.register_b_transport(this, &CatchAll::b_rsp);
        dat_sock.register_b_transport(this, &CatchAll::b_dat);
        snp_sock.register_b_transport(this, &CatchAll::b_snp);
    }
    void b_rsp(tlm::tlm_generic_payload& gp, sc_time&) {
        if (auto* e = gp.get_extension<chi::chi_ctrl_extension>())
            last_rsp_op = e->resp.get_opcode();
        rsp_count++;
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_dat(tlm::tlm_generic_payload& gp, sc_time&) {
        if (auto* e = gp.get_extension<chi::chi_data_extension>())
            last_dat_op = e->dat.get_opcode();
        dat_count++;
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void b_snp(tlm::tlm_generic_payload& gp, sc_time&) {
        snp_count++;
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void reset() { rsp_count = dat_count = snp_count = 0; }
};

SC_MODULE(InjectRaw) {
    tlm_utils::simple_initiator_socket<InjectRaw> req_sock;
    tlm_utils::simple_initiator_socket<InjectRaw> dat_sock;
    tlm_utils::simple_initiator_socket<InjectRaw> rsp_sock;
    SC_CTOR(InjectRaw)
      : req_sock("req_sock"), dat_sock("dat_sock"), rsp_sock("rsp_sock") {}

    // Send with explicit data_length (for edge-case tests); no extension
    tlm::tlm_response_status send_noext(
            tlm_utils::simple_initiator_socket<InjectRaw>& s,
            Addr addr, int data_len)
    {
        static uint8_t buf[128] = {};
        tlm::tlm_generic_payload gp;
        gp.set_address(addr);
        gp.set_command(tlm::TLM_WRITE_COMMAND);
        gp.set_data_ptr(data_len > 0 ? buf : nullptr);
        gp.set_data_length(static_cast<unsigned>(data_len));
        sc_time d = SC_ZERO_TIME;
        s->b_transport(gp, d);
        return gp.get_response_status();
    }

    // Send DAT beat with explicit data_length
    tlm::tlm_response_status send_dat_ext(unsigned txn, uint8_t data_id,
                                         const uint8_t* data, int data_len,
                                         chi::dat_resptype_e resp =
                                           chi::dat_resptype_e::CopyBackWrData_UC)
    {
        auto* ext = new chi::chi_data_extension();
        ext->cmn.set_txn_id(txn);
        ext->cmn.set_src_id(1);
        ext->dat.set_opcode(chi::dat_optype_e::CopyBackWrData);
        ext->dat.set_data_id(data_id);
        ext->dat.set_resp(resp);

        static uint8_t buf[128] = {};
        if (data && data_len > 0)
            memcpy(buf, data, static_cast<size_t>(data_len));

        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(0x1000);
        gp.set_command(tlm::TLM_WRITE_COMMAND);
        gp.set_data_ptr(data_len > 0 ? buf : nullptr);
        gp.set_data_length(static_cast<unsigned>(data_len > 0 ? data_len : 0));
        sc_time d = SC_ZERO_TIME;
        dat_sock->b_transport(gp, d);
        return gp.get_response_status();
    }
};

static void push_rsp_out(CHI_VIP_Controller& ctrl, CHIResponseType t,
                          Addr txn, RubySystem& rs) {
    auto msg = std::make_shared<CHIResponseMsg>(curTick(), 64, &rs);
    msg->settype(t); msg->settxnId(txn);
    MachineID src{MachineType_Directory, 0}; msg->setresponder(src);
    MachineID tgt{MachineType_Cache, 1};     msg->getDestination().add(tgt);
    ctrl.getRspIn()->enqueue(msg, curTick(), 0, false, false);
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_edge_cases ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{MachineType_Directory, 0};

    CHI_VIP_Controller ctrl("ctrl", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    CatchAll  catchers("catchers");
    InjectRaw inj("inj");

    ctrl.txrsp_socket.bind(catchers.rsp_sock);
    ctrl.txdat_socket.bind(catchers.dat_sock);
    ctrl.txsnp_socket.bind(catchers.snp_sock);
    inj.req_sock.bind(ctrl.rxreq_socket);
    inj.dat_sock.bind(ctrl.rxdat_socket);
    inj.rsp_sock.bind(ctrl.rxrsp_socket);
    ctrl.init();

    // ── 1. AtomicNoReturn → AtomicCompare (was AtomicStoreAdd) ───────────────
    printf("\n[1] gem5req_to_scc: AtomicNoReturn → AtomicCompare\n");
    {
        using O = chi::req_optype_e;
        O got = CHI_VIP_Controller::gem5req_to_scc(CHIRequestType_AtomicNoReturn);
        CHECK(got == O::AtomicCompare,
              "AtomicNoReturn → AtomicCompare (not AtomicStoreAdd)");
        // Also verify AtomicReturn is still AtomicSwap
        O got2 = CHI_VIP_Controller::gem5req_to_scc(CHIRequestType_AtomicReturn);
        CHECK(got2 == O::AtomicSwap, "AtomicReturn → AtomicSwap unchanged");
    }

    // ── 2. All AtomicStore variants still map to AtomicStore ─────────────────
    printf("\n[2] gem5req_to_scc AtomicStore → AtomicStoreAdd group\n");
    {
        using O = chi::req_optype_e;
        // AtomicStore in gem5 encompasses Add/Clr/Eor/Set variants
        O got = CHI_VIP_Controller::gem5req_to_scc(CHIRequestType_AtomicStore);
        // Should be one of the AtomicStore* group
        bool is_store = (got == O::AtomicStoreAdd || got == O::AtomicStoreClr ||
                         got == O::AtomicStoreEor || got == O::AtomicStoreSet);
        CHECK(is_store, "AtomicStore maps to an AtomicStore* variant");
    }

    // ── 3. wakeup() with empty buffers — no crash ────────────────────────────
    printf("\n[3] wakeup() with all In buffers empty — no crash\n");
    {
        catchers.reset();
        ctrl.wakeup();
        CHECK(catchers.rsp_count == 0 && catchers.dat_count == 0 &&
              catchers.snp_count == 0,
              "wakeup() on empty buffers delivers nothing");
        // Second wakeup() immediately after — still no crash
        ctrl.wakeup();
        CHECK(catchers.rsp_count == 0, "second empty wakeup() also safe");
    }

    // ── 4. b_transport_rxreq: no extension → error response ──────────────────
    printf("\n[4] no-extension on inbound sockets → TLM_GENERIC_ERROR_RESPONSE\n");
    {
        auto st_req = inj.send_noext(inj.req_sock, 0x4000, 0);
        CHECK(st_req == tlm::TLM_GENERIC_ERROR_RESPONSE,
              "rxreq no-extension → TLM_GENERIC_ERROR_RESPONSE");

        auto st_dat = inj.send_noext(inj.dat_sock, 0x4000, 64);
        CHECK(st_dat == tlm::TLM_GENERIC_ERROR_RESPONSE,
              "rxdat no-extension → TLM_GENERIC_ERROR_RESPONSE");

        auto st_rsp = inj.send_noext(inj.rsp_sock, 0x4000, 0);
        CHECK(st_rsp == tlm::TLM_GENERIC_ERROR_RESPONSE,
              "rxrsp no-extension → TLM_GENERIC_ERROR_RESPONSE");
    }

    // ── 5. b_transport_rxdat: data_length=0 falls back to single-beat 64B ────
    printf("\n[5] b_transport_rxdat data_length=0 → single 64-byte beat\n");
    {
        uint8_t fill[64];
        memset(fill, 0xCC, 64);
        // data_len=0 but we must still provide a data ptr; the fallback sets
        // beat_size = cacheLineSize (64), num_beats = 1
        auto st = inj.send_dat_ext(/*txn=*/5, /*data_id=*/0, fill, /*len=*/0);
        // With len=0, gp.get_data_length()==0, fallback kicks in.
        // The message should appear (num_beats=1, 1 beat done ≥ 1).
        const auto* m = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        CHECK(m && m->gettxnId() == 5,
              "data_length=0 treated as single-beat; message enqueued");
        if (m) ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 6. Large TxnID (0xFFF) preserved through unpack_req ─────────────────
    printf("\n[6] large TxnID (0xFFF) preserved through all three inbound paths\n");
    {
        // REQ
        {
            auto* ext = new chi::chi_ctrl_extension();
            ext->cmn.set_txn_id(0xFFF);
            ext->cmn.set_src_id(1);
            ext->req.set_opcode(chi::req_optype_e::ReadShared);
            ext->req.set_tgt_id(0);
            tlm::tlm_generic_payload gp;
            gp.set_extension(ext);
            gp.set_address(0x6000);
            gp.set_command(tlm::TLM_IGNORE_COMMAND);
            gp.set_data_length(0);
            gp.set_data_ptr(nullptr);
            sc_time d = SC_ZERO_TIME;
            inj.req_sock->b_transport(gp, d);
        }
        const auto* req_m = dynamic_cast<const CHIRequestMsg*>(ctrl.getReqOut()->peek());
        CHECK(req_m && req_m->gettxnId() == 0xFFF,
              "REQ TxnID=0xFFF preserved");
        if (req_m) ctrl.getReqOut()->dequeue(curTick());

        // DAT
        {
            auto* ext = new chi::chi_data_extension();
            ext->cmn.set_txn_id(0xFFF);
            ext->cmn.set_src_id(1);
            ext->dat.set_opcode(chi::dat_optype_e::CopyBackWrData);
            ext->dat.set_data_id(0);
            ext->dat.set_resp(chi::dat_resptype_e::CopyBackWrData_UC);
            static uint8_t buf[64] = {};
            tlm::tlm_generic_payload gp;
            gp.set_extension(ext);
            gp.set_address(0);
            gp.set_command(tlm::TLM_WRITE_COMMAND);
            gp.set_data_ptr(buf);
            gp.set_data_length(64);
            sc_time d = SC_ZERO_TIME;
            inj.dat_sock->b_transport(gp, d);
        }
        const auto* dat_m = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        CHECK(dat_m && dat_m->gettxnId() == 0xFFF,
              "DAT TxnID=0xFFF preserved");
        if (dat_m) ctrl.getDatOut()->dequeue(curTick());

        // RSP
        {
            auto* ext = new chi::chi_ctrl_extension();
            ext->cmn.set_txn_id(0xFFF);
            ext->cmn.set_src_id(1);
            ext->resp.set_opcode(chi::rsp_optype_e::CompAck);
            ext->resp.set_tgt_id(0);
            tlm::tlm_generic_payload gp;
            gp.set_extension(ext);
            gp.set_address(0);
            gp.set_command(tlm::TLM_IGNORE_COMMAND);
            gp.set_data_length(0);
            gp.set_data_ptr(nullptr);
            sc_time d = SC_ZERO_TIME;
            inj.rsp_sock->b_transport(gp, d);
        }
        const auto* rsp_m = dynamic_cast<const CHIResponseMsg*>(ctrl.getRspOut()->peek());
        CHECK(rsp_m && rsp_m->gettxnId() == 0xFFF,
              "RSP TxnID=0xFFF preserved");
        if (rsp_m) ctrl.getRspOut()->dequeue(curTick());
    }

    // ── 7. wakeup() after partial delivery — remainder on next wakeup ─────────
    printf("\n[7] wakeup() twice: first delivers pending, second is empty\n");
    {
        catchers.reset();
        push_rsp_out(ctrl, CHIResponseType_Comp_I, 70, rs);
        ctrl.wakeup();
        int after_first = catchers.rsp_count;
        ctrl.wakeup();
        int after_second = catchers.rsp_count;
        CHECK(after_first == 1,  "first wakeup delivers 1 RSP");
        CHECK(after_second == 1, "second wakeup on empty delivers nothing extra");
    }

    // ── 8. Zero-address REQ preserved correctly ───────────────────────────────
    printf("\n[8] address=0x0 is valid and preserved\n");
    {
        auto* ext = new chi::chi_ctrl_extension();
        ext->cmn.set_txn_id(80);
        ext->cmn.set_src_id(2);
        ext->req.set_opcode(chi::req_optype_e::ReadOnce);
        ext->req.set_tgt_id(0);
        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(0x0);
        gp.set_command(tlm::TLM_IGNORE_COMMAND);
        gp.set_data_length(0);
        gp.set_data_ptr(nullptr);
        sc_time d = SC_ZERO_TIME;
        inj.req_sock->b_transport(gp, d);

        const auto* m = dynamic_cast<const CHIRequestMsg*>(ctrl.getReqOut()->peek());
        CHECK(m && m->getaddr() == 0x0, "address=0 preserved");
        if (m) ctrl.getReqOut()->dequeue(curTick());
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    sc_start(1, SC_NS);
    return g_fail != 0 ? 1 : 0;
}
