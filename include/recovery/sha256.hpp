#ifndef RECOVERY_SHA256_HPP
#define RECOVERY_SHA256_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace recovery {

// Deterministic SHA-256. Implemented locally so that digests never depend on a
// platform cryptography provider being present, and so that the byte-level
// result is identical on every supported host.
class Sha256 {
public:
    static constexpr std::size_t kDigestBytes = 32;

    Sha256();

    void update(const void* data, std::size_t size);
    void update(std::string_view text) { update(text.data(), text.size()); }

    [[nodiscard]] std::array<std::uint8_t, kDigestBytes> finish();

    static std::array<std::uint8_t, kDigestBytes> hash(const void* data, std::size_t size);
    static std::array<std::uint8_t, kDigestBytes> hash(std::string_view text);

private:
    void transform(const std::uint8_t* block);

    std::array<std::uint32_t, 8> state_{};
    std::array<std::uint8_t, 64> buffer_{};
    std::uint64_t totalBytes_{0};
    std::size_t bufferSize_{0};
    bool finished_{false};
};

}  // namespace recovery

#endif  // RECOVERY_SHA256_HPP
