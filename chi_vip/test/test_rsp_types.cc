// test_rsp_types.cc
//
// Comprehensive tests for outbound RSP (gem5 → Questa) opcode and resp-state
// encoding, and inbound RSP (Questa → gem5) opcode and resp-state decoding.
//
// Bugs caught/fixed by these tests:
//   • pack_rsp SnpResp_SC: was sending SnpResp with resp=SnpResp_I (default)
//     instead of SnpResp_SC (0b001).
//   • pack_rsp SnpRespFwded: no resp state was set for any variant.
//   • unpack_rsp SnpResp: always returned SnpResp_I regardless of resp state.

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

    // Convenience: check the single beat's extension
    chi::chi_ctrl_extension* last() {
        return beats.empty() ? nullptr : beats.back().ctrl.get();
    }
};

// ── InjectRsp: drives rxrsp_socket (Questa → gem5) ───────────────────────────
SC_MODULE(InjectRsp) {
    tlm_utils::simple_initiator_socket<InjectRsp> sock;
    SC_CTOR(InjectRsp) : sock("sock") {}

    void send(chi::rsp_optype_e op, unsigned txn_id, unsigned src_id,
              chi::rsp_resptype_e resp = chi::rsp_resptype_e::SnpResp_I)
    {
        auto* ext = new chi::chi_ctrl_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(src_id);
        ext->resp.set_opcode(op);
        ext->resp.set_resp(resp);
        ext->resp.set_tgt_id(0);
        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(0);
        gp.set_command(tlm::TLM_IGNORE_COMMAND);
        gp.set_data_length(0);
        gp.set_data_ptr(nullptr);
        sc_time d = SC_ZERO_TIME;
        sock->b_transport(gp, d);
    }
};

// ── inject into rspIn (simulates gem5 HN-F producing a response) ─────────────
static void push_rsp(CHI_VIP_Controller& ctrl,
                     CHIResponseType rtype, Addr txn_id, Addr dbid,
                     RubySystem& rs, unsigned src_num = 0)
{
    auto msg = std::make_shared<CHIResponseMsg>(curTick(), 64, &rs);
    msg->settype(rtype);
    msg->settxnId(txn_id);
    msg->setdbid(dbid);
    MachineID src{MachineType_Directory, static_cast<NodeID>(src_num)};
    msg->setresponder(src);
    MachineID tgt{MachineType_Cache, 3};
    msg->getDestination().add(tgt);
    ctrl.getRspIn()->enqueue(msg, curTick(), 0, false, false);
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_rsp_types ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{MachineType_Directory, 0};

    CHI_VIP_Controller ctrl("ctrl", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    CatchRsp  catch_rsp("catch_rsp");
    InjectRsp inj_rsp("inj_rsp");

    ctrl.txrsp_socket.bind(catch_rsp.sock);
    inj_rsp.sock.bind(ctrl.rxrsp_socket);
    ctrl.init();

    // Helper: inject one RSP into rspIn, call wakeup(), return last extension
    auto outbound = [&](CHIResponseType t, Addr txn = 1, Addr dbid = 0,
                        unsigned src = 0) -> chi::chi_ctrl_extension* {
        catch_rsp.reset();
        push_rsp(ctrl, t, txn, dbid, rs, src);
        ctrl.wakeup();
        return catch_rsp.last();
    };

    // ══ OUTBOUND pack_rsp tests ══════════════════════════════════════════════

    // ── 1. Comp cache states: opcode + resp state ─────────────────────────────
    printf("\n[1] Comp* cache states\n");
    {
        using R = chi::rsp_optype_e;
        using S = chi::rsp_resptype_e;

        struct { CHIResponseType gem5; chi::rsp_resptype_e scc; const char* name; } cases[] = {
            { CHIResponseType_Comp_I,    S::Comp_I,    "Comp_I"    },
            { CHIResponseType_Comp_UC,   S::Comp_UC,   "Comp_UC"   },
            { CHIResponseType_Comp_SC,   S::Comp_SC,   "Comp_SC"   },
            { CHIResponseType_Comp_UD_PD,S::Comp_UD_PD,"Comp_UD_PD"},
        };
        for (auto& c : cases) {
            auto* e = outbound(c.gem5);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s opcode==Comp", c.name);
            CHECK(e && e->resp.get_opcode() == R::Comp, buf);
            snprintf(buf, sizeof(buf), "%s resp state correct", c.name);
            CHECK(e && e->resp.get_resp() == c.scc, buf);
        }
    }

    // ── 2. Compound/control opcodes ───────────────────────────────────────────
    printf("\n[2] compound and control RSP opcodes\n");
    {
        using R = chi::rsp_optype_e;
        struct { CHIResponseType gem5; R scc; const char* name; } cases[] = {
            { CHIResponseType_CompDBIDResp, R::CompDBIDResp, "CompDBIDResp" },
            { CHIResponseType_DBIDResp,     R::DBIDResp,     "DBIDResp"     },
            { CHIResponseType_ReadReceipt,  R::ReadReceipt,  "ReadReceipt"  },
            { CHIResponseType_RespSepData,  R::RespSepData,  "RespSepData"  },
            { CHIResponseType_CompAck,      R::CompAck,      "CompAck"      },
        };
        for (auto& c : cases) {
            auto* e = outbound(c.gem5);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s opcode correct", c.name);
            CHECK(e && e->resp.get_opcode() == c.scc, buf);
        }
    }

    // ── 3. SnpResp: resp state distinguishes I from SC ────────────────────────
    printf("\n[3] SnpResp resp state: I vs SC\n");
    {
        using R = chi::rsp_optype_e;
        using S = chi::rsp_resptype_e;

        auto* e_i  = outbound(CHIResponseType_SnpResp_I);
        CHECK(e_i && e_i->resp.get_opcode() == R::SnpResp,   "SnpResp_I opcode");
        CHECK(e_i && e_i->resp.get_resp()   == S::SnpResp_I, "SnpResp_I resp state");

        auto* e_sc = outbound(CHIResponseType_SnpResp_SC);
        CHECK(e_sc && e_sc->resp.get_opcode() == R::SnpResp,    "SnpResp_SC opcode");
        CHECK(e_sc && e_sc->resp.get_resp()   == S::SnpResp_SC, "SnpResp_SC resp state");
    }

    // ── 4. SnpRespFwded: resp state carries the responder's final state ───────
    printf("\n[4] SnpRespFwded variants with resp state\n");
    {
        using R = chi::rsp_optype_e;
        using S = chi::rsp_resptype_e;
        struct { CHIResponseType gem5; S resp_state; const char* name; } cases[] = {
            { CHIResponseType_SnpResp_I_Fwded_UC,     S::SnpResp_I,  "I_Fwded_UC"     },
            { CHIResponseType_SnpResp_I_Fwded_UD_PD,  S::SnpResp_I,  "I_Fwded_UD_PD"  },
            { CHIResponseType_SnpResp_SC_Fwded_SC,     S::SnpResp_SC, "SC_Fwded_SC"    },
            { CHIResponseType_SnpResp_SC_Fwded_SD_PD,  S::SnpResp_SC, "SC_Fwded_SD_PD" },
            { CHIResponseType_SnpResp_SC_Fwded_I,      S::SnpResp_SC, "SC_Fwded_I"     },
            { CHIResponseType_SnpResp_UC_Fwded_I,      S::SnpResp_UC, "UC_Fwded_I"     },
            { CHIResponseType_SnpResp_UD_Fwded_I,      S::SnpResp_UC, "UD_Fwded_I"     },
            { CHIResponseType_SnpResp_SD_Fwded_I,      S::SnpResp_SD, "SD_Fwded_I"     },
        };
        for (auto& c : cases) {
            auto* e = outbound(c.gem5);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s opcode==SnpRespFwded", c.name);
            CHECK(e && e->resp.get_opcode() == R::SnpRespFwded, buf);
            snprintf(buf, sizeof(buf), "%s resp state correct", c.name);
            CHECK(e && e->resp.get_resp() == c.resp_state, buf);
        }
    }

    // ── 5. Field propagation: TxnID, DBID, SrcID, TgtID ─────────────────────
    printf("\n[5] field propagation in outbound RSP\n");
    {
        auto* e = outbound(CHIResponseType_DBIDResp, /*txn=*/77, /*dbid=*/13, /*src=*/4);
        CHECK(e && e->cmn.get_txn_id()   == 77, "TxnID == 77");
        CHECK(e && e->resp.get_db_id()   == 13, "DBID  == 13");
        CHECK(e && e->cmn.get_src_id()   == 4,  "SrcID == 4  (responder.num)");
        CHECK(e && e->resp.get_tgt_id()  == 3,  "TgtID == 3  (Destination.num)");
    }

    // ── 6. Multiple RSPs in one wakeup() — count and order ───────────────────
    printf("\n[6] multiple RSPs delivered in FIFO order by wakeup()\n");
    {
        catch_rsp.reset();
        push_rsp(ctrl, CHIResponseType_Comp_UC,   1, 0, rs);
        push_rsp(ctrl, CHIResponseType_DBIDResp,  2, 7, rs);
        push_rsp(ctrl, CHIResponseType_CompAck,   3, 0, rs);
        ctrl.wakeup();

        CHECK(catch_rsp.beats.size() == 3, "3 RSPs delivered");
        if (catch_rsp.beats.size() == 3) {
            CHECK(catch_rsp.beats[0].ctrl &&
                  catch_rsp.beats[0].ctrl->resp.get_opcode() == chi::rsp_optype_e::Comp,
                  "first: Comp");
            CHECK(catch_rsp.beats[1].ctrl &&
                  catch_rsp.beats[1].ctrl->resp.get_db_id() == 7,
                  "second: DBIDResp dbid=7");
            CHECK(catch_rsp.beats[2].ctrl &&
                  catch_rsp.beats[2].ctrl->resp.get_opcode() == chi::rsp_optype_e::CompAck,
                  "third: CompAck");
        }
    }

    // ══ INBOUND unpack_rsp tests ═════════════════════════════════════════════

    // ── 7. CompAck inbound ────────────────────────────────────────────────────
    printf("\n[7] inbound CompAck (Questa → gem5)\n");
    {
        inj_rsp.send(chi::rsp_optype_e::CompAck, /*txn=*/10, /*src=*/2);
        const auto* m = dynamic_cast<const CHIResponseMsg*>(
                            ctrl.getRspOut()->peek());
        CHECK(m && m->gettype()  == CHIResponseType_CompAck, "type == CompAck");
        CHECK(m && m->gettxnId() == 10,                      "txnId == 10");
        CHECK(m && m->getresponder().num == 2,               "responder.num == 2");
        ctrl.getRspOut()->dequeue(curTick());
    }

    // ── 8. SnpResp_I inbound ─────────────────────────────────────────────────
    printf("\n[8] inbound SnpResp with resp=I (Questa → gem5)\n");
    {
        inj_rsp.send(chi::rsp_optype_e::SnpResp, /*txn=*/20, /*src=*/5,
                     chi::rsp_resptype_e::SnpResp_I);
        const auto* m = dynamic_cast<const CHIResponseMsg*>(
                            ctrl.getRspOut()->peek());
        CHECK(m && m->gettype() == CHIResponseType_SnpResp_I,
              "resp=I → SnpResp_I");
        ctrl.getRspOut()->dequeue(curTick());
    }

    // ── 9. SnpResp_SC inbound ────────────────────────────────────────────────
    printf("\n[9] inbound SnpResp with resp=SC (Questa → gem5)\n");
    {
        inj_rsp.send(chi::rsp_optype_e::SnpResp, /*txn=*/21, /*src=*/5,
                     chi::rsp_resptype_e::SnpResp_SC);
        const auto* m = dynamic_cast<const CHIResponseMsg*>(
                            ctrl.getRspOut()->peek());
        CHECK(m && m->gettype() == CHIResponseType_SnpResp_SC,
              "resp=SC → SnpResp_SC");
        ctrl.getRspOut()->dequeue(curTick());
    }

    // ── 10. Inbound TxnID preservation ───────────────────────────────────────
    printf("\n[10] inbound TxnID preserved through unpack_rsp\n");
    {
        inj_rsp.send(chi::rsp_optype_e::CompAck, /*txn=*/99, /*src=*/7);
        const auto* m = dynamic_cast<const CHIResponseMsg*>(
                            ctrl.getRspOut()->peek());
        CHECK(m && m->gettxnId()         == 99, "txnId == 99");
        CHECK(m && m->getresponder().num == 7,  "responder.num == 7");
        ctrl.getRspOut()->dequeue(curTick());
    }

    // ── 11. SnpRespFwded inbound — resp state selects gem5 type ──────────────
    printf("\n[11] inbound SnpRespFwded resp state → gem5 type\n");
    {
        using S = chi::rsp_resptype_e;
        struct { S resp; CHIResponseType expected; const char* name; } cases[] = {
            { S::SnpResp_I,  CHIResponseType_SnpResp_I_Fwded_UC,   "I→I_Fwded_UC"  },
            { S::SnpResp_SC, CHIResponseType_SnpResp_SC_Fwded_SC,  "SC→SC_Fwded_SC" },
            { S::SnpResp_UC, CHIResponseType_SnpResp_UC_Fwded_I,   "UC→UC_Fwded_I"  },
            { S::SnpResp_SD, CHIResponseType_SnpResp_SD_Fwded_I,   "SD→SD_Fwded_I"  },
        };
        for (auto& c : cases) {
            inj_rsp.send(chi::rsp_optype_e::SnpRespFwded, /*txn=*/60, /*src=*/1, c.resp);
            const auto* m = dynamic_cast<const CHIResponseMsg*>(
                                ctrl.getRspOut()->peek());
            char buf[64];
            snprintf(buf, sizeof(buf), "SnpRespFwded resp=%s type correct", c.name);
            CHECK(m && m->gettype() == c.expected, buf);
            if (ctrl.getRspOut()->isReady(curTick()))
                ctrl.getRspOut()->dequeue(curTick());
        }
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    sc_start(1, SC_NS);
    return g_fail != 0 ? 1 : 0;
}
