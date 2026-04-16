#pragma once
#include <string>
#include <unordered_map>
#include "mem/ruby/common/MachineID.hh"
#include "mem/ruby/network/MessageBuffer.hh"

class Network {
    std::unordered_map<std::string, MessageBuffer*> from_;
    std::unordered_map<std::string, MessageBuffer*> to_;

    static std::string key(int node, int channel) {
        return std::to_string(node) + ":" + std::to_string(channel);
    }

public:
    // Signatures match what CHIGenericController.cc calls:
    //   setToNetQueue(node, ordered, channel, vnet_type, buffer)
    void setToNetQueue(int node, bool /*ordered*/, int channel,
                       const char* /*vnet_type*/, MessageBuffer* b) {
        to_[key(node, channel)] = b;
    }
    void setFromNetQueue(int node, bool /*ordered*/, int channel,
                         const char* /*vnet_type*/, MessageBuffer* b) {
        from_[key(node, channel)] = b;
    }

    MessageBuffer* getFromNetQueue(int node, int channel) {
        auto it = from_.find(key(node, channel));
        return it != from_.end() ? it->second : nullptr;
    }
    MessageBuffer* getToNetQueue(int node, int channel) {
        auto it = to_.find(key(node, channel));
        return it != to_.end() ? it->second : nullptr;
    }

    int  getNumNodes() const { return 2; }
    void init()              {}
};
