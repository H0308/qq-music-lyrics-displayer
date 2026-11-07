#pragma once

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace test_support {

struct TestFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

[[noreturn]] inline void fail(const char* file, int line, const std::string& message) {
    char buffer[1024]{};
    std::snprintf(buffer, sizeof(buffer), "%s:%d: %s", file, line, message.c_str());
    throw TestFailure(buffer);
}

inline void checkTrue(bool condition, const char* expr, const char* file, int line) {
    if (!condition)
        fail(file, line, std::string("EXPECT_TRUE failed: ") + expr);
}

inline void checkFalse(bool condition, const char* expr, const char* file, int line) {
    if (condition)
        fail(file, line, std::string("EXPECT_FALSE failed: ") + expr);
}

template <typename A, typename B>
void checkEq(const A& actual, const B& expected, const char* actualExpr, const char* expectedExpr,
             const char* file, int line) {
    if (!(actual == expected)) {
        fail(file, line,
             std::string("EXPECT_EQ failed: ") + actualExpr + " == " + expectedExpr);
    }
}

template <typename A, typename B>
void checkNe(const A& actual, const B& expected, const char* actualExpr, const char* expectedExpr,
             const char* file, int line) {
    if (actual == expected) {
        fail(file, line,
             std::string("EXPECT_NE failed: ") + actualExpr + " != " + expectedExpr);
    }
}

inline void checkNear(double actual, double expected, double tolerance, const char* actualExpr,
                      const char* expectedExpr, const char* file, int line) {
    if (!(std::fabs(actual - expected) <= tolerance)) {
        char detail[512]{};
        std::snprintf(detail, sizeof(detail),
                      "EXPECT_NEAR failed: |%s (%g) - %s (%g)| > %g", actualExpr, actual,
                      expectedExpr, expected, tolerance);
        fail(file, line, detail);
    }
}

inline int runAllTests(int argc, char** argv) {
    std::string_view filter;
    if (argc > 1 && argv && argv[1])
        filter = argv[1];

    int passed = 0;
    int failed = 0;
    for (const auto& test : registry()) {
        if (!filter.empty() && std::string_view(test.name).find(filter) == std::string_view::npos)
            continue;
        try {
            test.fn();
            ++passed;
        } catch (const std::exception& ex) {
            ++failed;
            std::fprintf(stderr, "[FAILED] %s\n  %s\n", test.name.c_str(), ex.what());
        } catch (...) {
            ++failed;
            std::fprintf(stderr, "[FAILED] %s\n  unknown exception\n", test.name.c_str());
        }
    }
    std::printf("[SUMMARY] passed=%d failed=%d\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace test_support

#define TEST_CONCAT_INNER(a, b) a##b
#define TEST_CONCAT(a, b) TEST_CONCAT_INNER(a, b)

#define TEST_CASE(test_name)                                                                       \
    static void TEST_CONCAT(test_fn_, __LINE__)();                                                 \
    static ::test_support::Registrar TEST_CONCAT(test_reg_, __LINE__)(                             \
        test_name, &TEST_CONCAT(test_fn_, __LINE__));                                              \
    static void TEST_CONCAT(test_fn_, __LINE__)()

#define EXPECT_TRUE(expr) ::test_support::checkTrue(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define EXPECT_FALSE(expr) ::test_support::checkFalse(static_cast<bool>(expr), #expr, __FILE__, __LINE__)
#define EXPECT_EQ(a, b) ::test_support::checkEq((a), (b), #a, #b, __FILE__, __LINE__)
#define EXPECT_NE(a, b) ::test_support::checkNe((a), (b), #a, #b, __FILE__, __LINE__)
#define EXPECT_NEAR(a, b, tol) ::test_support::checkNear((a), (b), (tol), #a, #b, __FILE__, __LINE__)
