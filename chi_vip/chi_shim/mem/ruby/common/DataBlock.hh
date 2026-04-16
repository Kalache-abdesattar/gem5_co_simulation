#pragma once
#include <cstdint>
#include <cstring>

static constexpr int CACHE_LINE_BYTES = 64;

class DataBlock {
    uint8_t data_[CACHE_LINE_BYTES] = {};
public:
    DataBlock() = default;
    explicit DataBlock(int /*blockSize*/) {}  // blockSize ignored; fixed 64 bytes

    void setBlockSize(int) {}

    uint8_t*       getData()                     { return data_; }
    const uint8_t* getData()               const { return data_; }
    uint8_t        getByte(int i)          const { return data_[i]; }
    void           setByte(int i, uint8_t v)     { data_[i] = v; }
    void           setData(const uint8_t* s,
                           int offset, int len)  { memcpy(data_+offset, s, len); }
    void           clear()                       { memset(data_, 0, sizeof(data_)); }
    bool operator==(const DataBlock& o)    const {
        return memcmp(data_, o.data_, sizeof(data_)) == 0;
    }
};
