#ifndef RECOVERY_TEST_HPP
#define RECOVERY_TEST_HPP

// Minimal test framework. It exists so that the proof obligations in this
// repository are readable as proofs: a test names the invariant it checks, and
// a failure prints the values that broke it.
//
// There are deliberately no timeouts, no watchdogs, and no forced termination
// anywhere in this harness. A hang is a defect, and a harness that hides one
// behind a timer would be hiding the defect the tests exist to find.

#include <cstdint>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace rectest {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

class Registry {
public:
    [[nodiscard]] static Registry& instance();
    void add(std::string name, std::function<void()> body);
    [[nodiscard]] const std::vector<TestCase>& cases() const noexcept { return cases_; }
    void set_filter(std::string filter) { filter_ = std::move(filter); }
    [[nodiscard]] const std::string& filter() const noexcept { return filter_; }

private:
    std::vector<TestCase> cases_;
    std::string filter_;
};

struct Registrar {
    Registrar(const char* name, std::function<void()> body) { Registry::instance().add(name, std::move(body)); }
};

class Failure {
public:
    explicit Failure(std::string message) : message_(std::move(message)) {}
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

private:
    std::string message_;
};

// Every assertion failure carries file, line, and the values involved. The
// values are rendered with operator<< so a failing comparison shows both sides.
template <typename Lhs, typename Rhs>
void require_equal(const Lhs& lhs, const Rhs& rhs, const char* lhsText, const char* rhsText, const char* file,
                   int line) {
    if (!(lhs == rhs)) {
        std::ostringstream out;
        out << file << ":" << line << ": expected " << lhsText << " == " << rhsText << "\n  left:  " << lhs
            << "\n  right: " << rhs;
        throw Failure(out.str());
    }
}

template <typename Lhs, typename Rhs>
void require_not_equal(const Lhs& lhs, const Rhs& rhs, const char* lhsText, const char* rhsText, const char* file,
                       int line) {
    if (lhs == rhs) {
        std::ostringstream out;
        out << file << ":" << line << ": expected " << lhsText << " != " << rhsText << "\n  both: " << lhs;
        throw Failure(out.str());
    }
}

void require(bool condition, const char* text, const char* file, int line);
void require_message(bool condition, std::string message, const char* file, int line);

// Runs every registered test (or the single filtered one) and reports.
[[nodiscard]] int run_all(const char* suiteName);

}  // namespace rectest

#define RC_TEST(name)                                                                          \
    static void rc_test_##name();                                                              \
    static const ::rectest::Registrar rc_registrar_##name{#name, rc_test_##name};              \
    static void rc_test_##name()

#define RC_REQUIRE(condition) ::rectest::require((condition), #condition, __FILE__, __LINE__)
#define RC_REQUIRE_MSG(condition, message)                                                     \
    ::rectest::require_message((condition), (message), __FILE__, __LINE__)
#define RC_REQUIRE_EQ(lhs, rhs) ::rectest::require_equal((lhs), (rhs), #lhs, #rhs, __FILE__, __LINE__)
#define RC_REQUIRE_NE(lhs, rhs) ::rectest::require_not_equal((lhs), (rhs), #lhs, #rhs, __FILE__, __LINE__)

// Unwraps a Result, failing the test with the error description when the result
// is an error. This keeps the proof readable at the point of the decision.
#define RC_REQUIRE_OK(expression)                                                              \
    ([&] {                                                                                     \
        auto&& rc_result = (expression);                                                       \
        if (!rc_result.has_value()) {                                                          \
            ::rectest::require_message(false, std::string{#expression " failed: "} +           \
                                                 rc_result.error().describe(),                 \
                                       __FILE__, __LINE__);                                    \
        }                                                                                      \
        return std::move(rc_result).value();                                                   \
    }())

#define RC_REQUIRE_ERR(expression, expectedClass)                                              \
    ([&] {                                                                                     \
        auto&& rc_result = (expression);                                                       \
        if (rc_result.has_value()) {                                                           \
            ::rectest::require_message(false, std::string{#expression " unexpectedly succeeded"}, \
                                       __FILE__, __LINE__);                                    \
        }                                                                                      \
        if (rc_result.error().cls() != (expectedClass)) {                                      \
            ::rectest::require_message(false,                                                  \
                                       std::string{#expression " failed with the wrong class: "} + \
                                           rc_result.error().describe(),                   \
                                       __FILE__, __LINE__);                                    \
        }                                                                                      \
        return rc_result.error();                                                              \
    }())

#endif  // RECOVERY_TEST_HPP
