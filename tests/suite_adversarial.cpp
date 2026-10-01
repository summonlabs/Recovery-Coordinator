#include "test.hpp"

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "recovery/canonical.hpp"
#include "recovery/error.hpp"
#include "recovery/fileio.hpp"
#include "recovery/journal.hpp"
#include "recovery/json.hpp"

using namespace recovery;

namespace {

constexpr std::uint64_t kNow = 1767225600000000000ull;
constexpr std::size_t kFileHeaderSize = 96;
constexpr std::size_t kRecordPrefixSize = 48;
constexpr std::size_t kRecordChainSize = 32;
constexpr std::size_t kRecordTrailerSize = 40;

// ---------------------------------------------------------------------------
// Journal construction and byte level surgery
// ---------------------------------------------------------------------------
// A journal is written through the real writer, closed, and then damaged byte
// by byte. The damage is expressed in terms of the layout the journal header
// documents, so a shift in the format makes these tests fail rather than
// silently damage the wrong field.
// The scratch directory is shared with every other process that runs this
// binary, so nothing here may assume a file name is its own. Every test gets a
// directory that names the test and the process, which makes two concurrent
// copies of the suite - and two agents running it at once - independent.
[[nodiscard]] std::uint64_t process_token() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

[[nodiscard]] std::string scratch_path(const std::string& name) {
    const std::string directory =
        "scratch/adversarial/" + name + "-" + std::to_string(process_token());
    auto created = fileio::ensure_parent_directory(directory + "/placeholder");
    RC_REQUIRE_MSG(created.has_value(), "scratch directory: " + created.error().describe());
    return directory + "/journal.rcj";
}

void remove_if_present(const std::string& path) {
    if (fileio::exists(path)) {
        auto removed = fileio::remove_file(path);
        RC_REQUIRE_MSG(removed.has_value(), "remove " + path + ": " + removed.error().describe());
    }
    const std::string lock = path + ".lock";
    if (fileio::exists(lock)) {
        auto removed = fileio::remove_file(lock);
        RC_REQUIRE_MSG(removed.has_value(), "remove " + lock + ": " + removed.error().describe());
    }
}

// Writes a valid journal with the given number of equally sized records and
// returns its path.
[[nodiscard]] std::string write_journal(const std::string& name, std::size_t records) {
    const std::string path = scratch_path(name);
    remove_if_present(path);
    Journal journal;
    JournalOptions options;
    options.createIfMissing = true;
    auto opened = journal.open(path, options, TimePoint{kNow}, false, Duration{});
    RC_REQUIRE_MSG(opened.has_value(), "journal create: " + opened.error().describe());
    for (std::size_t index = 0; index < records; ++index) {
        const std::string payload = "checkpoint-payload-" + std::to_string(index) + "-tail";
        auto appended = journal.append(RecordKind::Checkpoint, payload);
        RC_REQUIRE_MSG(appended.has_value(), "journal append: " + appended.error().describe());
    }
    RC_REQUIRE_EQ(journal.record_count(), static_cast<std::uint64_t>(records));
    journal.close();
    return path;
}

struct JournalImage {
    std::vector<std::uint8_t> bytes{};
    // Byte offset of every record, in sequence order.
    std::vector<std::size_t> offsets{};
    std::size_t stride{0};
};

[[nodiscard]] std::uint16_t peek_u16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return static_cast<std::uint16_t>(bytes[offset] | (static_cast<std::uint16_t>(bytes[offset + 1]) << 8));
}

[[nodiscard]] std::uint32_t peek_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(bytes[offset + i]) << (8u * static_cast<unsigned>(i));
    }
    return value;
}

[[nodiscard]] std::uint64_t peek_u64(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8u * static_cast<unsigned>(i));
    }
    return value;
}

void poke_u16(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint16_t value) {
    bytes[offset] = static_cast<std::uint8_t>(value & 0xffu);
    bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
}

void poke_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
    for (std::size_t i = 0; i < 4; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (8u * static_cast<unsigned>(i))) & 0xffu);
    }
}

void poke_u64(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint64_t value) {
    for (std::size_t i = 0; i < 8; ++i) {
        bytes[offset + i] = static_cast<std::uint8_t>((value >> (8u * static_cast<unsigned>(i))) & 0xffu);
    }
}

[[nodiscard]] const std::vector<std::uint8_t>& kRecordMagic() {
    static const std::vector<std::uint8_t> magic{'R', 'C', 'V', 'R', 'E', 'C', '0', '1'};
    return magic;
}

[[nodiscard]] JournalImage load_image(const std::string& path) {
    auto read = fileio::read_file(path, 1ull << 20);
    RC_REQUIRE_MSG(read.has_value(), "read " + path + ": " + read.error().describe());
    JournalImage image;
    image.bytes = std::move(read.value());
    RC_REQUIRE(image.bytes.size() > kFileHeaderSize);
    std::size_t position = kFileHeaderSize;
    while (position + kRecordPrefixSize <= image.bytes.size()) {
        if (std::memcmp(image.bytes.data() + position, kRecordMagic().data(), kRecordMagic().size()) != 0) {
            break;
        }
        const std::uint32_t payloadLength = peek_u32(image.bytes, position + 12);
        image.offsets.push_back(position);
        if (image.stride == 0) {
            image.stride = kRecordPrefixSize + payloadLength + kRecordChainSize + kRecordTrailerSize;
        }
        position += kRecordPrefixSize + payloadLength + kRecordChainSize + kRecordTrailerSize;
    }
    return image;
}

void store_image(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    auto published = fileio::publish_atomically(path, bytes);
    RC_REQUIRE_MSG(published.has_value(), "publish damaged journal: " + published.error().describe());
}

// ---------------------------------------------------------------------------
// Documented refusal codes
// ---------------------------------------------------------------------------
// Every code the journal reader is documented to use for a refused file. A
// damaged journal must be refused with exactly one of these and never with a
// generic failure.
[[nodiscard]] const std::set<std::string>& documented_journal_codes() {
    static const std::set<std::string> codes{
        "journal.header_truncated", "journal.magic",          "journal.version",
        "journal.header_size",      "journal.header_crc",     "journal.record_length",
        "journal.sequence",         "journal.chain",          "journal.checksum",
        "journal.torn_tail",        "journal.interior_corruption", "journal.size_mismatch",
        "journal.append_offset",    "journal.read_trailer",   "journal.read_chain",
        "journal.walk",             "journal.staging_present"};
    return codes;
}

// Opens a damaged journal and requires a refusal with exactly the expected
// documented code and error class.
void expect_open_refused(const std::string& path, const char* expectedCode) {
    Journal journal;
    JournalOptions options;
    options.createIfMissing = false;
    auto opened = journal.open(path, options, TimePoint{kNow}, false, Duration{});
    RC_REQUIRE_MSG(!opened.has_value(), std::string{"the damaged journal was accepted; expected refusal "} +
                                            expectedCode);
    const Error& error = opened.error();
    const std::string actual = error.code();
    RC_REQUIRE_MSG(documented_journal_codes().count(actual) == 1,
                   "the refusal code is not one of the documented codes: " + error.describe());
    RC_REQUIRE_MSG(error.cls() == ErrorClass::CorruptState,
                   "a damaged journal must be refused as CorruptState, got " + error.describe());
    RC_REQUIRE_MSG(actual == std::string{expectedCode},
                   "expected refusal " + std::string{expectedCode} + ", got " + error.describe());
}

// ---------------------------------------------------------------------------
// JSON decoder refusals
// ---------------------------------------------------------------------------
[[nodiscard]] const std::set<std::string>& documented_json_codes() {
    static const std::set<std::string> codes{"json.string",  "json.unterminated", "json.control",
                                             "json.escape",  "json.surrogate",    "json.number",
                                             "json.array",   "json.object",       "json.utf8",
                                             "json.duplicate", "json.truncated",  "json.literal",
                                             "json.trailing", "json.depth"};
    return codes;
}

void expect_json_refused(const std::string& text, const char* expectedCode, const char* what) {
    json::Decoder decoder(text);
    auto value = decoder.decode();
    RC_REQUIRE_MSG(!value.has_value(),
                   std::string{"the decoder accepted "} + what + ": " + json::encode(json::Value{text}));
    const Error& error = value.error();
    const std::string actual = error.code();
    RC_REQUIRE_MSG(documented_json_codes().count(actual) == 1,
                   std::string{"the JSON refusal code is not documented for "} + what + ": " + error.describe());
    RC_REQUIRE_MSG(error.cls() == ErrorClass::CorruptState,
                   std::string{"the decoder must refuse "} + what + " as CorruptState, got " + error.describe());
    RC_REQUIRE_MSG(actual == std::string{expectedCode},
                   std::string{"expected "} + expectedCode + " for " + what + ", got " + error.describe());
}

}  // namespace

// ---------------------------------------------------------------------------
// Journal: header damage
// ---------------------------------------------------------------------------
RC_TEST(adversarial_journal_truncated_header_is_refused) {
    const std::string path = scratch_path("truncated-header");
    remove_if_present(path);
    const std::vector<std::uint8_t> stub(40, 0);
    store_image(path, stub);
    expect_open_refused(path, "journal.header_truncated");
}

RC_TEST(adversarial_journal_bad_magic_is_refused) {
    const std::string path = write_journal("bad-magic", 3);
    JournalImage image = load_image(path);
    image.bytes[0] = static_cast<std::uint8_t>('X');
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.magic");
}

RC_TEST(adversarial_journal_wrong_version_is_refused) {
    const std::string path = write_journal("wrong-version", 3);
    JournalImage image = load_image(path);
    poke_u16(image.bytes, 8, 2);
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.version");
}

RC_TEST(adversarial_journal_wrong_header_size_is_refused) {
    const std::string path = write_journal("wrong-header-size", 3);
    JournalImage image = load_image(path);
    poke_u16(image.bytes, 10, static_cast<std::uint16_t>(kFileHeaderSize * 2));
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.header_size");
}

RC_TEST(adversarial_journal_bad_header_checksum_is_refused) {
    const std::string path = write_journal("bad-header-checksum", 3);
    JournalImage image = load_image(path);
    // Offset 44 is the low byte of createdAt, which the header checksum covers.
    image.bytes[44] = static_cast<std::uint8_t>(image.bytes[44] ^ 0x01u);
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.header_crc");
}

// ---------------------------------------------------------------------------
// Journal: record damage
// ---------------------------------------------------------------------------
RC_TEST(adversarial_journal_oversized_payload_length_is_refused) {
    const std::string path = write_journal("oversized-payload", 3);
    JournalImage image = load_image(path);
    RC_REQUIRE(image.offsets.size() == 3);
    // A declared payload length beyond the maximum record size is not a torn
    // append: it is a record the format does not allow.
    poke_u32(image.bytes, image.offsets[0] + 12, 0x7fffffffu);
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.record_length");
}

RC_TEST(adversarial_journal_payload_length_beyond_the_file_is_refused) {
    const std::string path = write_journal("payload-beyond-file", 3);
    JournalImage image = load_image(path);
    RC_REQUIRE(image.offsets.size() == 3);
    // The declared payload does not fit in the file, and a committed record
    // follows it: this is interior damage, refused as a whole.
    poke_u32(image.bytes, image.offsets[0] + 12, static_cast<std::uint32_t>(image.bytes.size()));
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.interior_corruption");
}

RC_TEST(adversarial_journal_record_magic_damage_is_refused_as_a_whole) {
    const std::string path = write_journal("record-magic", 3);
    JournalImage image = load_image(path);
    RC_REQUIRE(image.offsets.size() == 3);
    image.bytes[image.offsets[0]] = static_cast<std::uint8_t>('X');
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.interior_corruption");
}

RC_TEST(adversarial_journal_missing_trailer_is_refused) {
    const std::string path = write_journal("missing-trailer", 3);
    JournalImage image = load_image(path);
    RC_REQUIRE(image.offsets.size() == 3);
    const std::uint32_t payloadLength = peek_u32(image.bytes, image.offsets[0] + 12);
    const std::size_t trailer = image.offsets[0] + kRecordPrefixSize + payloadLength + kRecordChainSize;
    image.bytes[trailer] = static_cast<std::uint8_t>('X');
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.interior_corruption");
}

RC_TEST(adversarial_journal_missing_trailer_on_the_final_record_is_a_torn_tail) {
    // The same damage on the final record is provably an interrupted append, so
    // it is truncated and reported instead of refused: the committed prefix is
    // intact and the torn bytes are not part of any generation.
    const std::string path = write_journal("torn-final-trailer", 3);
    JournalImage image = load_image(path);
    RC_REQUIRE(image.offsets.size() == 3);
    const std::uint32_t payloadLength = peek_u32(image.bytes, image.offsets[2] + 12);
    const std::size_t trailer = image.offsets[2] + kRecordPrefixSize + payloadLength + kRecordChainSize;
    image.bytes[trailer] = static_cast<std::uint8_t>('X');
    const std::size_t damagedSize = image.bytes.size();
    store_image(path, image.bytes);

    Journal journal;
    JournalOptions options;
    options.createIfMissing = false;
    auto opened = journal.open(path, options, TimePoint{kNow}, false, Duration{});
    RC_REQUIRE_MSG(opened.has_value(), "torn tail open: " + opened.error().describe());
    RC_REQUIRE(opened.value().tornTailRepaired);
    RC_REQUIRE_EQ(opened.value().tornTailBytes, static_cast<std::uint64_t>(damagedSize - image.offsets[2]));
    RC_REQUIRE_EQ(opened.value().recordCount, static_cast<std::uint64_t>(2));
    RC_REQUIRE_EQ(opened.value().lastSequence, static_cast<std::uint64_t>(2));
    auto records = journal.read_all();
    RC_REQUIRE_MSG(records.has_value(), "torn tail read: " + records.error().describe());
    RC_REQUIRE_EQ(records.value().size(), static_cast<std::size_t>(2));
    RC_REQUIRE_EQ(records.value()[0].sequence, static_cast<std::uint64_t>(1));
    RC_REQUIRE_EQ(records.value()[1].sequence, static_cast<std::uint64_t>(2));
    const auto size = fileio::file_size(path);
    RC_REQUIRE(size.has_value());
    RC_REQUIRE_EQ(*size, static_cast<std::uint64_t>(image.offsets[2]));
    // A fresh open of the repaired journal reports the same committed prefix and
    // no further repair.
    journal.close();
    Journal reopened;
    auto second = reopened.open(path, options, TimePoint{kNow}, false, Duration{});
    RC_REQUIRE_MSG(second.has_value(), "repaired open: " + second.error().describe());
    RC_REQUIRE(!second.value().tornTailRepaired);
    RC_REQUIRE_EQ(second.value().recordCount, static_cast<std::uint64_t>(2));
}

RC_TEST(adversarial_journal_wrong_record_checksum_is_refused) {
    const std::string path = write_journal("bad-checksum", 3);
    JournalImage image = load_image(path);
    RC_REQUIRE(image.offsets.size() == 3);
    image.bytes[image.offsets[0] + 40] = static_cast<std::uint8_t>(image.bytes[image.offsets[0] + 40] ^ 0xffu);
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.checksum");
}

RC_TEST(adversarial_journal_wrong_chain_is_refused) {
    const std::string path = write_journal("bad-chain", 3);
    JournalImage image = load_image(path);
    RC_REQUIRE(image.offsets.size() == 3);
    const std::uint32_t payloadLength = peek_u32(image.bytes, image.offsets[0] + 12);
    const std::size_t chain = image.offsets[0] + kRecordPrefixSize + payloadLength;
    image.bytes[chain] = static_cast<std::uint8_t>(image.bytes[chain] ^ 0xffu);
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.chain");
}

RC_TEST(adversarial_journal_records_out_of_sequence_are_refused) {
    const std::string path = write_journal("out-of-sequence", 3);
    JournalImage image = load_image(path);
    RC_REQUIRE(image.offsets.size() == 3);
    // The sequence number sits at offset 32 inside the record prefix.
    const std::uint64_t sequence = peek_u64(image.bytes, image.offsets[1] + 32);
    RC_REQUIRE_EQ(sequence, static_cast<std::uint64_t>(2));
    poke_u64(image.bytes, image.offsets[1] + 32, sequence + 5);
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.sequence");
}

RC_TEST(adversarial_journal_spliced_record_from_another_journal_is_refused) {
    // A record copied from a different journal carries another journal's uuid,
    // its own sequence number and a chain that does not include the preceding
    // record. The reader refuses it rather than adopting foreign state.
    const std::string donor = write_journal("splice-donor", 2);
    const std::string path = write_journal("splice-target", 2);
    JournalImage donorImage = load_image(donor);
    JournalImage image = load_image(path);
    RC_REQUIRE(donorImage.offsets.size() == 2);
    RC_REQUIRE(image.offsets.size() == 2);
    const std::size_t donorRecordSize = donorImage.stride;
    const std::size_t targetRecordSize = image.stride;
    RC_REQUIRE_EQ(donorRecordSize, targetRecordSize);
    std::memcpy(image.bytes.data() + image.offsets[1], donorImage.bytes.data() + donorImage.offsets[1],
                donorRecordSize);
    store_image(path, image.bytes);
    expect_open_refused(path, "journal.chain");
}

// ---------------------------------------------------------------------------
// JSON decoder
// ---------------------------------------------------------------------------
RC_TEST(adversarial_json_decoder_refuses_duplicate_and_trailing_content) {
    expect_json_refused("{\"a\":1,\"a\":2}", "json.duplicate", "a duplicate object member");
    expect_json_refused("{\"a\":1} {\"b\":2}", "json.trailing", "trailing content");
    expect_json_refused("[1,2]x", "json.trailing", "trailing garbage after an array");
}

RC_TEST(adversarial_json_decoder_refuses_non_canonical_numbers) {
    expect_json_refused("01", "json.number", "a leading zero");
    expect_json_refused("-01", "json.number", "a negative leading zero");
    expect_json_refused("1.5", "json.number", "a floating point member");
    expect_json_refused("1e3", "json.number", "an exponent member");
    expect_json_refused("-0", "json.number", "negative zero");
    expect_json_refused("{\"a\":1.0}", "json.number", "a floating point object member");
    expect_json_refused("18446744073709551616", "json.number", "an integer beyond the unsigned range");
    expect_json_refused("-9223372036854775809", "json.number", "an integer below the signed range");
    expect_json_refused("-", "json.number", "a lone minus sign");
}

RC_TEST(adversarial_json_decoder_refuses_unpaired_surrogates) {
    expect_json_refused("\"\\ud800\"", "json.surrogate", "a high surrogate without a low surrogate");
    expect_json_refused("\"\\udc00\"", "json.surrogate", "a lone low surrogate");
    expect_json_refused("\"\\ud800\\u0041\"", "json.surrogate", "a high surrogate followed by a non surrogate");
}

RC_TEST(adversarial_json_decoder_refuses_invalid_utf8_keys) {
    std::string text = "{\"";
    text.push_back(static_cast<char>(0xff));
    text += "\":1}";
    expect_json_refused(text, "json.utf8", "a key that is not valid UTF-8");

    std::string truncated = "{\"";
    truncated.push_back(static_cast<char>(0xc3));
    truncated += "\":1}";
    expect_json_refused(truncated, "json.utf8", "a truncated UTF-8 sequence in a key");

    std::string overlong = "{\"";
    overlong.push_back(static_cast<char>(0xc0));
    overlong.push_back(static_cast<char>(0xaf));
    overlong += "\":1}";
    expect_json_refused(overlong, "json.utf8", "an overlong UTF-8 encoding in a key");
}

RC_TEST(adversarial_json_decoder_refuses_over_deep_nesting) {
    std::string deep;
    for (int i = 0; i < 64; ++i) {
        deep.push_back('[');
    }
    for (int i = 0; i < 64; ++i) {
        deep.push_back(']');
    }
    expect_json_refused(deep, "json.depth", "nesting beyond the depth limit");

    std::string deepObject;
    for (int i = 0; i < 64; ++i) {
        deepObject += "{\"a\":";
    }
    deepObject += "1";
    for (int i = 0; i < 64; ++i) {
        deepObject.push_back('}');
    }
    expect_json_refused(deepObject, "json.depth", "object nesting beyond the depth limit");
}

RC_TEST(adversarial_json_decoder_refuses_structural_damage) {
    expect_json_refused("{\"a\":}", "json.number", "a member with no value");
    expect_json_refused("{\"a\":1,}", "json.string", "a trailing comma in an object");
    expect_json_refused("[1,]", "json.number", "a trailing comma in an array");
    expect_json_refused("[1 2]", "json.array", "a missing comma in an array");
    expect_json_refused("{\"a\" 1}", "json.object", "a missing colon");
    expect_json_refused("{\"a\":1", "json.object", "an unterminated object");
    expect_json_refused("[1,2", "json.array", "an unterminated array");
    expect_json_refused("\"abc", "json.unterminated", "an unterminated string");
    expect_json_refused("\"\\q\"", "json.escape", "an unknown escape");
    expect_json_refused("\"\\u00zz\"", "json.escape", "a non hexadecimal escape");
    expect_json_refused("", "json.truncated", "an empty input");
    expect_json_refused("tru", "json.literal", "a truncated literal");
    expect_json_refused("trux", "json.literal", "an unknown literal");
}

RC_TEST(adversarial_json_decoder_accepts_exactly_what_the_encoder_produces) {
    // The refusals above are not a blanket refusal: everything the canonical
    // encoder produces is accepted and round-trips byte for byte, including C0
    // control escapes and multi byte UTF-8 keys.
    json::Object object;
    object["control"] = json::Value{std::string{"\x01\x1f"}};
    object["caf\xc3\xa9"] = json::Value{static_cast<unsigned long long>(7)};
    object["negative"] = json::Value{static_cast<long long>(-9007199254740993LL)};
    object["nested"] = json::Value{json::Array{json::Value{true}, json::Value{nullptr}, json::Value{"text"}}};
    const std::string encoded = json::encode(json::Value{object});
    json::Decoder decoder(encoded);
    auto value = decoder.decode();
    RC_REQUIRE_MSG(value.has_value(), "the encoder output was refused: " + value.error().describe());
    RC_REQUIRE_EQ(json::encode(value.value()), encoded);
    // Trailing whitespace is insignificant and is therefore accepted. The
    // decoded text is kept alive: the decoder reads a view of it.
    const std::string padded = "  " + encoded + "\n";
    json::Decoder paddedDecoder(padded);
    auto paddedValue = paddedDecoder.decode();
    RC_REQUIRE_MSG(paddedValue.has_value(), "padded input: " + paddedValue.error().describe());
    RC_REQUIRE_EQ(json::encode(paddedValue.value()), encoded);
}
