// test_snp_types.cc
//
// Comprehensive tests for the outbound SNP channel (gem5 → Questa):
//   • All pack_snp opcodes: non-forwarding and forwarding variants
//   • Field propagation: TxnID, SrcID, address, RetToSrc
//   • DoNotGoToSD: always false (CHIRequestMsg has no such field)
//   • FwdTxnID: always 0 (gem5 CHIRequestMsg only carries fwdRequestor NodeID,
//     not a FwdTxnID; protocol limitation of the shim)

#include <systemc>
#include <chi/chi_tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

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

// ── CatchSnp: captures b_transport calls on txsnp_socket ─────────────────────
SC_MODULE(CatchSnp) {
    tlm_utils::simple_target_socket<CatchSnp> sock;

    struct Beat {
        Addr addr = 0;
        std::unique_ptr<chi::chi_snp_extension> ext;
    };
    std::vector<Beat> beats;

    SC_CTOR(CatchSnp) : sock("sock") {
        sock.register_b_transport(this, &CatchSnp::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& gp, sc_time&) {
        Beat b;
        b.addr = static_cast<Addr>(gp.get_address());
        if (auto* e = gp.get_extension<chi::chi_snp_extension>())
            b.ext.reset(static_cast<chi::chi_snp_extension*>(e->clone()));
        beats.push_back(std::move(b));
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void reset() { beats.clear(); }
    chi::chi_snp_extension* last() {
        return beats.empty() ? nullptr : beats.back().ext.get();
    }
    Addr last_addr() {
        return beats.empty() ? static_cast<Addr>(~0ULL) : beats.back().addr;
    }
};

// ── Inject into snpIn (gem5 HN-F → Questa RN-F) ──────────────────────────────
static void push_snp(CHI_VIP_Controller& ctrl,
                     CHIRequestType rtype, Addr addr, Addr txn_id,
                     RubySystem& rs,
                     unsigned src_num = 0, bool ret_to_src = false,
                     unsigned fwd_num = 0)
{
    auto msg = std::make_shared<CHIRequestMsg>(curTick(), 64, &rs);
    msg->settype(rtype);
    msg->setaddr(addr);
    msg->settxnId(txn_id);
    msg->setusesTxnId(true);
    MachineID src{MachineType_Directory, static_cast<NodeID>(src_num)};
    msg->setrequestor(src);
    MachineID fwd{MachineType_Cache, static_cast<NodeID>(fwd_num)};
    msg->setfwdRequestor(fwd);
    msg->setretToSrc(ret_to_src);
    MachineID tgt{MachineType_Cache, 1};
    msg->getDestination().add(tgt);
    ctrl.getSnpIn()->enqueue(msg, curTick(), 0, false, false);
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_snp_types ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{MachineType_Directory, 0};

    CHI_VIP_Controller ctrl("ctrl", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    CatchSnp catch_snp("catch_snp");
    ctrl.txsnp_socket.bind(catch_snp.sock);
    ctrl.init();

    auto outbound = [&](CHIRequestType t, Addr addr = 0x1000,
                        Addr txn = 1, unsigned src = 0,
                        bool ret_to_src = false, unsigned fwd = 0)
                        -> chi::chi_snp_extension* {
        catch_snp.reset();
        push_snp(ctrl, t, addr, txn, rs, src, ret_to_src, fwd);
        ctrl.wakeup();
        return catch_snp.last();
    };

    // ── 1. Non-forwarding SNP opcodes ────────────────────────────────────────
    printf("\n[1] non-forwarding SNP opcodes\n");
    {
        using S = chi::snp_optype_e;
        struct { CHIRequestType gem5; S scc; const char* name; } cases[] = {
            { CHIRequestType_SnpShared,       S::SnpShared,       "SnpShared"       },
            { CHIRequestType_SnpUnique,       S::SnpUnique,       "SnpUnique"       },
            { CHIRequestType_SnpOnce,         S::SnpOnce,         "SnpOnce"         },
            { CHIRequestType_SnpCleanInvalid, S::SnpCleanInvalid, "SnpCleanInvalid" },
        };
        for (auto& c : cases) {
            auto* e = outbound(c.gem5);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s opcode correct", c.name);
            CHECK(e && e->req.get_opcode() == c.scc, buf);
        }
    }

    // ── 2. Forwarding SNP opcodes ────────────────────────────────────────────
    printf("\n[2] forwarding SNP opcodes\n");
    {
        using S = chi::snp_optype_e;
        struct { CHIRequestType gem5; S scc; const char* name; } cases[] = {
            { CHIRequestType_SnpSharedFwd,         S::SnpSharedFwd,         "SnpSharedFwd"         },
            { CHIRequestType_SnpNotSharedDirtyFwd, S::SnpNotSharedDirtyFwd, "SnpNotSharedDirtyFwd" },
            { CHIRequestType_SnpUniqueFwd,         S::SnpUniqueFwd,         "SnpUniqueFwd"         },
            { CHIRequestType_SnpOnceFwd,           S::SnpOnceFwd,           "SnpOnceFwd"           },
        };
        for (auto& c : cases) {
            auto* e = outbound(c.gem5);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s opcode correct", c.name);
            CHECK(e && e->req.get_opcode() == c.scc, buf);
        }
    }

    // ── 3. DVM SNP opcode ────────────────────────────────────────────────────
    printf("\n[3] DVM SNP opcode\n");
    {
        using S = chi::snp_optype_e;
        CHIRequestType dvm_types[] = {
            CHIRequestType_SnpDvmOpSync_P1,
            CHIRequestType_SnpDvmOpSync_P2,
            CHIRequestType_SnpDvmOpNonSync_P1,
            CHIRequestType_SnpDvmOpNonSync_P2,
        };
        const char* names[] = {
            "SnpDvmOpSync_P1", "SnpDvmOpSync_P2",
            "SnpDvmOpNonSync_P1", "SnpDvmOpNonSync_P2",
        };
        for (int i = 0; i < 4; i++) {
            auto* e = outbound(dvm_types[i]);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s → SnpDVMOp", names[i]);
            CHECK(e && e->req.get_opcode() == S::SnpDVMOp, buf);
        }
    }

    // ── 4. Field propagation: TxnID, SrcID, address ──────────────────────────
    printf("\n[4] field propagation: TxnID, SrcID, address\n");
    {
        auto* e = outbound(CHIRequestType_SnpShared,
                           /*addr=*/0xCAFE0080, /*txn=*/55, /*src=*/7);
        CHECK(e && e->cmn.get_txn_id() == 55,       "TxnID == 55");
        CHECK(e && e->cmn.get_src_id() == 7,        "SrcID == 7 (requestor.num)");
        CHECK(!catch_snp.beats.empty() &&
              catch_snp.beats.back().addr == 0xCAFE0080, "address preserved");
    }

    // ── 5. RetToSrc propagation ──────────────────────────────────────────────
    printf("\n[5] RetToSrc flag propagation\n");
    {
        auto* e_true = outbound(CHIRequestType_SnpShared, 0x1000, 1, 0,
                                /*ret_to_src=*/true);
        CHECK(e_true && e_true->req.is_ret_to_src() == true,
              "RetToSrc=true propagated");

        auto* e_false = outbound(CHIRequestType_SnpShared, 0x1040, 2, 0,
                                 /*ret_to_src=*/false);
        CHECK(e_false && e_false->req.is_ret_to_src() == false,
              "RetToSrc=false propagated");
    }

    // ── 6. DoNotGoToSD is always false ────────────────────────────────────────
    printf("\n[6] DoNotGoToSD is always false (CHIRequestMsg has no such field)\n");
    {
        auto* e = outbound(CHIRequestType_SnpShared);
        CHECK(e && e->req.is_do_not_goto_sd() == false,
              "DoNotGoToSD == false");
    }

    // ── 7. Multiple SNPs in one wakeup — FIFO order ──────────────────────────
    printf("\n[7] multiple SNPs delivered in FIFO order\n");
    {
        catch_snp.reset();
        push_snp(ctrl, CHIRequestType_SnpShared,  0x1000, 1, rs, 0, false);
        push_snp(ctrl, CHIRequestType_SnpUnique,  0x2000, 2, rs, 0, true);
        push_snp(ctrl, CHIRequestType_SnpOnce,    0x3000, 3, rs, 0, false);
        ctrl.wakeup();

        CHECK(catch_snp.beats.size() == 3, "3 SNPs delivered");
        if (catch_snp.beats.size() == 3) {
            CHECK(catch_snp.beats[0].ext &&
                  catch_snp.beats[0].ext->req.get_opcode() == chi::snp_optype_e::SnpShared,
                  "first: SnpShared");
            CHECK(catch_snp.beats[1].ext &&
                  catch_snp.beats[1].ext->req.is_ret_to_src() == true,
                  "second: RetToSrc=true (SnpUnique)");
            CHECK(catch_snp.beats[2].ext &&
                  catch_snp.beats[2].ext->req.get_opcode() == chi::snp_optype_e::SnpOnce,
                  "third: SnpOnce");
        }
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    sc_start(1, SC_NS);
    return g_fail != 0 ? 1 : 0;
}
