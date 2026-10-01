#ifndef RECOVERY_CANONICAL_HPP
#define RECOVERY_CANONICAL_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "recovery/digest.hpp"

namespace recovery {

// Canonical output rules for the whole library:
//
//   * No canonical encoding depends on map or unordered_map iteration order,
//     on pointer values, on thread timing, or on locale.
//   * Every collection is emitted in an explicitly defined total order
//     (documented next to the type being encoded).
//   * Text is emitted as UTF-8 with the only escapes being \\ and \" plus
//     the C0 control escapes; everything else is passed through byte for byte.
//
// CanonicalWriter builds such an encoding incrementally and can be converted
// into the exact bytes it produced, so that a digest can be bound to the very
// encoding the coordinator validated.
class CanonicalWriter {
public:
    CanonicalWriter() = default;

    void raw(std::string_view text) { out_.append(text); }
    void escaped(std::string_view text);
    void boolean(bool value);
    void signed_integer(std::int64_t value);
    void unsigned_integer(std::uint64_t value);
    void quoted(std::string_view text);
    void byte_string(const std::uint8_t* data, std::size_t size);
    void digest(const Digest& value);

    [[nodiscard]] const std::string& str() const noexcept { return out_; }
    [[nodiscard]] std::string take() { return std::move(out_); }
    [[nodiscard]] std::size_t size() const noexcept { return out_.size(); }
    [[nodiscard]] Digest digest_of() const { return Digest::of(out_); }

private:
    std::string out_;
};

// Length-prefixed framing used for hashing structured content: each field is
// prefixed by its decimal byte length and a colon, which makes concatenation
// unambiguous without relying on a separator that could appear in a field.
void append_framed(CanonicalWriter& writer, std::string_view field);
void append_framed_u64(CanonicalWriter& writer, std::uint64_t value);
void append_framed_i64(CanonicalWriter& writer, std::int64_t value);
void append_framed_bool(CanonicalWriter& writer, bool value);

// Deterministic ordering helpers. These give a total order over byte sequences
// and over structured keys without ever consulting a hash table.
[[nodiscard]] bool byte_less(const std::string& lhs, const std::string& rhs);
[[nodiscard]] bool byte_less(std::string_view lhs, std::string_view rhs);

// Joins already-canonical fragments with a single comma, in the order given.
[[nodiscard]] std::string join_canonical(const std::vector<std::string>& fragments);

// Encoding of a UTF-8 string as a JSON-style quoted token.
[[nodiscard]] std::string canonical_quote(std::string_view text);

// True when the byte sequence is well formed UTF-8. The coordinator refuses
// invalid UTF-8 in identity-bearing text instead of repairing it.
[[nodiscard]] bool is_valid_utf8(std::string_view text);

}  // namespace recovery

#endif  // RECOVERY_CANONICAL_HPP
