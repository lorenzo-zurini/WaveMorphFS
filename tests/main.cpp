// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdio>
#include <cstring>

#include "test.hpp"

int main(int argc, char** argv) {
    int failed = 0, run = 0;
    for (auto& c : wmtest::registry()) {
        if (argc > 1 && !std::strstr(c.name, argv[1])) continue;
        run++;
        try {
            c.fn();
            std::printf("ok    %s\n", c.name);
        } catch (const std::exception& e) {
            failed++;
            std::printf("FAIL  %s\n      %s\n", c.name, e.what());
        }
        std::fflush(stdout);
    }
    std::printf("\n%d passed, %d failed\n", run - failed, failed);
    return failed ? 1 : 0;
}
