#ifndef RECOVERY_JSON_HPP
#define RECOVERY_JSON_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "recovery/digest.hpp"
#include "recovery/error.hpp"

namespace recovery {
namespace json {

// ---------------------------------------------------------------------------
// A deliberately small JSON value model used only for durable, canonical
// coordinator state.
// ---------------------------------------------------------------------------
// The model is not a general purpose JSON library:
//
//   * objects keep their members in a std::map, so iteration order is a
//     property of the key bytes, never of insertion or of a hash table;
//   * integers are split into signed and unsigned so that a value round-trips
//     without loss and without sign ambiguity;
//   * there is no floating point member at all. Recovery decisions never
//     depend on binary floating point.
//
// Encoding is RFC 8259 with a canonical member order (ascending key bytes), no
// insignificant whitespace, and \\u escapes only for C0 control characters.
// Two Json values that differ in any structural way always encode to different
// byte strings.
class Value;
using Array = std::vector<Value>;
using Object = std::map<std::string, Value>;

class Value {
public:
    enum class Kind { Null, Boolean, Signed, Unsigned, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool value) : kind_(Kind::Boolean), boolean_(value) {}
    Value(int value) : kind_(Kind::Signed), signed_(value) {}
    Value(long long value) : kind_(Kind::Signed), signed_(value) {}
    Value(unsigned value) : kind_(Kind::Unsigned), unsigned_(value) {}
    Value(unsigned long long value) : kind_(Kind::Unsigned), unsigned_(value) {}
    Value(std::string value) : kind_(Kind::String), string_(std::move(value)) {}
    Value(const char* value) : kind_(Kind::String), string_(value) {}
    Value(Array value) : kind_(Kind::Array), array_(std::move(value)) {}
    Value(Object value) : kind_(Kind::Object), object_(std::move(value)) {}

    [[nodiscard]] Kind kind() const noexcept { return kind_; }
    [[nodiscard]] bool is_null() const noexcept { return kind_ == Kind::Null; }

    [[nodiscard]] bool boolean() const noexcept { return boolean_; }
    [[nodiscard]] std::int64_t signed_value() const noexcept { return signed_; }
    [[nodiscard]] std::uint64_t unsigned_value() const noexcept { return unsigned_; }
    [[nodiscard]] const std::string& string() const noexcept { return string_; }
    [[nodiscard]] const Array& array() const noexcept { return array_; }
    [[nodiscard]] const Object& object() const noexcept { return object_; }

    [[nodiscard]] Array& array() noexcept { return array_; }
    [[nodiscard]] Object& object() noexcept { return object_; }

    void set(const std::string& key, Value value) { object_[key] = std::move(value); }
    void push(Value value) { array_.push_back(std::move(value)); }

private:
    Kind kind_{Kind::Null};
    bool boolean_{false};
    std::int64_t signed_{0};
    std::uint64_t unsigned_{0};
    std::string string_;
    Array array_;
    Object object_;
};

[[nodiscard]] std::string encode(const Value& value);

// ---------------------------------------------------------------------------
// Strict decoder
// ---------------------------------------------------------------------------
// The decoder refuses anything the encoder would not have produced:
// duplicate keys, non-ASCII escapes, leading zeros, "-0", out of range
// integers, unpaired surrogates, trailing content, and invalid UTF-8 in keys.
// A span index is produced for every member so that a caller can re-encode a
// member and compare it byte for byte with the bytes it consumed.
class Decoder {
public:
    explicit Decoder(std::string_view text) : text_(text) {}

    [[nodiscard]] Result<Value> decode();

    // Byte range of the value that the member with the given key was decoded
    // from. Only valid after a successful decode().
    [[nodiscard]] std::optional<std::pair<std::size_t, std::size_t>> span_of(std::string_view key_path) const;

private:
    [[nodiscard]] Result<Value> parse_value(std::string_view path, unsigned depth);
    [[nodiscard]] Result<Value> parse_object(std::string_view path, unsigned depth);
    [[nodiscard]] Result<Value> parse_array(std::string_view path, unsigned depth);
    [[nodiscard]] Result<std::string> parse_string();
    [[nodiscard]] Result<std::string> parse_number(Value& out);
    void skip_whitespace();

    [[nodiscard]] bool at_end() const { return position_ >= text_.size(); }
    [[nodiscard]] char peek() const { return at_end() ? '\0' : text_[position_]; }

    std::string_view text_;
    std::size_t position_{0};
    std::map<std::string, std::pair<std::size_t, std::size_t>> spans_;
};

}  // namespace json

// Canonical-JSON fragment helpers used by the record encoders. Each helper
// appends a framed, unambiguous encoding to a plain string.
void json_put_string(std::string& out, std::string_view value);
void json_put_digest(std::string& out, const Digest& value);

}  // namespace recovery

#endif  // RECOVERY_JSON_HPP
