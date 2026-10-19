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

// test_wakeup.cc
// Tests MessageBuffer latency, wakeup() dispatch, and EventQueue scheduling.
//
// Buffer semantics (CHI_VIP_Controller acts as RN-F proxy):
//   Inbound (Questa → gem5): b_transport callback, latency=0 → reqOut/datOut/rspOut
//   Outbound (gem5 → Questa): injected into rspIn/datIn/snpIn with latency=0 →
//                              wakeup() → recvXxxMsg → txXxx_socket
//
// For sendMessage() latency tests we manually enqueue into rspOut (own output)
// via the public sendResponseMsg() path, which uses cyclesToTicks(1) = 1000 ps.

#include <systemc>
#include <chi/chi_tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <cstdio>
#include <cstring>

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

// ── CatchTarget ───────────────────────────────────────────────────────────────
SC_MODULE(CatchTarget) {
    tlm_utils::simple_target_socket<CatchTarget> sock;
    int call_count = 0;
    std::unique_ptr<chi::chi_ctrl_extension> last_ctrl;

    SC_CTOR(CatchTarget) : sock("sock") {
        sock.register_b_transport(this, &CatchTarget::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& gp, sc_time&) {
        call_count++;
        if (auto* e = gp.get_extension<chi::chi_ctrl_extension>())
            last_ctrl.reset(static_cast<chi::chi_ctrl_extension*>(e->clone()));
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void reset() { call_count = 0; last_ctrl.reset(); }
};

// ── InjectReq: drives rxreq_socket ───────────────────────────────────────────
SC_MODULE(InjectReq) {
    tlm_utils::simple_initiator_socket<InjectReq> sock;
    SC_CTOR(InjectReq) : sock("sock") {}

    void send(chi::req_optype_e op, unsigned txn_id) {
        auto* ext = new chi::chi_ctrl_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(1);
        ext->req.set_opcode(op);
        ext->req.set_tgt_id(0);
        ext->req.set_non_secure(true);
        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(0x8000);
        gp.set_command(tlm::TLM_IGNORE_COMMAND);
        gp.set_data_length(0);
        gp.set_data_ptr(nullptr);
        sc_time d = SC_ZERO_TIME;
        sock->b_transport(gp, d);
    }
};

// ── helper: inject RSP directly into rspIn (simulates HN-F → this node) ──────
static void push_rsp_to_rspIn(CHI_VIP_Controller& ctrl,
                                CHIResponseType rtype, Addr txn_id,
                                RubySystem& rs, Tick latency = 0)
{
    auto msg = std::make_shared<CHIResponseMsg>(curTick(), 64, &rs);
    msg->settype(rtype);
    msg->settxnId(txn_id);
    msg->setdbid(0);
    MachineID src{MachineType_Directory, 0};
    msg->setresponder(src);
    ctrl.getRspIn()->enqueue(msg, curTick(), latency, false, false);
}

// ─────────────────────────────────────────────────────────────────────────────
SC_MODULE(WakeupTb) {
    RubySystem          rs;
    Network             net;
    CHI_VIP_Controller* ctrl;
    CatchTarget*        catch_rsp;
    InjectReq*          inj;

    SC_CTOR(WakeupTb) {
        rs.m_cache_line_size = 64;
        MachineID hnf_id{ MachineType_Directory, 0 };

        ctrl      = new CHI_VIP_Controller("hnf0", hnf_id, &rs, &net);
        catch_rsp = new CatchTarget("catch_rsp");
        inj       = new InjectReq("inj");

        ctrl->txrsp_socket.bind(catch_rsp->sock);
        inj->sock.bind(ctrl->rxreq_socket);
        ctrl->downstream_ = hnf_id;

        SC_THREAD(run);
    }

    void run() {
        ctrl->init();

        // ── A: inbound REQ is ready immediately (latency=0) ──────────────────
        printf("\n[A] inbound REQ enqueued with latency=0 is immediately ready\n");
        {
            inj->send(chi::req_optype_e::ReadShared, 1);
            CHECK(ctrl->getReqOut()->isReady(curTick()) == true,
                  "msg ready immediately at t=0 (latency=0)");
            ctrl->getReqOut()->dequeue(curTick());
        }

        // ── B: outbound RSP via rspIn, latency=0, wakeup() drains it ─────────
        printf("\n[B] wakeup() drains 3 ready RSPs from rspIn in one call\n");
        {
            catch_rsp->reset();
            for (int i = 0; i < 3; i++)
                push_rsp_to_rspIn(*ctrl, CHIResponseType_Comp_UC, (Addr)i, rs, 0);

            CHECK(ctrl->getRspIn()->size() == 3,  "rspIn has 3 messages");
            ctrl->wakeup();
            CHECK(catch_rsp->call_count == 3,
                  "txrsp_socket received exactly 3 RSP deliveries");
            CHECK(ctrl->getRspIn()->isEmpty(), "rspIn empty after wakeup");
        }

        // ── C: RSP with latency=1000ps not ready at t=0, ready after wait ─────
        printf("\n[C] RSP with 1000ps latency not ready before wait, ready after\n");
        {
            catch_rsp->reset();
            Tick t0 = curTick();
            push_rsp_to_rspIn(*ctrl, CHIResponseType_Comp, 99, rs, 1000);

            CHECK(ctrl->getRspIn()->isReady(t0) == false,
                  "not ready at enqueue tick");
            ctrl->wakeup();
            CHECK(catch_rsp->call_count == 0,
                  "wakeup() at t0 delivers nothing");

            wait(1001, SC_PS);  // advance past latency
            CHECK(ctrl->getRspIn()->isReady(curTick()) == true,
                  "ready after 1001 ps");
            ctrl->wakeup();
            CHECK(catch_rsp->call_count == 1,
                  "wakeup() after latency delivers 1 RSP");
        }

        // ── D: scheduleEvent(1) queues at curTick() + TICK_PER_CYCLE ──────────
        printf("\n[D] scheduleEvent(1) queues at curTick() + TICK_PER_CYCLE\n");
        {
            // Drain any stale events left by previous tests
            Consumer::globalEventQueue().drain(curTick() + 1);

            Tick before = curTick();
            ctrl->scheduleEvent(1);
            bool not_empty = !Consumer::globalEventQueue().empty();
            CHECK(not_empty, "event queue not empty after scheduleEvent(1)");
            if (not_empty) {
                Tick next = Consumer::globalEventQueue().nextTick();
                CHECK(next == before + Consumer::TICK_PER_CYCLE,
                      "scheduled tick == curTick() + TICK_PER_CYCLE");
            }
            // drain so it doesn't interfere with subsequent tests
            Consumer::globalEventQueue().drain(before + Consumer::TICK_PER_CYCLE + 1);
        }

        // ── E: FIFO ordering in rspIn ─────────────────────────────────────────
        printf("\n[E] wakeup() delivers RSPs in FIFO order\n");
        {
            catch_rsp->reset();
            push_rsp_to_rspIn(*ctrl, CHIResponseType_Comp_UC, 10, rs, 0);
            push_rsp_to_rspIn(*ctrl, CHIResponseType_DBIDResp, 20, rs, 0);
            push_rsp_to_rspIn(*ctrl, CHIResponseType_CompAck,  30, rs, 0);

            ctrl->wakeup();
            CHECK(catch_rsp->call_count == 3, "all 3 RSPs delivered");
            // last_ctrl holds the last one — CompAck, txnId=30
            CHECK(catch_rsp->last_ctrl != nullptr, "last ctrl extension present");
            if (catch_rsp->last_ctrl)
                CHECK(catch_rsp->last_ctrl->resp.get_opcode() == chi::rsp_optype_e::CompAck,
                      "last RSP opcode == CompAck (CompAck is last sent)");
        }

        // ── F: reqOut and rspIn are independent ───────────────────────────────
        printf("\n[F] reqOut (inbound) and rspIn (outbound) are independent\n");
        {
            catch_rsp->reset();
            inj->send(chi::req_optype_e::ReadShared,  10);
            inj->send(chi::req_optype_e::ReadUnique,  11);
            push_rsp_to_rspIn(*ctrl, CHIResponseType_Comp_UC, 20, rs, 0);

            ctrl->wakeup();  // drains rspIn → txrsp_socket; reqOut is untouched

            CHECK(ctrl->getReqOut()->size() == 2, "reqOut has 2 pending (untouched)");
            CHECK(catch_rsp->call_count == 1,     "txrsp received 1 RSP");

            ctrl->getReqOut()->dequeue(curTick());
            ctrl->getReqOut()->dequeue(curTick());
        }

        printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
        sc_stop();
    }
};

int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_wakeup ===\n");
    WakeupTb tb("tb");
    sc_start();
    return g_fail != 0 ? 1 : 0;
}
