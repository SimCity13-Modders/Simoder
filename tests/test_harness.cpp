#include "test_harness.hpp"
#include <iostream>

namespace sc13::tests {
namespace {
int failures{};
}
void Expect(bool condition, const char* expression) {
    if (!condition) {
        ++failures;
        std::cerr << "FAILED: " << expression << '\n';
    }
}
int FailureCount() noexcept { return failures; }
}
