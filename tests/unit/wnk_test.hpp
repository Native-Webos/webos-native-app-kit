// SPDX-License-Identifier: GPL-3.0-or-later
//
// A deliberately tiny test harness: TEST(name) registers a function, CHECK/CHECK_EQ record failures
// without aborting the test, REQUIRE stops it. Each test binary defines WNK_TEST_MAIN in one file.
#pragma once

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace wnk::test {

struct Case {
    const char *name;
    std::function<void()> fn;
};

inline std::vector<Case> &registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int &failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char *name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

struct RequireFailed {};

inline void report(const char *file, int line, const std::string &what) {
    ++failures();
    std::fprintf(stderr, "  %s:%d: FAILED %s\n", file, line, what.c_str());
}

template <typename A, typename B>
std::string describe(const char *ea, const char *eb, const A &a, const B &b) {
    std::ostringstream os;
    os << ea << " == " << eb << "  (got: " << a << " vs " << b << ")";
    return os.str();
}

inline int run_all() {
    int failed_cases = 0;
    for (const auto &c : registry()) {
        const int before = failures();
        try {
            c.fn();
        } catch (const RequireFailed &) {
        } catch (const std::exception &e) {
            report(__FILE__, __LINE__, std::string("unexpected exception: ") + e.what());
        }
        const bool ok = failures() == before;
        std::printf("[%s] %s\n", ok ? " ok " : "FAIL", c.name);
        failed_cases += !ok;
    }
    std::printf("%zu tests, %d failed\n", registry().size(), failed_cases);
    return failed_cases == 0 ? 0 : 1;
}

}  // namespace wnk::test

#define WNK_TEST_CAT2(a, b) a##b
#define WNK_TEST_CAT(a, b) WNK_TEST_CAT2(a, b)
#define TEST(name)                                                                                  \
    static void WNK_TEST_CAT(wnk_test_fn_, __LINE__)();                                             \
    static ::wnk::test::Registrar WNK_TEST_CAT(wnk_test_reg_, __LINE__)(name, &WNK_TEST_CAT(wnk_test_fn_, __LINE__)); \
    static void WNK_TEST_CAT(wnk_test_fn_, __LINE__)()

#define CHECK(expr)                                                     \
    do {                                                                \
        if (!(expr)) ::wnk::test::report(__FILE__, __LINE__, #expr);    \
    } while (0)

#define CHECK_EQ(a, b)                                                                                        \
    do {                                                                                                      \
        const auto &wnk_a_ = (a);                                                                             \
        const auto &wnk_b_ = (b);                                                                             \
        if (!(wnk_a_ == wnk_b_)) ::wnk::test::report(__FILE__, __LINE__, ::wnk::test::describe(#a, #b, wnk_a_, wnk_b_)); \
    } while (0)

#define REQUIRE(expr)                                                   \
    do {                                                                \
        if (!(expr)) {                                                  \
            ::wnk::test::report(__FILE__, __LINE__, #expr);             \
            throw ::wnk::test::RequireFailed{};                         \
        }                                                               \
    } while (0)

#ifdef WNK_TEST_MAIN
int main() { return ::wnk::test::run_all(); }
#endif
