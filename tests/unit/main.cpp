// SPDX-License-Identifier: GPL-3.0-or-later
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#include "check.hpp"

namespace check {

std::vector<Test>& registry() {
    static std::vector<Test> tests;
    return tests;
}

namespace {
struct Failed : std::runtime_error {
    using std::runtime_error::runtime_error;
};
}   

void fail(const char* file, int line, const std::string& what) {
    throw Failed(std::string(file) + ":" + std::to_string(line) + ": " + what);
}

}   

int main(int argc, char** argv) {
    int failed = 0, ran = 0;
    for (const auto& t : check::registry()) {
        if (argc > 1 && std::strstr(t.name, argv[1]) == nullptr) {
            continue;
        }
        ++ran;
        try {
            t.body();
            std::printf("ok      %s\n", t.name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAILED  %s\n        %s\n", t.name, e.what());
        }
    }
    std::printf("%d tests, %d failed\n", ran, failed);
    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
