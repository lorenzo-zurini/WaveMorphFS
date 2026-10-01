// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal test registry: TEST(name) { CHECK(...); }
#pragma once

#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace wmtest {

struct Failure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Reg {
    Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); }
};

}  // namespace wmtest

#define TEST(name)                                         \
    static void test_##name();                             \
    static ::wmtest::Reg reg_##name(#name, test_##name);   \
    static void test_##name()

#define CHECK(cond)                                                                                  \
    do {                                                                                             \
        if (!(cond)) throw ::wmtest::Failure(std::format("{}:{}: CHECK({})", __FILE__, __LINE__, #cond)); \
    } while (0)

#define CHECK_MSG(cond, ...)                                                                                              \
    do {                                                                                                                  \
        if (!(cond)) throw ::wmtest::Failure(std::format("{}:{}: CHECK({}): {}", __FILE__, __LINE__, #cond, std::format(__VA_ARGS__))); \
    } while (0)

#define CHECK_THROWS(expr)                                                                               \
    do {                                                                                                 \
        bool threw_ = false;                                                                             \
        try {                                                                                            \
            (void)(expr);                                                                                \
        } catch (...) {                                                                                  \
            threw_ = true;                                                                               \
        }                                                                                                \
        if (!threw_) throw ::wmtest::Failure(std::format("{}:{}: expected exception from {}", __FILE__, __LINE__, #expr)); \
    } while (0)
