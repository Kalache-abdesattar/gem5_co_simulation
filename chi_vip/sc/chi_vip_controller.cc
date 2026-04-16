#include "chi_vip_controller.hh"
#include <cstdio>
#include <cstring>
#include <vector>

namespace gem5 { namespace ruby {

using namespace CHI;

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────

CHI_VIP_Controller::CHI_VIP_Controller(sc_core::sc_module_name sc_name,
                                       MachineID id,
                                       RubySystem* rs,
                                       Network* net)
    : sc_core::sc_module(sc_name)
    , CHIGenericController(static_cast<const char*>(sc_name), id, rs, net)
    , rxreq_socket("rxreq")
    , rxdat_socket("rxdat")
    , rxrsp_socket("rxrsp")
    , txrsp_socket("txrsp")
    , txdat_socket("txdat")
    , txsnp_socket("txsnp")
{
    rxreq_socket.register_b_transport(this, &CHI_VIP_Controller::b_transport_rxreq);
    rxdat_socket.register_b_transport(this, &CHI_VIP_Controller::b_transport_rxdat);
    rxrsp_socket.register_b_transport(this, &CHI_VIP_Controller::b_transport_rxrsp);
}

// ─────────────────────────────────────────────────────────────────────────────
// Outbound: gem5 HN-F → Questa RN-F
// ─────────────────────────────────────────────────────────────────────────────

bool CHI_VIP_Controller::recvResponseMsg(const CHIResponseMsg* msg)
{
    if (txrsp_socket.size() == 0) return true;  // not connected, discard
    auto* ext = new chi::chi_ctrl_extension();
    pack_rsp(msg, ext);

    tlm::tlm_generic_payload gp;
    gp.set_extension(ext);
    gp.set_address(msg->getaddr());
    gp.set_command(tlm::TLM_IGNORE_COMMAND);
    gp.set_data_length(0);
    gp.set_data_ptr(nullptr);

    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    txrsp_socket->b_transport(gp, delay);
    return gp.get_response_status() == tlm::TLM_OK_RESPONSE;
}

bool CHI_VIP_Controller::recvDataMsg(const CHIDataMsg* msg)
{
    if (txdat_socket.size() == 0) return true;  // not connected, discard

    const int beat_size = dat_width_bytes_;
    const int num_beats = cacheLineSize / beat_size;
    const uint8_t* all_data = msg->getdataBlk().getData();

    for (int i = 0; i < num_beats; i++) {
        std::vector<uint8_t> beat_buf(beat_size);
        memcpy(beat_buf.data(), all_data + i * beat_size, beat_size);

        auto* ext = new chi::chi_data_extension();
        pack_dat(msg, ext);
        // DataID encodes which cache-line chunk this beat carries:
        //   byte_offset = data_id * (cacheLineSize / 4)
        // Invert: data_id = (i * beat_size) / (cacheLineSize / 4) = i * beat_size / 16
        ext->dat.set_data_id(static_cast<uint8_t>(i * beat_size / 16));

        tlm::tlm_generic_payload gp;
        gp.set_extension(ext);
        gp.set_address(msg->getaddr());
        gp.set_command(tlm::TLM_WRITE_COMMAND);
        gp.set_data_ptr(beat_buf.data());
        gp.set_data_length(static_cast<unsigned>(beat_size));

        sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
        txdat_socket->b_transport(gp, delay);
        if (gp.get_response_status() != tlm::TLM_OK_RESPONSE)
            return false;
    }
    return true;
}

bool CHI_VIP_Controller::recvSnoopMsg(const CHIRequestMsg* msg)
{
    if (txsnp_socket.size() == 0) return true;  // not connected, discard
    auto* ext = new chi::chi_snp_extension();
    pack_snp(msg, ext);

    tlm::tlm_generic_payload gp;
    gp.set_extension(ext);
    gp.set_address(msg->getaddr());
    gp.set_command(tlm::TLM_IGNORE_COMMAND);
    gp.set_data_length(0);
    gp.set_data_ptr(nullptr);

    sc_core::sc_time delay = sc_core::SC_ZERO_TIME;
    txsnp_socket->b_transport(gp, delay);
    return gp.get_response_status() == tlm::TLM_OK_RESPONSE;
}

bool CHI_VIP_Controller::recvRequestMsg(const CHIRequestMsg* /*msg*/)
{
    // HN-F should not emit outgoing REQ in normal operation
    fprintf(stderr, "[CHI_VIP_Controller] WARNING: unexpected recvRequestMsg – consuming\n");
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Inbound: Questa RN-F → gem5 HN-F
// ─────────────────────────────────────────────────────────────────────────────

void CHI_VIP_Controller::b_transport_rxreq(tlm::tlm_generic_payload& gp,
                                            sc_core::sc_time&)
{
    auto* ext = gp.get_extension<chi::chi_ctrl_extension>();
    if (!ext) {
        gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    // Address is carried in the TLM generic payload, not in the extension
    Addr addr = static_cast<Addr>(gp.get_address());
    auto msg = unpack_req(ext, addr, curTick());
    // reqOut: this proxy acts as RN-F — REQ from Questa goes OUT to gem5's Ruby network.
    // Latency=0: b_transport is synchronous, message is available this tick.
    reqOut->enqueue(msg, curTick(), 0, false, false);
    scheduleEvent(1);
    gp.set_response_status(tlm::TLM_OK_RESPONSE);
}

void CHI_VIP_Controller::b_transport_rxdat(tlm::tlm_generic_payload& gp,
                                            sc_core::sc_time&)
{
    auto* ext = gp.get_extension<chi::chi_data_extension>();
    if (!ext) {
        gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }

    // Beat size from the TLM payload; fall back to full cache line if unset.
    int beat_size = static_cast<int>(gp.get_data_length());
    if (beat_size <= 0 || beat_size > cacheLineSize)
        beat_size = cacheLineSize;
    const int num_beats = cacheLineSize / beat_size;

    // DataID encodes which chunk of the cache line this beat carries.
    // byte_offset = data_id * (cacheLineSize / 4)  [= data_id * 16 for 64-byte line]
    const uint8_t  data_id     = ext->dat.get_data_id();
    const int      byte_offset = data_id * (cacheLineSize / 4);
    const uint32_t txn_id      = static_cast<uint32_t>(ext->cmn.get_txn_id());

    const Addr addr = static_cast<Addr>(gp.get_address());

    // Find or create the assembly entry for this transaction.
    auto it = pending_dat_.find(txn_id);
    if (it == pending_dat_.end()) {
        // First beat: create the CHIDataMsg skeleton (no data yet).
        auto msg = unpack_dat(ext, nullptr, addr, curTick());
        pending_dat_[txn_id] = PendingDat{msg, 0, beat_size};
        it = pending_dat_.find(txn_id);
    }
    PendingDat& pd = it->second;

    // Copy this beat's payload into the correct slice of the DataBlock.
    const uint8_t* data = gp.get_data_ptr();
    if (data && beat_size > 0)
        pd.msg->getdataBlk().setData(data, byte_offset, beat_size);

    // Populate WriteMask from TLM byte enables (partial-write support).
    // If no byte-enable array is provided, the entire beat is considered valid.
    const uint8_t* be_ptr = gp.get_byte_enable_ptr();
    const unsigned be_len = gp.get_byte_enable_length();
    if (be_ptr && be_len > 0) {
        for (int i = 0; i < beat_size && static_cast<unsigned>(i) < be_len; i++)
            if (be_ptr[i])
                pd.msg->getbitMask().set(byte_offset + i);
    } else {
        for (int i = 0; i < beat_size; i++)
            pd.msg->getbitMask().set(byte_offset + i);
    }

    gp.set_response_status(tlm::TLM_OK_RESPONSE);
    pd.beats_done++;

    // Enqueue to datOut only when all beats for this transaction have arrived.
    if (pd.beats_done >= num_beats) {
        datOut->enqueue(pd.msg, curTick(), 0, false, false);
        pending_dat_.erase(it);
        scheduleEvent(1);
    }
}

void CHI_VIP_Controller::b_transport_rxrsp(tlm::tlm_generic_payload& gp,
                                            sc_core::sc_time&)
{
    auto* ext = gp.get_extension<chi::chi_ctrl_extension>();
    if (!ext) {
        gp.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    auto msg = unpack_rsp(ext, curTick());
    rspOut->enqueue(msg, curTick(), 0, false, false);
    scheduleEvent(1);
    gp.set_response_status(tlm::TLM_OK_RESPONSE);
}

// ─────────────────────────────────────────────────────────────────────────────
// pack_rsp: CHIResponseMsg → chi_ctrl_extension (RSP channel, HN→RN)
// ─────────────────────────────────────────────────────────────────────────────

void CHI_VIP_Controller::pack_rsp(const CHIResponseMsg* m,
                                   chi::chi_ctrl_extension* e)
{
    e->cmn.set_txn_id(static_cast<unsigned>(m->gettxnId()));
    e->cmn.set_src_id(static_cast<unsigned>(m->getresponder().num));
    e->cmn.set_qos(0);

    using R = chi::rsp_optype_e;
    using Resp = chi::rsp_resptype_e;

    switch (m->gettype()) {
    case CHIResponseType_Comp_I:
        e->resp.set_opcode(R::Comp);
        e->resp.set_resp(Resp::Comp_I);
        break;
    case CHIResponseType_Comp_UC:
        e->resp.set_opcode(R::Comp);
        e->resp.set_resp(Resp::Comp_UC);
        break;
    case CHIResponseType_Comp_SC:
        e->resp.set_opcode(R::Comp);
        e->resp.set_resp(Resp::Comp_SC);
        break;
    case CHIResponseType_Comp_UD_PD:
        e->resp.set_opcode(R::Comp);
        e->resp.set_resp(Resp::Comp_UD_PD);
        break;
    case CHIResponseType_Comp:
        e->resp.set_opcode(R::Comp);
        break;
    case CHIResponseType_CompAck:
        e->resp.set_opcode(R::CompAck);
        break;
    case CHIResponseType_CompDBIDResp:
        e->resp.set_opcode(R::CompDBIDResp);
        break;
    case CHIResponseType_DBIDResp:
        e->resp.set_opcode(R::DBIDResp);
        break;
    case CHIResponseType_ReadReceipt:
        e->resp.set_opcode(R::ReadReceipt);
        break;
    case CHIResponseType_RespSepData:
        e->resp.set_opcode(R::RespSepData);
        break;
    case CHIResponseType_RetryAck:
        e->resp.set_opcode(R::RetryAck);
        break;
    case CHIResponseType_PCrdGrant:
        e->resp.set_opcode(R::PCrdGrant);
        break;
    // SnpResp: opcode + resp state (I vs SC distinguishes the cache state)
    case CHIResponseType_SnpResp_I:
        e->resp.set_opcode(R::SnpResp);
        e->resp.set_resp(Resp::SnpResp_I);
        break;
    case CHIResponseType_SnpResp_SC:
        e->resp.set_opcode(R::SnpResp);
        e->resp.set_resp(Resp::SnpResp_SC);
        break;
    // SnpRespFwded: resp field carries the responder's final cache state
    case CHIResponseType_SnpResp_I_Fwded_UC:
    case CHIResponseType_SnpResp_I_Fwded_UD_PD:
        e->resp.set_opcode(R::SnpRespFwded);
        e->resp.set_resp(Resp::SnpResp_I);
        break;
    case CHIResponseType_SnpResp_SC_Fwded_SC:
    case CHIResponseType_SnpResp_SC_Fwded_SD_PD:
    case CHIResponseType_SnpResp_SC_Fwded_I:
        e->resp.set_opcode(R::SnpRespFwded);
        e->resp.set_resp(Resp::SnpResp_SC);
        break;
    case CHIResponseType_SnpResp_UC_Fwded_I:
    case CHIResponseType_SnpResp_UD_Fwded_I:
        e->resp.set_opcode(R::SnpRespFwded);
        e->resp.set_resp(Resp::SnpResp_UC);  // UC and UD share 0b010
        break;
    case CHIResponseType_SnpResp_SD_Fwded_I:
        e->resp.set_opcode(R::SnpRespFwded);
        e->resp.set_resp(Resp::SnpResp_SD);
        break;
    default:
        e->resp.set_opcode(R::Comp);
        break;
    }

    e->resp.set_db_id(static_cast<uint16_t>(m->getdbid()));
    e->resp.set_tgt_id(static_cast<uint16_t>(m->getDestination().num));
}

// ─────────────────────────────────────────────────────────────────────────────
// pack_dat: CHIDataMsg → chi_data_extension (DAT channel, HN→RN)
// ─────────────────────────────────────────────────────────────────────────────

void CHI_VIP_Controller::pack_dat(const CHIDataMsg* m,
                                   chi::chi_data_extension* e)
{
    e->cmn.set_txn_id(static_cast<unsigned>(m->gettxnId()));
    e->cmn.set_src_id(static_cast<unsigned>(m->getresponder().num));
    e->cmn.set_qos(0);

    using D        = chi::dat_optype_e;
    using dat_Resp = chi::dat_resptype_e;

    switch (m->gettype()) {
    case CHIDataType_CompData_I:
        e->dat.set_opcode(D::CompData);
        e->dat.set_resp(dat_Resp::CompData_I);
        break;
    case CHIDataType_CompData_UC:
        e->dat.set_opcode(D::CompData);
        e->dat.set_resp(dat_Resp::CompData_UC);
        break;
    case CHIDataType_CompData_SC:
        e->dat.set_opcode(D::CompData);
        e->dat.set_resp(dat_Resp::CompData_SC);
        break;
    case CHIDataType_CompData_UD_PD:
        e->dat.set_opcode(D::CompData);
        e->dat.set_resp(dat_Resp::CompData_UD_PD);
        break;
    case CHIDataType_CompData_SD_PD:
        e->dat.set_opcode(D::CompData);
        e->dat.set_resp(dat_Resp::CompData_SD_PD);
        break;
    case CHIDataType_DataSepResp_UC:
        e->dat.set_opcode(D::DataSepResp);
        e->dat.set_resp(dat_Resp::DataSepResp_UC);
        break;
    case CHIDataType_CBWrData_UC:
        e->dat.set_opcode(D::CopyBackWrData);
        e->dat.set_resp(dat_Resp::CopyBackWrData_UC);
        break;
    case CHIDataType_CBWrData_SC:
        e->dat.set_opcode(D::CopyBackWrData);
        e->dat.set_resp(dat_Resp::CopyBackWrData_SC);
        break;
    case CHIDataType_CBWrData_UD_PD:
        e->dat.set_opcode(D::CopyBackWrData);
        e->dat.set_resp(dat_Resp::CopyBackWrData_UD_PD);
        break;
    case CHIDataType_CBWrData_SD_PD:
        e->dat.set_opcode(D::CopyBackWrData);
        e->dat.set_resp(dat_Resp::CopyBackWrData_SD_PD);
        break;
    case CHIDataType_CBWrData_I:
        e->dat.set_opcode(D::CopyBackWrData);
        e->dat.set_resp(dat_Resp::CopyBackWrData_I);
        break;
    case CHIDataType_NCBWrData:
        e->dat.set_opcode(D::NonCopyBackWrData);
        e->dat.set_resp(dat_Resp::NonCopyBackWrData);
        break;
    // SnpRespData: opcode + resp state (cache state of the snooped node)
    case CHIDataType_SnpRespData_I:
        e->dat.set_opcode(D::SnpRespData);
        e->dat.set_resp(dat_Resp::SnpRespData_I);
        break;
    case CHIDataType_SnpRespData_I_PD:
        e->dat.set_opcode(D::SnpRespData);
        e->dat.set_resp(dat_Resp::SnpRespData_I_PD);
        break;
    case CHIDataType_SnpRespData_SC:
        e->dat.set_opcode(D::SnpRespData);
        e->dat.set_resp(dat_Resp::SnpRespData_SC);
        break;
    case CHIDataType_SnpRespData_SC_PD:
        e->dat.set_opcode(D::SnpRespData);
        e->dat.set_resp(dat_Resp::SnpRespData_SC_PD);
        break;
    case CHIDataType_SnpRespData_SD:
        e->dat.set_opcode(D::SnpRespData);
        e->dat.set_resp(dat_Resp::SnpRespData_SD);
        break;
    case CHIDataType_SnpRespData_UC:
    case CHIDataType_SnpRespData_UD:
        e->dat.set_opcode(D::SnpRespData);
        e->dat.set_resp(dat_Resp::SnpRespData_UC);  // UC and UD share 0b010
        break;
    // SnpRespDataFwded: distinct opcode (0x6 vs 0x1);
    // resp field carries the snooped node's final cache state.
    case CHIDataType_SnpRespData_I_Fwded_SD_PD:
    case CHIDataType_SnpRespData_I_Fwded_SC:
        e->dat.set_opcode(D::SnpRespDataFwded);
        e->dat.set_resp(dat_Resp::SnpRespData_I);
        break;
    case CHIDataType_SnpRespData_I_PD_Fwded_SC:
        e->dat.set_opcode(D::SnpRespDataFwded);
        e->dat.set_resp(dat_Resp::SnpRespData_I_PD);
        break;
    case CHIDataType_SnpRespData_SC_Fwded_SC:
    case CHIDataType_SnpRespData_SC_Fwded_SD_PD:
        e->dat.set_opcode(D::SnpRespDataFwded);
        e->dat.set_resp(dat_Resp::SnpRespData_SC);
        break;
    case CHIDataType_SnpRespData_SC_PD_Fwded_SC:
        e->dat.set_opcode(D::SnpRespDataFwded);
        e->dat.set_resp(dat_Resp::SnpRespData_SC_PD);
        break;
    case CHIDataType_SnpRespData_SD_Fwded_SC:
        e->dat.set_opcode(D::SnpRespDataFwded);
        e->dat.set_resp(dat_Resp::SnpRespData_SD);
        break;
    default:
        e->dat.set_opcode(D::CompData);
        break;
    }

    e->dat.set_tgt_id(static_cast<uint16_t>(m->getDestination().num));
    // HomeNID identifies the Home Node that originated this data response.
    // For all CHI-B compliant DAT flits from HN-F, HomeNID == SrcID.
    e->dat.set_home_n_id(static_cast<uint16_t>(m->getresponder().num));
    e->dat.set_db_id(0);
    e->dat.set_resp_err(chi::rsp_resperrtype_e::OK);
    // DataID and data bytes are set per-beat by the caller (recvDataMsg).
}

// ─────────────────────────────────────────────────────────────────────────────
// pack_snp: CHIRequestMsg (snoop) → chi_snp_extension (SNP channel, HN→RN)
// ─────────────────────────────────────────────────────────────────────────────

void CHI_VIP_Controller::pack_snp(const CHIRequestMsg* m,
                                   chi::chi_snp_extension* e)
{
    e->cmn.set_txn_id(static_cast<unsigned>(m->gettxnId()));
    e->cmn.set_src_id(static_cast<unsigned>(m->getrequestor().num));
    e->cmn.set_qos(0);

    e->req.set_opcode(gem5req_to_scc_snp(m->gettype()));
    // FwdNID: for forwarding snoops, carries the forward target's node ID.
    // For non-forwarding snoops, fwdRequestor.num is 0 (harmless).
    e->req.set_fwd_n_id(static_cast<uint16_t>(m->getfwdRequestor().num));
    e->req.set_fwd_txn_id(0);
    e->req.set_ret_to_src(m->getretToSrc());
    e->req.set_do_not_goto_sd(false);
    e->req.set_do_not_data_pull(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// unpack_req: chi_ctrl_extension + gp address → CHIRequestMsg (Questa RN-F → HN-F)
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<CHIRequestMsg>
CHI_VIP_Controller::unpack_req(const chi::chi_ctrl_extension* e,
                                Addr addr, Tick t)
{
    auto msg = std::make_shared<CHIRequestMsg>(t, cacheLineSize, m_ruby_system);
    msg->setaddr(addr);
    msg->settype(scc_to_gem5req(e->req.get_opcode()));
    msg->settxnId(static_cast<Addr>(e->cmn.get_txn_id()));
    msg->setusesTxnId(true);
    // SCC uses is_non_secure() for the NS bit
    msg->setns(e->req.is_non_secure());
    msg->setallowRetry(e->req.is_allow_retry());
    // RetToSrc is a SNP-channel field (HN→RN); not present in REQ flits.

    MachineID src{MachineType_Cache, static_cast<NodeID>(e->cmn.get_src_id())};
    msg->setrequestor(src);

    MachineID tgt{MachineType_Directory, static_cast<NodeID>(e->req.get_tgt_id())};
    msg->getDestination().add(tgt);

    return msg;
}

// ─────────────────────────────────────────────────────────────────────────────
// unpack_dat: chi_data_extension → CHIDataMsg (Questa RN-F write data → HN-F)
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<CHIDataMsg>
CHI_VIP_Controller::unpack_dat(const chi::chi_data_extension* e,
                                const uint8_t* data, Addr addr, Tick t)
{
    auto msg = std::make_shared<CHIDataMsg>(t, cacheLineSize, m_ruby_system);
    msg->setaddr(addr);
    msg->settxnId(static_cast<Addr>(e->cmn.get_txn_id()));
    msg->setusesTxnId(true);

    using D    = chi::dat_optype_e;
    using Resp = chi::dat_resptype_e;
    switch (e->dat.get_opcode()) {
    case D::CopyBackWrData: {
        // Dispatch on resp state to distinguish CBWrData_UC/SC/UD_PD/SD_PD/I.
        // Use underlying integer to avoid switch collisions between enum aliases.
        auto rv = static_cast<uint8_t>(e->dat.get_resp());
        if      (rv == static_cast<uint8_t>(Resp::CopyBackWrData_SC))
            msg->settype(CHIDataType_CBWrData_SC);
        else if (rv == static_cast<uint8_t>(Resp::CopyBackWrData_UD_PD))
            msg->settype(CHIDataType_CBWrData_UD_PD);
        else if (rv == static_cast<uint8_t>(Resp::CopyBackWrData_SD_PD))
            msg->settype(CHIDataType_CBWrData_SD_PD);
        else if (rv == static_cast<uint8_t>(Resp::CopyBackWrData_I))
            msg->settype(CHIDataType_CBWrData_I);
        else
            msg->settype(CHIDataType_CBWrData_UC);   // CopyBackWrData_UC = 0b010
        break;
    }
    case D::NonCopyBackWrData:    msg->settype(CHIDataType_NCBWrData); break;
    // NCBWrDataCompAck (0xC) combines write-data + implicit CompAck.
    // gem5 has no separate CHIDataType for this; NCBWrData is the closest.
    case D::NCBWrDataCompAck:     msg->settype(CHIDataType_NCBWrData); break;
    case D::SnpRespDataFwded: {
        // Forwarded snoop data: dispatch on resp (snooped node's final state).
        // The forwarded target's state is encoded in fwd_state but gem5's
        // CHIDataType names embed only one state; pick _SC as the forwarded
        // target's state for I/I_PD/SD (no better alternative without
        // extending CHIDataMsg), and _SC for the SC/SC_PD variants.
        auto rv = static_cast<uint8_t>(e->dat.get_resp());
        if      (rv == static_cast<uint8_t>(Resp::SnpRespData_SC))
            msg->settype(CHIDataType_SnpRespData_SC_Fwded_SC);
        else if (rv == static_cast<uint8_t>(Resp::SnpRespData_SC_PD))
            msg->settype(CHIDataType_SnpRespData_SC_PD_Fwded_SC);
        else if (rv == static_cast<uint8_t>(Resp::SnpRespData_SD))
            msg->settype(CHIDataType_SnpRespData_SD_Fwded_SC);
        else if (rv == static_cast<uint8_t>(Resp::SnpRespData_I_PD))
            msg->settype(CHIDataType_SnpRespData_I_PD_Fwded_SC);
        else  // SnpRespData_I = 0b000 (default) and UC share 0b010 → I_Fwded_SC
            msg->settype(CHIDataType_SnpRespData_I_Fwded_SC);
        break;
    }
    case D::SnpRespData: {
        // Dispatch on resp field to determine the snooped cache's final state.
        // Same aliasing issue as CopyBackWrData: use uint8_t cast.
        auto rv = static_cast<uint8_t>(e->dat.get_resp());
        if      (rv == static_cast<uint8_t>(Resp::SnpRespData_SC))
            msg->settype(CHIDataType_SnpRespData_SC);
        else if (rv == static_cast<uint8_t>(Resp::SnpRespData_UC))
            msg->settype(CHIDataType_SnpRespData_UC);   // also covers UD (same bit)
        else if (rv == static_cast<uint8_t>(Resp::SnpRespData_SD))
            msg->settype(CHIDataType_SnpRespData_SD);
        else if (rv == static_cast<uint8_t>(Resp::SnpRespData_I_PD))
            msg->settype(CHIDataType_SnpRespData_I_PD);
        else if (rv == static_cast<uint8_t>(Resp::SnpRespData_SC_PD))
            msg->settype(CHIDataType_SnpRespData_SC_PD);
        else
            msg->settype(CHIDataType_SnpRespData_I);
        break;
    }
    default: msg->settype(CHIDataType_NCBWrData); break;
    }

    MachineID src{MachineType_Cache, static_cast<NodeID>(e->cmn.get_src_id())};
    msg->setresponder(src);
    MachineID tgt{MachineType_Directory, static_cast<NodeID>(e->dat.get_tgt_id())};
    msg->getDestination().add(tgt);

    if (data)
        msg->getdataBlk().setData(data, 0, cacheLineSize);

    return msg;
}

// ─────────────────────────────────────────────────────────────────────────────
// unpack_rsp: chi_ctrl_extension → CHIResponseMsg (CompAck/SnpResp from RN)
// ─────────────────────────────────────────────────────────────────────────────

std::shared_ptr<CHIResponseMsg>
CHI_VIP_Controller::unpack_rsp(const chi::chi_ctrl_extension* e, Tick t)
{
    auto msg = std::make_shared<CHIResponseMsg>(t, cacheLineSize, m_ruby_system);
    msg->settxnId(static_cast<Addr>(e->cmn.get_txn_id()));
    msg->setusesTxnId(true);

    using R    = chi::rsp_optype_e;
    using Resp = chi::rsp_resptype_e;
    switch (e->resp.get_opcode()) {
    case R::CompAck:
        msg->settype(CHIResponseType_CompAck);
        break;
    case R::SnpResp:
        // Resp field distinguishes I (0b000) from SC (0b001); UC/SD not
        // represented in gem5's inbound CHIResponseType, so they fall to I.
        if (static_cast<uint8_t>(e->resp.get_resp()) ==
                static_cast<uint8_t>(Resp::SnpResp_SC))
            msg->settype(CHIResponseType_SnpResp_SC);
        else
            msg->settype(CHIResponseType_SnpResp_I);
        break;
    case R::SnpRespFwded: {
        // Resp field carries the snooped node's final cache state.
        // The forwarded target's state is not separately encoded in the
        // basic SCC extension — pick the most representative gem5 type
        // for each snooped-node state.
        auto rv = static_cast<uint8_t>(e->resp.get_resp());
        if (rv == static_cast<uint8_t>(Resp::SnpResp_SC))
            msg->settype(CHIResponseType_SnpResp_SC_Fwded_SC);
        else if (rv == static_cast<uint8_t>(Resp::SnpResp_UC))
            msg->settype(CHIResponseType_SnpResp_UC_Fwded_I);
        else if (rv == static_cast<uint8_t>(Resp::SnpResp_SD))
            msg->settype(CHIResponseType_SnpResp_SD_Fwded_I);
        else  // SnpResp_I = 0b000 (default)
            msg->settype(CHIResponseType_SnpResp_I_Fwded_UC);
        break;
    }
    default:
        msg->settype(CHIResponseType_Comp);
        break;
    }

    MachineID src{MachineType_Cache, static_cast<NodeID>(e->cmn.get_src_id())};
    msg->setresponder(src);
    MachineID tgt{MachineType_Directory, static_cast<NodeID>(e->resp.get_tgt_id())};
    msg->getDestination().add(tgt);

    return msg;
}

// ─────────────────────────────────────────────────────────────────────────────
// Opcode conversion tables
// ─────────────────────────────────────────────────────────────────────────────

chi::req_optype_e CHI_VIP_Controller::gem5req_to_scc(CHI::CHIRequestType t)
{
    using O = chi::req_optype_e;
    switch (t) {
    case CHIRequestType_ReadShared:         return O::ReadShared;
    case CHIRequestType_ReadNotSharedDirty: return O::ReadNotSharedDirty;
    case CHIRequestType_ReadUnique:         return O::ReadUnique;
    case CHIRequestType_ReadOnce:           return O::ReadOnce;
    case CHIRequestType_CleanUnique:        return O::CleanUnique;
    case CHIRequestType_MakeReadUnique:     return O::MakeReadUnique;
    case CHIRequestType_Evict:              return O::Evict;
    case CHIRequestType_WriteBackFull:      return O::WriteBackFull;
    case CHIRequestType_WriteCleanFull:     return O::WriteCleanFull;
    case CHIRequestType_WriteEvictFull:     return O::WriteEvictFull;
    case CHIRequestType_WriteUniquePtl:     return O::WriteUniquePtl;
    case CHIRequestType_WriteUniqueFull:    return O::WriteUniqueFull;
    case CHIRequestType_WriteUniqueZero:    return O::WriteUniqueZero;
    case CHIRequestType_WriteNoSnpPtl:      return O::WriteNoSnpPtl;
    case CHIRequestType_WriteNoSnp:         return O::WriteNoSnpFull;
    case CHIRequestType_ReadNoSnp:          return O::ReadNoSnp;
    case CHIRequestType_StashOnceShared:    return O::StashOnceShared;
    case CHIRequestType_StashOnceUnique:    return O::StashOnceUnique;
    case CHIRequestType_DvmOpNonSync:       return O::DVMOp;
    case CHIRequestType_DvmOpSync:          return O::DVMOp;
    case CHIRequestType_AtomicLoad:         return O::AtomicLoadAdd;
    case CHIRequestType_AtomicStore:        return O::AtomicStoreAdd;
    case CHIRequestType_AtomicReturn:       return O::AtomicSwap;
    case CHIRequestType_AtomicNoReturn:     return O::AtomicCompare;
    default:                                return O::ReadOnce;
    }
}

CHI::CHIRequestType CHI_VIP_Controller::scc_to_gem5req(chi::req_optype_e op)
{
    using O = chi::req_optype_e;
    switch (op) {
    case O::ReadShared:         return CHIRequestType_ReadShared;
    case O::ReadNotSharedDirty: return CHIRequestType_ReadNotSharedDirty;
    case O::ReadUnique:         return CHIRequestType_ReadUnique;
    case O::ReadOnce:           return CHIRequestType_ReadOnce;
    case O::CleanUnique:        return CHIRequestType_CleanUnique;
    case O::MakeReadUnique:     return CHIRequestType_MakeReadUnique;
    case O::Evict:              return CHIRequestType_Evict;
    case O::WriteBackFull:      return CHIRequestType_WriteBackFull;
    case O::WriteCleanFull:     return CHIRequestType_WriteCleanFull;
    case O::WriteEvictFull:     return CHIRequestType_WriteEvictFull;
    case O::WriteUniquePtl:     return CHIRequestType_WriteUniquePtl;
    case O::WriteUniqueFull:    return CHIRequestType_WriteUniqueFull;
    case O::WriteUniqueZero:    return CHIRequestType_WriteUniqueZero;
    case O::WriteNoSnpPtl:      return CHIRequestType_WriteNoSnpPtl;
    case O::WriteNoSnpFull:     return CHIRequestType_WriteNoSnp;
    case O::ReadNoSnp:          return CHIRequestType_ReadNoSnp;
    case O::ReadNoSnpSep:       return CHIRequestType_ReadNoSnpSep;
    case O::StashOnceShared:    return CHIRequestType_StashOnceShared;
    case O::StashOnceUnique:    return CHIRequestType_StashOnceUnique;
    case O::DVMOp:              return CHIRequestType_DvmOpNonSync;
    case O::AtomicLoadAdd:
    case O::AtomicLoadClr:
    case O::AtomicLoadEor:
    case O::AtomicLoadSet:
    case O::AtomicLoadSmax:
    case O::AtomicLoadSmin:
    case O::AtomicLoadUmax:
    case O::AtomicLoadUmin:     return CHIRequestType_AtomicLoad;
    case O::AtomicStoreAdd:
    case O::AtomicStoreClr:
    case O::AtomicStoreEor:
    case O::AtomicStoreSet:
    case O::AtomicStoreSmax:
    case O::AtomicStoreSmin:
    case O::AtomicStoreUmax:
    case O::AtomicStoreUmin:    return CHIRequestType_AtomicStore;
    case O::AtomicSwap:         return CHIRequestType_AtomicReturn;
    case O::AtomicCompare:      return CHIRequestType_AtomicNoReturn;
    default:                    return CHIRequestType_ReadOnce;
    }
}

chi::snp_optype_e CHI_VIP_Controller::gem5req_to_scc_snp(CHI::CHIRequestType t)
{
    using S = chi::snp_optype_e;
    switch (t) {
    case CHIRequestType_SnpSharedFwd:         return S::SnpSharedFwd;
    case CHIRequestType_SnpNotSharedDirtyFwd: return S::SnpNotSharedDirtyFwd;
    case CHIRequestType_SnpUniqueFwd:         return S::SnpUniqueFwd;
    case CHIRequestType_SnpOnceFwd:           return S::SnpOnceFwd;
    case CHIRequestType_SnpOnce:              return S::SnpOnce;
    case CHIRequestType_SnpShared:            return S::SnpShared;
    case CHIRequestType_SnpUnique:            return S::SnpUnique;
    case CHIRequestType_SnpCleanInvalid:      return S::SnpCleanInvalid;
    case CHIRequestType_SnpDvmOpSync_P1:
    case CHIRequestType_SnpDvmOpSync_P2:
    case CHIRequestType_SnpDvmOpNonSync_P1:
    case CHIRequestType_SnpDvmOpNonSync_P2:   return S::SnpDVMOp;
    default:                                  return S::SnpOnce;
    }
}

} } // namespace gem5::ruby
