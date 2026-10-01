#include "test.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

#include "recovery/adapters/synthetic.hpp"
#include "recovery/canonical.hpp"
#include "recovery/engine.hpp"
#include "recovery/error.hpp"
#include "recovery/fileio.hpp"
#include "recovery/journal.hpp"
#include "recovery/json.hpp"
#include "recovery/plan.hpp"
#include "recovery/ports.hpp"
#include "recovery/process.hpp"

using namespace recovery;

// ---------------------------------------------------------------------------
// Multiprocess crash proofs
// ---------------------------------------------------------------------------
// Nothing in this suite is simulated. A real coordinator_child.exe process is
// started through recovery::process, it is killed with Child::terminate() at a
// durable point it announces by publishing a marker file, and the durable state
// it left behind is then reopened in this process and checked against three
// properties:
//
//   (a) the committed prefix is intact: the record count, the last sequence
//       number and the whole chain verify, and no torn tail was repaired;
//   (b) no attempt is automatically reissued. The precise invariant is about
//       identity: a request that may already have reached the authority is
//       never repeated under a new idempotency key, and an in-flight attempt is
//       reconciled rather than re-sent. An attempt that was provably never sent
//       (durably committed, dispatch count zero, adapter not called) may be
//       replayed under exactly the identical key, because that is what the
//       key exists for: the authority applies it exactly once either way. A
//       step whose effect is already verified is never attempted again;
//   (c) the effect journal in the world file shows exactly one application per
//       key: no key appears twice and no key was applied more than once.
//
// What each pause point guarantees, and why the kill is meaningful:
//
//   before-dispatch        the child blocks inside the append of the
//                          attempt_committed record for the first attempt, so
//                          the attempt is on the device and the adapter has not
//                          been called. The guarantee is checkable from the
//                          durable record itself: dispatch_count == 0. Re-sending
//                          that attempt is safe and is asserted to reuse the
//                          identical idempotency key.
//   dispatch-ambiguous     the child blocks after the attempt_assessed record
//                          that marks the attempt ambiguous; the synthetic
//                          authority applied the effect and the response was
//                          lost, so the request may already have landed. This is
//                          the state in which a reissue would be unsafe.
//   after-acknowledgement  the child blocks after the record that carries the
//                          acknowledgement, before the step state record that
//                          follows it. The acknowledgement and the coordinator's
//                          verified assessment are durable; the step state is
//                          not yet.
//   after-verified-effect  the child blocks after the step_state_changed record
//                          that publishes state verified, so the durable state
//                          says the step's effect is proven present.
//
// Waits are bounded and the bound is part of the failure message. A bound that
// is reached terminates the child and fails the test; it never lets a hang pass
// as a success.

namespace {

// The child is killed only after it has published its marker. The bound is
// generous (the child does a few hundred milliseconds of work) and is quoted in
// the failure message.
constexpr std::uint64_t kMarkerBoundMillis = 30000;
// Budget for the kernel to release a dead process's single-writer lock.
constexpr NanoCount kLockReleaseBudgetNanos = 2000000000ull;
// Ticks the restarted coordinator is given to reach a definite state.
constexpr int kRestartTicks = 8;
// One hour after the child's own deterministic timeline, so the restarted
// incarnation's clock is strictly later than every instant the child wrote.
constexpr NanoCount kRestartNow = 1767225600000000000ull + 3600ull * 1000000000ull;

[[nodiscard]] std::uint64_t process_token() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

// Every scenario owns a directory that names the scenario and the process, so
// concurrent runs of this binary, and concurrent agents, cannot collide.
[[nodiscard]] std::string scenario_directory(const std::string& name) {
    const std::string directory = "scratch/multiprocess/" + name + "-" + std::to_string(process_token());
    auto created = fileio::ensure_parent_directory(directory + "/placeholder");
    RC_REQUIRE_MSG(created.has_value(), "scratch directory: " + created.error().describe());
    return directory;
}

void remove_if_present(const std::string& path) {
    if (!fileio::exists(path)) {
        return;
    }
    auto removed = fileio::remove_file(path);
    RC_REQUIRE_MSG(removed.has_value(), "remove " + path + ": " + removed.error().describe());
}

// The real child, next to this test binary: <build>/tests/recovery_tests.exe ->
// <build>/tools/coordinator_child.exe. A missing child is a build problem, not
// a reason to skip the proof.
[[nodiscard]] std::string child_executable() {
    const std::string self = process::current_executable();
    const std::size_t lastSeparator = self.find_last_of("\\/");
    RC_REQUIRE_MSG(lastSeparator != std::string::npos, "the running executable path has no separator: " + self);
    const std::string testsDirectory = self.substr(0, lastSeparator);
    const std::size_t buildSeparator = testsDirectory.find_last_of("\\/");
    RC_REQUIRE_MSG(buildSeparator != std::string::npos,
                   "the test binary does not live in a build directory: " + testsDirectory);
    const char separator = self[lastSeparator];
    const std::string candidate =
        testsDirectory.substr(0, buildSeparator) + separator + "tools" + separator + "coordinator_child.exe";
    RC_REQUIRE_MSG(fileio::exists(candidate), "the real child process was not built: " + candidate);
    return candidate;
}

[[nodiscard]] std::string read_text(const std::string& path) {
    auto bytes = fileio::read_file(path, 4ull << 20);
    if (!bytes.has_value()) {
        return std::string{"<unreadable: "} + bytes.error().describe() + ">";
    }
    return std::string(bytes.value().begin(), bytes.value().end());
}

[[nodiscard]] std::string tail_of(const std::string& text, std::size_t lines) {
    std::vector<std::string> found;
    std::size_t position = 0;
    while (position <= text.size()) {
        const std::size_t end = text.find('\n', position);
        const std::string line = text.substr(position, end == std::string::npos ? std::string::npos : end - position);
        if (!line.empty()) {
            found.push_back(line);
        }
        if (end == std::string::npos) {
            break;
        }
        position = end + 1;
    }
    std::string out;
    const std::size_t first = found.size() > lines ? found.size() - lines : 0;
    for (std::size_t index = first; index < found.size(); ++index) {
        out += "    " + found[index] + "\n";
    }
    return out.empty() ? std::string{"    <no output>\n"} : out;
}

// ---------------------------------------------------------------------------
// The marker the child publishes at the pause point
// ---------------------------------------------------------------------------
struct Marker {
    std::string point{};
    std::uint64_t sequence{0};
    std::uint64_t records{0};
    std::string kind{};
};

[[nodiscard]] Marker read_marker(const std::string& path) {
    const std::string text = read_text(path);
    Marker marker;
    std::size_t position = 0;
    while (position < text.size()) {
        const std::size_t end = text.find_first_of(" \r\n", position);
        const std::string token =
            text.substr(position, end == std::string::npos ? std::string::npos : end - position);
        position = end == std::string::npos ? text.size() : end + 1;
        const std::size_t equals = token.find('=');
        if (equals == std::string::npos) {
            continue;
        }
        const std::string key = token.substr(0, equals);
        const std::string value = token.substr(equals + 1);
        if (key == "point") {
            marker.point = value;
        } else if (key == "sequence") {
            marker.sequence = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "records") {
            marker.records = std::strtoull(value.c_str(), nullptr, 10);
        } else if (key == "kind") {
            marker.kind = value;
        }
    }
    return marker;
}

// ---------------------------------------------------------------------------
// Durable record inspection
// ---------------------------------------------------------------------------
[[nodiscard]] const json::Value* member_of(const json::Value& value, const char* key) {
    if (value.kind() != json::Value::Kind::Object) {
        return nullptr;
    }
    const auto it = value.object().find(key);
    if (it == value.object().end()) {
        return nullptr;
    }
    return &it->second;
}

[[nodiscard]] std::string text_of(const json::Value& value, const char* key, const std::string& fallback = {}) {
    const json::Value* found = member_of(value, key);
    if (found == nullptr || found->kind() != json::Value::Kind::String) {
        return fallback;
    }
    return found->string();
}

[[nodiscard]] bool flag_of(const json::Value& value, const char* key) {
    const json::Value* found = member_of(value, key);
    return found != nullptr && found->kind() == json::Value::Kind::Boolean && found->boolean();
}

[[nodiscard]] std::vector<json::Value> decoded_payloads(const std::vector<JournalRecord>& records,
                                                       RecordKind kind) {
    std::vector<json::Value> decoded;
    for (const JournalRecord& record : records) {
        if (record.kind != kind) {
            continue;
        }
        json::Decoder decoder(record.payload);
        auto value = decoder.decode();
        if (value.has_value()) {
            decoded.push_back(std::move(value.value()));
        }
    }
    return decoded;
}

[[nodiscard]] std::vector<std::string> committed_keys(const std::vector<JournalRecord>& records) {
    std::vector<std::string> keys;
    for (const json::Value& payload : decoded_payloads(records, RecordKind::AttemptCommitted)) {
        keys.push_back(text_of(payload, "idempotency_key"));
    }
    return keys;
}

[[nodiscard]] std::map<std::string, std::size_t> committed_per_step(const std::vector<JournalRecord>& records) {
    std::map<std::string, std::size_t> counts;
    for (const json::Value& payload : decoded_payloads(records, RecordKind::AttemptCommitted)) {
        counts[text_of(payload, "step")] += 1;
    }
    return counts;
}

[[nodiscard]] std::set<std::string> verified_steps(const std::vector<JournalRecord>& records) {
    std::set<std::string> steps;
    for (const json::Value& payload : decoded_payloads(records, RecordKind::StepStateChanged)) {
        if (text_of(payload, "state") == "verified") {
            steps.insert(text_of(payload, "step"));
        }
    }
    return steps;
}

[[nodiscard]] bool has_ambiguous_attempt(const std::vector<JournalRecord>& records) {
    for (const json::Value& payload : decoded_payloads(records, RecordKind::AttemptAssessed)) {
        if (flag_of(payload, "ambiguous")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_acknowledged_attempt(const std::vector<JournalRecord>& records) {
    for (const JournalRecord& record : records) {
        if (record.kind != RecordKind::AttemptAssessed && record.kind != RecordKind::AttemptAcknowledged) {
            continue;
        }
        json::Decoder decoder(record.payload);
        auto value = decoder.decode();
        if (!value.has_value()) {
            continue;
        }
        const json::Value* acknowledgement = member_of(value.value(), "acknowledgement");
        if (acknowledgement != nullptr && flag_of(*acknowledgement, "present")) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool has_verified_step(const std::vector<JournalRecord>& records) {
    return !verified_steps(records).empty();
}

// The highest epoch that exists as a committed BeginEpoch record. The value
// JournalOpenInfo reports comes from the journal file header, and the header is
// only brought up to date when the writer records a claim, so after an abrupt
// kill it can lag behind the records that are already committed. A successor
// must exceed every epoch durable state has seen, including epochs that exist
// only as records, so the committed records are the authoritative source: this
// helper is also the honest operator procedure for choosing the next epoch.
[[nodiscard]] std::uint64_t highest_recorded_epoch(const std::vector<JournalRecord>& records) {
    std::uint64_t highest = 0;
    for (const JournalRecord& record : records) {
        if (record.kind != RecordKind::BeginEpoch) {
            continue;
        }
        json::Decoder decoder(record.payload);
        auto value = decoder.decode();
        if (!value.has_value()) {
            continue;
        }
        const json::Value* epoch = member_of(value.value(), "epoch");
        if (epoch != nullptr && epoch->kind() == json::Value::Kind::Unsigned) {
            highest = std::max(highest, epoch->unsigned_value());
        }
    }
    return highest;
}

// The epoch a successor must claim: strictly greater than the header value and
// strictly greater than every recorded claim.
[[nodiscard]] std::uint64_t successor_epoch(const JournalOpenInfo& info, const std::vector<JournalRecord>& records) {
    return std::max(info.lastEpoch.value(), highest_recorded_epoch(records)) + 1;
}

// The idempotency key of the first attempt whose durable record carries an
// acknowledgement.
[[nodiscard]] std::string first_key_with_acknowledgement(const std::vector<JournalRecord>& records) {
    for (const JournalRecord& record : records) {
        if (record.kind != RecordKind::AttemptAssessed && record.kind != RecordKind::AttemptAcknowledged) {
            continue;
        }
        json::Decoder decoder(record.payload);
        auto value = decoder.decode();
        if (!value.has_value()) {
            continue;
        }
        const json::Value* acknowledgement = member_of(value.value(), "acknowledgement");
        if (acknowledgement != nullptr && flag_of(*acknowledgement, "present")) {
            return text_of(value.value(), "idempotency_key");
        }
    }
    return {};
}

// Reports the key of the first attempt whose durable record carries a flag.
[[nodiscard]] std::string first_key_where(const std::vector<JournalRecord>& records, RecordKind kind,
                                          const char* member, bool expected) {
    for (const json::Value& payload : decoded_payloads(records, kind)) {
        if (flag_of(payload, member) == expected) {
            return text_of(payload, "idempotency_key");
        }
    }
    return {};
}

// ---------------------------------------------------------------------------
// A port that records what the restarted coordinator actually asked the
// authority to do. This is the direct evidence for "no attempt is reissued":
// a reissue would appear as a dispatch carrying an idempotency key that was
// already durable before the kill.
// ---------------------------------------------------------------------------
class RecordingPort final : public AdjacentAuthorityPort {
public:
    explicit RecordingPort(synthetic::Adapter& inner) : inner_(&inner) {}

    [[nodiscard]] std::string authority_name() const override { return inner_->authority_name(); }

    [[nodiscard]] Result<AdapterResponse> dispatch(const Request& request) override {
        dispatched.push_back(request.idempotency_key());
        return inner_->dispatch(request);
    }

    [[nodiscard]] Result<AdapterResponse> inspect(const Request& request) override {
        inspected.push_back(request.idempotency_key());
        return inner_->inspect(request);
    }

    [[nodiscard]] Result<AdapterResponse> observe(const Request& request, Domain domain) override {
        return inner_->observe(request, domain);
    }

    [[nodiscard]] Result<AuthorityRef> current_authority(Domain domain) const override {
        return inner_->current_authority(domain);
    }

    std::vector<std::string> dispatched{};
    std::vector<std::string> inspected{};

private:
    synthetic::Adapter* inner_{nullptr};
};

[[nodiscard]] bool contains(const std::vector<std::string>& values, const std::string& wanted) {
    return std::find(values.begin(), values.end(), wanted) != values.end();
}

// (c) Exactly one application per key, read back from the world file itself.
void require_exactly_once(const synthetic::World& world, const std::string& when) {
    // The property is exactly-once PER KEY, not "one application in total": a
    // plan with several effect steps legitimately applies several keys, and the
    // assertion must survive a change to the tool's plan.
    std::map<std::string, std::size_t> perKey;
    for (const std::string& key : world.effect_journal()) {
        perKey[key] += 1;
    }
    for (const auto& [key, count] : perKey) {
        RC_REQUIRE_MSG(count == 1, when + ": the world applied the key " + key + " " + std::to_string(count) +
                                       " times; exactly one application per idempotency key is the invariant");
        RC_REQUIRE_MSG(world.apply_count(key) == count,
                       when + ": the world reports " + std::to_string(world.apply_count(key)) +
                           " application(s) for the key " + key + " but its effect journal has " +
                           std::to_string(count));
    }
}

// The exact number of applications the world must show at a kill point: the
// plan's request-bearing steps that were dispatched before the kill.
void require_application_count(const synthetic::World& world, std::size_t expected, const std::string& when) {
    require_exactly_once(world, when);
    RC_REQUIRE_MSG(world.effect_journal().size() == expected,
                   when + ": expected " + std::to_string(expected) + " application(s), the world file records " +
                       std::to_string(world.effect_journal().size()));
}

// ---------------------------------------------------------------------------
// The durable state of one attempt, folded from its records
// ---------------------------------------------------------------------------
// Attempt records name their step and attempt number and carry the attempt's
// status, so the current durable state of a step's attempt is the last record
// for the highest attempt number of that step. "Idle" statuses are the ones
// that were neither completed nor resolved when the process died.
struct AttemptState {
    std::string step{};
    std::uint64_t attemptNumber{0};
    std::string status{};
    std::string key{};
    bool ambiguous{false};
    bool acknowledged{false};
    bool dispatchCountZero{false};
};

[[nodiscard]] std::vector<AttemptState> attempt_states(const std::vector<JournalRecord>& records) {
    std::map<std::string, AttemptState> current;
    for (const JournalRecord& record : records) {
        if (record.kind != RecordKind::AttemptCommitted && record.kind != RecordKind::AttemptAssessed) {
            continue;
        }
        json::Decoder decoder(record.payload);
        auto value = decoder.decode();
        if (!value.has_value()) {
            continue;
        }
        const json::Value& payload = value.value();
        const std::string step = text_of(payload, "step");
        if (step.empty()) {
            continue;
        }
        const json::Value* number = member_of(payload, "attempt_number");
        if (number == nullptr || number->kind() != json::Value::Kind::Unsigned) {
            continue;
        }
        AttemptState state;
        state.step = step;
        state.attemptNumber = number->unsigned_value();
        state.status = text_of(payload, "status");
        state.key = text_of(payload, "idempotency_key");
        state.ambiguous = flag_of(payload, "ambiguous");
        const json::Value* acknowledgement = member_of(payload, "acknowledgement");
        state.acknowledged = acknowledgement != nullptr && flag_of(*acknowledgement, "present");
        const json::Value* dispatchCount = member_of(payload, "dispatch_count");
        state.dispatchCountZero =
            dispatchCount != nullptr && dispatchCount->kind() == json::Value::Kind::Unsigned &&
            dispatchCount->unsigned_value() == 0;

        auto existing = current.find(step);
        if (existing == current.end() || existing->second.attemptNumber <= state.attemptNumber) {
            // The last record for the highest attempt number wins: that is the
            // state the attempt was in when the process died.
            if (existing != current.end() && existing->second.attemptNumber == state.attemptNumber) {
                state.dispatchCountZero = state.dispatchCountZero && existing->second.dispatchCountZero;
            }
            current[step] = state;
        }
    }
    std::vector<AttemptState> states;
    states.reserve(current.size());
    for (const auto& [step, state] : current) {
        (void)step;
        states.push_back(state);
    }
    std::sort(states.begin(), states.end(), [](const AttemptState& lhs, const AttemptState& rhs) {
        return lhs.step < rhs.step;
    });
    return states;
}

// ---------------------------------------------------------------------------
// One kill scenario
// ---------------------------------------------------------------------------
struct Scenario {
    std::string point{};
    bool loseResponse{false};
    // How many effects the world must show for this kill point.
    std::size_t applications{0};
};

void start_child(process::Child& child, const Scenario& scenario, const std::string& journalPath,
                 const std::string& worldPath, const std::string& markerPath, const std::string& stdoutPath,
                 const std::string& stderrPath) {
    std::vector<std::string> arguments{"--journal", journalPath, "--world", worldPath, "--epoch", "1",
                                       "--pause-at", scenario.point, "--pause-marker", markerPath};
    if (scenario.loseResponse) {
        arguments.push_back("--lose-response-after");
        arguments.push_back("1");
    }
    process::SpawnOptions options;
    options.executable = child_executable();
    options.arguments = std::move(arguments);
    options.stdoutFile = stdoutPath;
    options.stderrFile = stderrPath;
    const process::Result_ started = child.start(options);
    RC_REQUIRE_MSG(started.ok(), "the child process did not start: " + started.error);
}

// Kills the child at the scenario's pause point and returns the marker. The
// child is terminated on every path, including a failure.
[[nodiscard]] Marker pause_and_kill(const Scenario& scenario, const std::string& journalPath,
                                    const std::string& worldPath, const std::string& markerPath,
                                    const std::string& stdoutPath) {
    process::Child child;
    start_child(child, scenario, journalPath, worldPath, markerPath, stdoutPath, journalPath + ".err");
    if (!process::wait_for_file(markerPath, kMarkerBoundMillis)) {
        child.terminate();
        RC_REQUIRE_MSG(false, "the child never reached the '" + scenario.point + "' pause point within " +
                                  std::to_string(kMarkerBoundMillis) +
                                  " ms; the bound was reached and the child was terminated; child output tail:\n" +
                                  tail_of(read_text(stdoutPath), 6));
    }
    const Marker marker = read_marker(markerPath);
    RC_REQUIRE_MSG(child.running(),
                   "the child exited instead of blocking at the '" + scenario.point + "' pause point; output tail:\n" +
                       tail_of(read_text(stdoutPath), 6));
    child.terminate();
    RC_REQUIRE_MSG(!child.running(), "the child survived Child::terminate()");
    return marker;
}

struct KillResult {
    Marker marker{};
    std::vector<JournalRecord> prefix{};
    std::string journalPath{};
    std::string worldPath{};
};

// The shared half of every scenario: kill, reopen, and prove (a).
[[nodiscard]] KillResult kill_and_reopen(const Scenario& scenario) {
    const std::string directory = scenario_directory(scenario.point);
    KillResult result;
    result.journalPath = directory + "/journal.rcj";
    result.worldPath = directory + "/world.json";
    const std::string markerPath = directory + "/paused.marker";
    const std::string stdoutPath = directory + "/child.out";
    const std::string stderrPath = directory + "/child.err";
    remove_if_present(result.journalPath);
    remove_if_present(result.journalPath + ".lock");
    remove_if_present(result.worldPath);
    remove_if_present(markerPath);
    remove_if_present(stdoutPath);
    remove_if_present(stderrPath);

    result.marker = pause_and_kill(scenario, result.journalPath, result.worldPath, markerPath, stdoutPath);
    RC_REQUIRE_EQ(result.marker.point, scenario.point);
    RC_REQUIRE_MSG(result.marker.sequence > 0 && result.marker.records > 0,
                   "the child published an empty marker: " + std::to_string(result.marker.sequence) + "/" +
                       std::to_string(result.marker.records));

    // (a) The committed prefix is intact: the file the killed process left
    // behind opens, verifies, reports exactly the sequence and record count the
    // child announced, and needs no torn tail repair.
    auto bytes = fileio::read_file(result.journalPath, 64ull << 20);
    RC_REQUIRE_MSG(bytes.has_value(), "the child's journal could not be read: " + bytes.error().describe());
    RC_REQUIRE(bytes.value().size() > 96);

    Journal journal;
    JournalOptions options;
    options.createIfMissing = false;
    auto opened = journal.open(result.journalPath, options, TimePoint{kRestartNow}, false, Duration{});
    RC_REQUIRE_MSG(opened.has_value(),
                   "the journal written by the killed child was refused: " + opened.error().describe());
    RC_REQUIRE_MSG(!opened.value().tornTailRepaired,
                   "the killed child left a torn tail; a flushed record must survive a kill intact");
    RC_REQUIRE_EQ(opened.value().recordCount, result.marker.records);
    RC_REQUIRE_EQ(opened.value().lastSequence, result.marker.sequence);
    auto records = journal.read_all();
    RC_REQUIRE_MSG(records.has_value(), "the child's journal did not verify: " + records.error().describe());
    RC_REQUIRE_EQ(records.value().size(), static_cast<std::size_t>(result.marker.records));
    result.prefix = std::move(records.value());
    journal.close();
    return result;
}

// A compact rendering of the durable execution state, used in every failure
// message and in the per-kill diagnostic line.
[[nodiscard]] std::string describe_view(const ExecutionView& view) {
    std::ostringstream out;
    out << "lifecycle=" << to_string(view.lifecycle) << " steps=[";
    bool first = true;
    for (const auto& [step, state] : view.steps) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << step.hex().substr(0, 8) << ":" << to_string(state);
    }
    out << "] attempts=[";
    first = true;
    for (const AttemptView& attempt : view.attempts) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << attempt.step.hex().substr(0, 8) << "#" << attempt.attempt_number << ":" << to_string(attempt.status)
            << (attempt.ambiguous ? "/ambiguous" : "") << (attempt.acknowledged ? "/acknowledged" : "")
            << "/effect=" << to_string(attempt.effect);
    }
    out << "]";
    return out.str();
}

// The rest of a scenario: open a new incarnation, present the world again,
// drive bounded ticks, and prove (b) and (c).
void restart_and_prove(const Scenario& scenario, const KillResult& result) {
    Journal journal;
    JournalOptions options;
    options.createIfMissing = false;
    auto opened = journal.open(result.journalPath, options, TimePoint{kRestartNow}, true,
                               Duration{kLockReleaseBudgetNanos});
    RC_REQUIRE_MSG(opened.has_value(), "the journal could not be reopened after the kill: " + opened.error().describe());

    // A successor never inherits authority: it must claim an epoch strictly
    // greater than every epoch durable state has seen. The epoch is derived
    // from the committed records, not from the header, because the header can
    // lag after an abrupt kill.
    const std::uint64_t recordedEpoch = highest_recorded_epoch(result.prefix);
    const std::uint64_t claim = successor_epoch(opened.value(), result.prefix);
    RC_REQUIRE_MSG(recordedEpoch >= 1, "the killed child left no committed epoch claim");
    RC_REQUIRE_MSG(claim > recordedEpoch, "the successor epoch does not advance beyond the committed records");

    {
        // The rule is proved on an independent replay of exactly the same
        // records, so a refusal cannot disturb the journal under test.
        MemoryLog probeLog;
        for (const JournalRecord& record : result.prefix) {
            RC_REQUIRE_OK(probeLog.append_record(record.kind, record.payload));
        }
        synthetic::World probeWorld = RC_REQUIRE_OK(synthetic::World::open(result.worldPath));
        synthetic::Adapter probeAdapter{probeWorld, synthetic::Adapter::Options{}};
        FixedClock probeClock{TimePoint{kRestartNow}};
        Engine::Options probeOptions;
        Engine probe{probeLog, probeAdapter, probeClock, probeOptions};
        const Error stale =
            RC_REQUIRE_ERR(probe.open(Epoch{recordedEpoch}, "stale-probe"), ErrorClass::StaleAuthority);
        RC_REQUIRE_EQ(stale.code(), std::string{"epoch.not_advancing"});
        RC_REQUIRE_EQ(probeLog.last_sequence(), result.marker.records);
    }
    RC_REQUIRE_EQ(journal.record_count(), result.marker.records);

    synthetic::World world = RC_REQUIRE_OK(synthetic::World::open(result.worldPath));
    require_application_count(world, scenario.applications, "at the kill point");
    const std::vector<std::string> appliedAtKill = world.effect_journal();

    synthetic::Adapter adapter{world, synthetic::Adapter::Options{}};
    RecordingPort port{adapter};
    FixedClock clock{TimePoint{kRestartNow}};
    JournalLog log{journal};
    // The documented defaults: RestartPolicy::ReconcileOnly, which never
    // reissues an in-flight request, and one dispatch per tick.
    Engine::Options engineOptions;
    Engine engine{log, port, clock, engineOptions};

    auto report = RC_REQUIRE_OK(engine.open(Epoch{claim}, "restart-probe"));
    RC_REQUIRE_EQ(report.records_replayed, result.marker.records);
    RC_REQUIRE_EQ(report.executions_restored, static_cast<std::uint64_t>(1));
    RC_REQUIRE_EQ(report.claimed_epoch.value(), claim);

    // Every incarnation is shown the world again. The authority reference the
    // child accepted is re-presented; accepting it twice is idempotent, so the
    // plan binding stays current.
    for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity, Domain::Safety}) {
        auto reference = RC_REQUIRE_OK(adapter.current_authority(domain));
        RC_REQUIRE_OK(engine.accept_authority(domain, reference));
        auto response = RC_REQUIRE_OK(adapter.observe(Request{}, domain));
        for (const AdapterObservation& observation : response.observations()) {
            RC_REQUIRE_OK(engine.observe(observation));
        }
    }

    const std::vector<ExecutionId> executions = engine.execution_ids();
    RC_REQUIRE_EQ(executions.size(), static_cast<std::size_t>(1));
    const ExecutionId execution = executions.front();
    auto before = engine.execution(execution);
    RC_REQUIRE_MSG(before.has_value(), "the execution the child started was not restored");
    const Lifecycle lifecycleBefore = before->lifecycle;
    const std::size_t committedBefore = committed_keys(result.prefix).size();

    std::size_t work = 0;
    for (int index = 0; index < kRestartTicks; ++index) {
        auto tick = RC_REQUIRE_OK(engine.tick());
        work += tick.transitions.size() + tick.dispatches.size() + tick.reconciled.size();
    }

    auto after = engine.execution(execution);
    RC_REQUIRE_MSG(after.has_value(), "the execution disappeared after the restart");

    auto afterRecords = journal.read_all();
    RC_REQUIRE_MSG(afterRecords.has_value(), "the journal stopped verifying after the restart: " +
                                                 afterRecords.error().describe());

    // The successor's claim is durable: the journal now carries a BeginEpoch
    // record for exactly the epoch this incarnation claimed, and that epoch is
    // the highest one in the stream.
    RC_REQUIRE_EQ(highest_recorded_epoch(afterRecords.value()), claim);

    // (b) No attempt is automatically reissued.
    //
    // Identity is what matters here. An attempt that was in flight when the
    // previous incarnation died may already have reached the authority, so the
    // coordinator must reconcile it, and must never repeat it under a new
    // idempotency key. An attempt that was committed but provably never sent
    // (dispatch count zero, the adapter was not called) may be replayed, and
    // the replay must reuse exactly the identical key: that is precisely what
    // makes the authority apply it once.
    const std::vector<AttemptState> attemptsAtKill = attempt_states(result.prefix);
    const std::vector<AttemptState> attemptsAfter = attempt_states(afterRecords.value());
    for (const AttemptState& state : attemptsAtKill) {
        const bool inFlight = state.status == "dispatched";
        const bool neverSent = state.status == "pending" && state.dispatchCountZero;
        if (!inFlight && !neverSent) {
            continue;
        }
        for (const AttemptState& later : attemptsAfter) {
            if (later.step != state.step) {
                continue;
            }
            if (inFlight) {
                RC_REQUIRE_MSG(later.attemptNumber == state.attemptNumber && later.key == state.key,
                               "the restarted coordinator reissued an in-flight request under a new identity: the "
                               "attempt that was dispatched when the process died was attempt " +
                                   std::to_string(state.attemptNumber) + " with idempotency key " + state.key +
                                   ", and the restart used attempt " + std::to_string(later.attemptNumber) +
                                   " with idempotency key " + later.key + " at the '" + scenario.point + "' point");
            } else {
                RC_REQUIRE_MSG(later.attemptNumber == state.attemptNumber && later.key == state.key,
                               "the restarted coordinator invented a new identity for the attempt that was committed "
                               "but never sent: committed attempt " +
                                   std::to_string(state.attemptNumber) + " key " + state.key + ", restart attempt " +
                                   std::to_string(later.attemptNumber) + " key " + later.key);
            }
        }
    }
    // A step whose effect was already verified is never attempted again.
    const std::set<std::string> verifiedBefore = verified_steps(result.prefix);
    const std::map<std::string, std::size_t> attemptsBefore = committed_per_step(result.prefix);
    for (const auto& [step, count] : committed_per_step(afterRecords.value())) {
        if (verifiedBefore.count(step) == 1) {
            const auto original = attemptsBefore.find(step);
            const std::size_t expected = original == attemptsBefore.end() ? 0 : original->second;
            RC_REQUIRE_MSG(count == expected, "the restarted coordinator committed a new attempt for the already verified step " +
                                                  step + " at the '" + scenario.point + "' kill point");
        }
    }
    RC_REQUIRE_MSG(committed_keys(afterRecords.value()).size() >= committedBefore,
                   "the restarted coordinator lost committed attempts");

    // (c) Exactly one application per idempotency key, read back from the world
    // file after the restarted coordinator has run. Applications recorded
    // before the kill must still be there exactly once.
    synthetic::World reloaded = RC_REQUIRE_OK(synthetic::World::open(result.worldPath));
    require_exactly_once(reloaded, "after the restart");
    for (const std::string& key : appliedAtKill) {
        RC_REQUIRE_MSG(contains(reloaded.effect_journal(), key),
                       "an application recorded before the kill disappeared from the world file: " + key);
    }

    std::cout << "[ kill ] point=" << scenario.point << " sequence=" << result.marker.sequence
              << " records=" << result.marker.records << " kind=" << result.marker.kind
              << " attempts_before=" << committedBefore << " dispatched_after=" << port.dispatched.size()
              << " inspected_after=" << port.inspected.size() << " work=" << work
              << " lifecycle_before=" << to_string(lifecycleBefore)
              << " lifecycle_after=" << to_string(after->lifecycle) << std::endl
              << "[ kill ] restored " << describe_view(*before) << std::endl
              << "[ kill ] after    " << describe_view(*after) << std::endl;

    // The point specific durable state, asserted on the reopened journal and on
    // the restored execution.
    if (scenario.point == "before-dispatch") {
        // What this kill point guarantees: the attempt record is durable and
        // the adapter was never called. The dispatch count in the durable
        // record is the proof, so the point is unambiguous.
        RC_REQUIRE_EQ(attemptsAtKill.size(), static_cast<std::size_t>(1));
        RC_REQUIRE_EQ(attemptsAtKill.front().status, std::string{"pending"});
        RC_REQUIRE_MSG(attemptsAtKill.front().dispatchCountZero,
                       "the 'before dispatch' kill point did not leave dispatch_count == 0, so the adapter may have "
                       "been called and the point does not mean what it claims");
        RC_REQUIRE(!has_verified_step(result.prefix));
        const std::string pendingKey = attemptsAtKill.front().key;
        // The replay must have applied the effect exactly once: recovery
        // happened, and it happened once.
        RC_REQUIRE_EQ(reloaded.apply_count(pendingKey), static_cast<std::uint64_t>(1));
    } else if (scenario.point == "dispatch-ambiguous") {
        RC_REQUIRE_MSG(has_ambiguous_attempt(result.prefix),
                       "the kill point did not leave an ambiguous attempt in the durable prefix");
        const std::string ambiguousKey = first_key_where(result.prefix, RecordKind::AttemptAssessed, "ambiguous", true);
        RC_REQUIRE_MSG(!ambiguousKey.empty(), "the ambiguous attempt has no idempotency key");
        RC_REQUIRE_MSG(contains(port.inspected, ambiguousKey),
                       "the restarted coordinator never asked the authority about the ambiguous key " + ambiguousKey);
        RC_REQUIRE_MSG(!contains(port.dispatched, ambiguousKey),
                       "the restarted coordinator re-dispatched the ambiguous attempt " + ambiguousKey);
        RC_REQUIRE_EQ(reloaded.apply_count(ambiguousKey), static_cast<std::uint64_t>(1));
    } else if (scenario.point == "after-acknowledgement") {
        RC_REQUIRE_MSG(has_acknowledged_attempt(result.prefix),
                       "the kill point did not leave an acknowledged attempt in the durable prefix");
        const std::string acknowledgedKey = first_key_with_acknowledgement(result.prefix);
        RC_REQUIRE_MSG(!acknowledgedKey.empty(), "the acknowledged attempt has no idempotency key");
        RC_REQUIRE_MSG(!contains(port.dispatched, acknowledgedKey),
                       "the restarted coordinator re-dispatched the acknowledged attempt " + acknowledgedKey);
        RC_REQUIRE_EQ(reloaded.apply_count(acknowledgedKey), static_cast<std::uint64_t>(1));
    } else {
        RC_REQUIRE_MSG(has_verified_step(result.prefix),
                       "the kill point did not leave a verified step state in the durable prefix");
    }

    engine.shutdown();
}

// The outcome of a restart probe: did the execution move, and did the
// coordinator leave a durably verified effect unapplied to its step?
struct Liveness {
    bool progressed{false};
    bool verifiedEffectLeftUnapplied{false};
    std::string detail{};
};

// Drives the restart and looks for the two ways an execution can be wedged:
// nothing at all happens and no definite state is reported, or a durable
// verified effect exists while the step that owns it is still treated as
// unresolved. The second one is the sharper defect: an operator cannot act on a
// stall whose cause is already proven present.
[[nodiscard]] Liveness restart_liveness(const Scenario& scenario) {
    const KillResult result = kill_and_reopen(scenario);
    Journal journal;
    JournalOptions options;
    options.createIfMissing = false;
    auto opened = journal.open(result.journalPath, options, TimePoint{kRestartNow}, true,
                               Duration{kLockReleaseBudgetNanos});
    RC_REQUIRE_MSG(opened.has_value(), "reopen: " + opened.error().describe());

    synthetic::World world = RC_REQUIRE_OK(synthetic::World::open(result.worldPath));
    synthetic::Adapter adapter{world, synthetic::Adapter::Options{}};
    RecordingPort port{adapter};
    FixedClock clock{TimePoint{kRestartNow}};
    JournalLog log{journal};
    Engine::Options engineOptions;
    Engine engine{log, port, clock, engineOptions};
    auto prefix = journal.read_all();
    RC_REQUIRE_MSG(prefix.has_value(), "the journal did not verify: " + prefix.error().describe());
    RC_REQUIRE_OK(engine.open(Epoch{successor_epoch(opened.value(), prefix.value())}, "restart-probe"));
    for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity, Domain::Safety}) {
        auto reference = RC_REQUIRE_OK(adapter.current_authority(domain));
        RC_REQUIRE_OK(engine.accept_authority(domain, reference));
        auto response = RC_REQUIRE_OK(adapter.observe(Request{}, domain));
        for (const AdapterObservation& observation : response.observations()) {
            RC_REQUIRE_OK(engine.observe(observation));
        }
    }
    const std::vector<ExecutionId> executions = engine.execution_ids();
    RC_REQUIRE_EQ(executions.size(), static_cast<std::size_t>(1));
    const ExecutionId execution = executions.front();
    auto before = engine.execution(execution);
    RC_REQUIRE_MSG(before.has_value(), "the execution was not restored");
    const Lifecycle lifecycleBefore = before->lifecycle;
    std::size_t work = 0;
    bool transitioned = false;
    for (int index = 0; index < kRestartTicks; ++index) {
        auto tick = RC_REQUIRE_OK(engine.tick());
        work += tick.transitions.size() + tick.dispatches.size() + tick.reconciled.size();
        if (!tick.transitions.empty()) {
            transitioned = true;
        }
    }
    auto after = engine.execution(execution);
    RC_REQUIRE_MSG(after.has_value(), "the execution disappeared");

    Liveness liveness;
    liveness.progressed = transitioned || work > 0 || lifecycleBefore != after->lifecycle;
    for (const AttemptView& attempt : after->attempts) {
        if (attempt.effect != EffectStatus::Verified) {
            continue;
        }
        const auto step = after->steps.find(attempt.step);
        if (step == after->steps.end() || step->second != StepState::Verified) {
            liveness.verifiedEffectLeftUnapplied = true;
        }
    }
    liveness.detail = "work=" + std::to_string(work) + " transitions=" + (transitioned ? "yes" : "no") +
                      " dispatches=" + std::to_string(port.dispatched.size()) +
                      " inspections=" + std::to_string(port.inspected.size()) +
                      " applications=" + std::to_string(world.effect_journal().size()) + " restored=[" +
                      describe_view(*before) + "] after=[" + describe_view(*after) + "]";
    std::cout << "[ liveness ] point=" << scenario.point << " progressed=" << (liveness.progressed ? "true" : "false")
              << " verified_effect_left_unapplied=" << (liveness.verifiedEffectLeftUnapplied ? "true" : "false")
              << " " << liveness.detail << std::endl;
    engine.shutdown();
    return liveness;
}

}  // namespace

// ---------------------------------------------------------------------------
// The four documented kill points
// ---------------------------------------------------------------------------
RC_TEST(multiprocess_crash_before_dispatch) {
    Scenario scenario;
    scenario.point = "before-dispatch";
    scenario.applications = 0;
    const KillResult result = kill_and_reopen(scenario);
    restart_and_prove(scenario, result);
}

RC_TEST(multiprocess_crash_during_dispatch_ambiguity) {
    Scenario scenario;
    scenario.point = "dispatch-ambiguous";
    scenario.loseResponse = true;
    scenario.applications = 1;
    const KillResult result = kill_and_reopen(scenario);
    restart_and_prove(scenario, result);
}

RC_TEST(multiprocess_crash_after_acknowledgement) {
    Scenario scenario;
    scenario.point = "after-acknowledgement";
    scenario.applications = 1;
    const KillResult result = kill_and_reopen(scenario);
    restart_and_prove(scenario, result);
}

RC_TEST(multiprocess_crash_after_verified_effect_publication) {
    Scenario scenario;
    scenario.point = "after-verified-effect";
    scenario.applications = 1;
    const KillResult result = kill_and_reopen(scenario);
    restart_and_prove(scenario, result);
}

// ---------------------------------------------------------------------------
// The pause flags are additive
// ---------------------------------------------------------------------------
// The crash injection flags exist only for this suite. Without them the child
// must behave exactly as it did before they existed: a normal run reaches
// Lifecycle::Complete and prints its machine readable result line. This test
// guards the tool change itself.
RC_TEST(multiprocess_child_without_pause_flags_is_unchanged) {
    const std::string directory = scenario_directory("normal-run");
    const std::string journalPath = directory + "/journal.rcj";
    const std::string worldPath = directory + "/world.json";
    const std::string stdoutPath = directory + "/child.out";
    const std::string stderrPath = directory + "/child.err";
    remove_if_present(journalPath);
    remove_if_present(journalPath + ".lock");
    remove_if_present(worldPath);
    remove_if_present(stdoutPath);
    remove_if_present(stderrPath);

    process::SpawnOptions options;
    options.executable = child_executable();
    options.arguments = {"--journal", journalPath, "--world", worldPath, "--epoch", "1"};
    options.stdoutFile = stdoutPath;
    options.stderrFile = stderrPath;
    const process::Result_ finished = process::run(options);
    RC_REQUIRE_MSG(finished.ok(), "the child did not run: " + finished.error);
    RC_REQUIRE_MSG(finished.exitCode == 0,
                   "a normal child run failed with exit code " + std::to_string(finished.exitCode) +
                       "; output tail:\n" + tail_of(read_text(stdoutPath), 6));
    const std::string output = read_text(stdoutPath);
    RC_REQUIRE_MSG(output.find("result lifecycle=complete") != std::string::npos,
                   "a normal child run did not reach Lifecycle::Complete; output tail:\n" + tail_of(output, 8));
    std::cout << "[ child ] normal run: " << output.substr(output.find("result lifecycle=")) << std::flush;
}

// ---------------------------------------------------------------------------
// The single writer lock across a real process death
// ---------------------------------------------------------------------------
RC_TEST(multiprocess_single_writer_lock_is_exclusive_and_released_by_death) {
    Scenario scenario;
    scenario.point = "before-dispatch";
    scenario.applications = 0;
    const std::string directory = scenario_directory("lock");
    const std::string journalPath = directory + "/journal.rcj";
    const std::string worldPath = directory + "/world.json";
    const std::string markerPath = directory + "/paused.marker";
    const std::string stdoutPath = directory + "/child.out";
    const std::string stderrPath = directory + "/child.err";
    remove_if_present(journalPath);
    remove_if_present(journalPath + ".lock");
    remove_if_present(worldPath);
    remove_if_present(markerPath);

    process::Child child;
    start_child(child, scenario, journalPath, worldPath, markerPath, stdoutPath, stderrPath);
    if (!process::wait_for_file(markerPath, kMarkerBoundMillis)) {
        child.terminate();
        RC_REQUIRE_MSG(false, "the child never reached its pause point within " +
                                  std::to_string(kMarkerBoundMillis) + " ms; child output tail:\n" +
                                  tail_of(read_text(stdoutPath), 6));
    }
    RC_REQUIRE(child.running());

    // While the child is alive and blocked, its journal is open and the kernel
    // lock is held: a second writer must be refused.
    Journal contender;
    JournalOptions options;
    options.createIfMissing = false;
    auto contended = contender.open(journalPath, options, TimePoint{kRestartNow}, false, Duration{});
    RC_REQUIRE_MSG(!contended.has_value(),
                   "a second process took the single-writer lock while the child held it");
    RC_REQUIRE_EQ(static_cast<int>(contended.error().cls()), static_cast<int>(ErrorClass::Contended));
    RC_REQUIRE_EQ(contended.error().code(), std::string{"lock.contended"});

    const std::uint32_t childId = child.id();
    child.terminate();
    RC_REQUIRE_MSG(!child.running(), "the child survived Child::terminate()");
    std::cout << "[ kill ] point=lock-holder pid=" << childId << " contended=" << contended.error().code()
              << " lock_release_budget_nanos=" << kLockReleaseBudgetNanos << std::endl;

    // The lock is owned by the kernel, so the death of the holder releases it.
    // The wait is bounded and the bound is reported.
    Journal successor;
    auto opened = successor.open(journalPath, options, TimePoint{kRestartNow}, true, Duration{kLockReleaseBudgetNanos});
    RC_REQUIRE_MSG(opened.has_value(),
                   "the single-writer lock outlived the process that held it within " +
                       std::to_string(kLockReleaseBudgetNanos) + " ns: " + opened.error().describe());

    // A successor never inherits authority: it must claim an epoch strictly
    // greater than every epoch durable state has seen, including epochs that
    // exist only as committed records (see highest_recorded_epoch).
    auto records = successor.read_all();
    RC_REQUIRE_MSG(records.has_value(), "the journal did not verify: " + records.error().describe());
    const std::uint64_t recordedEpoch = highest_recorded_epoch(records.value());
    const std::uint64_t claim = successor_epoch(opened.value(), records.value());
    RC_REQUIRE_MSG(recordedEpoch >= 1, "the killed child left no committed epoch claim");
    RC_REQUIRE_MSG(claim > recordedEpoch, "the successor epoch does not advance beyond the committed records");
    auto claimed = successor.claim_epoch(Epoch{claim}, "successor");
    RC_REQUIRE_MSG(claimed.has_value(), "a strictly newer epoch was refused: " + claimed.error().describe());
    RC_REQUIRE_EQ(claimed.value().value(), claim);
    successor.close();
}

// ---------------------------------------------------------------------------
// Liveness after every kill point
// ---------------------------------------------------------------------------
// A kill point may not leave the execution wedged. Two failures are named here,
// at every point, instead of being hidden behind a timeout:
//
//   * nothing progresses and no definite state is reported, so an operator sees
//     a running execution that will never do anything;
//   * a durable attempt is verified while the step that owns it is still
//     unresolved, so the execution stalls on a fact the coordinator has already
//     proven. The effect is present; the step must be verified.
RC_TEST(multiprocess_restart_after_every_kill_point_must_not_wedge) {
    struct Point {
        const char* name;
        bool loseResponse;
        std::size_t applications;
    };
    const Point points[] = {{"before-dispatch", false, 0},
                            {"dispatch-ambiguous", true, 1},
                            {"after-acknowledgement", false, 1},
                            {"after-verified-effect", false, 1}};
    std::vector<std::string> stalled;
    std::vector<std::string> unapplied;
    for (const Point& point : points) {
        Scenario scenario;
        scenario.point = point.name;
        scenario.loseResponse = point.loseResponse;
        scenario.applications = point.applications;
        const Liveness liveness = restart_liveness(scenario);
        if (!liveness.progressed) {
            stalled.push_back(std::string{point.name} + " (" + liveness.detail + ")");
        }
        if (liveness.verifiedEffectLeftUnapplied) {
            unapplied.push_back(std::string{point.name} + " (" + liveness.detail + ")");
        }
    }
    std::string message;
    for (const std::string& entry : stalled) {
        message += "\n  the execution made no progress and reported no definite state at the kill point " + entry;
    }
    for (const std::string& entry : unapplied) {
        message += "\n  a durably verified attempt was left unapplied to its step at the kill point " + entry;
    }
    RC_REQUIRE_MSG(message.empty(), "the restart left the execution wedged:" + message);
}
