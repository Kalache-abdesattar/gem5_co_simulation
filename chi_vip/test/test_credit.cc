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

// test_credit.cc
//
// Tests for CHI P-Credit (protocol credit / retry) flow through CHI_VIP_Controller.
//
// CHI retry handshake:
//   1. RN-F sends REQ with AllowRetry=1 (telling HN-F it may refuse)
//   2. HN-F sends RetryAck (insufficient resources at this moment)
//   3. HN-F sends PCrdGrant (credit granted, RN-F may now retry)
//   4. RN-F retries with AllowRetry=0 (credit consumed on retry)
//
// The bridge must:
//   • Forward AllowRetry bit from inbound TLM REQ → CHIRequestMsg::m_allowRetry
//   • Pack PCrdGrant correctly into chi_ctrl_extension on txrsp_socket
//   • Handle the full sequence without dropping or reordering messages

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
#include "CHIResponseMsg.hh"
#include "CHIRequestMsg.hh"

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

// ── InjectReq: drives rxreq_socket with configurable AllowRetry ──────────────
SC_MODULE(InjectReq) {
    tlm_utils::simple_initiator_socket<InjectReq> sock;
    SC_CTOR(InjectReq) : sock("sock") {}

    void send(Addr addr, chi::req_optype_e op, unsigned txn_id,
              unsigned src_id, bool allow_retry, bool ns = true)
    {
        auto* ext = new chi::chi_ctrl_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(src_id);
        ext->req.set_opcode(op);
        ext->req.set_tgt_id(0);
        ext->req.set_non_secure(ns);
        ext->req.set_allow_retry(allow_retry);

        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(addr);
        gp.set_command(tlm::TLM_IGNORE_COMMAND);
        gp.set_data_length(0);
        gp.set_data_ptr(nullptr);
        sc_time d = SC_ZERO_TIME;
        sock->b_transport(gp, d);
    }
};

// ── CatchRsp: captures every b_transport call on txrsp_socket ────────────────
SC_MODULE(CatchRsp) {
    tlm_utils::simple_target_socket<CatchRsp> sock;

    struct Beat {
        std::unique_ptr<chi::chi_ctrl_extension> ctrl;
    };
    std::vector<Beat> beats;

    SC_CTOR(CatchRsp) : sock("sock") {
        sock.register_b_transport(this, &CatchRsp::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& gp, sc_time&) {
        Beat b{};
        if (auto* e = gp.get_extension<chi::chi_ctrl_extension>())
            b.ctrl.reset(static_cast<chi::chi_ctrl_extension*>(e->clone()));
        beats.push_back(std::move(b));
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void reset() { beats.clear(); }
};

// ── inject a CHIResponseMsg into rspIn (simulates HN-F sending to Questa) ────
static void push_rsp(CHI_VIP_Controller& ctrl,
                     CHIResponseType rtype, Addr txn_id,
                     RubySystem& rs)
{
    auto msg = std::make_shared<CHIResponseMsg>(curTick(), 64, &rs);
    msg->settype(rtype);
    msg->settxnId(txn_id);
    msg->setdbid(0);
    MachineID src{MachineType_Directory, 0};
    msg->setresponder(src);
    ctrl.getRspIn()->enqueue(msg, curTick(), 0, false, false);
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_credit ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{MachineType_Directory, 0};

    CHI_VIP_Controller ctrl("ctrl", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    InjectReq inj("inj");
    CatchRsp  catch_rsp("catch_rsp");

    inj.sock.bind(ctrl.rxreq_socket);
    ctrl.txrsp_socket.bind(catch_rsp.sock);
    ctrl.init();

    // ── A: AllowRetry=true on inbound REQ ────────────────────────────────────
    printf("\n[A] inbound REQ with AllowRetry=1 — forwarded to CHIRequestMsg\n");
    {
        inj.send(0x1000, chi::req_optype_e::ReadShared,
                 /*txn_id=*/10, /*src_id=*/3, /*allow_retry=*/true);

        const auto* m = dynamic_cast<const CHIRequestMsg*>(
                             ctrl.getReqOut()->peek());
        CHECK(m != nullptr,              "msg arrived in reqOut");
        CHECK(m && m->getallowRetry(),   "allowRetry == true");
        ctrl.getReqOut()->dequeue(curTick());
    }

    // ── B: AllowRetry=false on retry REQ ─────────────────────────────────────
    printf("\n[B] inbound REQ with AllowRetry=0 (retry)\n");
    {
        inj.send(0x1000, chi::req_optype_e::ReadShared,
                 /*txn_id=*/10, /*src_id=*/3, /*allow_retry=*/false);

        const auto* m = dynamic_cast<const CHIRequestMsg*>(
                             ctrl.getReqOut()->peek());
        CHECK(m != nullptr,             "msg arrived in reqOut");
        CHECK(m && !m->getallowRetry(), "allowRetry == false");
        ctrl.getReqOut()->dequeue(curTick());
    }

    // ── C: RetryAck outbound — opcode forwarded to txrsp_socket ──────────────
    printf("\n[C] RetryAck outbound (HN-F → Questa) — opcode correct\n");
    {
        catch_rsp.reset();
        push_rsp(ctrl, CHIResponseType_RetryAck, /*txn_id=*/10, rs);
        ctrl.wakeup();

        CHECK(catch_rsp.beats.size() == 1,
              "txrsp_socket received 1 beat");
        if (!catch_rsp.beats.empty() && catch_rsp.beats[0].ctrl) {
            CHECK(catch_rsp.beats[0].ctrl->resp.get_opcode()
                      == chi::rsp_optype_e::RetryAck,
                  "opcode == RetryAck");
            CHECK(catch_rsp.beats[0].ctrl->cmn.get_txn_id() == 10,
                  "txnId == 10");
        }
    }

    // ── D: PCrdGrant outbound — opcode forwarded to txrsp_socket ─────────────
    printf("\n[D] PCrdGrant outbound (HN-F → Questa) — opcode correct\n");
    {
        catch_rsp.reset();
        push_rsp(ctrl, CHIResponseType_PCrdGrant, /*txn_id=*/10, rs);
        ctrl.wakeup();

        CHECK(catch_rsp.beats.size() == 1,
              "txrsp_socket received 1 beat");
        if (!catch_rsp.beats.empty() && catch_rsp.beats[0].ctrl) {
            CHECK(catch_rsp.beats[0].ctrl->resp.get_opcode()
                      == chi::rsp_optype_e::PCrdGrant,
                  "opcode == PCrdGrant");
        }
    }

    // ── E: full retry handshake sequence ─────────────────────────────────────
    // Step 1: Questa RN-F sends REQ with AllowRetry=1
    // Step 2: HN-F sends RetryAck
    // Step 3: HN-F sends PCrdGrant
    // Step 4: Questa RN-F retries with AllowRetry=0
    printf("\n[E] full retry handshake (REQ→RetryAck→PCrdGrant→retry REQ)\n");
    {
        catch_rsp.reset();
        const Addr txn    = 20;
        const Addr addr   = 0xCAFE000;

        // Step 1: initial REQ with AllowRetry=1
        inj.send(addr, chi::req_optype_e::ReadShared,
                 txn, /*src_id=*/5, /*allow_retry=*/true);
        {
            const auto* m = dynamic_cast<const CHIRequestMsg*>(
                                ctrl.getReqOut()->peek());
            CHECK(m && m->getallowRetry(),   "step1: allowRetry=1 in reqOut");
            CHECK(m && m->getaddr() == addr, "step1: address preserved");
            ctrl.getReqOut()->dequeue(curTick());
        }

        // Step 2: HN-F sends RetryAck
        push_rsp(ctrl, CHIResponseType_RetryAck, txn, rs);
        ctrl.wakeup();
        CHECK(catch_rsp.beats.size() == 1 &&
              catch_rsp.beats[0].ctrl &&
              catch_rsp.beats[0].ctrl->resp.get_opcode()
                  == chi::rsp_optype_e::RetryAck,
              "step2: RetryAck on txrsp_socket");

        // Step 3: HN-F sends PCrdGrant
        catch_rsp.reset();
        push_rsp(ctrl, CHIResponseType_PCrdGrant, txn, rs);
        ctrl.wakeup();
        CHECK(catch_rsp.beats.size() == 1 &&
              catch_rsp.beats[0].ctrl &&
              catch_rsp.beats[0].ctrl->resp.get_opcode()
                  == chi::rsp_optype_e::PCrdGrant,
              "step3: PCrdGrant on txrsp_socket");

        // Step 4: Questa retries with AllowRetry=0
        inj.send(addr, chi::req_optype_e::ReadShared,
                 txn, /*src_id=*/5, /*allow_retry=*/false);
        {
            const auto* m = dynamic_cast<const CHIRequestMsg*>(
                                ctrl.getReqOut()->peek());
            CHECK(m && !m->getallowRetry(),  "step4: allowRetry=0 on retry");
            CHECK(m && m->gettxnId() == txn, "step4: same txnId");
            CHECK(m && m->getaddr()  == addr,"step4: same address");
            ctrl.getReqOut()->dequeue(curTick());
        }
    }

    // ── F: RetryAck and PCrdGrant are delivered in enqueue order ─────────────
    printf("\n[F] RetryAck then PCrdGrant arrive in order on txrsp_socket\n");
    {
        catch_rsp.reset();
        push_rsp(ctrl, CHIResponseType_RetryAck, 30, rs);
        push_rsp(ctrl, CHIResponseType_PCrdGrant, 30, rs);
        ctrl.wakeup();   // drains both in one call

        CHECK(catch_rsp.beats.size() == 2, "2 RSPs delivered");
        if (catch_rsp.beats.size() == 2) {
            CHECK(catch_rsp.beats[0].ctrl &&
                  catch_rsp.beats[0].ctrl->resp.get_opcode()
                      == chi::rsp_optype_e::RetryAck,
                  "first delivery: RetryAck");
            CHECK(catch_rsp.beats[1].ctrl &&
                  catch_rsp.beats[1].ctrl->resp.get_opcode()
                      == chi::rsp_optype_e::PCrdGrant,
                  "second delivery: PCrdGrant");
        }
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    sc_start(1, SC_NS);
    return g_fail != 0 ? 1 : 0;
}
