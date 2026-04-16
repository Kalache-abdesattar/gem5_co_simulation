// test_dat_types.cc
//
// Comprehensive tests for DAT message encoding/decoding:
//   • Outbound (gem5 → Questa): all pack_dat resp states for CompData,
//     CopyBackWrData, NCBWrData, DataSepResp, SnpRespData, SnpRespDataFwded
//   • Inbound (Questa → gem5): unpack_dat CopyBackWrData resp-state dispatch
//     (previously: all five variants were silently mapped to CBWrData_UC)
//   • Field propagation: TxnID, SrcID, TgtID in both directions

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

// ── CatchDat: captures one or more b_transport calls on txdat_socket ─────────
SC_MODULE(CatchDat) {
    tlm_utils::simple_target_socket<CatchDat> sock;

    struct Beat {
        std::unique_ptr<chi::chi_data_extension> dat;
        int data_len = 0;
        uint8_t data[64] = {};
    };
    std::vector<Beat> beats;

    SC_CTOR(CatchDat) : sock("sock") {
        sock.register_b_transport(this, &CatchDat::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& gp, sc_time&) {
        Beat b{};
        if (auto* e = gp.get_extension<chi::chi_data_extension>())
            b.dat.reset(static_cast<chi::chi_data_extension*>(e->clone()));
        b.data_len = static_cast<int>(gp.get_data_length());
        if (gp.get_data_ptr() && b.data_len > 0 && b.data_len <= 64)
            memcpy(b.data, gp.get_data_ptr(), b.data_len);
        beats.push_back(std::move(b));
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void reset() { beats.clear(); }

    chi::chi_data_extension* last() {
        return beats.empty() ? nullptr : beats.back().dat.get();
    }
};

// ── InjectDat: drives rxdat_socket (Questa → gem5) ───────────────────────────
SC_MODULE(InjectDat) {
    tlm_utils::simple_initiator_socket<InjectDat> sock;
    SC_CTOR(InjectDat) : sock("sock") {}

    void send(chi::dat_optype_e op, unsigned txn_id, unsigned src_id,
              chi::dat_resptype_e resp_state,
              uint8_t data_id = 0,
              const uint8_t* data = nullptr, int data_len = 64)
    {
        auto* ext = new chi::chi_data_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(src_id);
        ext->dat.set_opcode(op);
        ext->dat.set_tgt_id(0);
        ext->dat.set_data_id(data_id);
        ext->dat.set_resp(resp_state);

        std::vector<uint8_t> buf(data_len, 0);
        if (data) memcpy(buf.data(), data, data_len);

        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(0x4000);
        gp.set_command(tlm::TLM_WRITE_COMMAND);
        gp.set_data_ptr(buf.data());
        gp.set_data_length(static_cast<unsigned>(data_len));
        sc_time d = SC_ZERO_TIME;
        sock->b_transport(gp, d);
    }
};

// ── inject gem5 DAT msg into datIn ───────────────────────────────────────────
static void push_dat(CHI_VIP_Controller& ctrl,
                     CHIDataType dtype, Addr txn_id, RubySystem& rs,
                     unsigned src_num = 1, unsigned tgt_num = 3,
                     const uint8_t* bytes = nullptr)
{
    auto msg = std::make_shared<CHIDataMsg>(curTick(), 64, &rs);
    msg->settype(dtype);
    msg->settxnId(txn_id);
    MachineID src{MachineType_Directory, static_cast<NodeID>(src_num)};
    msg->setresponder(src);
    MachineID tgt{MachineType_Cache, static_cast<NodeID>(tgt_num)};
    msg->getDestination().add(tgt);
    if (bytes) {
        DataBlock blk;
        for (int i = 0; i < 64; i++) blk.setByte(i, bytes[i]);
        msg->setdataBlk(blk);
    }
    ctrl.getDatIn()->enqueue(msg, curTick(), 0, false, false);
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_dat_types ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{MachineType_Directory, 0};

    CHI_VIP_Controller ctrl("ctrl", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    CatchDat  catch_dat("catch_dat");
    InjectDat inj_dat("inj_dat");

    ctrl.txdat_socket.bind(catch_dat.sock);
    inj_dat.sock.bind(ctrl.rxdat_socket);
    ctrl.init();

    // Helper: inject one gem5 DAT into datIn, wakeup, return last extension
    auto outbound = [&](CHIDataType t, Addr txn = 1,
                        unsigned src = 1, unsigned tgt = 3,
                        const uint8_t* bytes = nullptr)
                        -> chi::chi_data_extension* {
        catch_dat.reset();
        push_dat(ctrl, t, txn, rs, src, tgt, bytes);
        ctrl.wakeup();
        return catch_dat.last();
    };

    // ══ OUTBOUND pack_dat tests ══════════════════════════════════════════════

    // ── 1. CompData resp states ───────────────────────────────────────────────
    printf("\n[1] CompData all resp states\n");
    {
        using S = chi::dat_resptype_e;
        using D = chi::dat_optype_e;
        struct { CHIDataType gem5; S scc; const char* name; } cases[] = {
            { CHIDataType_CompData_I,    S::CompData_I,    "CompData_I"    },
            { CHIDataType_CompData_UC,   S::CompData_UC,   "CompData_UC"   },
            { CHIDataType_CompData_SC,   S::CompData_SC,   "CompData_SC"   },
            { CHIDataType_CompData_UD_PD,S::CompData_UD_PD,"CompData_UD_PD"},
            { CHIDataType_CompData_SD_PD,S::CompData_SD_PD,"CompData_SD_PD"},
        };
        for (auto& c : cases) {
            auto* e = outbound(c.gem5);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s opcode==CompData", c.name);
            CHECK(e && e->dat.get_opcode() == D::CompData, buf);
            snprintf(buf, sizeof(buf), "%s resp state correct", c.name);
            CHECK(e && e->dat.get_resp() == c.scc, buf);
        }
    }

    // ── 2. CopyBackWrData resp states ────────────────────────────────────────
    printf("\n[2] CopyBackWrData all resp states\n");
    {
        using S = chi::dat_resptype_e;
        using D = chi::dat_optype_e;
        struct { CHIDataType gem5; S scc; const char* name; } cases[] = {
            { CHIDataType_CBWrData_UC,    S::CopyBackWrData_UC,    "CBWrData_UC"    },
            { CHIDataType_CBWrData_SC,    S::CopyBackWrData_SC,    "CBWrData_SC"    },
            { CHIDataType_CBWrData_UD_PD, S::CopyBackWrData_UD_PD, "CBWrData_UD_PD" },
            { CHIDataType_CBWrData_SD_PD, S::CopyBackWrData_SD_PD, "CBWrData_SD_PD" },
            { CHIDataType_CBWrData_I,     S::CopyBackWrData_I,     "CBWrData_I"     },
        };
        for (auto& c : cases) {
            auto* e = outbound(c.gem5);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s opcode==CopyBackWrData", c.name);
            CHECK(e && e->dat.get_opcode() == D::CopyBackWrData, buf);
            snprintf(buf, sizeof(buf), "%s resp state correct", c.name);
            CHECK(e && e->dat.get_resp() == c.scc, buf);
        }
    }

    // ── 3. NCBWrData and DataSepResp ─────────────────────────────────────────
    printf("\n[3] NCBWrData and DataSepResp opcodes\n");
    {
        using D = chi::dat_optype_e;
        auto* e_ncb = outbound(CHIDataType_NCBWrData);
        CHECK(e_ncb && e_ncb->dat.get_opcode() == D::NonCopyBackWrData,
              "NCBWrData → NonCopyBackWrData");

        auto* e_sep = outbound(CHIDataType_DataSepResp_UC);
        CHECK(e_sep && e_sep->dat.get_opcode() == D::DataSepResp,
              "DataSepResp_UC → DataSepResp");
        CHECK(e_sep && e_sep->dat.get_resp() ==
              chi::dat_resptype_e::DataSepResp_UC,
              "DataSepResp_UC resp state");
    }

    // ── 4. SnpRespData resp states ────────────────────────────────────────────
    printf("\n[4] SnpRespData resp states\n");
    {
        using S = chi::dat_resptype_e;
        using D = chi::dat_optype_e;
        struct { CHIDataType gem5; S scc; const char* name; } cases[] = {
            { CHIDataType_SnpRespData_I,    S::SnpRespData_I,    "SnpRespData_I"    },
            { CHIDataType_SnpRespData_I_PD, S::SnpRespData_I_PD, "SnpRespData_I_PD" },
            { CHIDataType_SnpRespData_SC,   S::SnpRespData_SC,   "SnpRespData_SC"   },
            { CHIDataType_SnpRespData_SC_PD,S::SnpRespData_SC_PD,"SnpRespData_SC_PD"},
            { CHIDataType_SnpRespData_SD,   S::SnpRespData_SD,   "SnpRespData_SD"   },
            { CHIDataType_SnpRespData_UC,   S::SnpRespData_UC,   "SnpRespData_UC"   },
            { CHIDataType_SnpRespData_UD,   S::SnpRespData_UC,   "SnpRespData_UD"   },
        };
        for (auto& c : cases) {
            auto* e = outbound(c.gem5);
            char buf[64];
            snprintf(buf, sizeof(buf), "%s opcode==SnpRespData", c.name);
            CHECK(e && e->dat.get_opcode() == D::SnpRespData, buf);
            snprintf(buf, sizeof(buf), "%s resp state correct", c.name);
            CHECK(e && e->dat.get_resp() == c.scc, buf);
        }
    }

    // ── 5. SnpRespDataFwded — distinct opcode + resp state ───────────────────
    printf("\n[5] SnpRespDataFwded opcode 0x6 and resp state per snooped-node state\n");
    {
        using D = chi::dat_optype_e;
        using S = chi::dat_resptype_e;
        struct { CHIDataType gem5; S resp; const char* name; } cases[] = {
            { CHIDataType_SnpRespData_I_Fwded_SD_PD,  S::SnpRespData_I,    "I_Fwded_SD_PD"  },
            { CHIDataType_SnpRespData_I_Fwded_SC,      S::SnpRespData_I,    "I_Fwded_SC"     },
            { CHIDataType_SnpRespData_I_PD_Fwded_SC,   S::SnpRespData_I_PD, "I_PD_Fwded_SC"  },
            { CHIDataType_SnpRespData_SC_Fwded_SC,      S::SnpRespData_SC,   "SC_Fwded_SC"    },
            { CHIDataType_SnpRespData_SC_Fwded_SD_PD,  S::SnpRespData_SC,   "SC_Fwded_SD_PD" },
            { CHIDataType_SnpRespData_SC_PD_Fwded_SC,  S::SnpRespData_SC_PD,"SC_PD_Fwded_SC" },
            { CHIDataType_SnpRespData_SD_Fwded_SC,     S::SnpRespData_SD,   "SD_Fwded_SC"    },
        };
        for (auto& c : cases) {
            auto* e = outbound(c.gem5);
            char buf[80];
            snprintf(buf, sizeof(buf), "%s opcode==SnpRespDataFwded", c.name);
            CHECK(e && e->dat.get_opcode() == D::SnpRespDataFwded, buf);
            snprintf(buf, sizeof(buf), "%s resp state correct", c.name);
            CHECK(e && e->dat.get_resp() == c.resp, buf);
        }
    }

    // ── 6. DAT field propagation: TxnID, SrcID, TgtID ────────────────────────
    printf("\n[6] DAT field propagation (outbound)\n");
    {
        auto* e = outbound(CHIDataType_CompData_UC, /*txn=*/55,
                           /*src=*/7, /*tgt=*/4);
        CHECK(e && e->cmn.get_txn_id()  == 55, "TxnID == 55");
        CHECK(e && e->cmn.get_src_id()  == 7,  "SrcID == 7  (responder.num)");
        CHECK(e && e->dat.get_tgt_id()  == 4,  "TgtID == 4  (Destination.num)");
    }

    // ── 7. DAT data bytes round-trip (outbound) ───────────────────────────────
    printf("\n[7] data bytes preserved through pack_dat\n");
    {
        uint8_t payload[64];
        for (int i = 0; i < 64; i++) payload[i] = static_cast<uint8_t>(0xE0 + i);
        outbound(CHIDataType_CompData_SC, 1, 1, 3, payload);
        CHECK(!catch_dat.beats.empty() && catch_dat.beats[0].data_len == 64,
              "data length == 64");
        CHECK(!catch_dat.beats.empty() && catch_dat.beats[0].data[0]  == 0xE0,
              "data[0] == 0xE0");
        CHECK(!catch_dat.beats.empty() && catch_dat.beats[0].data[63] ==
              static_cast<uint8_t>(0xE0 + 63), "data[63] correct");
    }

    // ══ INBOUND unpack_dat tests ═════════════════════════════════════════════

    // ── 8. CopyBackWrData resp state dispatch ────────────────────────────────
    printf("\n[8] inbound CopyBackWrData resp state → gem5 type\n");
    {
        using S = chi::dat_resptype_e;
        using D = chi::dat_optype_e;
        struct { S resp; CHIDataType expected; const char* name; } cases[] = {
            { S::CopyBackWrData_UC,    CHIDataType_CBWrData_UC,    "UC"    },
            { S::CopyBackWrData_SC,    CHIDataType_CBWrData_SC,    "SC"    },
            { S::CopyBackWrData_UD_PD, CHIDataType_CBWrData_UD_PD, "UD_PD" },
            { S::CopyBackWrData_SD_PD, CHIDataType_CBWrData_SD_PD, "SD_PD" },
            { S::CopyBackWrData_I,     CHIDataType_CBWrData_I,     "I"     },
        };
        for (auto& c : cases) {
            inj_dat.send(D::CopyBackWrData, /*txn=*/30, /*src=*/2, c.resp);
            const auto* m = dynamic_cast<const CHIDataMsg*>(
                                ctrl.getDatOut()->peek());
            char buf[64];
            snprintf(buf, sizeof(buf), "CBWrData_%s type correct", c.name);
            CHECK(m && m->gettype() == c.expected, buf);
            if (ctrl.getDatOut()->isReady(curTick()))
                ctrl.getDatOut()->dequeue(curTick());
        }
    }

    // ── 9. NonCopyBackWrData inbound ─────────────────────────────────────────
    printf("\n[9] inbound NonCopyBackWrData\n");
    {
        using S = chi::dat_resptype_e;
        using D = chi::dat_optype_e;
        inj_dat.send(D::NonCopyBackWrData, /*txn=*/40, /*src=*/2, S::NonCopyBackWrData);
        const auto* m = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        CHECK(m && m->gettype() == CHIDataType_NCBWrData, "type == NCBWrData");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 10. Inbound TxnID and SrcID preservation ─────────────────────────────
    printf("\n[10] inbound DAT field propagation (TxnID, SrcID)\n");
    {
        using S = chi::dat_resptype_e;
        using D = chi::dat_optype_e;
        inj_dat.send(D::CopyBackWrData, /*txn=*/88, /*src=*/6, S::CopyBackWrData_SC);
        const auto* m = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        CHECK(m && m->gettxnId()         == 88, "txnId == 88");
        CHECK(m && m->getresponder().num == 6,  "responder.num == 6");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 11. Inbound data bytes preserved ─────────────────────────────────────
    printf("\n[11] inbound data bytes preserved through unpack_dat\n");
    {
        using S = chi::dat_resptype_e;
        using D = chi::dat_optype_e;
        uint8_t payload[64];
        for (int i = 0; i < 64; i++) payload[i] = static_cast<uint8_t>(i * 3 + 1);
        inj_dat.send(D::CopyBackWrData, /*txn=*/99, /*src=*/1, S::CopyBackWrData_UC,
                     /*data_id=*/0, payload, 64);
        const auto* m = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        CHECK(m && m->getdataBlk().getByte(0)  == 0x01, "data[0]  == 0x01");
        CHECK(m && m->getdataBlk().getByte(63) == static_cast<uint8_t>(63*3+1),
              "data[63] correct");
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 12. Inbound SnpRespData resp state dispatch ───────────────────────────
    printf("\n[12] inbound SnpRespData resp state → gem5 type\n");
    {
        using S = chi::dat_resptype_e;
        using D = chi::dat_optype_e;
        struct { S resp; CHIDataType expected; const char* name; } cases[] = {
            { S::SnpRespData_I,    CHIDataType_SnpRespData_I,    "I"     },
            { S::SnpRespData_SC,   CHIDataType_SnpRespData_SC,   "SC"    },
            { S::SnpRespData_UC,   CHIDataType_SnpRespData_UC,   "UC"    },
            { S::SnpRespData_SD,   CHIDataType_SnpRespData_SD,   "SD"    },
            { S::SnpRespData_I_PD, CHIDataType_SnpRespData_I_PD, "I_PD"  },
            { S::SnpRespData_SC_PD,CHIDataType_SnpRespData_SC_PD,"SC_PD" },
        };
        for (auto& c : cases) {
            inj_dat.send(D::SnpRespData, /*txn=*/50, /*src=*/3, c.resp);
            const auto* m = dynamic_cast<const CHIDataMsg*>(
                                ctrl.getDatOut()->peek());
            char buf[64];
            snprintf(buf, sizeof(buf), "SnpRespData_%s type correct", c.name);
            CHECK(m && m->gettype() == c.expected, buf);
            if (ctrl.getDatOut()->isReady(curTick()))
                ctrl.getDatOut()->dequeue(curTick());
        }
    }

    // ── 13. Inbound SnpRespDataFwded resp-state dispatch ─────────────────────
    printf("\n[13] inbound SnpRespDataFwded resp state → gem5 type\n");
    {
        using S = chi::dat_resptype_e;
        using D = chi::dat_optype_e;
        // Previously all fell to NCBWrData (opcode 0x6 had no case in unpack_dat)
        struct { S resp; CHIDataType expected; const char* name; } cases[] = {
            { S::SnpRespData_I,    CHIDataType_SnpRespData_I_Fwded_SC,   "I→I_Fwded_SC"     },
            { S::SnpRespData_SC,   CHIDataType_SnpRespData_SC_Fwded_SC,  "SC→SC_Fwded_SC"   },
            { S::SnpRespData_SD,   CHIDataType_SnpRespData_SD_Fwded_SC,  "SD→SD_Fwded_SC"   },
            { S::SnpRespData_I_PD, CHIDataType_SnpRespData_I_PD_Fwded_SC,"I_PD→I_PD_Fwded_SC"},
            { S::SnpRespData_SC_PD,CHIDataType_SnpRespData_SC_PD_Fwded_SC,"SC_PD→SC_PD_Fwded_SC"},
        };
        for (auto& c : cases) {
            inj_dat.send(D::SnpRespDataFwded, /*txn=*/60, /*src=*/3, c.resp);
            const auto* m = dynamic_cast<const CHIDataMsg*>(
                                ctrl.getDatOut()->peek());
            char buf[80];
            snprintf(buf, sizeof(buf), "SnpRespDataFwded resp=%s type correct", c.name);
            CHECK(m && m->gettype() == c.expected, buf);
            if (ctrl.getDatOut()->isReady(curTick()))
                ctrl.getDatOut()->dequeue(curTick());
        }
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    sc_start(1, SC_NS);
    return g_fail != 0 ? 1 : 0;
}
