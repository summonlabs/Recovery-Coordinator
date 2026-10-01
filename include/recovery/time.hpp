#ifndef RECOVERY_TIME_HPP
#define RECOVERY_TIME_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "recovery/error.hpp"

namespace recovery {

using NanoCount = std::uint64_t;
using MilliCount = std::uint64_t;
using SecondCount = std::uint64_t;

// The coordinator never reads a wall clock on its own. Every time value that
// reaches durable state or a decision is supplied by the caller (a clock
// function on the engine, or a "now" argument on a pure query), which is what
// makes replay and deterministic tests possible.
//
// Internally all durations are integer nanoseconds. Conversion from coarser
// units uses checked arithmetic and refuses to overflow.
class Duration {
public:
    constexpr Duration() = default;
    explicit constexpr Duration(NanoCount nanos) : nanos_(nanos) {}

    [[nodiscard]] static std::optional<Duration> from_millis(std::uint64_t millis);
    [[nodiscard]] static std::optional<Duration> from_seconds(std::uint64_t seconds);

    [[nodiscard]] constexpr NanoCount nanos() const noexcept { return nanos_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }

    [[nodiscard]] constexpr Duration max_with(const Duration& other) const noexcept {
        return Duration{nanos_ > other.nanos_ ? nanos_ : other.nanos_};
    }

    friend constexpr bool operator==(const Duration& lhs, const Duration& rhs) noexcept {
        return lhs.nanos_ == rhs.nanos_;
    }
    friend constexpr bool operator!=(const Duration& lhs, const Duration& rhs) noexcept { return !(lhs == rhs); }
    friend constexpr bool operator<(const Duration& lhs, const Duration& rhs) noexcept {
        return lhs.nanos_ < rhs.nanos_;
    }
    friend constexpr bool operator<=(const Duration& lhs, const Duration& rhs) noexcept {
        return lhs.nanos_ <= rhs.nanos_;
    }

private:
    NanoCount nanos_{0};
};

// A point on the coordinator's caller-supplied timeline, measured in
// nanoseconds since the Unix epoch. Pure integer, no floating point.
class TimePoint {
public:
    constexpr TimePoint() = default;
    explicit constexpr TimePoint(NanoCount sinceEpoch) : nanos_(sinceEpoch) {}

    [[nodiscard]] constexpr NanoCount nanos() const noexcept { return nanos_; }
    [[nodiscard]] constexpr bool is_zero() const noexcept { return nanos_ == 0; }

    // Checked advance. Returns nullopt when the resulting instant would exceed
    // the representable range instead of wrapping into the past.
    [[nodiscard]] std::optional<TimePoint> add(const Duration& delta) const;

    // Signed difference. Returns nullopt on underflow rather than wrapping.
    [[nodiscard]] std::optional<Duration> since(const TimePoint& earlier) const;

    [[nodiscard]] bool is_after(const TimePoint& other) const noexcept { return nanos_ > other.nanos_; }

    friend constexpr bool operator==(const TimePoint& lhs, const TimePoint& rhs) noexcept {
        return lhs.nanos_ == rhs.nanos_;
    }
    friend constexpr bool operator!=(const TimePoint& lhs, const TimePoint& rhs) noexcept { return !(lhs == rhs); }
    friend constexpr bool operator<(const TimePoint& lhs, const TimePoint& rhs) noexcept {
        return lhs.nanos_ < rhs.nanos_;
    }
    friend constexpr bool operator<=(const TimePoint& lhs, const TimePoint& rhs) noexcept {
        return lhs.nanos_ <= rhs.nanos_;
    }

private:
    NanoCount nanos_{0};
};

// Strict RFC 3339 UTC rendering, e.g. "2026-02-11T08:15:00.123456789Z".
// Only the exact canonical form is accepted when parsing: fixed width fields,
// no offsets, no fractional seconds other than 1, 3, 6 or 9 digits.
[[nodiscard]] std::string to_rfc3339(TimePoint value);
[[nodiscard]] std::optional<TimePoint> parse_rfc3339(std::string_view text);

// Checked addition of two externally influenced counters.
[[nodiscard]] std::optional<std::uint64_t> checked_add(std::uint64_t lhs, std::uint64_t rhs);
[[nodiscard]] std::optional<std::uint64_t> checked_mul(std::uint64_t lhs, std::uint64_t rhs);
[[nodiscard]] std::optional<std::uint64_t> checked_increment(std::uint64_t value);

}  // namespace recovery

#endif  // RECOVERY_TIME_HPP
