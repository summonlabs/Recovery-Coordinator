#ifndef RECOVERY_JOURNAL_HPP
#define RECOVERY_JOURNAL_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "recovery/digest.hpp"
#include "recovery/epoch.hpp"
#include "recovery/error.hpp"
#include "recovery/lockfile.hpp"
#include "recovery/time.hpp"

namespace recovery {

// ---------------------------------------------------------------------------
// Durable journal
// ---------------------------------------------------------------------------
// One journal file is one authoritative generation of coordinator state. The
// format has an explicit identifier and version, a checksum over every record,
// and a hash chain that makes reordering, removal, and splicing detectable.
//
// Publication protocol for every record:
//   1. append the complete record (header, payload, trailer);
//   2. flush the file to the device;
//   3. return success.
//
// A record is committed when its trailer is on the device. On reopen, a
// trailing byte range that is not a complete, checksum-correct, chain-correct
// record is a torn tail: it is provably not committed, so it is truncated away
// and reported. Corruption that is not confined to the tail is refused
// outright. There is never a partially applied generation.
//
// Byte layout
//   header  : magic(8) version(2) headerSize(2) uuid(16) createdAt(8)
//             formatEpoch(8) crc32c(4) reserved(48)  -> 96 bytes
//   record  : magic(8) version(2) kind(2) payloadLength(4) uuid(16)
//             sequence(8) crc32c(4) reserved(4) payload(N)
//             chain(32) trailerMagic(8)  -> 64 + N + 40 bytes
enum class RecordKind : std::uint16_t {
    BeginEpoch = 1,
    PlanPublished = 2,
    PlanSuperseded = 3,
    ExecutionStarted = 4,
    ExecutionLifecycle = 5,
    AuthorityAccepted = 6,
    AuthorityRetired = 7,
    EvidenceAccepted = 8,
    AttemptCommitted = 9,
    AttemptDispatched = 10,
    AttemptAcknowledged = 11,
    AttemptObserved = 12,
    AttemptAssessed = 13,
    AttemptSuperseded = 14,
    StepStateChanged = 15,
    ExecutionFenced = 16,
    ExecutionClosed = 17,
    CounterChanged = 18,
    Checkpoint = 19,
};

[[nodiscard]] std::string_view to_string(RecordKind value);
[[nodiscard]] std::optional<RecordKind> record_kind_from_string(std::string_view text);

struct JournalRecord {
    std::uint64_t sequence{0};
    RecordKind kind{RecordKind::Checkpoint};
    Digest chain{};
    std::string payload{};
};

struct JournalOpenInfo {
    std::string path{};
    std::string uuidHex{};
    TimePoint createdAt{};
    Epoch lastEpoch{};
    std::uint64_t lastSequence{0};
    std::uint64_t recordCount{0};
    // True when a torn tail was found and cut away. The committed generation is
    // unaffected; this is reported so that a crash that interrupted an append
    // is visible rather than hidden.
    bool tornTailRepaired{false};
    std::uint64_t tornTailBytes{0};
};

struct JournalOptions {
    bool createIfMissing{false};
    bool truncateTornTail{true};
    bool removeStagingResidue{true};
    bool verifySize{true};
    std::uint64_t maxJournalBytes{1ull << 32};
    std::uint64_t maxRecordBytes{1ull << 24};
};

class Journal {
public:
    // Defined in the translation unit that also defines Impl.
    Journal();
    ~Journal();

    Journal(const Journal&) = delete;
    Journal& operator=(const Journal&) = delete;
    Journal(Journal&&) noexcept;
    Journal& operator=(Journal&&) noexcept;

    // Opens (or creates) the journal and takes the single-writer lock on
    // "<path>.lock". A live holder causes a Contended error. The lock is
    // released by the kernel if the holder dies, so a successor is never
    // blocked by a dead process.
    [[nodiscard]] Result<JournalOpenInfo> open(const std::string& path, const JournalOptions& options, TimePoint now,
                                               bool waitForLock, Duration lockBudget);

    // Appends one record and flushes it to the device. Returns the chain value
    // of the committed record.
    [[nodiscard]] Result<Digest> append(RecordKind kind, std::string_view payload);

    // Reads the committed record stream and verifies the checksum and hash
    // chain of every record. Any mismatch is an error.
    [[nodiscard]] Result<std::vector<JournalRecord>> read_all() const;

    // Reads records without verifying the chain. Used only by diagnostic tools
    // that must be able to describe a damaged journal.
    [[nodiscard]] Result<std::vector<JournalRecord>> read_all_unverified() const;

    // Records the epoch this incarnation claimed. The epoch must be strictly
    // greater than every epoch the journal has seen; the coordinator never
    // silently adopts a predecessor's authority.
    [[nodiscard]] Result<Epoch> claim_epoch(Epoch requested, std::string_view incarnation);

    [[nodiscard]] bool is_open() const noexcept;
    [[nodiscard]] std::string path() const;
    [[nodiscard]] std::string lock_path() const;
    [[nodiscard]] std::uint64_t last_sequence() const noexcept;
    [[nodiscard]] Epoch last_epoch() const noexcept;
    [[nodiscard]] std::string uuid_hex() const;
    [[nodiscard]] TimePoint created_at() const noexcept;
    [[nodiscard]] std::uint64_t record_count() const noexcept;

    void close();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace recovery

#endif  // RECOVERY_JOURNAL_HPP
