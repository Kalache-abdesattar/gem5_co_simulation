#pragma once
#include <string>
#include <functional>

namespace gem5 { namespace statistics {

struct Group {
    Group(Group* = nullptr, const char* = nullptr) {}
    virtual ~Group() = default;
    virtual void regStats()   {}
    virtual void resetStats() {}
};

struct Scalar {
    Scalar(Group* = nullptr,
           const char* = nullptr,
           const char* = nullptr) {}
    void operator++(int)          {}
    void operator--(int)          {}
    Scalar& operator+=(double)    { return *this; }
    Scalar& operator=(double)     { return *this; }
    double value()          const { return 0.0; }
    void reset()                  {}
    void flags(int)               {}
    void precision(int)           {}
    void prereq(const Scalar&)    {}
    std::string name()      const { return ""; }
};

struct Average : Scalar {
    Average(Group* g = nullptr,
            const char* n = nullptr,
            const char* d = nullptr) : Scalar(g, n, d) {}
};

struct Distribution {
    Distribution(Group* = nullptr,
                 const char* = nullptr,
                 const char* = nullptr) {}
    void sample(double) {}
    void reset()        {}
    void init(double, double, double) {}
    void flags(int)     {}
};

struct Formula {
    Formula(Group* = nullptr,
            const char* = nullptr,
            const char* = nullptr) {}
    Formula& operator=(const Scalar&)  { return *this; }
    Formula& operator+=(const Scalar&) { return *this; }
    Formula& operator/(const Scalar&)  { return *this; }
};

inline void registerDumpCallback(std::function<void()>)  {}
inline void registerResetCallback(std::function<void()>) {}

static constexpr int pdf    = 0;
static constexpr int cdf    = 0;
static constexpr int nozero = 0;
static constexpr int nonan  = 0;

}} // namespace gem5::statistics

#define ADD_STAT(member, ...) do {} while(0)