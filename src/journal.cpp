#include "recovery/journal.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include "recovery/canonical.hpp"
#include "recovery/fileio.hpp"
#include "recovery/sha256.hpp"

#include "fileio_internal.hpp"

namespace recovery {
namespace {

constexpr std::array<std::uint8_t, 8> kFileMagic = {'R', 'C', 'V', 'J', 'R', 'N', 'L', '1'};
constexpr std::array<std::uint8_t, 8> kRecordMagic = {'R', 'C', 'V', 'R', 'E', 'C', '0', '1'};
constexpr std::array<std::uint8_t, 8> kTrailerMagic = {'R', 'C', 'V', 'T', 'R', 'L', '0', '1'};
constexpr std::uint16_t kJournalFormatVersion = 1;
constexpr std::uint16_t kRecordFormatVersion = 1;
constexpr std::size_t kFileHeaderSize = 96;
constexpr std::size_t kRecordPrefixSize = 48;
constexpr std::size_t kRecordChainSize = 32;
constexpr std::size_t kRecordTrailerSize = 40;
constexpr std::size_t kChecksumOffset = 40;
// Byte offset of the record sequence number inside the prefix: magic(8) +
// version(2) + kind(2) + payloadLength(4) + uuid(16).
constexpr std::size_t kSequenceOffset = 32;

void put_u16(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value & 0xffu));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xffu));
}

void put_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    }
}

void put_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu));
    }
}

void write_u16(std::vector<std::uint8_t>& out, std::size_t offset, std::uint16_t value) {
    out[offset] = static_cast<std::uint8_t>(value & 0xffu);
    out[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xffu);
}

void write_u32(std::vector<std::uint8_t>& out, std::size_t offset, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) {
        out[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
    }
}

void write_u64(std::vector<std::uint8_t>& out, std::size_t offset, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[offset + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xffu);
    }
}

[[nodiscard]] std::uint16_t read_u16(const std::vector<std::uint8_t>& data, std::size_t offset) {
    return static_cast<std::uint16_t>(data[offset] | (static_cast<std::uint16_t>(data[offset + 1]) << 8));
}

[[nodiscard]] std::uint32_t read_u32(const std::vector<std::uint8_t>& data, std::size_t offset) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) {
        value |= static_cast<std::uint32_t>(data[offset + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return value;
}

[[nodiscard]] std::uint64_t read_u64(const std::vector<std::uint8_t>& data, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(data[offset + static_cast<std::size_t>(i)]) << (8 * i);
    }
    return value;
}

[[nodiscard]] bool magic_matches(const std::vector<std::uint8_t>& data, std::size_t offset,
                                 const std::array<std::uint8_t, 8>& magic) {
    if (offset + magic.size() > data.size()) {
        return false;
    }
    return std::memcmp(data.data() + offset, magic.data(), magic.size()) == 0;
}

[[nodiscard]] std::string uuid_to_hex(const std::uint8_t* bytes) { return to_hex(bytes, 16); }

// Structural validity of a record at an offset: magic, format version, a
// declared payload length that fits, a trailer magic, and a checksum that
// matches. The hash chain and the sequence number are deliberately not checked,
// because this helper decides whether committed records exist beyond a damaged
// range.
[[nodiscard]] bool record_looks_valid(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    if (offset + kRecordPrefixSize > bytes.size()) {
        return false;
    }
    if (!magic_matches(bytes, offset, kRecordMagic)) {
        return false;
    }
    if (read_u16(bytes, offset + 8) != kRecordFormatVersion) {
        return false;
    }
    const std::uint32_t payloadLength = read_u32(bytes, offset + 12);
    const std::uint64_t totalLength =
        kRecordPrefixSize + static_cast<std::uint64_t>(payloadLength) + kRecordChainSize + kRecordTrailerSize;
    if (totalLength > bytes.size() - offset) {
        return false;
    }
    const std::size_t trailerOffset = offset + kRecordPrefixSize + payloadLength + kRecordChainSize;
    if (!magic_matches(bytes, trailerOffset, kTrailerMagic)) {
        return false;
    }
    // The published checksum covers the prefix with its own checksum field
    // zeroed, the payload and the chain value, exactly as the writer computed
    // it. The probe must repeat that derivation: hashing the bytes as they sit
    // on disk, checksum field included, can never reproduce the stored value,
    // and a probe that can never match classifies every interior corruption as
    // a torn tail and silently truncates committed records away.
    std::array<std::uint8_t, kRecordPrefixSize> prefix{};
    std::memcpy(prefix.data(), bytes.data() + offset, prefix.size());
    for (std::size_t i = 0; i < 4; ++i) {
        prefix[kChecksumOffset + i] = 0;
    }
    std::uint32_t checksum = fileio::crc32c_extend(0xffffffffu, prefix.data(), prefix.size());
    checksum = fileio::crc32c_extend(checksum, bytes.data() + offset + kRecordPrefixSize,
                                     payloadLength + kRecordChainSize);
    checksum ^= 0xffffffffu;
    return read_u32(bytes, offset + kChecksumOffset) == checksum;
}

[[nodiscard]] std::vector<std::uint8_t> build_file_header(const std::array<std::uint8_t, 16>& uuid,
                                                          TimePoint createdAt, Epoch epoch) {
    std::vector<std::uint8_t> header(kFileHeaderSize, 0);
    std::memcpy(header.data(), kFileMagic.data(), kFileMagic.size());
    write_u16(header, 8, kJournalFormatVersion);
    write_u16(header, 10, static_cast<std::uint16_t>(kFileHeaderSize));
    std::memcpy(header.data() + 12, uuid.data(), uuid.size());
    write_u64(header, 44, createdAt.nanos());
    write_u64(header, 52, epoch.value());
    const std::uint32_t crc = fileio::crc32c(header.data(), 56);
    write_u32(header, 60, crc);
    return header;
}

}  // namespace

std::string_view to_string(RecordKind value) {
    switch (value) {
        case RecordKind::BeginEpoch:
            return "begin_epoch";
        case RecordKind::PlanPublished:
            return "plan_published";
        case RecordKind::PlanSuperseded:
            return "plan_superseded";
        case RecordKind::ExecutionStarted:
            return "execution_started";
        case RecordKind::ExecutionLifecycle:
            return "execution_lifecycle";
        case RecordKind::AuthorityAccepted:
            return "authority_accepted";
        case RecordKind::AuthorityRetired:
            return "authority_retired";
        case RecordKind::EvidenceAccepted:
            return "evidence_accepted";
        case RecordKind::AttemptCommitted:
            return "attempt_committed";
        case RecordKind::AttemptDispatched:
            return "attempt_dispatched";
        case RecordKind::AttemptAcknowledged:
            return "attempt_acknowledged";
        case RecordKind::AttemptObserved:
            return "attempt_observed";
        case RecordKind::AttemptAssessed:
            return "attempt_assessed";
        case RecordKind::AttemptSuperseded:
            return "attempt_superseded";
        case RecordKind::StepStateChanged:
            return "step_state_changed";
        case RecordKind::ExecutionFenced:
            return "execution_fenced";
        case RecordKind::ExecutionClosed:
            return "execution_closed";
        case RecordKind::CounterChanged:
            return "counter_changed";
        case RecordKind::Checkpoint:
            return "checkpoint";
    }
    return "unknown";
}

std::optional<RecordKind> record_kind_from_string(std::string_view text) {
    for (std::uint16_t raw = 1; raw <= 19; ++raw) {
        const auto kind = static_cast<RecordKind>(raw);
        if (to_string(kind) == text) {
            return kind;
        }
    }
    return std::nullopt;
}

struct Journal::Impl {
    JournalOptions options{};
    std::string path{};
    std::string lockPath{};
    std::array<std::uint8_t, 16> uuid{};
    TimePoint createdAt{};
    Epoch lastEpoch{};
    std::uint64_t lastSequence{0};
    std::uint64_t recordCount{0};
    Digest lastChain{};
    LockFile lock{};
    fileio::AppendHandle append{};
    bool open{false};
};

Journal::Journal() = default;

Journal::~Journal() { close(); }

Journal::Journal(Journal&& other) noexcept : impl_(std::move(other.impl_)) {}

Journal& Journal::operator=(Journal&& other) noexcept {
    if (this != &other) {
        close();
        impl_ = std::move(other.impl_);
    }
    return *this;
}
Result<JournalOpenInfo> Journal::open(const std::string& path, const JournalOptions& options, TimePoint now,
                                       bool waitForLock, Duration lockBudget) {
    if (impl_ != nullptr && impl_->open) {
        return errors::precondition_failed("journal.already_open", "the journal is already open in this object");
    }
    if (path.empty()) {
        return errors::invalid_argument("journal.path", "journal path is empty");
    }

    auto directory = fileio::ensure_parent_directory(path);
    if (!directory.has_value()) {
        return directory.error();
    }

    auto impl = std::make_unique<Impl>();
    impl->options = options;
    impl->path = path;
    impl->lockPath = path + ".lock";

    auto lockResult = waitForLock ? impl->lock.acquire(impl->lockPath, lockBudget, now)
                                  : impl->lock.try_acquire(impl->lockPath);
    if (!lockResult.has_value()) {
        return lockResult.error();
    }

    const std::string stagingPath = path + ".staging";
    if (fileio::exists(stagingPath)) {
        if (!options.removeStagingResidue) {
            return errors::corrupt_state("journal.staging_present",
                                         "a staging file is present; the previous publish did not complete");
        }
        auto removed = fileio::remove_file(stagingPath);
        if (!removed.has_value()) {
            return removed.error();
        }
    }

    std::vector<std::uint8_t> bytes;
    if (!fileio::exists(path)) {
        if (!options.createIfMissing) {
            return errors::not_found("journal.missing", "the journal does not exist and creation was not requested");
        }
        // Deterministic identity: the journal identity is derived from the fully
        // qualified path, the creation instant, and a fixed domain separator. No
        // random device, no host name, no process identifier.
        CanonicalWriter writer;
        append_framed(writer, "recovery.journal_identity.v1");
        append_framed(writer, fileio::absolute_path(path));
        append_framed_u64(writer, now.nanos());
        const Digest identity = writer.digest_of();
        std::array<std::uint8_t, 16> uuid{};
        std::copy(identity.bytes().begin(), identity.bytes().begin() + 16, uuid.begin());
        bytes = build_file_header(uuid, now, Epoch{0});
        auto published = fileio::publish_atomically(path, bytes);
        if (!published.has_value()) {
            return published.error();
        }
    } else {
        auto read = fileio::read_file(path, options.maxJournalBytes);
        if (!read.has_value()) {
            return read.error();
        }
        bytes = std::move(read.value());
    }

    if (bytes.size() < kFileHeaderSize) {
        return errors::corrupt_state("journal.header_truncated", "the journal header is truncated");
    }
    if (!magic_matches(bytes, 0, kFileMagic)) {
        return errors::corrupt_state("journal.magic", "the file is not a Recovery Coordinator journal");
    }
    if (read_u16(bytes, 8) != kJournalFormatVersion) {
        return errors::corrupt_state("journal.version", "the journal format version is not supported");
    }
    if (read_u16(bytes, 10) != kFileHeaderSize) {
        return errors::corrupt_state("journal.header_size", "the journal header size field is not supported");
    }
    if (fileio::crc32c(bytes.data(), 56) != read_u32(bytes, 60)) {
        return errors::corrupt_state("journal.header_crc", "the journal header checksum does not match");
    }

    std::memcpy(impl->uuid.data(), bytes.data() + 12, 16);
    impl->createdAt = TimePoint{read_u64(bytes, 44)};
    impl->lastEpoch = Epoch{read_u64(bytes, 52)};
    impl->lastSequence = 0;
    impl->recordCount = 0;
    impl->lastChain = Digest{};

    // The record walk. Every record must be structurally valid, checksum
    // correct, chain correct, and exactly one past its predecessor. Anything
    // else stops the walk; whether the remainder is a torn tail or real damage
    // is decided after the walk.
    std::uint64_t validEnd = kFileHeaderSize;
    bool torn = false;
    std::size_t position = kFileHeaderSize;
    while (position < bytes.size()) {
        const std::size_t remaining = bytes.size() - position;
        if (remaining < kRecordPrefixSize) {
            torn = true;
            break;
        }
        if (!magic_matches(bytes, position, kRecordMagic)) {
            torn = true;
            break;
        }
        const std::uint16_t version = read_u16(bytes, position + 8);
        const std::uint32_t payloadLength = read_u32(bytes, position + 12);
        if (version != kRecordFormatVersion) {
            torn = true;
            break;
        }
        if (payloadLength > options.maxRecordBytes) {
            return errors::corrupt_state("journal.record_length", "a record declares an unsupported payload length");
        }
        const std::uint64_t totalLength = kRecordPrefixSize + static_cast<std::uint64_t>(payloadLength) +
                                          kRecordChainSize + kRecordTrailerSize;
        if (totalLength > remaining) {
            torn = true;
            break;
        }
        const std::size_t trailerOffset = position + kRecordPrefixSize + payloadLength + kRecordChainSize;
        if (!magic_matches(bytes, trailerOffset, kTrailerMagic)) {
            torn = true;
            break;
        }
        const std::uint32_t storedCrc = read_u32(bytes, position + kChecksumOffset);
        const std::uint64_t sequence = read_u64(bytes, position + kSequenceOffset);
        if (sequence != impl->lastSequence + 1) {
            return errors::corrupt_state("journal.sequence", "record sequence numbers are not contiguous");
        }

        std::array<std::uint8_t, Digest::kBytes> chainBytes{};
        // The chain value sits immediately after the prefix and the payload; the
        // trailer magic follows it.
        std::memcpy(chainBytes.data(), bytes.data() + position + kRecordPrefixSize + payloadLength,
                    Digest::kBytes);
        const Digest chain{chainBytes};

        // Verifier. The writer derives the chain value before the checksum is
        // patched, so the chain covers the prefix with a zero checksum field;
        // the checksum then covers the prefix as it stands on disk (checksum
        // field zeroed, so the value is a fixed point), the payload and the
        // chain value. Repeating both derivations over the bytes that were read
        // reproduces exactly the two values the writer published.
        // On-disk bytes of this record: prefix, payload and chain value.
        std::vector<std::uint8_t> recordBytes(bytes.data() + position,
                                              bytes.data() + position + kRecordPrefixSize + payloadLength +
                                                  kRecordChainSize);
        // Checksum: over the same bytes with the checksum field zeroed, which is
        // how the writer computed it.
        std::vector<std::uint8_t> checksumInput = recordBytes;
        for (std::size_t i = 0; i < 4; ++i) {
            checksumInput[kChecksumOffset + i] = 0;
        }
        // Chain: over the prefix (checksum field zeroed), the payload and the
        // reserved chain region as zeros, which is what the writer hashed before
        // it wrote the chain value.
        std::vector<std::uint8_t> chainInput = checksumInput;
        for (std::size_t i = kRecordPrefixSize + payloadLength; i < chainInput.size(); ++i) {
            chainInput[i] = 0;
        }
        // The writer derives the chain value over the previous chain value, the
        // prefix (checksum field zeroed) and the payload; the chain value itself
        // and the trailer magic are outside that range.
        const Digest expectedChain{[&] {
            Sha256 hasher;
            hasher.update(impl->lastChain.bytes().data(), Digest::kBytes);
            hasher.update(chainInput.data(), chainInput.size());
            return hasher.finish();
        }()};
        if (expectedChain != chain) {
            return errors::corrupt_state("journal.chain", "the record hash chain does not match");
        }
        if (fileio::crc32c(checksumInput.data(), checksumInput.size()) != storedCrc) {
            return errors::corrupt_state("journal.checksum", "the record checksum does not match");
        }

        // The highest epoch the journal has seen is derived from the committed
        // BeginEpoch records, not from the header. The header carries an epoch
        // field that is written once when the file is created and never
        // maintained afterwards, so trusting it would under-report the epochs a
        // successor must exceed and make a legitimate restart look like a
        // corrupted journal. The journal writes this record shape itself, in
        // claim_epoch, so reading it back is not a layering violation: the
        // journal owns both ends of the format.
        if (static_cast<RecordKind>(read_u16(bytes, position + 10)) == RecordKind::BeginEpoch) {
            const char* payload = reinterpret_cast<const char*>(bytes.data() + position + kRecordPrefixSize);
            const std::size_t length = payloadLength;
            if (length > 9 && std::string_view{payload, 9} == "{\"epoch\":") {
                std::uint64_t epoch = 0;
                std::size_t index = 9;
                bool digits = false;
                while (index < length && payload[index] >= '0' && payload[index] <= '9') {
                    epoch = epoch * 10u + static_cast<std::uint64_t>(payload[index] - '0');
                    digits = true;
                    ++index;
                }
                if (digits && index < length && payload[index] == ',') {
                    if (epoch > impl->lastEpoch.value()) {
                        impl->lastEpoch = Epoch{epoch};
                    }
                }
            }
        }

        impl->lastSequence = sequence;
        impl->lastChain = chain;
        ++impl->recordCount;
        position += static_cast<std::size_t>(totalLength);
        validEnd = position;
    }

    JournalOpenInfo info;
    info.path = path;
    info.uuidHex = uuid_to_hex(impl->uuid.data());
    info.createdAt = impl->createdAt;
    info.lastEpoch = impl->lastEpoch;
    info.lastSequence = impl->lastSequence;
    info.recordCount = impl->recordCount;

    if (torn) {
        // Distinguish a torn tail from interior corruption. A torn tail is an
        // incomplete final append: after the damage there is no further record
        // that satisfies the record structure and checksum. Damage followed by a
        // structurally valid record means committed records exist beyond it, and
        // such a journal is refused as a whole rather than silently truncated.
        for (std::size_t probe = validEnd; probe < bytes.size(); ++probe) {
            if (record_looks_valid(bytes, probe)) {
                return errors::corrupt_state("journal.interior_corruption",
                                             "damage was found before committed records; the journal is refused");
            }
        }
        if (!options.truncateTornTail) {
            return errors::corrupt_state("journal.torn_tail",
                                         "a torn tail was found and truncation was not permitted");
        }
        auto truncated = fileio::truncate_file(path, validEnd);
        if (!truncated.has_value()) {
            return truncated.error();
        }
        info.tornTailRepaired = true;
        info.tornTailBytes = static_cast<std::uint64_t>(bytes.size() - validEnd);
    }

    if (options.verifySize && fileio::file_size(path).value_or(validEnd) != validEnd) {
        return errors::corrupt_state("journal.size_mismatch",
                                     "the journal size does not match the committed record boundary");
    }

    auto opened = impl->append.open(path, true);
    if (!opened.has_value()) {
        return opened.error();
    }
    if (impl->append.size() != validEnd) {
        return errors::corrupt_state("journal.append_offset",
                                     "the append offset does not match the committed record boundary");
    }

    impl->open = true;
    impl_ = std::move(impl);
    return info;
}

// The chain binds the previous chain value, the complete record prefix as it
// appears on disk (checksum field included), and the payload. The checksum
// covers the prefix, the payload and the chain value. Both are computed over
// bytes that are present in the record buffer at the time they are computed.
Result<Digest> Journal::append(RecordKind kind, std::string_view payload) {
    if (impl_ == nullptr || !impl_->open) {
        return errors::precondition_failed("journal.closed", "the journal is not open");
    }
    if (payload.size() > impl_->options.maxRecordBytes) {
        return errors::limit_exceeded("journal.payload", "the record payload exceeds the configured limit");
    }
    if (impl_->lastSequence == 0xffffffffffffffffull) {
        return errors::overflow("journal.sequence", "the journal sequence counter is exhausted");
    }

    const std::uint64_t sequence = impl_->lastSequence + 1;

    // Fixed-size staging buffer. Every field sits at a constant offset, so the
    // byte layout the checksum and the chain value are computed over is the
    // exact layout that reaches the device. A verifier reconstructs the same
    // buffer from the bytes it read and repeats the two derivations.
    const std::size_t payloadLength = payload.size();
    const std::size_t recordSize = kRecordPrefixSize + payloadLength + kRecordChainSize + kRecordTrailerSize;
    std::vector<std::uint8_t> record(recordSize, 0);
    std::memcpy(record.data(), kRecordMagic.data(), kRecordMagic.size());
    write_u16(record, 8, kRecordFormatVersion);
    write_u16(record, 10, static_cast<std::uint16_t>(kind));
    write_u32(record, 12, static_cast<std::uint32_t>(payloadLength));
    std::memcpy(record.data() + 16, impl_->uuid.data(), impl_->uuid.size());
    write_u64(record, kSequenceOffset, sequence);
    // The checksum field stays zero for both derivations, which is what makes
    // the published checksum verifiable: it covers a range that contains the
    // checksum field as zero.
    std::memcpy(record.data() + kRecordPrefixSize, payload.data(), payloadLength);

    std::array<std::uint8_t, Digest::kBytes> chainBytes{};
    {
        Sha256 hasher;
        hasher.update(impl_->lastChain.bytes().data(), Digest::kBytes);
        hasher.update(record.data(), kRecordPrefixSize + payloadLength + kRecordChainSize);
        chainBytes = hasher.finish();
    }
    std::memcpy(record.data() + kRecordPrefixSize + payloadLength, chainBytes.data(), chainBytes.size());

    const std::uint32_t crc =
        fileio::crc32c(record.data(), kRecordPrefixSize + payloadLength + kRecordChainSize);
    write_u32(record, kChecksumOffset, crc);

    std::memcpy(record.data() + kRecordPrefixSize + payloadLength + kRecordChainSize, kTrailerMagic.data(),
                kTrailerMagic.size());

    auto written = impl_->append.append_and_flush(record);
    if (!written.has_value()) {
        return written.error();
    }

    impl_->lastSequence = sequence;
    impl_->lastChain = Digest{chainBytes};
    ++impl_->recordCount;
    return impl_->lastChain;
}

namespace {

[[nodiscard]] JournalRecord decode_record(const std::vector<std::uint8_t>& bytes, std::size_t position) {
    JournalRecord record;
    const std::uint32_t payloadLength = read_u32(bytes, position + 12);
    record.sequence = read_u64(bytes, position + kSequenceOffset);
    record.kind = static_cast<RecordKind>(read_u16(bytes, position + 10));
    record.payload.assign(reinterpret_cast<const char*>(bytes.data() + position + kRecordPrefixSize), payloadLength);
    std::array<std::uint8_t, Digest::kBytes> chainBytes{};
    // The chain value sits immediately after the prefix and the payload; the
    // trailer magic follows it.
    std::memcpy(chainBytes.data(), bytes.data() + position + kRecordPrefixSize + payloadLength, Digest::kBytes);
    record.chain = Digest{chainBytes};
    return record;
}

// Returns the byte offset one past the last structurally complete record.
[[nodiscard]] std::size_t walk_records(const std::vector<std::uint8_t>& bytes, std::vector<JournalRecord>& out) {
    std::size_t position = kFileHeaderSize;
    while (position + kRecordPrefixSize <= bytes.size()) {
        if (!magic_matches(bytes, position, kRecordMagic)) {
            break;
        }
        const std::uint16_t version = read_u16(bytes, position + 8);
        const std::uint32_t payloadLength = read_u32(bytes, position + 12);
        if (version != kRecordFormatVersion) {
            break;
        }
        const std::uint64_t totalLength = kRecordPrefixSize + static_cast<std::uint64_t>(payloadLength) +
                                          kRecordChainSize + kRecordTrailerSize;
        if (totalLength > bytes.size() - position) {
            break;
        }
        const std::size_t trailerOffset = position + kRecordPrefixSize + payloadLength + kRecordChainSize;
        if (!magic_matches(bytes, trailerOffset, kTrailerMagic)) {
            break;
        }
        out.push_back(decode_record(bytes, position));
        position += static_cast<std::size_t>(totalLength);
    }
    return position;
}

}  // namespace

Result<std::vector<JournalRecord>> Journal::read_all() const {
    if (impl_ == nullptr) {
        return errors::precondition_failed("journal.closed", "the journal is not open");
    }
    auto bytes = fileio::read_file(impl_->path, impl_->options.maxJournalBytes);
    if (!bytes.has_value()) {
        return bytes.error();
    }
    std::vector<JournalRecord> records;
    records.reserve(static_cast<std::size_t>(impl_->recordCount));
    std::size_t position = kFileHeaderSize;
    Digest chain{};
    std::uint64_t sequence = 0;
    while (position + kRecordPrefixSize <= bytes.value().size()) {
        if (!magic_matches(bytes.value(), position, kRecordMagic)) {
            break;
        }
        const std::uint32_t payloadLength = read_u32(bytes.value(), position + 12);
        const std::uint64_t totalLength =
            kRecordPrefixSize + static_cast<std::uint64_t>(payloadLength) + kRecordChainSize + kRecordTrailerSize;
        if (totalLength > bytes.value().size() - position) {
            break;
        }
        if (!magic_matches(bytes.value(), position + kRecordPrefixSize + payloadLength + kRecordChainSize,
                           kTrailerMagic)) {
            return errors::corrupt_state("journal.read_trailer", "a record trailer is missing");
        }
        JournalRecord record = decode_record(bytes.value(), position);
        std::vector<std::uint8_t> recordBytes(bytes.value().data() + position,
                                              bytes.value().data() + position + kRecordPrefixSize + payloadLength +
                                                  kRecordChainSize);
        std::vector<std::uint8_t> checksumInput = recordBytes;
        for (std::size_t i = 0; i < 4; ++i) {
            checksumInput[kChecksumOffset + i] = 0;
        }
        std::vector<std::uint8_t> chainInput = checksumInput;
        for (std::size_t i = kRecordPrefixSize + payloadLength; i < chainInput.size(); ++i) {
            chainInput[i] = 0;
        }
        const Digest expectedChain{[&] {
            Sha256 hasher;
            hasher.update(chain.bytes().data(), Digest::kBytes);
            hasher.update(chainInput.data(), chainInput.size());
            return hasher.finish();
        }()};
        if (expectedChain != record.chain || record.sequence != sequence + 1 ||
            fileio::crc32c(checksumInput.data(), checksumInput.size()) !=
                read_u32(bytes.value(), position + kChecksumOffset)) {
            return errors::corrupt_state("journal.read_chain", "the journal changed while it was being read");
        }

        chain = record.chain;
        sequence = record.sequence;
        records.push_back(std::move(record));
        position += static_cast<std::size_t>(totalLength);
    }
    return records;
}

Result<std::vector<JournalRecord>> Journal::read_all_unverified() const {
    if (impl_ == nullptr) {
        return errors::precondition_failed("journal.closed", "the journal is not open");
    }
    auto bytes = fileio::read_file(impl_->path, impl_->options.maxJournalBytes);
    if (!bytes.has_value()) {
        return bytes.error();
    }
    std::vector<JournalRecord> records;
    const std::size_t end = walk_records(bytes.value(), records);
    if (end > bytes.value().size()) {
        return errors::corrupt_state("journal.walk", "the record walk left the file");
    }
    return records;
}

Result<Epoch> Journal::claim_epoch(Epoch requested, std::string_view incarnation) {
    if (impl_ == nullptr || !impl_->open) {
        return errors::precondition_failed("journal.closed", "the journal is not open");
    }
    if (requested.value() <= impl_->lastEpoch.value()) {
        return errors::stale_authority("epoch.not_advancing",
                                       "the claimed epoch must be strictly greater than every epoch already seen");
    }
    CanonicalWriter writer;
    writer.raw("{\"epoch\":");
    writer.unsigned_integer(requested.value());
    writer.raw(",\"incarnation\":");
    writer.quoted(incarnation);
    writer.raw("}");
    auto chain = append(RecordKind::BeginEpoch, writer.str());
    if (!chain.has_value()) {
        return chain.error();
    }
    impl_->lastEpoch = requested;
    return requested;
}

bool Journal::is_open() const noexcept { return impl_ != nullptr && impl_->open; }

std::string Journal::path() const { return impl_ == nullptr ? std::string{} : impl_->path; }

std::string Journal::lock_path() const { return impl_ == nullptr ? std::string{} : impl_->lockPath; }

std::uint64_t Journal::last_sequence() const noexcept { return impl_ == nullptr ? 0 : impl_->lastSequence; }

Epoch Journal::last_epoch() const noexcept { return impl_ == nullptr ? Epoch{} : impl_->lastEpoch; }

std::string Journal::uuid_hex() const { return impl_ == nullptr ? std::string{} : uuid_to_hex(impl_->uuid.data()); }

TimePoint Journal::created_at() const noexcept { return impl_ == nullptr ? TimePoint{} : impl_->createdAt; }

std::uint64_t Journal::record_count() const noexcept { return impl_ == nullptr ? 0 : impl_->recordCount; }

void Journal::close() {
    if (impl_ != nullptr && impl_->open) {
        impl_->append.close();
        impl_->open = false;
        impl_->lock.release();
    }
    impl_.reset();
}

}  // namespace recovery
