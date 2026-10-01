#include "recovery/json.hpp"

#include <cstdio>
#include <limits>

#include "recovery/canonical.hpp"

namespace recovery {
namespace json {
namespace {

constexpr unsigned kMaxDepth = 48;

void encode_into(std::string& out, const Value& value);

void encode_string(std::string& out, std::string_view text) {
    out.push_back('"');
    for (const char raw_char : text) {
        const auto byte = static_cast<unsigned char>(raw_char);
        switch (byte) {
            case '"':
                out.append("\\\"");
                break;
            case '\\':
                out.append("\\\\");
                break;
            case '\b':
                out.append("\\b");
                break;
            case '\f':
                out.append("\\f");
                break;
            case '\n':
                out.append("\\n");
                break;
            case '\r':
                out.append("\\r");
                break;
            case '\t':
                out.append("\\t");
                break;
            default:
                if (byte < 0x20u) {
                    char buffer[8];
                    std::snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(byte));
                    out.append(buffer);
                } else {
                    out.push_back(raw_char);
                }
                break;
        }
    }
    out.push_back('"');
}

void encode_into(std::string& out, const Value& value) {
    switch (value.kind()) {
        case Value::Kind::Null:
            out.append("null");
            break;
        case Value::Kind::Boolean:
            out.append(value.boolean() ? "true" : "false");
            break;
        case Value::Kind::Signed:
            out.append(std::to_string(value.signed_value()));
            break;
        case Value::Kind::Unsigned:
            out.append(std::to_string(value.unsigned_value()));
            break;
        case Value::Kind::String:
            encode_string(out, value.string());
            break;
        case Value::Kind::Array: {
            out.push_back('[');
            bool first = true;
            for (const Value& element : value.array()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                encode_into(out, element);
            }
            out.push_back(']');
            break;
        }
        case Value::Kind::Object: {
            out.push_back('{');
            bool first = true;
            for (const auto& [key, member] : value.object()) {
                if (!first) {
                    out.push_back(',');
                }
                first = false;
                encode_string(out, key);
                out.push_back(':');
                encode_into(out, member);
            }
            out.push_back('}');
            break;
        }
    }
}

[[nodiscard]] bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

[[nodiscard]] unsigned hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<unsigned>(c - 'a' + 10);
    }
    return static_cast<unsigned>(c - 'A' + 10);
}

void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80u) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800u) {
        out.push_back(static_cast<char>(0xc0u | (code >> 6)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
    } else if (code < 0x10000u) {
        out.push_back(static_cast<char>(0xe0u | (code >> 12)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
    } else {
        out.push_back(static_cast<char>(0xf0u | (code >> 18)));
        out.push_back(static_cast<char>(0x80u | ((code >> 12) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | ((code >> 6) & 0x3fu)));
        out.push_back(static_cast<char>(0x80u | (code & 0x3fu)));
    }
}

}  // namespace

std::string encode(const Value& value) {
    std::string out;
    encode_into(out, value);
    return out;
}

void Decoder::skip_whitespace() {
    while (!at_end()) {
        const char c = text_[position_];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            ++position_;
        } else {
            break;
        }
    }
}

Result<std::string> Decoder::parse_string() {
    if (at_end() || text_[position_] != '"') {
        return errors::corrupt_state("json.string", "expected a string member");
    }
    ++position_;
    std::string out;
    while (true) {
        if (at_end()) {
            return errors::corrupt_state("json.unterminated", "string literal is not terminated");
        }
        const char c = text_[position_];
        if (c == '"') {
            ++position_;
            return out;
        }
        if (static_cast<unsigned char>(c) < 0x20u) {
            return errors::corrupt_state("json.control", "unescaped control character in string");
        }
        if (c != '\\') {
            out.push_back(c);
            ++position_;
            continue;
        }
        ++position_;
        if (at_end()) {
            return errors::corrupt_state("json.escape", "truncated escape sequence");
        }
        const char esc = text_[position_++];
        switch (esc) {
            case '"':
                out.push_back('"');
                break;
            case '\\':
                out.push_back('\\');
                break;
            case '/':
                out.push_back('/');
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                if (position_ + 4 > text_.size()) {
                    return errors::corrupt_state("json.escape", "truncated unicode escape");
                }
                std::uint32_t code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char digit = text_[position_ + static_cast<std::size_t>(i)];
                    if (!is_hex(digit)) {
                        return errors::corrupt_state("json.escape", "invalid unicode escape digit");
                    }
                    code = (code << 4) | hex_digit(digit);
                }
                position_ += 4;
                if (code >= 0xd800u && code <= 0xdbffu) {
                    if (position_ + 6 > text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
                        return errors::corrupt_state("json.surrogate", "high surrogate without low surrogate");
                    }
                    std::uint32_t low = 0;
                    for (int i = 0; i < 4; ++i) {
                        const char digit = text_[position_ + 2 + static_cast<std::size_t>(i)];
                        if (!is_hex(digit)) {
                            return errors::corrupt_state("json.escape", "invalid unicode escape digit");
                        }
                        low = (low << 4) | hex_digit(digit);
                    }
                    if (low < 0xdc00u || low > 0xdfffu) {
                        return errors::corrupt_state("json.surrogate", "high surrogate followed by a non low surrogate");
                    }
                    position_ += 6;
                    code = 0x10000u + ((code - 0xd800u) << 10) + (low - 0xdc00u);
                } else if (code >= 0xdc00u && code <= 0xdfffu) {
                    return errors::corrupt_state("json.surrogate", "lone low surrogate");
                }
                append_utf8(out, code);
                break;
            }
            default:
                return errors::corrupt_state("json.escape", "unknown escape sequence");
        }
    }
}

Result<std::string> Decoder::parse_number(Value& out) {
    const std::size_t start = position_;
    if (!at_end() && text_[position_] == '-') {
        ++position_;
    }
    if (at_end() || text_[position_] < '0' || text_[position_] > '9') {
        return errors::corrupt_state("json.number", "expected a digit");
    }
    if (text_[position_] == '0') {
        ++position_;
        if (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') {
            return errors::corrupt_state("json.number", "leading zero in integer literal");
        }
    } else {
        while (!at_end() && text_[position_] >= '0' && text_[position_] <= '9') {
            ++position_;
        }
    }
    if (!at_end() && (text_[position_] == '.' || text_[position_] == 'e' || text_[position_] == 'E')) {
        return errors::corrupt_state("json.number", "floating point members are not part of the canonical model");
    }
    const std::string literal{text_.substr(start, position_ - start)};
    if (literal == "-0") {
        return errors::corrupt_state("json.number", "negative zero is not part of the canonical model");
    }
    if (literal.front() == '-') {
        std::int64_t value = 0;
        for (std::size_t i = 1; i < literal.size(); ++i) {
            const int digit = literal[i] - '0';
            if (value < (std::numeric_limits<std::int64_t>::min() + digit) / 10) {
                return errors::corrupt_state("json.number", "integer literal out of range");
            }
            value = value * 10 - digit;
        }
        out = Value{static_cast<long long>(value)};
    } else {
        std::uint64_t value = 0;
        for (const char digit_char : literal) {
            const auto digit = static_cast<std::uint64_t>(digit_char - '0');
            if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) {
                return errors::corrupt_state("json.number", "integer literal out of range");
            }
            value = value * 10u + digit;
        }
        out = Value{static_cast<unsigned long long>(value)};
    }
    return std::string{};
}

Result<Value> Decoder::parse_array(std::string_view path, unsigned depth) {
    ++position_;  // consume '['
    Array array;
    skip_whitespace();
    if (!at_end() && text_[position_] == ']') {
        ++position_;
        return Value{std::move(array)};
    }
    std::size_t index = 0;
    while (true) {
        skip_whitespace();
        std::string childPath{path};
        childPath.push_back('/');
        childPath.append(std::to_string(index));
        auto element = parse_value(childPath, depth + 1);
        if (!element.has_value()) {
            return element.error();
        }
        array.push_back(std::move(element.value()));
        ++index;
        skip_whitespace();
        if (at_end()) {
            return errors::corrupt_state("json.array", "array literal is not terminated");
        }
        if (text_[position_] == ',') {
            ++position_;
            continue;
        }
        if (text_[position_] == ']') {
            ++position_;
            return Value{std::move(array)};
        }
        return errors::corrupt_state("json.array", "expected ',' or ']' in array");
    }
}

Result<Value> Decoder::parse_object(std::string_view path, unsigned depth) {
    ++position_;  // consume '{'
    Object object;
    skip_whitespace();
    if (!at_end() && text_[position_] == '}') {
        ++position_;
        return Value{std::move(object)};
    }
    while (true) {
        skip_whitespace();
        auto key = parse_string();
        if (!key.has_value()) {
            return key.error();
        }
        if (!is_valid_utf8(key.value())) {
            return errors::corrupt_state("json.utf8", "object member key is not valid UTF-8");
        }
        if (object.find(key.value()) != object.end()) {
            return errors::corrupt_state("json.duplicate", "duplicate object member key");
        }
        skip_whitespace();
        if (at_end() || text_[position_] != ':') {
            return errors::corrupt_state("json.object", "expected ':' after object member key");
        }
        ++position_;
        skip_whitespace();
        std::string childPath{path};
        childPath.push_back('/');
        childPath.append(key.value());
        const std::size_t valueStart = position_;
        auto member = parse_value(childPath, depth + 1);
        if (!member.has_value()) {
            return member.error();
        }
        spans_[childPath] = {valueStart, position_};
        object.emplace(key.value(), std::move(member.value()));
        skip_whitespace();
        if (at_end()) {
            return errors::corrupt_state("json.object", "object literal is not terminated");
        }
        if (text_[position_] == ',') {
            ++position_;
            continue;
        }
        if (text_[position_] == '}') {
            ++position_;
            return Value{std::move(object)};
        }
        return errors::corrupt_state("json.object", "expected ',' or '}' in object");
    }
}

Result<Value> Decoder::parse_value(std::string_view path, unsigned depth) {
    if (depth > kMaxDepth) {
        return errors::corrupt_state("json.depth", "nesting depth limit exceeded");
    }
    skip_whitespace();
    if (at_end()) {
        return errors::corrupt_state("json.truncated", "unexpected end of input");
    }
    const char c = text_[position_];
    switch (c) {
        case '{':
            return parse_object(path, depth);
        case '[':
            return parse_array(path, depth);
        case '"': {
            auto text = parse_string();
            if (!text.has_value()) {
                return text.error();
            }
            Value out{std::move(text.value())};
            return out;
        }
        case 't':
            if (text_.substr(position_, 4) == "true") {
                position_ += 4;
                return Value{true};
            }
            return errors::corrupt_state("json.literal", "invalid literal");
        case 'f':
            if (text_.substr(position_, 5) == "false") {
                position_ += 5;
                return Value{false};
            }
            return errors::corrupt_state("json.literal", "invalid literal");
        case 'n':
            if (text_.substr(position_, 4) == "null") {
                position_ += 4;
                return Value{};
            }
            return errors::corrupt_state("json.literal", "invalid literal");
        default: {
            Value out;
            auto parsed = parse_number(out);
            if (!parsed.has_value()) {
                return parsed.error();
            }
            return Result<Value>{std::move(out)};
        }
    }
}

Result<Value> Decoder::decode() {
    skip_whitespace();
    auto value = parse_value("", 0);
    if (!value.has_value()) {
        return value.error();
    }
    skip_whitespace();
    if (!at_end()) {
        return errors::corrupt_state("json.trailing", "trailing content after the top level value");
    }
    return value;
}

std::optional<std::pair<std::size_t, std::size_t>> Decoder::span_of(std::string_view key_path) const {
    const auto it = spans_.find(std::string{key_path});
    if (it == spans_.end()) {
        return std::nullopt;
    }
    return it->second;
}

}  // namespace json

void json_put_string(std::string& out, std::string_view value) { out.append(json::encode(json::Value{std::string{value}})); }

void json_put_digest(std::string& out, const Digest& value) { out.append(json::encode(json::Value{value.hex()})); }

}  // namespace recovery
