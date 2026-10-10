// Entry point for the C++ unit-test binary.
//
// Usage:
//   imu_lab_tests                 # run everything
//   imu_lab_tests Pipeline        # run only tests whose "Suite.Name" contains it
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "imu_lab/test_framework.hpp"

int main(int argc, char** argv) {
    std::string filter;
    bool list_only = false;

    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--list") {
            list_only = true;
        } else if (argument == "--help" || argument == "-h") {
            std::cout << "imu_lab_tests [--list] [substring-filter]\n";
            return 0;
        } else {
            filter = argument;
        }
    }

    if (list_only) {
        for (const auto& test : imu_lab::test::registry()) {
            std::cout << test.suite << '.' << test.name << '\n';
        }
        return 0;
    }

    const int failed = imu_lab::test::run_all(filter);
    if (failed != 0) {
        std::cout << failed << " test(s) FAILED\n";
    }
    return failed == 0 ? 0 : 1;
}
