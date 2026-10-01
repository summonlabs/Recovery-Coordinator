#include "test.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "recovery/fileio.hpp"
#include "recovery/journal.hpp"

using namespace recovery;

// ---------------------------------------------------------------------------
// Durable journal
// ---------------------------------------------------------------------------
// The publication protocol is: append the complete record, flush it to the
// device, and only then report success. These tests assert the consequences:
// committed records survive, an incomplete tail is provably not committed and is
// cut away (when the caller allowed it), and damage that is not confined to the
// tail is refused as a whole rather than silently repaired.
//
// The byte offsets used below are the layout the header documents:
//   header: 96 bytes
//   record: 48 byte prefix, payload, 32 byte chain, 40 byte trailer

namespace {

constexpr std::uint64_t kFileHeaderSize = 96;
constexpr std::uint64_t kRecordPrefixSize = 48;
constexpr std::uint64_t kRecordChainSize = 32;
constexpr std::uint64_t kRecordTrailerSize = 40;
constexpr std::uint64_t kChecksumOffset = 40;

[[nodiscard]] std::string scratch(const char* name) { return std::string{"scratch/"} + name; }

void remove_journal(const std::string& path) {
    (void)fileio::remove_file(path);
    (void)fileio::remove_file(path + ".lock");
    (void)fileio::remove_file(path + ".staging");
}

[[nodiscard]] std::uint64_t record_size(std::uint64_t payloadLength) {
    return kRecordPrefixSize + payloadLength + kRecordChainSize + kRecordTrailerSize;
}

[[nodiscard]] std::uint64_t record_offset(std::uint64_t index, std::uint64_t payloadLength) {
    return kFileHeaderSize + (index - 1) * record_size(payloadLength);
}

void append_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::app);
    RC_REQUIRE_MSG(out.good(), "the test could not open the journal file for appending");
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    out.flush();
    RC_REQUIRE_MSG(out.good(), "the test could not append to the journal file");
}

void patch_byte(const std::string& path, std::uint64_t offset, std::uint8_t value) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
    RC_REQUIRE_MSG(file.good(), "the test could not open the journal file for patching");
    file.seekp(static_cast<std::streamoff>(offset));
    file.put(static_cast<char>(value));
    file.flush();
    RC_REQUIRE_MSG(file.good(), "the test could not patch the journal file");
}

[[nodiscard]] std::uint64_t size_of(const std::string& path) {
    const auto size = fileio::file_size(path);
    RC_REQUIRE_MSG(size.has_value(), "the journal file disappeared while the test was running");
    return *size;
}

[[nodiscard]] JournalOptions create_options() {
    JournalOptions options;
    options.createIfMissing = true;
    return options;
}

[[nodiscard]] JournalOptions reopen_options() {
    JournalOptions options;
    options.createIfMissing = false;
    return options;
}

// A record prefix that declares more payload than the file holds: the shape a
// crash leaves behind when the append was interrupted.
[[nodiscard]] std::vector<std::uint8_t> torn_record_prefix() {
    std::vector<std::uint8_t> bytes;
    const char magic[8] = {'R', 'C', 'V', 'R', 'E', 'C', '0', '1'};
    for (const char byte : magic) {
        bytes.push_back(static_cast<std::uint8_t>(byte));
    }
    bytes.push_back(1);  // record format version, little endian
    bytes.push_back(0);
    bytes.push_back(19);  // RecordKind::Checkpoint
    bytes.push_back(0);
    const std::uint32_t payloadLength = 4096;
    for (int i = 0; i < 4; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((payloadLength >> (8 * i)) & 0xffu));
    }
    for (int i = 0; i < 16; ++i) {
        bytes.push_back(0x5a);
    }
    const std::uint64_t sequence = 4;
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(static_cast<std::uint8_t>((sequence >> (8 * i)) & 0xffu));
    }
    for (int i = 0; i < 8; ++i) {
        bytes.push_back(0);
    }
    RC_REQUIRE_MSG(bytes.size() == kRecordPrefixSize, "the torn prefix must be one record prefix long");
    return bytes;
}

}  // namespace

RC_TEST(journal_appends_and_reads_many_records) {
    const std::string path = scratch("r2_journal_roundtrip.rcj");
    remove_journal(path);

    Journal journal;
    const JournalOpenInfo created = RC_REQUIRE_OK(journal.open(path, create_options(), TimePoint{1000}, false,
                                                              Duration{}));
    RC_REQUIRE_MSG(created.recordCount == 0, "a freshly created journal holds no records");
    RC_REQUIRE_MSG(created.lastSequence == 0, "a freshly created journal has sequence zero");
    RC_REQUIRE_MSG(!created.tornTailRepaired, "a freshly created journal has no torn tail");
    RC_REQUIRE_MSG(!created.uuidHex.empty(), "a journal must have an identity");

    std::vector<RecordKind> kinds;
    std::vector<std::string> payloads;
    std::vector<Digest> chains;
    for (std::uint64_t index = 1; index <= 64; ++index) {
        const RecordKind kind = static_cast<RecordKind>(1 + (index % 19));
        const std::string payload =
            (index % 5 == 0) ? std::string{} : std::string{"{\"n\":"} + std::to_string(index) + "}";
        const Digest chain = RC_REQUIRE_OK(journal.append(kind, payload));
        RC_REQUIRE_MSG(!chain.is_zero(), "every committed record must have a chain value");
        kinds.push_back(kind);
        payloads.push_back(payload);
        chains.push_back(chain);
    }

    RC_REQUIRE_EQ(journal.last_sequence(), std::uint64_t{64});
    RC_REQUIRE_EQ(journal.record_count(), std::uint64_t{64});

    const std::vector<JournalRecord> records = RC_REQUIRE_OK(journal.read_all());
    RC_REQUIRE_EQ(records.size(), std::size_t{64});
    for (std::size_t index = 0; index < records.size(); ++index) {
        RC_REQUIRE_EQ(records[index].sequence, static_cast<std::uint64_t>(index) + 1);
        RC_REQUIRE_MSG(records[index].kind == kinds[index], "a record kind must round trip unchanged");
        RC_REQUIRE_EQ(records[index].payload, payloads[index]);
        RC_REQUIRE_MSG(records[index].chain == chains[index], "the chain value must round trip unchanged");
        if (index > 0) {
            RC_REQUIRE_MSG(records[index].chain != records[index - 1].chain,
                           "the hash chain must differ for every record");
        }
    }

    const std::string uuid = journal.uuid_hex();
    const TimePoint createdAt = journal.created_at();
    journal.close();

    Journal reopened;
    const JournalOpenInfo again = RC_REQUIRE_OK(reopened.open(path, reopen_options(), TimePoint{2000}, false,
                                                             Duration{}));
    RC_REQUIRE_EQ(again.recordCount, std::uint64_t{64});
    RC_REQUIRE_EQ(again.lastSequence, std::uint64_t{64});
    RC_REQUIRE_EQ(again.uuidHex, uuid);
    RC_REQUIRE_MSG(again.createdAt == createdAt, "a reopened journal must report its original creation instant");
    RC_REQUIRE_MSG(!again.tornTailRepaired, "a cleanly closed journal has no torn tail");
    RC_REQUIRE_EQ(reopened.record_count(), std::uint64_t{64});
    RC_REQUIRE_EQ(reopened.last_sequence(), std::uint64_t{64});
    RC_REQUIRE_EQ(RC_REQUIRE_OK(reopened.read_all()).size(), std::size_t{64});

    // A reopened journal appends at the committed boundary, so the sequence
    // continues rather than restarting.
    RC_REQUIRE_OK(reopened.append(RecordKind::Checkpoint, std::string{"{\"n\":65}"}));
    RC_REQUIRE_EQ(reopened.last_sequence(), std::uint64_t{65});
    RC_REQUIRE_EQ(RC_REQUIRE_OK(reopened.read_all()).size(), std::size_t{65});
    reopened.close();
    remove_journal(path);
}

RC_TEST(journal_truncates_and_reports_a_torn_tail) {
    const std::string path = scratch("r2_journal_torn.rcj");
    remove_journal(path);

    {
        Journal journal;
        RC_REQUIRE_OK(journal.open(path, create_options(), TimePoint{1000}, false, Duration{}));
        for (int index = 1; index <= 3; ++index) {
            RC_REQUIRE_OK(journal.append(RecordKind::Checkpoint,
                                         std::string{"{\"n\":"} + std::to_string(index) + "}"));
        }
        journal.close();
    }

    const std::uint64_t committedBytes = size_of(path);
    const std::vector<std::uint8_t> torn = torn_record_prefix();
    append_bytes(path, torn);
    RC_REQUIRE_EQ(size_of(path), committedBytes + torn.size());

    Journal journal;
    const JournalOpenInfo info =
        RC_REQUIRE_OK(journal.open(path, reopen_options(), TimePoint{2000}, false, Duration{}));
    RC_REQUIRE_MSG(info.tornTailRepaired, "an incomplete final append must be reported as a torn tail");
    RC_REQUIRE_EQ(info.tornTailBytes, static_cast<std::uint64_t>(torn.size()));
    RC_REQUIRE_EQ(info.recordCount, std::uint64_t{3});
    RC_REQUIRE_EQ(info.lastSequence, std::uint64_t{3});
    RC_REQUIRE_EQ(size_of(path), committedBytes);

    const std::vector<JournalRecord> records = RC_REQUIRE_OK(journal.read_all());
    RC_REQUIRE_EQ(records.size(), std::size_t{3});
    RC_REQUIRE_EQ(records[2].payload, std::string{"{\"n\":3}"});

    // The repaired journal is usable: the next append is sequence 4.
    RC_REQUIRE_OK(journal.append(RecordKind::Checkpoint, std::string{"{\"n\":4}"}));
    RC_REQUIRE_EQ(journal.last_sequence(), std::uint64_t{4});
    journal.close();
    remove_journal(path);
}

RC_TEST(journal_refuses_a_torn_tail_when_truncation_is_not_permitted) {
    const std::string path = scratch("r2_journal_torn_refused.rcj");
    remove_journal(path);

    {
        Journal journal;
        RC_REQUIRE_OK(journal.open(path, create_options(), TimePoint{1000}, false, Duration{}));
        RC_REQUIRE_OK(journal.append(RecordKind::Checkpoint, std::string{"{\"n\":1}"}));
        journal.close();
    }
    const std::uint64_t committedBytes = size_of(path);
    const std::vector<std::uint8_t> torn = torn_record_prefix();
    append_bytes(path, torn);

    JournalOptions options = reopen_options();
    options.truncateTornTail = false;
    Journal journal;
    const Error error = RC_REQUIRE_ERR(journal.open(path, options, TimePoint{2000}, false, Duration{}),
                                       ErrorClass::CorruptState);
    RC_REQUIRE_EQ(error.code(), std::string{"journal.torn_tail"});
    RC_REQUIRE_MSG(!journal.is_open(), "a refused journal must not stay open");
    RC_REQUIRE_EQ(size_of(path), committedBytes + torn.size());
    remove_journal(path);
}

RC_TEST(journal_refuses_interior_corruption_as_a_whole) {
    const std::string path = scratch("r2_journal_interior.rcj");
    remove_journal(path);

    const std::uint64_t payloadLength = 16;
    {
        Journal journal;
        RC_REQUIRE_OK(journal.open(path, create_options(), TimePoint{1000}, false, Duration{}));
        for (int index = 1; index <= 4; ++index) {
            RC_REQUIRE_OK(journal.append(RecordKind::Checkpoint, std::string(payloadLength, 'x')));
        }
        journal.close();
    }
    const std::uint64_t intactBytes = size_of(path);

    // Break the magic of the second record while the third and fourth remain
    // committed records behind it.
    patch_byte(path, record_offset(2, payloadLength), static_cast<std::uint8_t>('X'));

    Journal journal;
    const Error error = RC_REQUIRE_ERR(journal.open(path, reopen_options(), TimePoint{2000}, false, Duration{}),
                                       ErrorClass::CorruptState);
    RC_REQUIRE_EQ(error.code(), std::string{"journal.interior_corruption"});
    RC_REQUIRE_MSG(!journal.is_open(), "a journal with interior damage must not be opened");
    RC_REQUIRE_EQ(size_of(path), intactBytes);
    remove_journal(path);
}

RC_TEST(journal_refuses_a_record_with_a_wrong_checksum) {
    const std::string path = scratch("r2_journal_checksum.rcj");
    remove_journal(path);

    const std::uint64_t payloadLength = 16;
    {
        Journal journal;
        RC_REQUIRE_OK(journal.open(path, create_options(), TimePoint{1000}, false, Duration{}));
        for (int index = 1; index <= 3; ++index) {
            RC_REQUIRE_OK(journal.append(RecordKind::Checkpoint, std::string(payloadLength, 'y')));
        }
        journal.close();
    }
    const std::uint64_t intactBytes = size_of(path);

    // The checksum field of the second record is inside the range the checksum
    // covers, so flipping it must be detected even though the hash chain (which
    // is computed with the field zeroed) still matches.
    const std::uint64_t checksumByte = record_offset(2, payloadLength) + kChecksumOffset;
    std::ifstream reader(path, std::ios::binary);
    RC_REQUIRE_MSG(reader.good(), "the test could not read the stored checksum");
    reader.seekg(static_cast<std::streamoff>(checksumByte));
    unsigned char stored = 0;
    reader.read(reinterpret_cast<char*>(&stored), 1);
    RC_REQUIRE_MSG(reader.good(), "the test could not read the stored checksum");
    reader.close();
    patch_byte(path, checksumByte, static_cast<std::uint8_t>(stored ^ 0xffu));

    Journal journal;
    const Error error = RC_REQUIRE_ERR(journal.open(path, reopen_options(), TimePoint{2000}, false, Duration{}),
                                       ErrorClass::CorruptState);
    RC_REQUIRE_EQ(error.code(), std::string{"journal.checksum"});
    RC_REQUIRE_EQ(size_of(path), intactBytes);
    remove_journal(path);
}

RC_TEST(journal_refuses_a_header_checksum_mismatch) {
    const std::string path = scratch("r2_journal_header.rcj");
    remove_journal(path);

    {
        Journal journal;
        RC_REQUIRE_OK(journal.open(path, create_options(), TimePoint{1000}, false, Duration{}));
        RC_REQUIRE_OK(journal.append(RecordKind::Checkpoint, std::string{"{\"n\":1}"}));
        journal.close();
    }
    const std::uint64_t intactBytes = size_of(path);
    const std::uint64_t headerChecksumOffset = 60;
    patch_byte(path, headerChecksumOffset, 0x00);

    Journal journal;
    const Error error = RC_REQUIRE_ERR(journal.open(path, reopen_options(), TimePoint{2000}, false, Duration{}),
                                       ErrorClass::CorruptState);
    RC_REQUIRE_EQ(error.code(), std::string{"journal.header_crc"});
    RC_REQUIRE_MSG(!journal.is_open(), "a journal whose header does not verify must not be opened");
    RC_REQUIRE_EQ(size_of(path), intactBytes);
    remove_journal(path);
}

RC_TEST(a_second_journal_cannot_take_the_single_writer_lock) {
    const std::string path = scratch("r2_journal_lock.rcj");
    remove_journal(path);

    Journal first;
    RC_REQUIRE_OK(first.open(path, create_options(), TimePoint{1000}, false, Duration{}));
    RC_REQUIRE_OK(first.append(RecordKind::Checkpoint, std::string{"{\"n\":1}"}));

    Journal second;
    const Error error = RC_REQUIRE_ERR(second.open(path, create_options(), TimePoint{1000}, false, Duration{}),
                                       ErrorClass::Contended);
    RC_REQUIRE_EQ(error.code(), std::string{"lock.contended"});
    RC_REQUIRE_MSG(!second.is_open(), "a contended journal must not be open");

    first.close();

    const JournalOpenInfo info =
        RC_REQUIRE_OK(second.open(path, reopen_options(), TimePoint{2000}, false, Duration{}));
    RC_REQUIRE_MSG(second.is_open(), "the lock must be released when the first writer closed the journal");
    RC_REQUIRE_EQ(info.recordCount, std::uint64_t{1});
    RC_REQUIRE_EQ(info.lastSequence, std::uint64_t{1});
    second.close();
    remove_journal(path);
}

RC_TEST(journal_rejects_an_empty_path) {
    Journal journal;
    const Error error =
        RC_REQUIRE_ERR(journal.open(std::string{}, create_options(), TimePoint{}, false, Duration{}),
                       ErrorClass::InvalidArgument);
    RC_REQUIRE_EQ(error.code(), std::string{"journal.path"});
    RC_REQUIRE_MSG(!journal.is_open(), "a journal with no path must not be open");
}
