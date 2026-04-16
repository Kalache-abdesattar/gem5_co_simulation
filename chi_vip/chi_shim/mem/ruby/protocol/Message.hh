#pragma once
#include <memory>
#include "base/types.hh"

namespace gem5 { namespace ruby {

class RubySystem;

// All CHI message types inherit from this base.
// Constructor signature matches the SLICC-generated headers:
//   Message(Tick curTime, int blockSize, RubySystem* rs)
class Message {
public:
    Tick m_time          = 0;
    int  block_size_bits = 0;

    Message() = default;

    Message(Tick curTime, int blockSize, RubySystem* /*rs*/)
        : m_time(curTime)
    {
        int b = blockSize;
        block_size_bits = 0;
        while (b > 1) { b >>= 1; block_size_bits++; }
    }

    // const RubySystem* overload for the full-arg constructors
    Message(Tick curTime, int blockSize, const RubySystem* rs)
        : Message(curTime, blockSize, const_cast<RubySystem*>(rs)) {}

    virtual ~Message() = default;

    Tick getTime() const { return m_time; }

    void setRubySystem(RubySystem*) {}
    void setRubySystem(const RubySystem*) {}

    virtual std::shared_ptr<Message> clone() const = 0;
};

using MsgPtr     = std::shared_ptr<Message>;
using RequestPtr = void*;

} } // namespace gem5::ruby
