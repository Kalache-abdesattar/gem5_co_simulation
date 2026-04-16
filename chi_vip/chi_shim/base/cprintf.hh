#pragma once
#include <string>
#include <cstdio>
#include <cstdarg>

inline std::string csprintf(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return std::string(buf);
}