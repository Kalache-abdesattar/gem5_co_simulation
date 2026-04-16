#pragma once
#include <cstdint>
#include <cstring>
#include "DataBlock.hh"

class WriteMask {
    uint8_t m_[CACHE_LINE_BYTES] = {};
public:
    WriteMask() = default;
    explicit WriteMask(int /*blockSize*/) {}  // blockSize ignored; fixed 64 bytes

    void setBlockSize(int) {}
    bool isSet(int i)  const { return m_[i] != 0; }
    void set(int i)          { m_[i] = 1; }
    void clear()             { memset(m_, 0, sizeof(m_)); }
    bool isFull()      const {
        for (int i = 0; i < CACHE_LINE_BYTES; i++)
            if (!m_[i]) return false;
        return true;
    }
};
