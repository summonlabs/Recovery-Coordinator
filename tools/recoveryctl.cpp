// recoveryctl: inspect and validate the durable state of a Recovery
// Coordinator deployment.
//
// The tool is deliberately read-only with respect to coordinator state. It can
// create a journal, dump it, verify its integrity, describe an engine snapshot,
// replay a committed journal into a snapshot, and print the plans a snapshot
// stores. It never claims durable authority and never dispatches anything: the
// replay below runs on an in-process copy of the committed records, so no
// subcommand of this tool writes a byte into a journal it did not create.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "recovery/adapters/synthetic.hpp"
#include "recovery/engine.hpp"
#include "recovery/error.hpp"
#include "recovery/fileio.hpp"
#include "recovery/journal.hpp"
#include "recovery/json.hpp"
#include "recovery/ports.hpp"

namespace {

constexpr std::uint64_t kMaxSnapshotBytes = 512ull * 1024ull * 1024ull;

void print_usage() {
    std::fprintf(stdout,
                 "recoveryctl <command> [options]\n"
                 "\n"
                 "commands:\n"
                 "  verify   --journal <path> [--allow-create]\n"
                 "           Verifies the journal header, every record checksum and the hash chain.\n"
                 "  dump     --journal <path> [--json]\n"
                 "           Prints every committed record.\n"
                 "  check    --snapshot <path>\n"
                 "           Validates that a snapshot file is canonical JSON and reports its top level keys.\n"
                 "  plan     --snapshot <path> [--plan <plan-id>]\n"
                 "           Prints the canonical JSON of every plan the snapshot stores, or of exactly\n"
                 "           the one plan named by --plan.\n"
                 "  snapshot --journal <path> [--world <path>] [--epoch <n>] [--incarnation <name>]\n"
                 "           Replays the committed journal with the synthetic adapter and prints\n"
                 "           Engine::snapshot_json(), so durable state can be read without writing code.\n"
                 "           The journal is opened read-only and the replay runs on an in-process copy of\n"
                 "           its committed records. The epoch and incarnation reported inside the printed\n"
                 "           snapshot are the ones this replay claimed; that is stated on stderr. Exactly\n"
                 "           the canonical bytes are written, so the output is accepted by 'check'.\n");
}

[[nodiscard]] std::string option(int argc, char** argv, const char* name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

[[nodiscard]] bool flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] const recovery::json::Value* member(const recovery::json::Value& object, const char* name) {
    if (object.kind() != recovery::json::Value::Kind::Object) {
        return nullptr;
    }
    const auto it = object.object().find(name);
    if (it == object.object().end()) {
        return nullptr;
    }
    return &it->second;
}

// A durable log over an already-read, already-verified record stream. Replay
// appends exactly one record, the incarnation claim Engine::open makes, and
// that append stays in memory. The journal file is never reopened for writing,
// so an operator can read durable state while the coordinator that owns it
// keeps running.
class ReplayLog final : public recovery::DurableLog {
public:
    explicit ReplayLog(std::vector<recovery::JournalRecord> records) : records_(std::move(records)) {
        if (!records_.empty()) {
            lastSequence_ = records_.back().sequence;
        }
    }

    [[nodiscard]] recovery::Result<std::uint64_t> append_record(recovery::RecordKind kind,
                                                                std::string_view payload) override {
        if (poisoned_) {
            return recovery::errors::persistence("log.poisoned", "the replay log is no longer usable");
        }
        recovery::JournalRecord record;
        record.sequence = lastSequence_ + 1;
        record.kind = kind;
        record.payload.assign(payload);
        lastSequence_ = record.sequence;
        records_.push_back(std::move(record));
        return lastSequence_;
    }

    [[nodiscard]] std::uint64_t last_sequence() const override { return lastSequence_; }
    [[nodiscard]] bool poisoned() const override { return poisoned_; }
    [[nodiscard]] recovery::Result<std::vector<recovery::JournalRecord>> read_records() const override {
        return records_;
    }

private:
    std::vector<recovery::JournalRecord> records_{};
    std::uint64_t lastSequence_{0};
    bool poisoned_{false};
};

}  // namespace

int main(int argc, char** argv) {
    using namespace recovery;
    if (argc < 2) {
        print_usage();
        return 2;
    }
    const std::string command{argv[1]};

    if (command == "verify") {
        const std::string path = option(argc, argv, "--journal", "");
        if (path.empty()) {
            std::fprintf(stderr, "verify requires --journal <path>\n");
            return 2;
        }
        Journal journal;
        JournalOptions options;
        options.createIfMissing = flag(argc, argv, "--allow-create");
        auto opened = journal.open(path, options, TimePoint{}, false, Duration{});
        if (!opened.has_value()) {
            std::fprintf(stderr, "refused: %s\n", opened.error().describe().c_str());
            return 1;
        }
        auto records = journal.read_all();
        if (!records.has_value()) {
            std::fprintf(stderr, "refused: %s\n", records.error().describe().c_str());
            return 1;
        }
        // The claimed epoch is printed because it is the value a successor must
        // exceed. An operator who has to restart a coordinator needs it, and
        // deriving it by hand from the record stream is exactly the mistake that
        // makes a restart look like a corrupted journal.
        std::fprintf(stdout,
                     "ok journal=%s uuid=%s records=%llu lastSequence=%llu lastClaimedEpoch=%llu "
                     "tornTailRepaired=%s tornTailBytes=%llu\n",
                     fileio::absolute_path(path).c_str(), opened.value().uuidHex.c_str(),
                     static_cast<unsigned long long>(opened.value().recordCount),
                     static_cast<unsigned long long>(opened.value().lastSequence),
                     static_cast<unsigned long long>(opened.value().lastEpoch.value()),
                     opened.value().tornTailRepaired ? "true" : "false",
                     static_cast<unsigned long long>(opened.value().tornTailBytes));
        return 0;
    }

    if (command == "dump") {
        const std::string path = option(argc, argv, "--journal", "");
        if (path.empty()) {
            std::fprintf(stderr, "dump requires --journal <path>\n");
            return 2;
        }
        Journal journal;
        JournalOptions options;
        options.createIfMissing = false;
        auto opened = journal.open(path, options, TimePoint{}, false, Duration{});
        if (!opened.has_value()) {
            std::fprintf(stderr, "refused: %s\n", opened.error().describe().c_str());
            return 1;
        }
        auto records = journal.read_all();
        if (!records.has_value()) {
            std::fprintf(stderr, "refused: %s\n", records.error().describe().c_str());
            return 1;
        }
        const bool asJson = flag(argc, argv, "--json");
        for (const JournalRecord& record : records.value()) {
            if (asJson) {
                std::fprintf(stdout, "{\"kind\":\"%s\",\"sequence\":%llu,\"payload\":\"%s\"}\n",
                             std::string{to_string(record.kind)}.c_str(),
                             static_cast<unsigned long long>(record.sequence), record.payload.c_str());
            } else {
                std::fprintf(stdout, "%6llu %-20s %s\n", static_cast<unsigned long long>(record.sequence),
                             std::string{to_string(record.kind)}.c_str(), record.payload.c_str());
            }
        }
        return 0;
    }

    if (command == "check") {
        const std::string path = option(argc, argv, "--snapshot", "");
        if (path.empty()) {
            std::fprintf(stderr, "check requires --snapshot <path>\n");
            return 2;
        }
        auto bytes = fileio::read_file(path, kMaxSnapshotBytes);
        if (!bytes.has_value()) {
            std::fprintf(stderr, "refused: %s\n", bytes.error().describe().c_str());
            return 1;
        }
        const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
        json::Decoder decoder(text);
        auto value = decoder.decode();
        if (!value.has_value()) {
            std::fprintf(stderr, "refused: %s\n", value.error().describe().c_str());
            return 1;
        }
        if (json::encode(value.value()) != text) {
            std::fprintf(stderr, "refused: the snapshot is valid JSON but not canonically encoded\n");
            return 1;
        }
        std::fprintf(stdout, "ok canonical=true bytes=%llu\n",
                     static_cast<unsigned long long>(bytes.value().size()));
        return 0;
    }

    if (command == "plan") {
        const std::string path = option(argc, argv, "--snapshot", "");
        if (path.empty()) {
            std::fprintf(stderr, "plan requires --snapshot <path>\n");
            return 2;
        }
        auto bytes = fileio::read_file(path, kMaxSnapshotBytes);
        if (!bytes.has_value()) {
            std::fprintf(stderr, "refused: %s\n", bytes.error().describe().c_str());
            return 1;
        }
        const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
        json::Decoder decoder(text);
        auto value = decoder.decode();
        if (!value.has_value()) {
            std::fprintf(stderr, "refused: %s\n", value.error().describe().c_str());
            return 1;
        }
        const json::Value* plans = member(value.value(), "plans");
        if (plans == nullptr || plans->kind() != json::Value::Kind::Array) {
            std::fprintf(stderr, "refused: the snapshot carries no plans array\n");
            return 1;
        }
        const std::string wanted = option(argc, argv, "--plan", "");
        std::size_t printed = 0;
        for (const json::Value& entry : plans->array()) {
            const json::Value* plan = member(entry, "plan");
            if (plan == nullptr) {
                std::fprintf(stderr, "refused: a snapshot plan entry carries no plan object\n");
                return 1;
            }
            if (!wanted.empty()) {
                const json::Value* id = member(*plan, "id");
                if (id == nullptr || id->kind() != json::Value::Kind::String || id->string() != wanted) {
                    continue;
                }
            }
            const std::string canonical = json::encode(*plan);
            std::fwrite(canonical.data(), 1, canonical.size(), stdout);
            std::fputc('\n', stdout);
            ++printed;
        }
        if (printed == 0) {
            std::fprintf(stderr, "refused: %s\n", wanted.empty() ? "the snapshot stores no plan"
                                                                  : "the snapshot stores no plan with that identity");
            return 1;
        }
        return 0;
    }

    if (command == "snapshot") {
        const std::string path = option(argc, argv, "--journal", "");
        if (path.empty()) {
            std::fprintf(stderr, "snapshot requires --journal <path>\n");
            return 2;
        }
        Journal journal;
        JournalOptions options;
        options.createIfMissing = false;
        auto opened = journal.open(path, options, TimePoint{}, false, Duration{});
        if (!opened.has_value()) {
            std::fprintf(stderr, "refused: %s\n", opened.error().describe().c_str());
            return 1;
        }
        auto records = journal.read_all();
        if (!records.has_value()) {
            std::fprintf(stderr, "refused: %s\n", records.error().describe().c_str());
            return 1;
        }
        // Replay must claim an epoch strictly greater than every epoch the
        // durable state has seen, and the durable state, not the journal
        // header, is the authority for that: the epoch lives in the records.
        std::uint64_t durableEpoch = opened.value().lastEpoch.value();
        for (const JournalRecord& record : records.value()) {
            if (record.kind != RecordKind::BeginEpoch) {
                continue;
            }
            json::Decoder payloadDecoder(record.payload);
            auto payload = payloadDecoder.decode();
            if (!payload.has_value()) {
                std::fprintf(stderr, "refused: an epoch record is not canonical JSON: %s\n",
                             payload.error().describe().c_str());
                return 1;
            }
            const json::Value* epoch = member(payload.value(), "epoch");
            if (epoch == nullptr || epoch->kind() != json::Value::Kind::Unsigned) {
                std::fprintf(stderr, "refused: an epoch record carries no epoch\n");
                return 1;
            }
            durableEpoch = std::max(durableEpoch, epoch->unsigned_value());
        }
        if (durableEpoch == 0xffffffffffffffffull) {
            std::fprintf(stderr, "refused: the durable epoch counter is exhausted\n");
            return 1;
        }

        // The journal is closed before anything else happens: the replay below
        // reads only the bytes that were already committed and verified.
        journal.close();

        synthetic::World world;
        const std::string worldPath = option(argc, argv, "--world", "");
        if (!worldPath.empty()) {
            auto loaded = synthetic::World::open(worldPath);
            if (!loaded.has_value()) {
                std::fprintf(stderr, "refused: %s\n", loaded.error().describe().c_str());
                return 1;
            }
            world = std::move(loaded.value());
        }

        synthetic::Adapter adapter{world, synthetic::Adapter::Options{}};
        FixedClock clock{opened.value().createdAt};
        ReplayLog log{std::move(records).value()};
        Engine::Options engineOptions;
        Engine engine{log, adapter, clock, engineOptions};

        std::uint64_t epochValue = durableEpoch + 1;
        const std::string epochText = option(argc, argv, "--epoch", "");
        if (!epochText.empty()) {
            epochValue = std::strtoull(epochText.c_str(), nullptr, 10);
        }
        const std::string incarnation = option(argc, argv, "--incarnation", "recoveryctl-replay");
        auto report = engine.open(Epoch{epochValue}, incarnation);
        if (!report.has_value()) {
            std::fprintf(stderr, "refused: %s\n", report.error().describe().c_str());
            return 1;
        }
        std::fprintf(stderr,
                     "recoveryctl: replayed records=%llu read-only from %s; the snapshot below claims epoch=%llu "
                     "incarnation=%s and was not published\n",
                     static_cast<unsigned long long>(report.value().records_replayed),
                     fileio::absolute_path(path).c_str(), static_cast<unsigned long long>(epochValue),
                     incarnation.c_str());
        // Exactly the canonical bytes, with nothing appended: the output of
        // this subcommand is a snapshot file that `check` accepts, so an
        // operator can pipe one into the other without editing bytes.
        const std::string snapshot = engine.snapshot_json();
        std::fwrite(snapshot.data(), 1, snapshot.size(), stdout);
        return 0;
    }

    print_usage();
    return 2;
}
