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

// test_req_types.cc
//
// Comprehensive tests for inbound REQ (Questa → gem5) opcode decoding and
// field propagation through unpack_req.
//
// Bugs caught/fixed by these tests:
//   • scc_to_gem5req missing WriteUniqueZero: always returned ReadOnce.
//   • scc_to_gem5req missing ReadNoSnpSep: always returned ReadOnce.

#include <systemc>
#include <chi/chi_tlm.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <cstdio>
#include <cstring>

#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/common/MachineID.hh"
#include "chi_vip_controller.hh"
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

// ── InjectReq: drives rxreq_socket ───────────────────────────────────────────
SC_MODULE(InjectReq) {
    tlm_utils::simple_initiator_socket<InjectReq> sock;
    SC_CTOR(InjectReq) : sock("sock") {}

    void send(Addr addr, chi::req_optype_e op, unsigned txn_id,
              unsigned src_id, bool allow_retry = false, bool ns = false)
    {
        auto* ext = new chi::chi_ctrl_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(src_id);
        ext->req.set_opcode(op);
        ext->req.set_tgt_id(0);
        ext->req.set_allow_retry(allow_retry);
        if (ns) ext->req.set_non_secure(true);

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

// Peek top of reqOut as CHIRequestMsg*
static const CHIRequestMsg* peek_req(CHI_VIP_Controller& ctrl) {
    return dynamic_cast<const CHIRequestMsg*>(ctrl.getReqOut()->peek());
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_req_types ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{MachineType_Directory, 0};

    CHI_VIP_Controller ctrl("ctrl", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    InjectReq inj("inj");
    inj.sock.bind(ctrl.rxreq_socket);
    ctrl.init();

    // Helper: send one REQ, return pointer to message in reqOut (or nullptr)
    auto inbound = [&](Addr addr, chi::req_optype_e op, unsigned txn = 1,
                       unsigned src = 5, bool allow_retry = false,
                       bool ns = false) -> const CHIRequestMsg* {
        inj.send(addr, op, txn, src, allow_retry, ns);
        return peek_req(ctrl);
    };
    auto dequeue = [&]() { ctrl.getReqOut()->dequeue(curTick()); };

    // ── 1. Read opcodes ──────────────────────────────────────────────────────
    printf("\n[1] Read request opcodes\n");
    {
        using O = chi::req_optype_e;
        struct { O scc; CHIRequestType gem5; const char* name; } cases[] = {
            { O::ReadShared,         CHIRequestType_ReadShared,         "ReadShared"         },
            { O::ReadNotSharedDirty, CHIRequestType_ReadNotSharedDirty, "ReadNotSharedDirty" },
            { O::ReadUnique,         CHIRequestType_ReadUnique,         "ReadUnique"         },
            { O::ReadOnce,           CHIRequestType_ReadOnce,           "ReadOnce"           },
            { O::ReadNoSnp,          CHIRequestType_ReadNoSnp,          "ReadNoSnp"          },
            { O::ReadNoSnpSep,       CHIRequestType_ReadNoSnpSep,       "ReadNoSnpSep"       },
        };
        for (auto& c : cases) {
            const auto* m = inbound(0x1000, c.scc);
            char buf[64]; snprintf(buf, sizeof(buf), "%s type correct", c.name);
            CHECK(m && m->gettype() == c.gem5, buf);
            if (m) dequeue();
        }
    }

    // ── 2. Write opcodes ─────────────────────────────────────────────────────
    printf("\n[2] Write request opcodes\n");
    {
        using O = chi::req_optype_e;
        struct { O scc; CHIRequestType gem5; const char* name; } cases[] = {
            { O::WriteBackFull,    CHIRequestType_WriteBackFull,    "WriteBackFull"    },
            { O::WriteCleanFull,   CHIRequestType_WriteCleanFull,   "WriteCleanFull"   },
            { O::WriteEvictFull,   CHIRequestType_WriteEvictFull,   "WriteEvictFull"   },
            { O::WriteUniquePtl,   CHIRequestType_WriteUniquePtl,   "WriteUniquePtl"   },
            { O::WriteUniqueFull,  CHIRequestType_WriteUniqueFull,  "WriteUniqueFull"  },
            { O::WriteUniqueZero,  CHIRequestType_WriteUniqueZero,  "WriteUniqueZero"  },
            { O::WriteNoSnpPtl,    CHIRequestType_WriteNoSnpPtl,    "WriteNoSnpPtl"    },
            { O::WriteNoSnpFull,   CHIRequestType_WriteNoSnp,       "WriteNoSnpFull"   },
        };
        for (auto& c : cases) {
            const auto* m = inbound(0x2000, c.scc);
            char buf[64]; snprintf(buf, sizeof(buf), "%s type correct", c.name);
            CHECK(m && m->gettype() == c.gem5, buf);
            if (m) dequeue();
        }
    }

    // ── 3. Maintenance and eviction opcodes ──────────────────────────────────
    printf("\n[3] Maintenance and eviction opcodes\n");
    {
        using O = chi::req_optype_e;
        struct { O scc; CHIRequestType gem5; const char* name; } cases[] = {
            { O::Evict,         CHIRequestType_Evict,         "Evict"         },
            { O::CleanUnique,   CHIRequestType_CleanUnique,   "CleanUnique"   },
            { O::MakeReadUnique,CHIRequestType_MakeReadUnique,"MakeReadUnique"},
        };
        for (auto& c : cases) {
            const auto* m = inbound(0x3000, c.scc);
            char buf[64]; snprintf(buf, sizeof(buf), "%s type correct", c.name);
            CHECK(m && m->gettype() == c.gem5, buf);
            if (m) dequeue();
        }
    }

    // ── 4. Stash opcodes ─────────────────────────────────────────────────────
    printf("\n[4] Stash request opcodes\n");
    {
        using O = chi::req_optype_e;
        const auto* m_sh = inbound(0x4000, O::StashOnceShared);
        CHECK(m_sh && m_sh->gettype() == CHIRequestType_StashOnceShared,
              "StashOnceShared type correct");
        if (m_sh) dequeue();

        const auto* m_un = inbound(0x4040, O::StashOnceUnique);
        CHECK(m_un && m_un->gettype() == CHIRequestType_StashOnceUnique,
              "StashOnceUnique type correct");
        if (m_un) dequeue();
    }

    // ── 5. Atomic opcodes ────────────────────────────────────────────────────
    printf("\n[5] Atomic opcodes\n");
    {
        using O = chi::req_optype_e;
        // AtomicLoad variants → AtomicLoad
        O atomic_loads[] = {
            O::AtomicLoadAdd, O::AtomicLoadClr, O::AtomicLoadEor, O::AtomicLoadSet,
            O::AtomicLoadSmax, O::AtomicLoadSmin, O::AtomicLoadUmax, O::AtomicLoadUmin,
        };
        const char* al_names[] = {
            "AtomicLoadAdd","AtomicLoadClr","AtomicLoadEor","AtomicLoadSet",
            "AtomicLoadSmax","AtomicLoadSmin","AtomicLoadUmax","AtomicLoadUmin",
        };
        for (int i = 0; i < 8; i++) {
            const auto* m = inbound(0x5000, atomic_loads[i]);
            char buf[64]; snprintf(buf, sizeof(buf), "%s → AtomicLoad", al_names[i]);
            CHECK(m && m->gettype() == CHIRequestType_AtomicLoad, buf);
            if (m) dequeue();
        }
        // AtomicStore variants → AtomicStore
        O atomic_stores[] = {
            O::AtomicStoreAdd, O::AtomicStoreClr, O::AtomicStoreEor, O::AtomicStoreSet,
        };
        const char* as_names[] = {
            "AtomicStoreAdd","AtomicStoreClr","AtomicStoreEor","AtomicStoreSet",
        };
        for (int i = 0; i < 4; i++) {
            const auto* m = inbound(0x5080, atomic_stores[i]);
            char buf[64]; snprintf(buf, sizeof(buf), "%s → AtomicStore", as_names[i]);
            CHECK(m && m->gettype() == CHIRequestType_AtomicStore, buf);
            if (m) dequeue();
        }
        // AtomicSwap → AtomicReturn
        const auto* m_sw = inbound(0x5100, O::AtomicSwap);
        CHECK(m_sw && m_sw->gettype() == CHIRequestType_AtomicReturn,
              "AtomicSwap → AtomicReturn");
        if (m_sw) dequeue();
        // AtomicCompare → AtomicNoReturn
        const auto* m_cmp = inbound(0x5140, O::AtomicCompare);
        CHECK(m_cmp && m_cmp->gettype() == CHIRequestType_AtomicNoReturn,
              "AtomicCompare → AtomicNoReturn");
        if (m_cmp) dequeue();
    }

    // ── 6. DVM opcode ────────────────────────────────────────────────────────
    printf("\n[6] DVM opcode\n");
    {
        const auto* m = inbound(0x6000, chi::req_optype_e::DVMOp);
        CHECK(m && m->gettype() == CHIRequestType_DvmOpNonSync,
              "DVMOp → DvmOpNonSync");
        if (m) dequeue();
    }

    // ── 7. Field propagation: address, TxnID, SrcID ──────────────────────────
    printf("\n[7] field propagation: address, TxnID, SrcID\n");
    {
        const auto* m = inbound(0xDEAD0080, chi::req_optype_e::ReadShared,
                                /*txn=*/42, /*src=*/9);
        CHECK(m && m->getaddr()             == 0xDEAD0080, "address preserved");
        CHECK(m && m->gettxnId()            == 42,         "txnId == 42");
        CHECK(m && m->getrequestor().num    == 9,          "requestor.num == 9");
        if (m) dequeue();
    }

    // ── 8. AllowRetry bit propagation ────────────────────────────────────────
    printf("\n[8] AllowRetry bit propagation\n");
    {
        const auto* m_true = inbound(0x8000, chi::req_optype_e::WriteUniqueFull,
                                     1, 3, /*allow_retry=*/true);
        CHECK(m_true && m_true->getallowRetry() == true,  "AllowRetry=true preserved");
        if (m_true) dequeue();

        const auto* m_false = inbound(0x8040, chi::req_optype_e::WriteUniqueFull,
                                      2, 3, /*allow_retry=*/false);
        CHECK(m_false && m_false->getallowRetry() == false, "AllowRetry=false preserved");
        if (m_false) dequeue();
    }

    // ── 9. NS bit propagation ────────────────────────────────────────────────
    printf("\n[9] NS (non-secure) bit propagation\n");
    {
        const auto* m_ns = inbound(0x9000, chi::req_optype_e::ReadShared,
                                   1, 3, false, /*ns=*/true);
        CHECK(m_ns && m_ns->getns() == true, "NS=true preserved");
        if (m_ns) dequeue();

        const auto* m_s = inbound(0x9040, chi::req_optype_e::ReadShared,
                                  2, 3, false, /*ns=*/false);
        CHECK(m_s && m_s->getns() == false, "NS=false preserved");
        if (m_s) dequeue();
    }

    // ── 10. FIFO ordering: 5 REQs enqueued, dequeued in order ────────────────
    printf("\n[10] FIFO ordering of inbound REQs\n");
    {
        using O = chi::req_optype_e;
        O ops[] = { O::ReadShared, O::WriteUniqueFull, O::Evict,
                    O::ReadNoSnp, O::CleanUnique };
        CHIRequestType expected[] = {
            CHIRequestType_ReadShared, CHIRequestType_WriteUniqueFull,
            CHIRequestType_Evict, CHIRequestType_ReadNoSnp, CHIRequestType_CleanUnique,
        };
        for (int i = 0; i < 5; i++)
            inj.send(static_cast<Addr>(0xA000 + i * 0x40), ops[i],
                     static_cast<unsigned>(i+1), 3);

        bool order_ok = true;
        for (int i = 0; i < 5; i++) {
            const auto* m = peek_req(ctrl);
            if (!m || m->gettype() != expected[i]) { order_ok = false; break; }
            ctrl.getReqOut()->dequeue(curTick());
        }
        CHECK(order_ok, "5 REQs delivered in FIFO order");
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    sc_start(1, SC_NS);
    return g_fail != 0 ? 1 : 0;
}
