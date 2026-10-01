#include "recovery/time.hpp"

#include <cstdio>
#include <limits>

namespace recovery {
namespace {

constexpr NanoCount kNanosPerSecond = 1000000000ull;
constexpr NanoCount kNanosPerMilli = 1000000ull;

[[nodiscard]] bool is_digit(char c) { return c >= '0' && c <= '9'; }

[[nodiscard]] bool parse_fixed(std::string_view text, std::size_t offset, std::size_t count, unsigned& out) {
    if (offset + count > text.size()) {
        return false;
    }
    unsigned value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const char c = text[offset + i];
        if (!is_digit(c)) {
            return false;
        }
        value = value * 10u + static_cast<unsigned>(c - '0');
    }
    out = value;
    return true;
}

// Days from civil date, Howard Hinnant's algorithm. Exact for the whole range.
[[nodiscard]] std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) {
    year -= month <= 2u ? 1 : 0;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153u * (month + (month > 2u ? static_cast<unsigned>(-3) : 9u)) + 2u) / 5u + day - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

void civil_from_days(std::int64_t z, std::int64_t& year, unsigned& month, unsigned& day) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    const std::int64_t y = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    const unsigned mp = (5u * doy + 2u) / 153u;
    day = doy - (153u * mp + 2u) / 5u + 1u;
    month = mp + (mp < 10u ? 3u : static_cast<unsigned>(-9));
    year = y + (month <= 2u ? 1 : 0);
}

}  // namespace

std::optional<Duration> Duration::from_millis(std::uint64_t millis) {
    if (millis > std::numeric_limits<NanoCount>::max() / kNanosPerMilli) {
        return std::nullopt;
    }
    return Duration{millis * kNanosPerMilli};
}

std::optional<Duration> Duration::from_seconds(std::uint64_t seconds) {
    if (seconds > std::numeric_limits<NanoCount>::max() / kNanosPerSecond) {
        return std::nullopt;
    }
    return Duration{seconds * kNanosPerSecond};
}

std::optional<TimePoint> TimePoint::add(const Duration& delta) const {
    if (delta.nanos() > std::numeric_limits<NanoCount>::max() - nanos_) {
        return std::nullopt;
    }
    return TimePoint{nanos_ + delta.nanos()};
}

std::optional<Duration> TimePoint::since(const TimePoint& earlier) const {
    if (nanos_ < earlier.nanos_) {
        return std::nullopt;
    }
    return Duration{nanos_ - earlier.nanos_};
}

std::string to_rfc3339(TimePoint value) {
    const NanoCount nanos = value.nanos();
    const auto seconds = static_cast<std::int64_t>(nanos / kNanosPerSecond);
    const auto remainder = static_cast<std::uint32_t>(nanos % kNanosPerSecond);

    std::int64_t days = seconds / 86400;
    std::int64_t secondsOfDay = seconds % 86400;
    if (secondsOfDay < 0) {
        secondsOfDay += 86400;
        --days;
    }

    std::int64_t year = 0;
    unsigned month = 0;
    unsigned day = 0;
    civil_from_days(days, year, month, day);

    const auto hour = static_cast<unsigned>(secondsOfDay / 3600);
    const auto minute = static_cast<unsigned>((secondsOfDay % 3600) / 60);
    const auto second = static_cast<unsigned>(secondsOfDay % 60);

    char buffer[64];
    if (remainder == 0) {
        std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02uZ", static_cast<long long>(year), month, day,
                      hour, minute, second);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%04lld-%02u-%02uT%02u:%02u:%02u.%09uZ", static_cast<long long>(year), month,
                      day, hour, minute, second, remainder);
    }
    return std::string{buffer};
}

std::optional<TimePoint> parse_rfc3339(std::string_view text) {
    if (text.size() < 20) {
        return std::nullopt;
    }
    if (text[4] != '-' || text[7] != '-' || text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return std::nullopt;
    }
    unsigned year = 0;
    unsigned month = 0;
    unsigned day = 0;
    unsigned hour = 0;
    unsigned minute = 0;
    unsigned second = 0;
    if (!parse_fixed(text, 0, 4, year) || !parse_fixed(text, 5, 2, month) || !parse_fixed(text, 8, 2, day) ||
        !parse_fixed(text, 11, 2, hour) || !parse_fixed(text, 14, 2, minute) || !parse_fixed(text, 17, 2, second)) {
        return std::nullopt;
    }
    if (month < 1u || month > 12u || day < 1u || day > 31u || hour > 23u || minute > 59u || second > 60u) {
        return std::nullopt;
    }

    std::size_t offset = 19;
    NanoCount fraction = 0;
    if (offset < text.size() && text[offset] == '.') {
        ++offset;
        std::size_t digits = 0;
        std::uint64_t value = 0;
        while (offset < text.size() && is_digit(text[offset])) {
            if (digits < 9) {
                value = value * 10u + static_cast<std::uint64_t>(text[offset] - '0');
            }
            ++digits;
            ++offset;
        }
        if (digits != 1 && digits != 3 && digits != 6 && digits != 9) {
            return std::nullopt;
        }
        for (std::size_t i = digits; i < 9; ++i) {
            value *= 10u;
        }
        fraction = value;
    }
    if (offset + 1 != text.size() || text[offset] != 'Z') {
        return std::nullopt;
    }
    if (second == 60u) {
        // A leap second is not representable on this timeline; refuse instead
        // of silently rolling into the next minute.
        return std::nullopt;
    }

    const std::int64_t days = days_from_civil(static_cast<std::int64_t>(year), month, day);
    if (days < 0) {
        return std::nullopt;
    }
    const auto totalSeconds = static_cast<std::uint64_t>(days) * 86400ull +
                              static_cast<std::uint64_t>(hour) * 3600ull + static_cast<std::uint64_t>(minute) * 60ull +
                              static_cast<std::uint64_t>(second);
    if (totalSeconds > std::numeric_limits<NanoCount>::max() / kNanosPerSecond) {
        return std::nullopt;
    }
    const NanoCount nanos = totalSeconds * kNanosPerSecond;
    if (nanos > std::numeric_limits<NanoCount>::max() - fraction) {
        return std::nullopt;
    }
    return TimePoint{nanos + fraction};
}

std::optional<std::uint64_t> checked_add(std::uint64_t lhs, std::uint64_t rhs) {
    if (rhs > std::numeric_limits<std::uint64_t>::max() - lhs) {
        return std::nullopt;
    }
    return lhs + rhs;
}

std::optional<std::uint64_t> checked_mul(std::uint64_t lhs, std::uint64_t rhs) {
    if (lhs != 0 && rhs > std::numeric_limits<std::uint64_t>::max() / lhs) {
        return std::nullopt;
    }
    return lhs * rhs;
}

std::optional<std::uint64_t> checked_increment(std::uint64_t value) {
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        return std::nullopt;
    }
    return value + 1;
}

}  // namespace recovery
