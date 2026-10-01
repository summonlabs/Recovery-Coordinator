#include "recovery/canonical.hpp"

#include <algorithm>

namespace recovery {
namespace {

constexpr char kHexDigits[] = "0123456789abcdef";

void append_hex4(std::string& out, unsigned value) {
    out.push_back('u');
    out.push_back(kHexDigits[(value >> 12) & 0xfu]);
    out.push_back(kHexDigits[(value >> 8) & 0xfu]);
    out.push_back(kHexDigits[(value >> 4) & 0xfu]);
    out.push_back(kHexDigits[value & 0xfu]);
}

}  // namespace

void CanonicalWriter::escaped(std::string_view text) {
    for (const char raw_char : text) {
        const auto byte = static_cast<unsigned char>(raw_char);
        switch (byte) {
            case '\\':
                out_.append("\\\\");
                break;
            case '"':
                out_.append("\\\"");
                break;
            case '\b':
                out_.append("\\b");
                break;
            case '\f':
                out_.append("\\f");
                break;
            case '\n':
                out_.append("\\n");
                break;
            case '\r':
                out_.append("\\r");
                break;
            case '\t':
                out_.append("\\t");
                break;
            default:
                if (byte < 0x20u) {
                    append_hex4(out_, byte);
                } else {
                    out_.push_back(raw_char);
                }
                break;
        }
    }
}

void CanonicalWriter::boolean(bool value) { out_.append(value ? "true" : "false"); }

void CanonicalWriter::signed_integer(std::int64_t value) { out_.append(std::to_string(value)); }

void CanonicalWriter::unsigned_integer(std::uint64_t value) { out_.append(std::to_string(value)); }

void CanonicalWriter::quoted(std::string_view text) {
    out_.push_back('"');
    escaped(text);
    out_.push_back('"');
}

void CanonicalWriter::byte_string(const std::uint8_t* data, std::size_t size) {
    out_.push_back('"');
    for (std::size_t i = 0; i < size; ++i) {
        out_.push_back(kHexDigits[data[i] >> 4]);
        out_.push_back(kHexDigits[data[i] & 0x0fu]);
    }
    out_.push_back('"');
}

void CanonicalWriter::digest(const Digest& value) {
    out_.push_back('"');
    out_.append(value.hex());
    out_.push_back('"');
}

void append_framed(CanonicalWriter& writer, std::string_view field) {
    writer.unsigned_integer(field.size());
    writer.raw(":");
    writer.raw(field);
}

void append_framed_u64(CanonicalWriter& writer, std::uint64_t value) { append_framed(writer, std::to_string(value)); }

void append_framed_i64(CanonicalWriter& writer, std::int64_t value) { append_framed(writer, std::to_string(value)); }

void append_framed_bool(CanonicalWriter& writer, bool value) { append_framed(writer, value ? "1" : "0"); }

bool byte_less(const std::string& lhs, const std::string& rhs) { return lhs < rhs; }

bool byte_less(std::string_view lhs, std::string_view rhs) { return lhs < rhs; }

std::string join_canonical(const std::vector<std::string>& fragments) {
    std::string out;
    for (std::size_t i = 0; i < fragments.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.append(fragments[i]);
    }
    return out;
}

std::string canonical_quote(std::string_view text) {
    CanonicalWriter writer;
    writer.quoted(text);
    return writer.take();
}

bool is_valid_utf8(std::string_view text) {
    std::size_t i = 0;
    const std::size_t n = text.size();
    while (i < n) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte < 0x80u) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        std::uint32_t code = 0;
        if ((byte & 0xe0u) == 0xc0u) {
            extra = 1;
            code = byte & 0x1fu;
        } else if ((byte & 0xf0u) == 0xe0u) {
            extra = 2;
            code = byte & 0x0fu;
        } else if ((byte & 0xf8u) == 0xf0u) {
            extra = 3;
            code = byte & 0x07u;
        } else {
            return false;
        }
        if (i + extra >= n) {
            return false;
        }
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xc0u) != 0x80u) {
                return false;
            }
            code = (code << 6) | (cont & 0x3fu);
        }
        if (extra == 1 && code < 0x80u) {
            return false;
        }
        if (extra == 2 && code < 0x800u) {
            return false;
        }
        if (extra == 3 && code < 0x10000u) {
            return false;
        }
        if (code > 0x10ffffu) {
            return false;
        }
        if (code >= 0xd800u && code <= 0xdfffu) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

}  // namespace recovery
