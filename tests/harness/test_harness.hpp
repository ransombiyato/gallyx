// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Project HYPERION contributors
//
// Minimal, dependency-free test harness.
//
// Rationale (see DECISIONS.md D-006): HYPERION must build on constrained
// Android/Termux-style hosts with no package manager, so the test harness
// carries no third-party dependency. It provides just enough structure for
// table-driven unit tests and differential tests.
#ifndef HYPERION_TEST_HARNESS_HPP
#define HYPERION_TEST_HARNESS_HPP

#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace hyperion::test {

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> fn;
};

// Global registry used by the REGISTER_TEST macro.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

// Thrown by the CHECK_* macros to abort the current test case.
class AssertionFailure : public std::exception {
public:
    explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
    const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

struct Registrar {
    Registrar(const char* suite, const char* name, std::function<void()> fn) {
        registry().push_back(TestCase{suite, name, std::move(fn)});
    }
};

// Runs every registered test. Returns 0 on success, 1 if any test failed.
int run_all(const char* program_name, int argc, char** argv);

// --- Assertion helpers ------------------------------------------------------
inline std::string to_display(const std::string& v) {
    return v;
}
inline std::string to_display(const char* v) {
    return v ? v : "(null)";
}
inline std::string to_display(bool v) {
    return v ? "true" : "false";
}

template<typename T> std::string to_display(const T& v) {
    std::ostringstream os;
    os << v;
    return os.str();
}

} // namespace hyperion::test

#define HYPERION_CONCAT_IMPL(a, b) a##b
#define HYPERION_CONCAT(a, b) HYPERION_CONCAT_IMPL(a, b)

#define HYPERION_TEST(suite_name, test_name)                                                       \
    static void HYPERION_CONCAT(hyperion_test_, __LINE__)();                                       \
    static ::hyperion::test::Registrar HYPERION_CONCAT(hyperion_registrar_, __LINE__)(             \
        suite_name, test_name, &HYPERION_CONCAT(hyperion_test_, __LINE__));                        \
    static void HYPERION_CONCAT(hyperion_test_, __LINE__)()

#define HYPERION_CHECK(cond)                                                                       \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            std::ostringstream hyperion_os_;                                                       \
            hyperion_os_ << __FILE__ << ':' << __LINE__ << ": CHECK failed: " << #cond;            \
            throw ::hyperion::test::AssertionFailure(hyperion_os_.str());                          \
        }                                                                                          \
    } while (false)

#define HYPERION_CHECK_EQ(a, b)                                                                    \
    do {                                                                                           \
        const auto& hyperion_a_ = (a);                                                             \
        const auto& hyperion_b_ = (b);                                                             \
        if (!(hyperion_a_ == hyperion_b_)) {                                                       \
            std::ostringstream hyperion_os_;                                                       \
            hyperion_os_ << __FILE__ << ':' << __LINE__ << ": CHECK_EQ failed: " << #a             \
                         << " == " << #b << " (" << ::hyperion::test::to_display(hyperion_a_)      \
                         << " != " << ::hyperion::test::to_display(hyperion_b_) << ')';            \
            throw ::hyperion::test::AssertionFailure(hyperion_os_.str());                          \
        }                                                                                          \
    } while (false)

#endif // HYPERION_TEST_HARNESS_HPP
