#pragma once
#include <deque>
#include <memory>
#include "base/types.hh"
#include "sim/eventq.hh"

// Message is now in namespace gem5::ruby (defined in mem/ruby/protocol/Message.hh).
// We only need a forward declaration here to avoid circular includes.
namespace gem5 { namespace ruby { class Message; } }

class MessageBuffer {
public:
    Consumer* m_consumer = nullptr;

    void setConsumer(Consumer* c) { m_consumer = c; }

    // up to 5 args; last 2 are bool defaults (randomization, warmup)
    void enqueue(std::shared_ptr<gem5::ruby::Message> m, Tick now, Tick latency,
                 bool = false, bool = false) {
        q_.push_back({now + latency, m});
        if (m_consumer) m_consumer->scheduleEvent(1);
    }

    bool isReady(Tick now) const {
        return !q_.empty() && q_.front().ready_at <= now;
    }

    // peek() with no args — returns raw pointer to front message (or nullptr)
    const gem5::ruby::Message* peek() const {
        return q_.empty() ? nullptr : q_.front().msg.get();
    }

    void dequeue(Tick now, bool = false) {
        if (isReady(now)) q_.pop_front();
    }

    // Unbounded queue — always has space
    bool areNSlotsAvailable(int, Tick) const { return true; }

    bool   isEmpty()   const { return q_.empty(); }
    size_t size()      const { return q_.size(); }
    void   clear()           { q_.clear(); }
    void   setOrdered(bool)  {}
    bool   getOrdered() const { return false; }

    // Functional access stubs
    unsigned int functionalRead(void*)  { return 0; }
    unsigned int functionalWrite(void*) { return 0; }

private:
    struct Entry {
        Tick ready_at;
        std::shared_ptr<gem5::ruby::Message> msg;
    };
    std::deque<Entry> q_;
};
