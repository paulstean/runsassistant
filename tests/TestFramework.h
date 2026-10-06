#pragma once

// Minimal dependency-free test framework for core conformance tests.
// Usage: TEST_CASE(name) { ... } with CHECK(cond) / CHECK_EQ(a, b).
// main() is provided by tests/TestMain.cpp.

#include <cstdio>
#include <cstring>
#include <vector>

namespace coretest
{

struct Case
{
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry()
{
    static std::vector<Case> r;
    return r;
}

inline int& failures()
{
    static int f = 0;
    return f;
}

inline int& checks()
{
    static int c = 0;
    return c;
}

struct Registrar
{
    Registrar (const char* name, void (*fn)())
    {
        registry().push_back ({ name, fn });
    }
};

#define TEST_CASE(name) \
    static void test_##name(); \
    static ::coretest::Registrar reg_##name (#name, test_##name); \
    static void test_##name()

#define CHECK(cond) do { \
    ++::coretest::checks(); \
    if (! (cond)) { \
        ++::coretest::failures(); \
        std::printf ("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } } while (0)

#define CHECK_EQ(a, b) do { \
    ++::coretest::checks(); \
    if (! ((a) == (b))) { \
        ++::coretest::failures(); \
        std::printf ("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b); \
    } } while (0)

#define CHECK_NEAR(a, b, eps) do { \
    ++::coretest::checks(); \
    double _a = (double) (a), _b = (double) (b); \
    double _d = _a > _b ? _a - _b : _b - _a; \
    if (! (_d <= (eps))) { \
        ++::coretest::failures(); \
        std::printf ("FAIL %s:%d: %s=%f vs %s=%f\n", __FILE__, __LINE__, #a, _a, #b, _b); \
    } } while (0)

} // namespace coretest
