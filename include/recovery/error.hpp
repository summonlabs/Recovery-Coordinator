#ifndef RECOVERY_ERROR_HPP
#define RECOVERY_ERROR_HPP

#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace recovery {

// Error classification. Callers are expected to branch on the class, never on
// the message text: messages are diagnostic, classes are contractual.
enum class ErrorClass {
    // The caller supplied something structurally invalid.
    InvalidArgument,
    // The request was rejected because a precondition about authority,
    // freshness, generation, or lifecycle did not hold. No effect was produced.
    PreconditionFailed,
    // The request attempts to move an object through an illegal lifecycle edge.
    IllegalTransition,
    // An identifier or generation is unknown to this coordinator instance.
    NotFound,
    // The same operation identity was reused with different content, or a
    // distinct operation reused an identity that is already committed.
    IdentityConflict,
    // Durable state could not be written, read, or verified.
    Persistence,
    // A stored representation was corrupt, truncated, ambiguous, or of an
    // unsupported version.
    CorruptState,
    // The requester is not the current single writer / incarnation.
    StaleAuthority,
    // Idempotent replay of an already committed operation.
    Replayed,
    // A checked arithmetic operation would have overflowed or underflowed.
    Overflow,
    // The operation is not implemented or not supported by this build.
    Unsupported,
    // Another thread or process currently owns the resource.
    Contended,
    // A bounded resource limit was exceeded.
    LimitExceeded,
    // The adapter or the environment reported a genuine operational failure.
    OperationFailed,
    // Internal invariant violation. Always a defect.
    Internal,
};

[[nodiscard]] std::string_view to_string(ErrorClass value);

class Error {
public:
    Error() = default;

    Error(ErrorClass cls, std::string code, std::string message)
        : cls_(cls), code_(std::move(code)), message_(std::move(message)) {}

    [[nodiscard]] ErrorClass cls() const noexcept { return cls_; }
    [[nodiscard]] const std::string& code() const noexcept { return code_; }
    [[nodiscard]] const std::string& message() const noexcept { return message_; }

    [[nodiscard]] std::string describe() const;

    friend bool operator==(const Error& lhs, const Error& rhs) noexcept {
        return lhs.cls_ == rhs.cls_ && lhs.code_ == rhs.code_ && lhs.message_ == rhs.message_;
    }

private:
    ErrorClass cls_{ErrorClass::Internal};
    std::string code_{"internal"};
    std::string message_;
};

// Result<T>: either a value or an Error. There is no exception-based control
// flow anywhere in the library's public surface.
template <typename T>
class Result {
public:
    Result(T value) : value_(std::move(value)), has_value_(true) {}
    Result(Error error) : error_(std::move(error)), has_value_(false) {}

    [[nodiscard]] bool has_value() const noexcept { return has_value_; }
    explicit operator bool() const noexcept { return has_value_; }

    [[nodiscard]] T& value() & { return value_; }
    [[nodiscard]] const T& value() const& { return value_; }
    [[nodiscard]] T&& value() && { return std::move(value_); }

    [[nodiscard]] const Error& error() const noexcept { return error_; }

    [[nodiscard]] T value_or(T fallback) const { return has_value_ ? value_ : std::move(fallback); }

    // Convenience accessors. They are unchecked by design: a caller that
    // dereferences a failed Result has already made a mistake, and the library
    // documents that every call site checks has_value() first.
    [[nodiscard]] T& operator*() & { return value_; }
    [[nodiscard]] const T& operator*() const& { return value_; }
    [[nodiscard]] T&& operator*() && { return std::move(value_); }
    [[nodiscard]] T* operator->() { return &value_; }
    [[nodiscard]] const T* operator->() const { return &value_; }

private:
    T value_{};
    Error error_{};
    bool has_value_{false};
};

// Convenience helpers used throughout the library.
[[nodiscard]] inline Error make_error(ErrorClass cls, std::string code, std::string message) {
    return Error{cls, std::move(code), std::move(message)};
}

namespace errors {

[[nodiscard]] Error invalid_argument(std::string code, std::string message);
[[nodiscard]] Error precondition_failed(std::string code, std::string message);
[[nodiscard]] Error illegal_transition(std::string code, std::string message);
[[nodiscard]] Error not_found(std::string code, std::string message);
[[nodiscard]] Error identity_conflict(std::string code, std::string message);
[[nodiscard]] Error persistence(std::string code, std::string message);
[[nodiscard]] Error corrupt_state(std::string code, std::string message);
[[nodiscard]] Error stale_authority(std::string code, std::string message);
[[nodiscard]] Error replayed(std::string code, std::string message);
[[nodiscard]] Error overflow(std::string code, std::string message);
[[nodiscard]] Error unsupported(std::string code, std::string message);
[[nodiscard]] Error contended(std::string code, std::string message);
[[nodiscard]] Error limit_exceeded(std::string code, std::string message);
[[nodiscard]] Error operation_failed(std::string code, std::string message);
[[nodiscard]] Error internal(std::string code, std::string message);

}  // namespace errors

}  // namespace recovery

#endif  // RECOVERY_ERROR_HPP
