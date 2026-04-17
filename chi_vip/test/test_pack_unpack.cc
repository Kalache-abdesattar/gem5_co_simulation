// test_pack_unpack.cc
//
// Architecture: CHI_VIP_Controller acts as an RN-F proxy in the Ruby network.
//   Inbound (Questa → gem5):
//     rxreq → reqOut  (RN-F sends REQ out to the network toward HN-F)
//     rxdat → datOut  (RN-F sends write data out to the network)
//     rxrsp → rspOut  (RN-F sends CompAck out to the network)
//   Outbound (gem5 → Questa):
//     rspIn ← RSP from HN-F → wakeup() → recvResponseMsg → txrsp_socket
//     datIn ← RDAT from HN-F → wakeup() → recvDataMsg     → txdat_socket
//     snpIn ← SNP from HN-F → wakeup() → recvSnoopMsg     → txsnp_socket

#include <systemc>
#include <chi/chi_tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <cstdio>
#include <cstring>

#include "chi_flit_pack.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/common/MachineID.hh"
#include "chi_vip_controller.hh"
#include "CHIResponseMsg.hh"
#include "CHIDataMsg.hh"
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
// Copies the extensions and data out of the gp before it is destroyed.
SC_MODULE(CatchTarget) {
    tlm_utils::simple_target_socket<CatchTarget> sock;

    bool                     got_gp    = false;
    uint32_t                 data_len  = 0;
    uint8_t                  data_copy[64] = {};

    // Owned copies of the last extensions seen (nullptr if absent)
    std::unique_ptr<chi::chi_ctrl_extension> last_ctrl;
    std::unique_ptr<chi::chi_data_extension> last_dat;
    std::unique_ptr<chi::chi_snp_extension>  last_snp;

    SC_CTOR(CatchTarget) : sock("sock") {
        sock.register_b_transport(this, &CatchTarget::b_transport);
    }

    void b_transport(tlm::tlm_generic_payload& gp, sc_time&) {
        got_gp   = true;
        data_len = gp.get_data_length();
        if (gp.get_data_ptr() && data_len > 0 && data_len <= 64)
            memcpy(data_copy, gp.get_data_ptr(), data_len);

        // Clone extensions so we can inspect them after gp goes out of scope
        if (auto* e = gp.get_extension<chi::chi_ctrl_extension>())
            last_ctrl.reset(static_cast<chi::chi_ctrl_extension*>(e->clone()));
        if (auto* e = gp.get_extension<chi::chi_data_extension>())
            last_dat.reset(static_cast<chi::chi_data_extension*>(e->clone()));
        if (auto* e = gp.get_extension<chi::chi_snp_extension>())
            last_snp.reset(static_cast<chi::chi_snp_extension*>(e->clone()));

        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }

    void reset() {
        got_gp = false; data_len = 0;
        last_ctrl.reset(); last_dat.reset(); last_snp.reset();
        memset(data_copy, 0, sizeof(data_copy));
    }
};

// ── InjectInitiator: drives inbound sockets ───────────────────────────────────
SC_MODULE(InjectInitiator) {
    tlm_utils::simple_initiator_socket<InjectInitiator> req_sock;
    tlm_utils::simple_initiator_socket<InjectInitiator> dat_sock;
    tlm_utils::simple_initiator_socket<InjectInitiator> rsp_sock;

    SC_CTOR(InjectInitiator) : req_sock("req_sock"),
                                dat_sock("dat_sock"),
                                rsp_sock("rsp_sock") {}

    void send_req(Addr addr, chi::req_optype_e op,
                  unsigned txn_id, unsigned src_id, bool ns = true) {
        auto* ext = new chi::chi_ctrl_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(src_id);
        ext->req.set_opcode(op);
        ext->req.set_tgt_id(0);
        ext->req.set_non_secure(ns);
        ext->req.set_allow_retry(false);
        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(addr);
        gp.set_command(tlm::TLM_IGNORE_COMMAND);
        gp.set_data_length(0);
        gp.set_data_ptr(nullptr);
        sc_time d = SC_ZERO_TIME;
        req_sock->b_transport(gp, d);
    }

    void send_dat(chi::dat_optype_e op, unsigned txn_id,
                  const uint8_t* data, uint16_t tgt_id,
                  chi::dat_resptype_e resp = chi::dat_resptype_e::NonCopyBackWrData) {
        auto* ext = new chi::chi_data_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(5);
        ext->dat.set_opcode(op);
        ext->dat.set_tgt_id(tgt_id);
        ext->dat.set_resp(resp);
        static uint8_t buf[64];
        if (data) memcpy(buf, data, 64); else memset(buf, 0, 64);
        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(0x1000);
        gp.set_command(tlm::TLM_WRITE_COMMAND);
        gp.set_data_ptr(buf);
        gp.set_data_length(64);
        sc_time d = SC_ZERO_TIME;
        dat_sock->b_transport(gp, d);
    }

    void send_rsp(chi::rsp_optype_e op, unsigned txn_id) {
        auto* ext = new chi::chi_ctrl_extension();
        ext->cmn.set_txn_id(txn_id);
        ext->cmn.set_src_id(3);
        ext->resp.set_opcode(op);
        ext->resp.set_tgt_id(0);
        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(0);
        gp.set_command(tlm::TLM_IGNORE_COMMAND);
        gp.set_data_length(0);
        gp.set_data_ptr(nullptr);
        sc_time d = SC_ZERO_TIME;
        rsp_sock->b_transport(gp, d);
    }
};

// ── peek helper ──────────────────────────────────────────────────────────────
template<typename T>
static const T* peek_buf(MessageBuffer* buf) {
    return dynamic_cast<const T*>(buf->peek());
}

// ── helper: inject a gem5 message directly into an "In" buffer ───────────────
// Simulates what the Ruby network delivers to this node from the HN-F.
static void inject_rsp_into_rspIn(CHI_VIP_Controller& ctrl,
                                   CHIResponseType rtype, Addr txn_id,
                                   Addr dbid, gem5::ruby::RubySystem& rs)
{
    auto msg = std::make_shared<CHIResponseMsg>(curTick(), 64, &rs);
    msg->settype(rtype);
    msg->settxnId(txn_id);
    msg->setdbid(dbid);
    MachineID src{MachineType_Directory, 0};
    msg->setresponder(src);
    // Latency=0: message is immediately ready for wakeup()
    ctrl.getRspIn()->enqueue(msg, curTick(), 0, false, false);
}

static void inject_dat_into_datIn(CHI_VIP_Controller& ctrl,
                                   CHIDataType dtype, Addr txn_id,
                                   const uint8_t* bytes,
                                   gem5::ruby::RubySystem& rs)
{
    auto msg = std::make_shared<CHIDataMsg>(curTick(), 64, &rs);
    msg->settype(dtype);
    msg->settxnId(txn_id);
    MachineID src{MachineType_Directory, 0};
    msg->setresponder(src);
    if (bytes) {
        DataBlock blk;
        for (int i = 0; i < 64; i++) blk.setByte(i, bytes[i]);
        msg->setdataBlk(blk);
    }
    ctrl.getDatIn()->enqueue(msg, curTick(), 0, false, false);
}

static void inject_snp_into_snpIn(CHI_VIP_Controller& ctrl,
                                   CHIRequestType rtype, Addr txn_id,
                                   bool ret_to_src,
                                   gem5::ruby::RubySystem& rs)
{
    auto msg = std::make_shared<CHIRequestMsg>(curTick(), 64, &rs);
    msg->settype(rtype);
    msg->settxnId(txn_id);
    msg->setretToSrc(ret_to_src);
    MachineID src{MachineType_Directory, 0};
    msg->setrequestor(src);
    ctrl.getSnpIn()->enqueue(msg, curTick(), 0, false, false);
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_pack_unpack ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{ MachineType_Directory, 0 };

    CHI_VIP_Controller ctrl("hnf0", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    CatchTarget    catch_rsp("catch_rsp");
    CatchTarget    catch_dat("catch_dat");
    CatchTarget    catch_snp("catch_snp");
    InjectInitiator inj("inj");

    ctrl.txrsp_socket.bind(catch_rsp.sock);
    ctrl.txdat_socket.bind(catch_dat.sock);
    ctrl.txsnp_socket.bind(catch_snp.sock);
    inj.req_sock.bind(ctrl.rxreq_socket);
    inj.dat_sock.bind(ctrl.rxdat_socket);
    inj.rsp_sock.bind(ctrl.rxrsp_socket);

    ctrl.init();

    // ═════════════════════════════════════════════════════════════════════════
    // INBOUND TESTS: Questa → gem5 (TLM2 → Ruby message)
    // Messages land in reqOut / datOut / rspOut (outgoing from this RN-F proxy)
    // ═════════════════════════════════════════════════════════════════════════

    // ── 1. unpack_req: ReadShared ─────────────────────────────────────────────
    printf("\n[1] unpack_req: ReadShared\n");
    {
        inj.send_req(0xDEAD000, chi::req_optype_e::ReadShared, 42, 7, true);
        const CHIRequestMsg* m = peek_buf<CHIRequestMsg>(ctrl.getReqOut());
        CHECK(m != nullptr,                                     "msg in reqOut");
        if (m) {
            CHECK(m->gettype()       == CHIRequestType_ReadShared, "type == ReadShared");
            CHECK(m->gettxnId()      == 42,                        "txnId == 42");
            CHECK(m->getaddr()       == 0xDEAD000,                 "address preserved");
            CHECK(m->getns()         == true,                      "NS bit preserved");
            CHECK(m->getallowRetry() == false,                     "allowRetry preserved");
            CHECK(m->getrequestor().num == 7,                      "src_id → requestor.num");
        }
        ctrl.getReqOut()->dequeue(curTick());
    }

    // ── 2. unpack_req: Excl=1 round-trips through pack/unpack ────────────────
    // Excl and SnoopMe share flit bit 108 (IHI0050E Table 13-1).
    // Verify unpack_req sets BOTH fields from that single bit.
    printf("\n[2] unpack_req: Excl=1 round-trips through flit pack/unpack\n");
    {
        using namespace chi_flit;
        sc_dt::sc_bv<CHI_REQ_FLIT_W> f = 0;
        bf_set(f, req_f::OPCODE_LO, req_f::OPCODE_W, 0x01); // ReadShared
        bf_setb(f, req_f::EXCL_LO, true);   // Excl=1 (== SnoopMe_LO)

        tlm::tlm_generic_payload gp;
        chi::chi_ctrl_extension ext;
        unpack_req(f, gp, ext);

        CHECK(ext.req.is_excl(),     "unpack_req: Excl=1 sets is_excl()");
        CHECK(ext.req.is_snoop_me(), "unpack_req: Excl=1 also sets is_snoop_me() (same bit)");

        // Round-trip: clear and repack (use pack_rsp path for symmetric check;
        // REQ pack isn't in chi_flit_pack.hh since REQs only travel inbound)
        sc_dt::sc_bv<CHI_REQ_FLIT_W> f2 = 0;
        bf_setb(f2, req_f::EXCL_LO, false);
        chi::chi_ctrl_extension ext2;
        unpack_req(f2, gp, ext2);
        CHECK(!ext2.req.is_excl(),     "unpack_req: Excl=0 → is_excl() false");
        CHECK(!ext2.req.is_snoop_me(), "unpack_req: Excl=0 → is_snoop_me() false");
    }

    // ── 3. unpack_req: WriteUniqueFull ────────────────────────────────────────
    printf("\n[3] unpack_req: WriteUniqueFull (VIP controller path)\n");
    {
        inj.send_req(0xC0FFEE00, chi::req_optype_e::WriteUniqueFull, 99, 3);
        const CHIRequestMsg* m = peek_buf<CHIRequestMsg>(ctrl.getReqOut());
        CHECK(m != nullptr,                                        "msg in reqOut");
        if (m) {
            CHECK(m->gettype()  == CHIRequestType_WriteUniqueFull, "type == WriteUniqueFull");
            CHECK(m->gettxnId() == 99,                             "txnId == 99");
            CHECK(m->getaddr()  == 0xC0FFEE00,                     "address preserved");
        }
        ctrl.getReqOut()->dequeue(curTick());
    }

    // ── 3. unpack_dat: CopyBackWrData + data bytes ────────────────────────────
    printf("\n[4] unpack_dat: CopyBackWrData\n");
    {
        uint8_t payload[64];
        for (int i = 0; i < 64; i++) payload[i] = (uint8_t)(i * 2 + 1);
        inj.send_dat(chi::dat_optype_e::CopyBackWrData, 77, payload, 0,
                     chi::dat_resptype_e::CopyBackWrData_UC);
        const CHIDataMsg* m = peek_buf<CHIDataMsg>(ctrl.getDatOut());
        CHECK(m != nullptr,                                 "msg in datOut");
        if (m) {
            CHECK(m->gettype()  == CHIDataType_CBWrData_UC,           "type == CBWrData_UC");
            CHECK(m->gettxnId() == 77,                                "txnId == 77");
            CHECK(m->getdataBlk().getByte(0)  == 0x01,               "data[0] correct");
            CHECK(m->getdataBlk().getByte(63) == (uint8_t)(63*2+1),  "data[63] correct");
        }
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 4. unpack_dat: NonCopyBackWrData ──────────────────────────────────────
    printf("\n[5] unpack_dat: NonCopyBackWrData\n");
    {
        inj.send_dat(chi::dat_optype_e::NonCopyBackWrData, 55, nullptr, 0);
        const CHIDataMsg* m = peek_buf<CHIDataMsg>(ctrl.getDatOut());
        CHECK(m != nullptr,                                "msg in datOut");
        if (m) {
            CHECK(m->gettype()  == CHIDataType_NCBWrData,  "type == NCBWrData");
            CHECK(m->gettxnId() == 55,                     "txnId == 55");
        }
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 5. unpack_rsp: CompAck ────────────────────────────────────────────────
    printf("\n[6] unpack_rsp: CompAck (RN-F → HN-F)\n");
    {
        inj.send_rsp(chi::rsp_optype_e::CompAck, 11);
        const CHIResponseMsg* m = peek_buf<CHIResponseMsg>(ctrl.getRspOut());
        CHECK(m != nullptr,                                  "msg in rspOut");
        if (m) {
            CHECK(m->gettype()  == CHIResponseType_CompAck,  "type == CompAck");
            CHECK(m->gettxnId() == 11,                       "txnId == 11");
        }
        ctrl.getRspOut()->dequeue(curTick());
    }

    // ── 6. unpack_rsp: SnpResp ────────────────────────────────────────────────
    printf("\n[7] unpack_rsp: SnpResp (RN-F → HN-F)\n");
    {
        inj.send_rsp(chi::rsp_optype_e::SnpResp, 22);
        const CHIResponseMsg* m = peek_buf<CHIResponseMsg>(ctrl.getRspOut());
        CHECK(m != nullptr,                                     "msg in rspOut");
        if (m) {
            CHECK(m->gettype()  == CHIResponseType_SnpResp_I,   "type == SnpResp_I");
            CHECK(m->gettxnId() == 22,                          "txnId == 22");
        }
        ctrl.getRspOut()->dequeue(curTick());
    }

    // ═════════════════════════════════════════════════════════════════════════
    // OUTBOUND TESTS: gem5 → Questa (Ruby message → TLM2)
    // Messages are injected into rspIn/datIn/snpIn (what the Ruby network
    // delivers to this node from the HN-F), then wakeup() forwards them.
    // ═════════════════════════════════════════════════════════════════════════

    // ── 7. pack_rsp: Comp_UC from HN-F → txrsp_socket ────────────────────────
    printf("\n[8] pack_rsp: Comp_UC outbound\n");
    {
        catch_rsp.reset();
        inject_rsp_into_rspIn(ctrl, CHIResponseType_Comp_UC, 88, 0, rs);
        ctrl.wakeup();

        CHECK(catch_rsp.last_ctrl != nullptr, "ctrl extension on txrsp");
        if (catch_rsp.last_ctrl) {
            CHECK(catch_rsp.last_ctrl->resp.get_opcode() == chi::rsp_optype_e::Comp,
                  "opcode == Comp");
            CHECK(catch_rsp.last_ctrl->resp.get_resp() == chi::rsp_resptype_e::Comp_UC,
                  "resp state == Comp_UC");
            CHECK(catch_rsp.last_ctrl->cmn.get_txn_id() == 88, "txnId == 88");
        }
    }

    // ── 8. pack_rsp: DBIDResp from HN-F → txrsp_socket ───────────────────────
    printf("\n[9] pack_rsp: DBIDResp outbound\n");
    {
        catch_rsp.reset();
        inject_rsp_into_rspIn(ctrl, CHIResponseType_DBIDResp, 33, 5, rs);
        ctrl.wakeup();

        CHECK(catch_rsp.last_ctrl != nullptr, "ctrl extension on txrsp");
        if (catch_rsp.last_ctrl) {
            CHECK(catch_rsp.last_ctrl->resp.get_opcode() == chi::rsp_optype_e::DBIDResp,
                  "opcode == DBIDResp");
            CHECK(catch_rsp.last_ctrl->resp.get_db_id() == 5, "db_id == 5");
        }
    }

    // ── 9. pack_dat: CompData_SC from HN-F → txdat_socket ────────────────────
    printf("\n[10] pack_dat: CompData_SC outbound\n");
    {
        catch_dat.reset();
        uint8_t bytes[64];
        for (int i = 0; i < 64; i++) bytes[i] = (uint8_t)(0xA0 + i);
        inject_dat_into_datIn(ctrl, CHIDataType_CompData_SC, 7, bytes, rs);
        ctrl.wakeup();

        CHECK(catch_dat.last_dat != nullptr, "data extension on txdat");
        if (catch_dat.last_dat) {
            CHECK(catch_dat.last_dat->dat.get_opcode() == chi::dat_optype_e::CompData,
                  "opcode == CompData");
            CHECK(catch_dat.last_dat->dat.get_resp() == chi::dat_resptype_e::CompData_SC,
                  "resp state == CompData_SC");
        }
        CHECK(catch_dat.got_gp,                        "gp received on txdat");
        CHECK(catch_dat.data_len == 64,                "data_length == 64");
        CHECK(catch_dat.data_copy[0]  == 0xA0,         "data[0] == 0xA0");
        CHECK(catch_dat.data_copy[63] == (uint8_t)(0xA0+63), "data[63] correct");
    }

    // ── 10. pack_snp: SnpUnique from HN-F → txsnp_socket ─────────────────────
    printf("\n[11] pack_snp: SnpUnique outbound\n");
    {
        catch_snp.reset();
        inject_snp_into_snpIn(ctrl, CHIRequestType_SnpUnique, 13, true, rs);
        ctrl.wakeup();

        CHECK(catch_snp.last_snp != nullptr, "snp extension on txsnp");
        if (catch_snp.last_snp) {
            CHECK(catch_snp.last_snp->req.get_opcode() == chi::snp_optype_e::SnpUnique,
                  "opcode == SnpUnique");
            CHECK(catch_snp.last_snp->req.is_ret_to_src() == true,
                  "RetToSrc == true");
        }
    }

    // ── 11. No extension → TLM_GENERIC_ERROR_RESPONSE ────────────────────────
    printf("\n[12] inbound without CHI extension → error response\n");
    {
        tlm::tlm_generic_payload gp;
        gp.set_address(0);
        gp.set_command(tlm::TLM_IGNORE_COMMAND);
        gp.set_data_length(0);
        gp.set_data_ptr(nullptr);
        sc_time d = SC_ZERO_TIME;

        inj.req_sock->b_transport(gp, d);
        CHECK(gp.get_response_status() == tlm::TLM_GENERIC_ERROR_RESPONSE,
              "rxreq: no-extension → TLM_GENERIC_ERROR_RESPONSE");

        inj.dat_sock->b_transport(gp, d);
        CHECK(gp.get_response_status() == tlm::TLM_GENERIC_ERROR_RESPONSE,
              "rxdat: no-extension → TLM_GENERIC_ERROR_RESPONSE");

        inj.rsp_sock->b_transport(gp, d);
        CHECK(gp.get_response_status() == tlm::TLM_GENERIC_ERROR_RESPONSE,
              "rxrsp: no-extension → TLM_GENERIC_ERROR_RESPONSE");
    }

    // ── 12. Three REQs queue in FIFO order ────────────────────────────────────
    printf("\n[13] multiple REQs queue in FIFO order (reqOut)\n");
    {
        inj.send_req(0x1000, chi::req_optype_e::ReadShared,    1, 1);
        inj.send_req(0x2000, chi::req_optype_e::ReadUnique,    2, 2);
        inj.send_req(0x3000, chi::req_optype_e::WriteBackFull, 3, 3);

        CHECK(ctrl.getReqOut()->size() == 3, "reqOut has 3 messages");

        const CHIRequestMsg* m0 = peek_buf<CHIRequestMsg>(ctrl.getReqOut());
        CHECK(m0 && m0->getaddr() == 0x1000, "msg[0] addr == 0x1000");
        ctrl.getReqOut()->dequeue(curTick());

        const CHIRequestMsg* m1 = peek_buf<CHIRequestMsg>(ctrl.getReqOut());
        CHECK(m1 && m1->getaddr() == 0x2000, "msg[1] addr == 0x2000");
        ctrl.getReqOut()->dequeue(curTick());

        const CHIRequestMsg* m2 = peek_buf<CHIRequestMsg>(ctrl.getReqOut());
        CHECK(m2 && m2->getaddr() == 0x3000, "msg[2] addr == 0x3000");
        ctrl.getReqOut()->dequeue(curTick());

        CHECK(ctrl.getReqOut()->isEmpty(), "reqOut empty after dequeue");
    }

    // ── 13. Comp_I and RetryAck outbound ─────────────────────────────────────
    printf("\n[14] pack_rsp: Comp_I and RetryAck outbound\n");
    {
        for (auto [rtype, exp_op] : std::initializer_list<
                std::pair<CHIResponseType, chi::rsp_optype_e>>{
            { CHIResponseType_Comp_I,   chi::rsp_optype_e::Comp     },
            { CHIResponseType_RetryAck, chi::rsp_optype_e::RetryAck },
        }) {
            catch_rsp.reset();
            inject_rsp_into_rspIn(ctrl, rtype, 1, 0, rs);
            ctrl.wakeup();
            CHECK(catch_rsp.last_ctrl != nullptr, "extension present");
            if (catch_rsp.last_ctrl)
                CHECK(catch_rsp.last_ctrl->resp.get_opcode() == exp_op, "opcode matches");
        }
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    return g_fail != 0 ? 1 : 0;
}
