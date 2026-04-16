#pragma once
#include <cstdio>
#include <cstdlib>

#define panic(fmt, ...) \
    do { fprintf(stderr, "[FATAL] " fmt "\n", ##__VA_ARGS__); abort(); } while(0)
#define fatal(fmt, ...) \
    do { fprintf(stderr, "[FATAL] " fmt "\n", ##__VA_ARGS__); abort(); } while(0)
#define warn(fmt, ...)  \
    do { fprintf(stderr, "[WARN]  " fmt "\n", ##__VA_ARGS__); } while(0)
#define inform(fmt, ...) \
    do { fprintf(stderr, "[INFO]  " fmt "\n", ##__VA_ARGS__); } while(0)
#define DPRINTF(flag, fmt, ...)  do {} while(0)
#define DPRINTFR(flag, fmt, ...) do {} while(0)