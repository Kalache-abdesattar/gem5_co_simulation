#pragma once

#include <chi/chi_tlm.h>
#include <systemc>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>
#include <memory>
#include <unordered_map>

#include "CHIGenericController.hh"
#include "CHIDataMsg.hh"
#include "CHIRequestMsg.hh"
#include "CHIResponseMsg.hh"

namespace gem5 { namespace ruby {

// CHI_VIP_Controller is a plain C++ class (not sc_module) that owns the
// gem5 Ruby message-buffer logic. It also inherits sc_module through the
// SC_HAS_PROCESS / sc_module path so that TLM sockets are legal.
//
// We achieve this by making it also a sc_module via multiple inheritance.
// The sc_module name is set in the constructor.

class CHI_VIP_Controller : public sc_core::sc_module,
                           public CHIGenericController
{
public:
    SC_HAS_PROCESS(CHI_VIP_Controller);

    // ── Target sockets: receive from Questa RN-F ──────────────────────────
    // _optional variant so smoke test passes without full binding
    tlm_utils::simple_target_socket_optional<CHI_VIP_Controller> rxreq_socket;
    tlm_utils::simple_target_socket_optional<CHI_VIP_Controller> rxdat_socket;
    tlm_utils::simple_target_socket_optional<CHI_VIP_Controller> rxrsp_socket;

    // ── Initiator sockets: send to Questa RN-F ────────────────────────────
    // _optional variant uses SC_ZERO_OR_MORE_BOUND so smoke test works unbound
    tlm_utils::simple_initiator_socket_optional<CHI_VIP_Controller> txrsp_socket;
    tlm_utils::simple_initiator_socket_optional<CHI_VIP_Controller> txdat_socket;
    tlm_utils::simple_initiator_socket_optional<CHI_VIP_Controller> txsnp_socket;

    // sc_module_name parameter keeps the name alive until after all
    // data member (socket) constructors run, avoiding E122.
    CHI_VIP_Controller(sc_core::sc_module_name sc_name,
                       MachineID id,
                       RubySystem* rs,
                       Network* net);

    // Data bus width in bytes (16, 32, or 64).  Controls how many TLM2
    // b_transport calls are issued per outbound DAT message, and how many
    // inbound beats are assembled before a CHIDataMsg is enqueued.
    // Default: 64 (single-beat, 512-bit bus — backward compatible).
    int dat_width_bytes_ = 64;

    // Clear any in-flight multi-beat DAT state.  Call from the reset path
    // so that TxnIDs can be safely reused after a link reset.
    void reset_pending_dat() { pending_dat_.clear(); }
    // Returns true when no beat reassembly is in progress (useful for tests).
    bool pending_dat_empty() const { return pending_dat_.empty(); }

    // ── Opcode conversion helpers ─────────────────────────────────────────
    static chi::req_optype_e gem5req_to_scc(CHI::CHIRequestType t);
    static CHI::CHIRequestType scc_to_gem5req(chi::req_optype_e op);
    static chi::snp_optype_e gem5req_to_scc_snp(CHI::CHIRequestType t);

protected:
    // Called by CHIGenericController::wakeup() when a gem5 message is ready
    bool recvRequestMsg (const CHIRequestMsg*  msg) override;  // HN-F shouldn't generate REQ
    bool recvSnoopMsg   (const CHIRequestMsg*  msg) override;  // → txsnp_socket
    bool recvResponseMsg(const CHIResponseMsg* msg) override;  // → txrsp_socket
    bool recvDataMsg    (const CHIDataMsg*     msg) override;  // → txdat_socket

private:
    // TLM target callbacks — inbound from Questa RN-F
    void b_transport_rxreq(tlm::tlm_generic_payload&, sc_core::sc_time&);
    void b_transport_rxdat(tlm::tlm_generic_payload&, sc_core::sc_time&);
    void b_transport_rxrsp(tlm::tlm_generic_payload&, sc_core::sc_time&);

    // Pack gem5 message → SCC extension (outbound to Questa)
    void pack_rsp(const CHIResponseMsg*, chi::chi_ctrl_extension*);
    // pack_dat fills extension fields only; data is copied per-beat by recvDataMsg.
    void pack_dat(const CHIDataMsg*,     chi::chi_data_extension*);
    void pack_snp(const CHIRequestMsg*,  chi::chi_snp_extension*);

    // Unpack SCC extension → gem5 message (inbound from Questa).
    // unpack_dat creates a CHIDataMsg skeleton; data bytes are assembled
    // beat-by-beat in b_transport_rxdat.
    std::shared_ptr<CHIRequestMsg>  unpack_req(const chi::chi_ctrl_extension*,
                                               Addr addr, Tick);
    std::shared_ptr<CHIDataMsg>     unpack_dat(const chi::chi_data_extension*,
                                               const uint8_t* data, Addr addr, Tick);
    std::shared_ptr<CHIResponseMsg> unpack_rsp(const chi::chi_ctrl_extension*, Tick);

    // In-progress multi-beat DAT reassembly (inbound Questa → gem5).
    // Key: TxnID.  Removed from map when the final beat arrives.
    struct PendingDat {
        std::shared_ptr<CHIDataMsg> msg;
        int beats_done;
        int beat_size;   // bytes per beat for this transaction
    };
    std::unordered_map<uint32_t, PendingDat> pending_dat_;
};

} } // namespace gem5::ruby
