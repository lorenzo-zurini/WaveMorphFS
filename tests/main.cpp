// SPDX-License-Identifier: GPL-3.0-or-later
// Runs every registered test, or those whose name contains the first argument;
// `--list` prints the names.
#include <chrono>
#include <cstdio>
#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
    const char* filter = argc > 1 ? argv[1] : nullptr;
    if (filter && std::strcmp(filter, "--list") == 0) {
        for (auto& c : wmtest::registry()) std::printf("%s\n", c.name);
        return 0;
    }
    int failed = 0, run = 0;
    for (auto& c : wmtest::registry()) {
        if (filter && !std::strstr(c.name, filter)) continue;
        run++;
        auto t0 = std::chrono::steady_clock::now();
        std::string err;
        try {
            c.fn();
        } catch (const std::exception& e) {
            err = e.what();
        } catch (...) {
            err = "unknown exception";
        }
        double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (err.empty()) {
            std::printf("ok    %-55s %8.0f ms\n", c.name, ms);
        } else {
            failed++;
            std::printf("FAIL  %s\n      %s\n", c.name, err.c_str());
        }
        std::fflush(stdout);
    }
    if (run == 0 && filter) std::printf("no test matches \"%s\"\n", filter);
    std::printf("\n%d passed, %d failed\n", run - failed, failed);
    return failed || run == 0 ? 1 : 0;
}
