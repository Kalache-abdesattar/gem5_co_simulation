#pragma once
#include "base/types.hh"

// Minimal stub — AbstractController references PacketPtr in
// functionalRead/Write methods that we stub as no-ops.
struct Packet {
    Addr getAddr() const { return 0; }
};
using PacketPtr = Packet*;