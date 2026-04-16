#pragma once
#include <map>
#include <string>
#include "base/types.hh"
#include "base/statistics.hh"
#include "sim/eventq.hh"
#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/network/MessageBuffer.hh"
#include "mem/ruby/network/Network.hh"
#include "mem/ruby/system/RubySystem.hh"
#include "mem/packet.hh"

namespace gem5 { namespace ruby {

class AbstractController : public Consumer {
public:
    // Fields accessed directly by CHIGenericController
    MachineID   m_machineID;
    Network*    m_net_ptr     = nullptr;
    RubySystem* m_ruby_system = nullptr;
    uint32_t    m_version     = 0;
    int         m_transitions_per_cycle = 32;

    AbstractController(const std::string& name,
                       MachineID id,
                       RubySystem* rs,
                       Network* net)
        : m_machineID(id)
        , m_net_ptr(net)
        , m_ruby_system(rs)
        , downstream_({MachineType_Directory, 0})
        , name_(name)
    {}

    virtual ~AbstractController() = default;

    virtual void init()           {}
    virtual void initNetQueues()  = 0;
    virtual void wakeup() override = 0;

    // Queue accessors — overridden by CHIGenericController
    virtual MessageBuffer* getMandatoryQueue() const { return nullptr; }
    virtual MessageBuffer* getMemReqQueue()    const { return nullptr; }
    virtual MessageBuffer* getMemRespQueue()   const { return nullptr; }

    MachineID   getMachineID() const { return m_machineID; }
    uint32_t    getVersion()   const { return m_version;   }
    std::string name()         const { return name_;       }
    bool        isBlocked(Addr) const { return false; }

    // Called by CacheController when computing outgoing flit TgtID
    MachineID mapAddressToDownstreamMachine(Addr) const {
        return downstream_;
    }
    MachineID mapAddressToDownstreamMachine(Addr, MachineType) const {
        return downstream_;
    }

    // Stats stubs
    virtual void regStats()     {}
    virtual void resetStats()   {}
    virtual void collateStats() {}

    // AccessPermission stub
    virtual AccessPermission getAccessPermission(const Addr&) {
        return AccessPermission_NotPresent;
    }

    // Functional access stubs
    virtual bool functionalReadBuffers(PacketPtr&)  { return false; }
    virtual bool functionalWriteBuffers(PacketPtr&) { return false; }

    // Public so CHI_VIP_Controller can set it after construction
    MachineID downstream_;

protected:
    std::string name_;
};

} } // namespace gem5::ruby
