#ifndef RECOVERY_DIGEST_HPP
#define RECOVERY_DIGEST_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace recovery {

// A 32 byte content digest (SHA-256). Digests are the only currency the
// coordinator accepts for "this is the same bytes I bound to earlier": an
// authority reference is a structural identity, its digest is the byte-level
// commitment to the content the coordinator actually consumed.
class Digest {
public:
    static constexpr std::size_t kBytes = 32;

    Digest() = default;
    explicit Digest(const std::array<std::uint8_t, kBytes>& bytes) : bytes_(bytes) {}

    [[nodiscard]] const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] std::string hex() const;

    // Strict parser: exactly 64 lower case hex characters. Upper case input is
    // rejected rather than normalised so that a digest written by a different
    // encoder can never be silently accepted as equal.
    [[nodiscard]] static std::optional<Digest> parse(std::string_view text);

    // Digest of a text payload, used for the canonical encodings in this library.
    [[nodiscard]] static Digest of(std::string_view payload);
    [[nodiscard]] static Digest of(const void* data, std::size_t size);

    [[nodiscard]] static Digest zero() { return Digest{}; }
    [[nodiscard]] bool is_zero() const noexcept;

    friend bool operator==(const Digest& lhs, const Digest& rhs) noexcept { return lhs.bytes_ == rhs.bytes_; }
    friend bool operator!=(const Digest& lhs, const Digest& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const Digest& lhs, const Digest& rhs) noexcept { return lhs.bytes_ < rhs.bytes_; }

private:
    std::array<std::uint8_t, kBytes> bytes_{};
};

[[nodiscard]] std::string to_hex(const std::uint8_t* data, std::size_t size);

}  // namespace recovery

#endif  // RECOVERY_DIGEST_HPP
