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

}  // namespace

std::string to_hex(const std::uint8_t* data, std::size_t size) {
    std::string out;
    out.resize(size * 2);
    for (std::size_t i = 0; i < size; ++i) {
        out[i * 2] = kHexDigits[data[i] >> 4];
        out[i * 2 + 1] = kHexDigits[data[i] & 0x0fu];
    }
    return out;
}

std::string Digest::hex() const {
    return to_hex(bytes_.data(), bytes_.size());
}

std::optional<Digest> Digest::parse(std::string_view text) {
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
    return Digest{bytes};
}

Digest Digest::of(std::string_view payload) {
    return Digest{Sha256::hash(payload)};
}

Digest Digest::of(const void* data, std::size_t size) {
    return Digest{Sha256::hash(data, size)};
}

bool Digest::is_zero() const noexcept {
    for (const std::uint8_t byte : bytes_) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

}  // namespace recovery
