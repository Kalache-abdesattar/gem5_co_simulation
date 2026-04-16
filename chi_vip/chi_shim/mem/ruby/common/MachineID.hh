#pragma once
#include <cstdint>
#include <string>
#include "base/types.hh"

using NodeID = uint32_t;

enum MachineType : uint8_t {
    MachineType_Cache     = 0,
    MachineType_Directory = 1,
    MachineType_Memory    = 2,
    MachineType_DMA       = 3,
    MachineType_NUM
};

inline std::string MachineType_to_string(MachineType t) {
    switch (t) {
        case MachineType_Cache:     return "Cache";
        case MachineType_Directory: return "Directory";
        case MachineType_Memory:    return "Memory";
        default:                    return "Unknown";
    }
}

struct MachineID {
    MachineType type = MachineType_Cache;
    uint32_t    num  = 0;

    bool operator==(const MachineID& o) const {
        return type == o.type && num == o.num;
    }
    bool operator!=(const MachineID& o) const { return !(*this == o); }
    bool operator< (const MachineID& o) const {
        return type < o.type || (type == o.type && num < o.num);
    }
    std::string to_string() const {
        return MachineType_to_string(type) + ":" + std::to_string(num);
    }
};

inline uint32_t MachineType_base_number(MachineType) { return 0; }

inline Addr makeLineAddress(Addr addr, uint32_t bits) {
    return (addr >> bits) << bits;
}