// SPDX-License-Identifier: GPL-3.0-or-later
// Minimal test registry: TEST(name) { CHECK(...); }
#pragma once

#include <format>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <stdlib.h>

#include <filesystem>

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

/// A fresh directory under the system temp dir, removed with everything in it
/// when the test ends, whether it passes or not.
class TempDir {
public:
    explicit TempDir(const char* prefix = "wm-test") {
        std::string tmpl = (std::filesystem::temp_directory_path() / (std::string(prefix) + "-XXXXXX")).string();
        if (!::mkdtemp(tmpl.data())) throw std::runtime_error("mkdtemp " + tmpl);
        path_ = tmpl;
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const std::filesystem::path& path() const { return path_; }
    operator const std::filesystem::path&() const { return path_; }
    std::filesystem::path operator/(const std::string& name) const { return path_ / name; }

private:
    std::filesystem::path path_;
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
