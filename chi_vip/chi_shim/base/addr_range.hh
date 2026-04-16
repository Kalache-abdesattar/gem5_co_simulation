#pragma once
#include "base/types.hh"
#include <vector>

struct AddrRange {
    Addr start_ = 0;
    Addr end_   = UINT64_MAX;

    AddrRange() = default;
    AddrRange(Addr s, Addr e) : start_(s), end_(e) {}

    bool contains(Addr a) const { return a >= start_ && a <= end_; }
    Addr start()          const { return start_; }
    Addr end()            const { return end_; }
};

using AddrRangeList = std::vector<AddrRange>;