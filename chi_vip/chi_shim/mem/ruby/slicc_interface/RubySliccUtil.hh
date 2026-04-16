#pragma once
#include "base/types.hh"

inline Addr makeLineAddress(Addr addr, int bits) {
    return (addr >> bits) << bits;
}
inline Addr getOffset(Addr addr, int bits) {
    return addr & ((1 << bits) - 1);
}
inline int floorLog2(unsigned int n) {
    int i = 0;
    while (n > 1) { n >>= 1; i++; }
    return i;
}