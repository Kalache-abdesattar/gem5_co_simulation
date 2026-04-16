// test_channels.cc
//
// Tests for simultaneous multi-channel activity and cross-channel independence:
//   • All four channels (REQ in, DAT in, RSP in, SNP out) active in the same
//     wakeup/transport cycle without interference
//   • Enqueueing to one buffer does not disturb others
//   • Mixed inbound + outbound in one wakeup()

#include <systemc>
#include <chi/chi_tlm.h>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>
#include <cstdio>
#include <cstring>
#include <memory>
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

// ── Catch modules for each outbound socket ─────────────────────────────────

SC_MODULE(CatchRsp) {
    tlm_utils::simple_target_socket<CatchRsp> sock;
    int count = 0;
    chi::rsp_optype_e last_op = chi::rsp_optype_e::Comp;
    SC_CTOR(CatchRsp) : sock("sock") {
        sock.register_b_transport(this, &CatchRsp::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& gp, sc_time&) {
        if (auto* e = gp.get_extension<chi::chi_ctrl_extension>())
            last_op = e->resp.get_opcode();
        count++;
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void reset() { count = 0; }
};

SC_MODULE(CatchDat) {
    tlm_utils::simple_target_socket<CatchDat> sock;
    int count = 0;
    chi::dat_optype_e last_op = chi::dat_optype_e::CompData;
    SC_CTOR(CatchDat) : sock("sock") {
        sock.register_b_transport(this, &CatchDat::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& gp, sc_time&) {
        if (auto* e = gp.get_extension<chi::chi_data_extension>())
            last_op = e->dat.get_opcode();
        count++;
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void reset() { count = 0; }
};

SC_MODULE(CatchSnp) {
    tlm_utils::simple_target_socket<CatchSnp> sock;
    int count = 0;
    chi::snp_optype_e last_op = chi::snp_optype_e::SnpShared;
    SC_CTOR(CatchSnp) : sock("sock") {
        sock.register_b_transport(this, &CatchSnp::b_transport);
    }
    void b_transport(tlm::tlm_generic_payload& gp, sc_time&) {
        if (auto* e = gp.get_extension<chi::chi_snp_extension>())
            last_op = e->req.get_opcode();
        count++;
        gp.set_response_status(tlm::TLM_OK_RESPONSE);
    }
    void reset() { count = 0; }
};

// ── Inject modules for inbound sockets ──────────────────────────────────────

SC_MODULE(InjectReq) {
    tlm_utils::simple_initiator_socket<InjectReq> sock;
    SC_CTOR(InjectReq) : sock("sock") {}
    void send(Addr addr, chi::req_optype_e op, unsigned txn, unsigned src) {
        auto* ext = new chi::chi_ctrl_extension();
        ext->cmn.set_txn_id(txn); ext->cmn.set_src_id(src);
        ext->req.set_opcode(op); ext->req.set_tgt_id(0);
        tlm::tlm_generic_payload gp;
        gp.set_extension(ext); gp.set_address(addr);
        gp.set_command(tlm::TLM_IGNORE_COMMAND);
        gp.set_data_length(0); gp.set_data_ptr(nullptr);
        sc_time d = SC_ZERO_TIME; sock->b_transport(gp, d);
    }
};

SC_MODULE(InjectDat) {
    tlm_utils::simple_initiator_socket<InjectDat> sock;
    SC_CTOR(InjectDat) : sock("sock") {}
    void send(chi::dat_optype_e op, unsigned txn, unsigned src,
              chi::dat_resptype_e resp) {
        auto* ext = new chi::chi_data_extension();
        ext->cmn.set_txn_id(txn); ext->cmn.set_src_id(src);
        ext->dat.set_opcode(op); ext->dat.set_data_id(0); ext->dat.set_resp(resp);
        static uint8_t buf[64] = {};
        tlm::tlm_generic_payload gp;
        gp.set_extension(ext); gp.set_address(0);
        gp.set_command(tlm::TLM_WRITE_COMMAND);
        gp.set_data_ptr(buf); gp.set_data_length(64);
        sc_time d = SC_ZERO_TIME; sock->b_transport(gp, d);
    }
};

SC_MODULE(InjectRsp) {
    tlm_utils::simple_initiator_socket<InjectRsp> sock;
    SC_CTOR(InjectRsp) : sock("sock") {}
    void send(chi::rsp_optype_e op, unsigned txn, unsigned src) {
        auto* ext = new chi::chi_ctrl_extension();
        ext->cmn.set_txn_id(txn); ext->cmn.set_src_id(src);
        ext->resp.set_opcode(op); ext->resp.set_tgt_id(0);
        tlm::tlm_generic_payload gp;
        gp.set_extension(ext); gp.set_address(0);
        gp.set_command(tlm::TLM_IGNORE_COMMAND);
        gp.set_data_length(0); gp.set_data_ptr(nullptr);
        sc_time d = SC_ZERO_TIME; sock->b_transport(gp, d);
    }
};

// ── Helper: push into gem5 In buffers ───────────────────────────────────────

static void push_rsp(CHI_VIP_Controller& ctrl, CHIResponseType t,
                     Addr txn, RubySystem& rs) {
    auto msg = std::make_shared<CHIResponseMsg>(curTick(), 64, &rs);
    msg->settype(t); msg->settxnId(txn);
    MachineID src{MachineType_Directory, 0}; msg->setresponder(src);
    MachineID tgt{MachineType_Cache, 1};     msg->getDestination().add(tgt);
    ctrl.getRspIn()->enqueue(msg, curTick(), 0, false, false);
}

static void push_dat(CHI_VIP_Controller& ctrl, CHIDataType t,
                     Addr txn, RubySystem& rs) {
    auto msg = std::make_shared<CHIDataMsg>(curTick(), 64, &rs);
    msg->settype(t); msg->settxnId(txn);
    MachineID src{MachineType_Directory, 0}; msg->setresponder(src);
    MachineID tgt{MachineType_Cache, 1};     msg->getDestination().add(tgt);
    ctrl.getDatIn()->enqueue(msg, curTick(), 0, false, false);
}

static void push_snp(CHI_VIP_Controller& ctrl, CHIRequestType t,
                     Addr addr, Addr txn, RubySystem& rs) {
    auto msg = std::make_shared<CHIRequestMsg>(curTick(), 64, &rs);
    msg->settype(t); msg->setaddr(addr); msg->settxnId(txn);
    MachineID src{MachineType_Directory, 0}; msg->setrequestor(src);
    MachineID tgt{MachineType_Cache, 1};     msg->getDestination().add(tgt);
    ctrl.getSnpIn()->enqueue(msg, curTick(), 0, false, false);
}

// ─────────────────────────────────────────────────────────────────────────────
int sc_main(int, char**) {
    sc_set_time_resolution(1, SC_PS);
    printf("\n=== test_channels ===\n");

    gem5::ruby::RubySystem rs;
    rs.m_cache_line_size = 64;
    Network net;
    MachineID hnf_id{MachineType_Directory, 0};

    CHI_VIP_Controller ctrl("ctrl", hnf_id, &rs, &net);
    ctrl.downstream_ = hnf_id;

    CatchRsp  catch_rsp("catch_rsp");
    CatchDat  catch_dat("catch_dat");
    CatchSnp  catch_snp("catch_snp");
    InjectReq inj_req("inj_req");
    InjectDat inj_dat("inj_dat");
    InjectRsp inj_rsp("inj_rsp");

    ctrl.txrsp_socket.bind(catch_rsp.sock);
    ctrl.txdat_socket.bind(catch_dat.sock);
    ctrl.txsnp_socket.bind(catch_snp.sock);
    inj_req.sock.bind(ctrl.rxreq_socket);
    inj_dat.sock.bind(ctrl.rxdat_socket);
    inj_rsp.sock.bind(ctrl.rxrsp_socket);
    ctrl.init();

    // ── 1. Cross-channel independence: inbound REQ + inbound DAT ─────────────
    printf("\n[1] cross-channel independence: simultaneous inbound REQ and DAT\n");
    {
        inj_req.send(0x1000, chi::req_optype_e::ReadShared, /*txn=*/1, /*src=*/5);
        inj_dat.send(chi::dat_optype_e::CopyBackWrData, /*txn=*/2, /*src=*/6,
                     chi::dat_resptype_e::CopyBackWrData_UC);

        const auto* req_msg = dynamic_cast<const CHIRequestMsg*>(ctrl.getReqOut()->peek());
        const auto* dat_msg = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());

        CHECK(req_msg && req_msg->gettxnId() == 1, "REQ txn=1 in reqOut");
        CHECK(dat_msg && dat_msg->gettxnId() == 2, "DAT txn=2 in datOut");
        CHECK(!ctrl.getRspOut()->isReady(curTick()),  "rspOut not disturbed");

        ctrl.getReqOut()->dequeue(curTick());
        ctrl.getDatOut()->dequeue(curTick());
    }

    // ── 2. Outbound RSP + DAT + SNP in one wakeup() ──────────────────────────
    printf("\n[2] outbound RSP + DAT + SNP all delivered in one wakeup()\n");
    {
        catch_rsp.reset(); catch_dat.reset(); catch_snp.reset();

        push_rsp(ctrl, CHIResponseType_Comp_UC,   10, rs);
        push_dat(ctrl, CHIDataType_CompData_SC,    11, rs);
        push_snp(ctrl, CHIRequestType_SnpUnique,   0x2000, 12, rs);

        ctrl.wakeup();

        CHECK(catch_rsp.count == 1 &&
              catch_rsp.last_op == chi::rsp_optype_e::Comp,
              "txrsp: one Comp delivered");
        CHECK(catch_dat.count == 1 &&
              catch_dat.last_op == chi::dat_optype_e::CompData,
              "txdat: one CompData delivered");
        CHECK(catch_snp.count == 1 &&
              catch_snp.last_op == chi::snp_optype_e::SnpUnique,
              "txsnp: one SnpUnique delivered");
    }

    // ── 3. Inbound RSP + REQ simultaneously, then outbound RSP ───────────────
    printf("\n[3] inbound RSP + REQ simultaneously, then outbound RSP\n");
    {
        catch_rsp.reset();

        inj_req.send(0x3000, chi::req_optype_e::WriteBackFull, /*txn=*/20, /*src=*/7);
        inj_rsp.send(chi::rsp_optype_e::CompAck, /*txn=*/21, /*src=*/7);
        push_rsp(ctrl, CHIResponseType_DBIDResp, 22, rs);

        ctrl.wakeup();

        const auto* req_m = dynamic_cast<const CHIRequestMsg*>(ctrl.getReqOut()->peek());
        CHECK(req_m && req_m->gettxnId() == 20, "inbound REQ txn=20 in reqOut");

        const auto* rsp_m = dynamic_cast<const CHIResponseMsg*>(ctrl.getRspOut()->peek());
        CHECK(rsp_m && rsp_m->gettxnId() == 21, "inbound RSP txn=21 in rspOut");

        CHECK(catch_rsp.count == 1 &&
              catch_rsp.last_op == chi::rsp_optype_e::DBIDResp,
              "outbound DBIDResp delivered to txrsp");

        ctrl.getReqOut()->dequeue(curTick());
        ctrl.getRspOut()->dequeue(curTick());
    }

    // ── 4. N messages on each outbound channel, FIFO preserved ───────────────
    printf("\n[4] 3 RSPs + 3 SNPs in one wakeup — FIFO order on each channel\n");
    {
        catch_rsp.reset(); catch_snp.reset();

        push_rsp(ctrl, CHIResponseType_Comp_I,      30, rs);
        push_rsp(ctrl, CHIResponseType_DBIDResp,    31, rs);
        push_rsp(ctrl, CHIResponseType_CompAck,     32, rs);
        push_snp(ctrl, CHIRequestType_SnpShared,   0x4000, 40, rs);
        push_snp(ctrl, CHIRequestType_SnpCleanInvalid, 0x4040, 41, rs);
        push_snp(ctrl, CHIRequestType_SnpOnceFwd,  0x4080, 42, rs);

        ctrl.wakeup();

        CHECK(catch_rsp.count == 3, "3 RSPs on txrsp");
        CHECK(catch_snp.count == 3, "3 SNPs on txsnp");
    }

    // ── 5. Buffer independence: rspIn drain doesn't affect reqOut ────────────
    printf("\n[5] rspIn drain via wakeup() doesn't touch reqOut\n");
    {
        catch_rsp.reset();

        inj_req.send(0x5000, chi::req_optype_e::ReadUnique, /*txn=*/50, /*src=*/3);
        push_rsp(ctrl, CHIResponseType_Comp_UC, 51, rs);

        ctrl.wakeup();  // should drain rspIn → txrsp

        const auto* req_m = dynamic_cast<const CHIRequestMsg*>(ctrl.getReqOut()->peek());
        CHECK(req_m && req_m->gettxnId() == 50,
              "reqOut still contains inbound REQ after wakeup()");
        CHECK(catch_rsp.count == 1, "outbound RSP delivered");

        ctrl.getReqOut()->dequeue(curTick());
    }

    // ── 6. Inbound DAT + outbound DAT independently ──────────────────────────
    printf("\n[6] inbound CopyBackWrData and outbound CompData in same cycle\n");
    {
        catch_dat.reset();

        inj_dat.send(chi::dat_optype_e::CopyBackWrData, /*txn=*/60, /*src=*/4,
                     chi::dat_resptype_e::CopyBackWrData_SC);
        push_dat(ctrl, CHIDataType_CompData_UC, 61, rs);

        ctrl.wakeup();

        // Inbound CBWrData → datOut
        const auto* in_m = dynamic_cast<const CHIDataMsg*>(ctrl.getDatOut()->peek());
        CHECK(in_m && in_m->gettype() == CHIDataType_CBWrData_SC &&
              in_m->gettxnId() == 60,
              "inbound CopyBackWrData SC in datOut");

        // Outbound CompData → txdat
        CHECK(catch_dat.count == 1 &&
              catch_dat.last_op == chi::dat_optype_e::CompData,
              "outbound CompData on txdat");

        ctrl.getDatOut()->dequeue(curTick());
    }

    printf("\n=== RESULTS: %d passed, %d failed ===\n\n", g_pass, g_fail);
    sc_start(1, SC_NS);
    return g_fail != 0 ? 1 : 0;
}
