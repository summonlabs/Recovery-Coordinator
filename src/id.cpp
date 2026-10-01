#include "recovery/id.hpp"

#include "recovery/canonical.hpp"
#include "recovery/digest.hpp"
#include "recovery/sha256.hpp"

namespace recovery {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

[[nodiscard]] int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

// SplitMix64: a fully specified, portable bit mixer. Output depends only on the
// input word, so derived seeds are reproducible across hosts and runs.
[[nodiscard]] std::uint64_t mix64(std::uint64_t value) {
    value += 0x9e3779b97f4a7c15ull;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
    return value ^ (value >> 31);
}

}  // namespace

std::string_view to_string(IdKind value) {
    switch (value) {
        case IdKind::Plan:
            return "plan";
        case IdKind::Step:
            return "step";
        case IdKind::Attempt:
            return "attempt";
        case IdKind::Request:
            return "request";
        case IdKind::Evidence:
            return "evidence";
        case IdKind::Event:
            return "event";
        case IdKind::Execution:
            return "execution";
    }
    return "unknown";
}

bool Id::is_zero() const noexcept {
    for (const std::uint8_t byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

std::string Id::hex() const { return to_hex(bytes_.data(), bytes_.size()); }

std::string_view Id::prefix(IdKind kind) { return to_string(kind); }

std::optional<IdKind> Id::kind_from_prefix(std::string_view text) {
    if (text == "plan") {
        return IdKind::Plan;
    }
    if (text == "step") {
        return IdKind::Step;
    }
    if (text == "attempt") {
        return IdKind::Attempt;
    }
    if (text == "request") {
        return IdKind::Request;
    }
    if (text == "evidence") {
        return IdKind::Evidence;
    }
    if (text == "event") {
        return IdKind::Event;
    }
    if (text == "execution") {
        return IdKind::Execution;
    }
    return std::nullopt;
}

std::optional<Id> Id::parse_hex(std::string_view text) {
    if (text.size() != kBytes * 2) {
        return std::nullopt;
    }
    std::array<std::uint8_t, kBytes> bytes{};
    for (std::size_t i = 0; i < kBytes; ++i) {
        const int high = hex_value(text[i * 2]);
        const int low = hex_value(text[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::nullopt;
        }
        bytes[i] = static_cast<std::uint8_t>((high << 4) | low);
    }
    return Id{bytes};
}

std::optional<Id> Id::parse(IdKind kind, std::string_view text) {
    const std::string_view want = prefix(kind);
    if (text.size() != want.size() + 1 + kBytes * 2) {
        return std::nullopt;
    }
    if (text.substr(0, want.size()) != want) {
        return std::nullopt;
    }
    if (text[want.size()] != '-') {
        return std::nullopt;
    }
    return parse_hex(text.substr(want.size() + 1));
}

std::string Id::to_text(IdKind kind) const {
    std::string out;
    out.append(prefix(kind));
    out.push_back('-');
    out.append(hex());
    return out;
}

IdAllocator::IdAllocator(std::uint64_t next) : next_(next == 0 ? 1 : next) {}

std::optional<Id> IdAllocator::allocate(IdKind kind) {
    if (next_ == 0) {
        return std::nullopt;
    }
    const std::uint64_t counter = next_;
    // Refuse rather than wrap when the counter is exhausted.
    if (next_ == 0xffffffffffffffffull) {
        next_ = 0;
    } else {
        next_ = counter + 1;
    }

    std::uint64_t hi = mix64(counter ^ 0x2a3b4c5d6e7f8091ull);
    std::uint64_t lo = mix64(static_cast<std::uint64_t>(kind) << 56 | counter);
    std::array<std::uint8_t, Id::kBytes> bytes{};
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[i] = static_cast<std::uint8_t>(hi >> (56u - 8u * i));
        bytes[8 + i] = static_cast<std::uint8_t>(lo >> (56u - 8u * i));
    }
    return Id{bytes};
}

void IdAllocator::observe(std::uint64_t counter) {
    if (counter == 0 || counter == 0xffffffffffffffffull) {
        next_ = 0;
        return;
    }
    if (counter >= next_) {
        next_ = counter + 1;
    }
}

std::uint64_t derive_seed(const Id& value, std::string_view purpose) {
    CanonicalWriter writer;
    append_framed(writer, "recovery.derive_seed.v1");
    append_framed(writer, value.hex());
    append_framed(writer, purpose);
    const Digest digest = writer.digest_of();
    std::uint64_t seed = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        seed = (seed << 8) | digest.bytes()[i];
    }
    return mix64(seed);
}

}  // namespace recovery