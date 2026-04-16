#pragma once
#include "../base/types.hh"
#include <functional>
#include <queue>
#include <vector>
#include <systemc>

// No-op debug print macro (replaces gem5's DPRINTF)
#define DPRINTF(flag, ...) do {} while(0)

inline Tick curTick() {
    return static_cast<Tick>(sc_core::sc_time_stamp().value());
}

// Consumer mixin — CHIGenericController inherits this.
// scheduleEvent() is called by the state machine to request a future wakeup.
class Consumer {
public:
    virtual void wakeup() = 0;
    virtual ~Consumer() = default;

    void scheduleEvent(Tick n = 1) {
        Tick when = curTick() + n * TICK_PER_CYCLE;
        globalEventQueue().schedule(when, [this]{ wakeup(); });
    }

    void scheduleEventAbsolute(Tick when) {
        globalEventQueue().schedule(when, [this]{ wakeup(); });
    }

    static constexpr Tick TICK_PER_CYCLE = 1000; // 1 ns at 1 ps resolution

    struct EventQueue {
        struct Entry {
            Tick when;
            std::function<void()> fn;
            bool operator>(const Entry& o) const { return when > o.when; }
        };
        std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> pq;

        void schedule(Tick when, std::function<void()> fn) {
            pq.push({when, std::move(fn)});
        }
        void drain(Tick now) {
            while (!pq.empty() && pq.top().when <= now) {
                auto f = pq.top().fn;
                pq.pop();
                f();
            }
        }
        bool empty()    const { return pq.empty(); }
        Tick nextTick() const { return pq.top().when; }
    };

    static EventQueue& globalEventQueue() {
        static EventQueue eq;
        return eq;
    }
};

// Free function returning cycles converted to ticks
inline Tick cyclesToTicks(Tick c) { return c * Consumer::TICK_PER_CYCLE; }

// Free function used by SLICC-generated code
inline Consumer::EventQueue* curEventQueue() {
    return &Consumer::globalEventQueue();
}
