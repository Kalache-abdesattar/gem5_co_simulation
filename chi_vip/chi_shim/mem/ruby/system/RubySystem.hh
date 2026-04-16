#pragma once
#include <cstdint>
#include <map>
#include "mem/ruby/common/MachineID.hh"

namespace gem5 { namespace ruby {

class AbstractController;

class RubySystem {
public:
    uint32_t m_cache_line_size = 64;
    std::map<int, int> m_num_controllers;

    uint32_t getCacheLineBits() const {
        uint32_t b = 0, s = m_cache_line_size;
        while (s > 1) { s >>= 1; b++; }
        return b;
    }
    uint32_t getCacheLineSize()  const { return m_cache_line_size; }
    int      getBlockSizeBytes() const { return static_cast<int>(m_cache_line_size); }

    bool getRandomization() const { return false; }
    bool getWarmupEnabled() const { return false; }

    static uint32_t MachineType_base_number(MachineType) { return 0; }

    // No-op: shim doesn't track controllers
    void registerAbstractController(...) {}

    void init() {}
};

} } // namespace gem5::ruby

// Global-scope alias so shim code that uses bare "RubySystem" still works
using RubySystem = gem5::ruby::RubySystem;
