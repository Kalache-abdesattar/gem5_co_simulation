#pragma once
#define GEM5_DEPRECATED(msg)
#define GEM5_NO_INLINE       __attribute__((noinline))
#define GEM5_LIKELY(x)       __builtin_expect(!!(x), 1)
#define GEM5_UNLIKELY(x)     __builtin_expect(!!(x), 0)