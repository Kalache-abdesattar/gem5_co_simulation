#pragma once
#include "mem/ruby/common/MachineID.hh"

namespace gem5 { namespace ruby {

class RubySystem;

class NetDest {
public:
    NetDest() = default;

    void setRubySystem(RubySystem*)       {}
    void setRubySystem(const RubySystem*) {}
    void add(MachineID id)                { num = id.num; }
    void clear()                          {}
    bool isElement(MachineID) const       { return false; }

    // Expose a .num member so pack functions can read it
    NodeID num = 0;
};

} } // namespace gem5::ruby
