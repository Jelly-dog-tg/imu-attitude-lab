// A deliberately tiny, dependency-free unit-test harness.
//
// Rationale: pulling GoogleTest or Catch2 in through CMake FetchContent would
// make the build depend on network access, which the grading/CI environment does
// not guarantee (and the local toolchain here has a flaky proxy). This header
// gives the same essential features in ~150 lines:
//
//   * self-registering test cases (no manual list to maintain);
//   * fatal assertions that report file/line and the actual values;
//   * a filter argument so a single suite can be run from the command line;
//   * a non-zero exit code on failure, which is all CTest needs.
#pragma once

#include <cmath>
#include <cstdlib>
#include <exception>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace imu_lab::test {

/// Thrown by a failing assertion; caught by the runner, which then moves on to
/// the next test case instead of aborting the whole binary.
class AssertionFailure : public std::exception {
public:
    explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

struct TestCase {
    std::string suite;
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

/// Registers a test case as a side effect of static initialisation.
struct Registrar {
    Registrar(std::string suite, std::string name, std::function<void()> body) {
        registry().push_back(TestCase{std::move(suite), std::move(name), std::move(body)});
    }
};

/// Detects whether `operator<<` can print a value, so `describe_value` works for
/// plain enums (which have no stream operator) as well as for numbers/strings.
template <typename T, typename = void>
struct is_streamable : std::false_type {};

template <typename T>
struct is_streamable<T, std::void_t<decltype(std::declval<std::ostream&>()
                                             << std::declval<const T&>())>> : std::true_type {};

/// Human-readable rendering of a value for assertion messages.
template <typename T>
std::string describe_value(const T& value) {
    if constexpr (is_streamable<T>::value) {
        std::ostringstream out;
        out << value;
        return out.str();
    } else if constexpr (std::is_enum_v<T>) {
        return "enum(" +
               std::to_string(static_cast<long long>(static_cast<std::underlying_type_t<T>>(value))) +
               ")";
    } else {
        return "<value>";
    }
}

inline std::string describe_value(bool value) { return value ? "true" : "false"; }

inline void fail(const std::string& expression, const std::string& detail, const char* file,
                 int line) {
    std::ostringstream out;
    out << file << ':' << line << ": CHECK failed: " << expression;
    if (!detail.empty()) {
        out << "\n    " << detail;
    }
    throw AssertionFailure(out.str());
}

/// Templated rather than `bool`-typed so that types with an *explicit*
/// `operator bool` (notably `Result<T>`) can be used directly:
/// `CHECK(parse("P:1,R:2"))`.
template <typename T>
void check(const T& condition, const std::string& expression, const char* file, int line) {
    if (!condition) {
        fail(expression, {}, file, line);
    }
}

template <typename A, typename B>
void check_eq(const A& actual, const B& expected, const std::string& expression, const char* file,
              int line) {
    if (!(actual == expected)) {
        fail(expression,
             "actual:   " + describe_value(actual) + "\nexpected: " + describe_value(expected),
             file, line);
    }
}

template <typename A, typename B>
void check_ne(const A& actual, const B& unexpected, const std::string& expression, const char* file,
              int line) {
    if (actual == unexpected) {
        fail(expression, "both were: " + describe_value(actual), file, line);
    }
}

inline void check_near(double actual, double expected, double tolerance,
                       const std::string& expression, const char* file, int line) {
    const bool both_nan = std::isnan(actual) && std::isnan(expected);
    if (!both_nan && !(std::fabs(actual - expected) <= tolerance)) {
        fail(expression, "actual: " + describe_value(actual) + "\nexpected: " +
                             describe_value(expected) + "\ntolerance: " +
                             describe_value(tolerance),
             file, line);
    }
}

/// Run every registered test; returns the number of failures.
///
/// @param filter substring match against "suite.name"; empty runs everything.
inline int run_all(const std::string& filter = {}) {
    std::size_t executed = 0;
    std::size_t failed = 0;

    for (const TestCase& test : registry()) {
        const std::string full_name = test.suite + "." + test.name;
        if (!filter.empty() && full_name.find(filter) == std::string::npos) {
            continue;
        }
        ++executed;
        try {
            test.body();
            std::cout << "[  PASS  ] " << full_name << '\n';
        } catch (const AssertionFailure& failure) {
            ++failed;
            std::cout << "[  FAIL  ] " << full_name << '\n' << failure.what() << '\n';
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "[  FAIL  ] " << full_name << "\n    unexpected exception: " << error.what()
                      << '\n';
        } catch (...) {
            ++failed;
            std::cout << "[  FAIL  ] " << full_name << "\n    unknown exception\n";
        }
    }

    std::cout << "\n" << (executed - failed) << "/" << executed << " tests passed";
    if (!filter.empty()) {
        std::cout << " (filter: " << filter << ")";
    }
    std::cout << '\n';
    return static_cast<int>(failed);
}

}  // namespace imu_lab::test

// --------------------------------------------------------------------------- //
// Macros
// --------------------------------------------------------------------------- //
#define IMU_TEST(suite_name, test_name)                                                     \
    static void suite_name##_##test_name##_body();                                          \
    namespace {                                                                             \
    const ::imu_lab::test::Registrar registrar_##suite_name##_##test_name(                   \
        #suite_name, #test_name, suite_name##_##test_name##_body);                           \
    }                                                                                       \
    static void suite_name##_##test_name##_body()

#define CHECK(condition) \
    ::imu_lab::test::check((condition), #condition, __FILE__, __LINE__)

#define CHECK_EQ(actual, expected) \
    ::imu_lab::test::check_eq((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#define CHECK_NE(actual, unexpected) \
    ::imu_lab::test::check_ne((actual), (unexpected), #actual " != " #unexpected, __FILE__, __LINE__)

#define CHECK_NEAR(actual, expected, tolerance)                                         \
    ::imu_lab::test::check_near((actual), (expected), (tolerance),                       \
                                #actual " ~= " #expected, __FILE__, __LINE__)

#define FAIL_TEST(message) ::imu_lab::test::fail(#message, {}, __FILE__, __LINE__)

/// Assert that `expression` throws `exception_type`.
#define CHECK_THROWS_AS(expression, exception_type)                                          \
    do {                                                                                     \
        bool imu_lab_test_threw_expected = false;                                             \
        try {                                                                                 \
            (void)(expression);                                                               \
        } catch (const exception_type&) {                                                     \
            imu_lab_test_threw_expected = true;                                               \
        } catch (...) {                                                                       \
            ::imu_lab::test::fail(#expression " threw the wrong exception type", {},           \
                                  __FILE__, __LINE__);                                        \
        }                                                                                     \
        if (!imu_lab_test_threw_expected) {                                                   \
            ::imu_lab::test::fail(#expression " did not throw " #exception_type, {},           \
                                  __FILE__, __LINE__);                                        \
        }                                                                                     \
    } while (false)
