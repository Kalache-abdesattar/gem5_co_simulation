#pragma once
#include <cstdint>

using Tick    = uint64_t;
using Addr    = uint64_t;
using Counter = uint64_t;
using Cycles  = uint64_t;
using RegVal  = uint64_t;

static constexpr Tick MaxTick = UINT64_MAX;