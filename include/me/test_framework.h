// test_framework.h — a ~60-line, zero-dependency unit test harness.
//
// The spec allows Catch2/GoogleTest but also prizes "built the hard parts, not
// glued libraries together" and minimal dependencies. A tiny harness keeps the
// build single-command on every platform with no vendoring. CHECK keeps going;
// REQUIRE aborts the current test on failure.
#ifndef ME_TEST_FRAMEWORK_H
#define ME_TEST_FRAMEWORK_H

#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace tf {

struct TestCase { std::string name; std::function<void()> fn; };

inline std::vector<TestCase>& registry() { static std::vector<TestCase> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline long& checks() { static long c = 0; return c; }

struct Registrar {
    Registrar(const char* n, std::function<void()> f) {
        registry().push_back({n, std::move(f)});
    }
};

inline void report_fail(const char* file, int line, const char* expr) {
    ++failures();
    std::printf("    [FAIL] %s:%d  %s\n", file, line, expr);
}

inline int run_all() {
    int passed = 0, failed = 0;
    std::printf("running %zu tests...\n", registry().size());
    std::fflush(stdout);
    for (auto& t : registry()) {
        std::printf("[run ] %s\n", t.name.c_str());
        std::fflush(stdout);
        const int before = failures();
        try {
            t.fn();
        } catch (const std::exception& e) {
            ++failures();
            std::printf("    [THROW] %s\n", e.what());
        }
        if (failures() == before) { ++passed; std::printf("[ ok ] %s\n", t.name.c_str()); }
        else                      { ++failed; std::printf("[FAIL] %s\n", t.name.c_str()); }
        std::fflush(stdout);  // so a later hard crash can't swallow progress
    }
    std::printf("\n==== %d passed, %d failed, %ld checks ====\n", passed, failed, checks());
    return failed == 0 ? 0 : 1;
}

}  // namespace tf

#define TF_CONCAT_(a, b) a##b
#define TF_CONCAT(a, b) TF_CONCAT_(a, b)

#define TEST_CASE(human_name)                                                  \
    static void TF_CONCAT(tf_test_, __LINE__)();                               \
    static tf::Registrar TF_CONCAT(tf_reg_, __LINE__)(                         \
        human_name, TF_CONCAT(tf_test_, __LINE__));                            \
    static void TF_CONCAT(tf_test_, __LINE__)()

#define CHECK(cond)                                                            \
    do { ++tf::checks();                                                       \
         if (!(cond)) tf::report_fail(__FILE__, __LINE__, #cond); } while (0)

#define REQUIRE(cond)                                                          \
    do { ++tf::checks();                                                       \
         if (!(cond)) { tf::report_fail(__FILE__, __LINE__, #cond); return; } \
    } while (0)

#endif  // ME_TEST_FRAMEWORK_H
