// coordinator_child: a real process that drives Recovery Coordinator and then
// dies at a chosen point. Crash-injection tests start this executable, kill it,
// and then reopen the durable state from an independent process.
//
// Every death this program performs is a plain process exit with a distinctive
// status. Nothing here opens a window, prompts, or waits for input.
//
// Every run that is not a crash injection ends with one machine readable line:
//
//   result lifecycle=<state> steps=<n> attempts=<n> records=<n>
//
// so a supervising process can read the outcome without parsing anything else.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "recovery/adapters/synthetic.hpp"
#include "recovery/canonical.hpp"
#include "recovery/engine.hpp"
#include "recovery/fileio.hpp"
#include "recovery/journal.hpp"
#include "recovery/json.hpp"
#include "recovery/ports.hpp"
#include "recovery/process.hpp"

namespace {

constexpr int kExitCrash = 90;

[[noreturn]] void crash_now(std::string_view why) {
    std::fprintf(stdout, "crash:%s\n", std::string{why}.c_str());
    std::fflush(stdout);
    std::_Exit(kExitCrash);
}

// Builds the canonical parameter text the synthetic adapter understands: a
// single object with an "effect" member whose value is the canonical JSON
// describing the world change.
[[nodiscard]] std::string effect_parameters(const std::string& effect) {
    recovery::CanonicalWriter writer;
    writer.raw("{\"effect\":");
    writer.quoted(effect);
    writer.raw("}");
    return writer.take();
}

[[nodiscard]] std::string argument_value(int argc, char** argv, const char* name, const std::string& fallback) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return argv[i + 1];
        }
    }
    return fallback;
}

[[nodiscard]] bool has_flag(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) {
            return true;
        }
    }
    return false;
}

// A clock that advances by a fixed step on every reading, so a child process
// produces a deterministic timeline without reading the system clock.
class SteppingClock final : public recovery::Clock {
public:
    SteppingClock() = default;
    [[nodiscard]] recovery::TimePoint now() const override {
        const auto value = recovery::TimePoint{baseNanos_ + step_ * counter_};
        ++counter_;
        return value;
    }

private:
    mutable std::uint64_t counter_{0};
    std::uint64_t baseNanos_{1767225600000000000ull};  // 2026-01-01T00:00:00Z
    std::uint64_t step_{1000000000ull};
};

// ---------------------------------------------------------------------------
// Crash injection at exact durable points
// ---------------------------------------------------------------------------
// The multiprocess suite must kill this process at four documented points, and
// a kill is only meaningful if the durable record that defines the point is on
// the device before the kill arrives. This wrapper provides that: when the
// record that marks the requested point has been appended and flushed, the
// child publishes a marker file and then blocks forever, and the parent kills
// it with a real process termination.
//
// The wrapper changes nothing else. With no --pause-at the wrapper is not
// installed, the engine is given the plain journal log, and the child behaves
// exactly as it did before these flags existed.
//
// Points:
//   before-dispatch        the attempt is durable and the adapter has not been
//                          called: the request provably has not left.
//   dispatch-ambiguous     the authority applied the effect and the response
//                          was lost; this is the state that makes an automatic
//                          reissue unsafe, so it is the state the restart
//                          proof cares about most.
//   after-acknowledgement  the acknowledgement is durable.
//   after-verified-effect  a verified step state is durable.
class PausingLog final : public recovery::DurableLog {
public:
    PausingLog(recovery::DurableLog& inner, const recovery::Journal& journal, std::string point, std::string marker)
        : inner_(&inner), journal_(&journal), point_(std::move(point)), marker_(std::move(marker)) {}

    [[nodiscard]] recovery::Result<std::uint64_t> append_record(recovery::RecordKind kind,
                                                               std::string_view payload) override {
        auto written = inner_->append_record(kind, payload);
        if (!written.has_value()) {
            return written.error();
        }
        if (!paused_ && matches(kind, payload)) {
            paused_ = true;
            publish_marker(written.value(), kind);
            block_until_killed();
        }
        return written;
    }

    [[nodiscard]] std::uint64_t last_sequence() const override { return inner_->last_sequence(); }
    [[nodiscard]] bool poisoned() const override { return inner_->poisoned(); }
    [[nodiscard]] recovery::Result<std::vector<recovery::JournalRecord>> read_records() const override {
        return inner_->read_records();
    }

private:
    [[nodiscard]] static const recovery::json::Value* member(const recovery::json::Value& value,
                                                            const char* key) {
        if (value.kind() != recovery::json::Value::Kind::Object) {
            return nullptr;
        }
        const auto it = value.object().find(key);
        if (it == value.object().end()) {
            return nullptr;
        }
        return &it->second;
    }

    [[nodiscard]] static bool flag_member(const recovery::json::Value& value, const char* key) {
        const recovery::json::Value* found = member(value, key);
        return found != nullptr && found->kind() == recovery::json::Value::Kind::Boolean && found->boolean();
    }

    [[nodiscard]] static bool text_member(const recovery::json::Value& value, const char* key,
                                          const char* expected) {
        const recovery::json::Value* found = member(value, key);
        return found != nullptr && found->kind() == recovery::json::Value::Kind::String &&
               found->string() == expected;
    }

    [[nodiscard]] bool matches(recovery::RecordKind kind, std::string_view payload) const {
        if (point_ == "before-dispatch") {
            return kind == recovery::RecordKind::AttemptCommitted;
        }
        recovery::json::Decoder decoder(payload);
        auto value = decoder.decode();
        if (!value.has_value()) {
            return false;
        }
        const recovery::json::Value& object = value.value();
        if (point_ == "dispatch-ambiguous") {
            return kind == recovery::RecordKind::AttemptAssessed && flag_member(object, "ambiguous");
        }
        if (point_ == "after-acknowledgement") {
            if (kind == recovery::RecordKind::AttemptAcknowledged) {
                return true;
            }
            const recovery::json::Value* acknowledgement = member(object, "acknowledgement");
            return kind == recovery::RecordKind::AttemptAssessed && acknowledgement != nullptr &&
                   flag_member(*acknowledgement, "present");
        }
        if (point_ == "after-verified-effect") {
            return kind == recovery::RecordKind::StepStateChanged && text_member(object, "state", "verified");
        }
        return false;
    }

    void publish_marker(std::uint64_t sequence, recovery::RecordKind kind) const {
        // The committed prefix at the instant of the pause: the sequence number
        // of the record that defines the point and the number of records the
        // journal holds. The parent asserts both after the kill.
        const std::string text = "point=" + point_ + " sequence=" + std::to_string(sequence) +
                                 " records=" + std::to_string(journal_->record_count()) +
                                 " kind=" + std::string{recovery::to_string(kind)} + "\n";
        const std::vector<std::uint8_t> bytes(text.begin(), text.end());
        auto published = recovery::fileio::publish_atomically(marker_, bytes);
        if (!published.has_value()) {
            std::fprintf(stderr, "marker: %s\n", published.error().describe().c_str());
        }
        std::fprintf(stdout, "paused:%s sequence=%llu kind=%s\n", point_.c_str(),
                     static_cast<unsigned long long>(sequence), std::string{recovery::to_string(kind)}.c_str());
        std::fflush(stdout);
    }

    [[noreturn]] static void block_until_killed() {
        // This process is expected to be terminated by its parent while it sits
        // here. It never returns on its own, which is the point: the single
        // writer lock stays held and the durable prefix stays exactly as the
        // pause point left it, so the parent observes the real crash state.
        while (true) {
            recovery::process::sleep_millis(20);
        }
    }

    recovery::DurableLog* inner_{nullptr};
    const recovery::Journal* journal_{nullptr};
    std::string point_;
    std::string marker_;
    bool paused_{false};
};

[[nodiscard]] bool known_pause_point(const std::string& point) {
    return point == "before-dispatch" || point == "dispatch-ambiguous" ||
           point == "after-acknowledgement" || point == "after-verified-effect";
}

}  // namespace

int main(int argc, char** argv) {
    using namespace recovery;

    const std::string journalPath = argument_value(argc, argv, "--journal", "");
    const std::string worldPath = argument_value(argc, argv, "--world", "");
    const std::string mode = argument_value(argc, argv, "--mode", "run");
    if (journalPath.empty()) {
        std::fprintf(stderr,
                     "usage: coordinator_child --journal <path> [--world <path>] [--mode <mode>]\n"
                     "       [--epoch <n>] [--incarnation <name>] [--lose-response-after <n>]\n"
                     "       [--pause-at before-dispatch|dispatch-ambiguous|after-acknowledgement|after-verified-effect\n"
                     "        --pause-marker <path>]\n");
        return 2;
    }

    const std::string pauseAt = argument_value(argc, argv, "--pause-at", "");
    const std::string pauseMarker = argument_value(argc, argv, "--pause-marker", "");
    if (!pauseAt.empty() && !known_pause_point(pauseAt)) {
        std::fprintf(stderr, "unknown --pause-at point: %s\n", pauseAt.c_str());
        return 2;
    }
    if (!pauseAt.empty() && pauseMarker.empty()) {
        std::fprintf(stderr, "--pause-at requires --pause-marker <path>\n");
        return 2;
    }

    auto world = synthetic::World::open(worldPath);
    if (!world.has_value()) {
        std::fprintf(stderr, "world: %s\n", world.error().describe().c_str());
        return 3;
    }
    synthetic::Adapter::Options adapterOptions;
    adapterOptions.lose_response_after =
        static_cast<std::uint64_t>(std::strtoull(argument_value(argc, argv, "--lose-response-after", "0").c_str(),
                                                 nullptr, 10));
    synthetic::Adapter adapter{world.value(), adapterOptions};

    Journal journal;
    JournalOptions journalOptions;
    journalOptions.createIfMissing = true;
    SteppingClock clock;
    auto opened = journal.open(journalPath, journalOptions, clock.now(), false, Duration{});
    if (!opened.has_value()) {
        std::fprintf(stderr, "journal: %s\n", opened.error().describe().c_str());
        return 4;
    }

    JournalLog log{journal};
    // With no --pause-at the engine receives the plain journal log and this
    // object is never consulted, so the child's behaviour is unchanged.
    PausingLog pausing{log, journal, pauseAt, pauseMarker};
    DurableLog* durable = pauseAt.empty() ? static_cast<DurableLog*>(&log)
                                          : static_cast<DurableLog*>(&pausing);
    Engine::Options options;
    options.max_attempts_per_step = 2;
    Engine engine{*durable, adapter, clock, options};

    const std::uint64_t epochValue = std::strtoull(argument_value(argc, argv, "--epoch", "1").c_str(), nullptr, 10);
    const std::string incarnation = argument_value(argc, argv, "--incarnation", "child");
    auto report = engine.open(Epoch{epochValue}, incarnation);
    if (!report.has_value()) {
        std::fprintf(stderr, "open: %s\n", report.error().describe().c_str());
        return 5;
    }
    std::fprintf(stdout, "opened:%s\n", report.value().summary().c_str());
    std::fflush(stdout);

    if (mode == "open") {
        engine.shutdown();
        return 0;
    }

    // Every incarnation must be shown the world again. A world that has never
    // been populated gets its baseline readings here; the accepted authority is
    // read after the baseline exists so that the digest the coordinator accepts
    // is the digest of the content it actually saw.
    if (!fileio::exists(worldPath)) {
        world.value().set_reading(Domain::Safety, "evacuated", Reading::flag(true));
        world.value().set_reading(Domain::Capacity, "usable_racks", Reading::count(8));
        world.value().set_reading(Domain::Power, "feed_state", Reading::text("A"));
        world.value().set_reading(Domain::Cooling, "loop_state", Reading::text("running"));
        auto saved = world.value().save();
        if (!saved.has_value()) {
            std::fprintf(stderr, "world save: %s\n", saved.error().describe().c_str());
            return 8;
        }
    }

    for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity, Domain::Safety}) {
        auto reference = adapter.current_authority(domain);
        if (!reference.has_value()) {
            std::fprintf(stderr, "authority: %s\n", reference.error().describe().c_str());
            return 6;
        }
        auto accepted = engine.accept_authority(domain, reference.value());
        if (!accepted.has_value()) {
            std::fprintf(stderr, "accept %s: %s\n", std::string{to_string(domain)}.c_str(),
                         accepted.error().describe().c_str());
            return 7;
        }
        auto response = adapter.observe(Request{}, domain);
        if (!response.has_value()) {
            return 9;
        }
        for (const AdapterObservation& observation : response.value().observations()) {
            auto observed = engine.observe(observation);
            if (!observed.has_value()) {
                std::fprintf(stderr, "observe: %s\n", observed.error().describe().c_str());
                return 9;
            }
        }
    }
    // One staged plan: restore power, confirm cooling, restore capacity, then
    // confirm that the dependent obligation is met. Step identities come from a
    // fixed seed so that the child process is deterministic across runs.
    PlanDefinition definition;
    definition.name = "child-recovery";
    definition.scope = "synthetic-facility/row-A";
    definition.strategy = PlanStrategy::Staged;

    IdAllocator steps{1000};
    const StepId power{*steps.allocate(IdKind::Step)};
    const StepId cooling{*steps.allocate(IdKind::Step)};
    const StepId capacity{*steps.allocate(IdKind::Step)};
    const StepId verify{*steps.allocate(IdKind::Step)};

    StepSpec powerStep;
    powerStep.id = power;
    powerStep.name = "restore-power";
    powerStep.evidence_domains = {Domain::Power};
    RequestBinding powerBinding;
    powerBinding.step = power;
    powerBinding.binding = 1;
    powerBinding.domain = Domain::Power;
    powerBinding.operation = "set-feed-state";
    powerBinding.parameters = effect_parameters(R"({"domain":"power","key":"feed_state","value":"B"})");
    powerStep.requests.push_back(powerBinding);
    powerStep.compensable = true;
    RequestBinding powerCompensation = powerBinding;
    powerCompensation.parameters = effect_parameters(R"({"domain":"power","key":"feed_state","value":"A"})");
    powerStep.compensation_requests.push_back(powerCompensation);
    definition.steps.push_back(powerStep);

    StepSpec coolingStep;
    coolingStep.id = cooling;
    coolingStep.name = "restore-cooling";
    coolingStep.depends_on.push_back(Dependency{power, EdgeOutcome::AwaitVerified});
    coolingStep.evidence_domains = {Domain::Cooling};
    RequestBinding coolingBinding;
    coolingBinding.step = cooling;
    coolingBinding.binding = 1;
    coolingBinding.domain = Domain::Cooling;
    coolingBinding.operation = "set-loop-state";
    coolingBinding.parameters = effect_parameters(R"({"domain":"cooling","key":"loop_state","value":"recovering"})");
    coolingStep.requests.push_back(coolingBinding);
    definition.steps.push_back(coolingStep);

    StepSpec capacityStep;
    capacityStep.id = capacity;
    capacityStep.name = "restore-capacity";
    capacityStep.depends_on.push_back(Dependency{cooling, EdgeOutcome::AwaitVerified});
    capacityStep.evidence_domains = {Domain::Capacity};
    RequestBinding capacityBinding;
    capacityBinding.step = capacity;
    capacityBinding.binding = 1;
    capacityBinding.domain = Domain::Capacity;
    capacityBinding.operation = "set-usable-racks";
    capacityBinding.parameters = effect_parameters(R"({"domain":"capacity","key":"usable_racks","value":16})");
    capacityStep.requests.push_back(capacityBinding);
    definition.steps.push_back(capacityStep);

    StepSpec verifyStep;
    verifyStep.id = verify;
    verifyStep.name = "verify-capacity";
    verifyStep.depends_on.push_back(Dependency{capacity, EdgeOutcome::AwaitVerified});
    verifyStep.evidence_domains = {Domain::Capacity};
    ReadinessGate gate;
    gate.domain = Domain::Capacity;
    gate.required_keys = {"usable_racks"};
    gate.expected.emplace("usable_racks", Reading::count(16));
    gate.max_age = Duration{};
    verifyStep.gates.push_back(gate);
    definition.steps.push_back(verifyStep);

    for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity, Domain::Safety}) {
        const auto reference = engine.current_authority(domain);
        if (reference.has_value()) {
            definition.authority_requirements[domain] = *reference;
        }
    }

    auto planId = engine.propose_plan(definition);
    if (!planId.has_value()) {
        std::fprintf(stderr, "propose: %s\n", planId.error().describe().c_str());
        return 10;
    }
    auto published = engine.publish_plan(planId.value());
    if (!published.has_value()) {
        std::fprintf(stderr, "publish: %s\n", published.error().describe().c_str());
        return 11;
    }
    auto execution = engine.start_execution(planId.value());
    if (!execution.has_value()) {
        std::fprintf(stderr, "start: %s\n", execution.error().describe().c_str());
        return 12;
    }
    std::fprintf(stdout, "execution:%s\n", execution.value().to_text().c_str());
    std::fflush(stdout);

    const int crashAfter = std::atoi(argument_value(argc, argv, "--crash-after", "-1").c_str());
    int tickIndex = 0;
    while (tickIndex < 32) {
        auto tick = engine.tick();
        if (!tick.has_value()) {
            std::fprintf(stderr, "tick: %s\n", tick.error().describe().c_str());
            return 13;
        }
        std::fprintf(stdout, "tick:%d %s\n", tickIndex, tick.value().summary().c_str());
        std::fflush(stdout);
        if (crashAfter >= 0 && tickIndex >= crashAfter) {
            crash_now("after-tick");
        }
        const auto view = engine.execution(execution.value());
        if (view.has_value() && is_terminal(view->lifecycle)) {
            std::fprintf(stdout, "lifecycle:%s\n", std::string{to_string(view->lifecycle)}.c_str());
            break;
        }
        if (tick.value().idle && tick.value().dispatches.empty()) {
            break;
        }
        ++tickIndex;
    }

    if (has_flag(argc, argv, "--crash-at-end")) {
        crash_now("at-end");
    }

    const auto view = engine.execution(execution.value());
    if (view.has_value()) {
        std::fprintf(stdout, "final:%s\n", std::string{to_string(view->lifecycle)}.c_str());
    }
    std::fprintf(stdout, "journal-records:%llu\n",
                 static_cast<unsigned long long>(journal.record_count()));
    // The machine readable summary. It is one line, it has a fixed shape, and
    // it is printed on every non-crashing run, including a run whose plan did
    // not complete: a caller never has to parse the human readable lines above
    // to learn what the durable state says.
    //
    // "attempts" is the number of committed attempt records that name this
    // execution, read back from the journal. ExecutionView::attempts is the
    // engine's latest attempt per attempt number, which collapses attempts made
    // by different steps of one execution, so it is not a count of attempts.
    std::uint64_t attempts = view.has_value() ? view->attempts.size() : 0;
    auto committed = journal.read_all();
    if (committed.has_value() && view.has_value()) {
        attempts = 0;
        for (const JournalRecord& record : committed.value()) {
            if (record.kind != RecordKind::AttemptCommitted) {
                continue;
            }
            json::Decoder decoder(record.payload);
            auto payload = decoder.decode();
            if (!payload.has_value() || payload.value().kind() != json::Value::Kind::Object) {
                continue;
            }
            const auto member = payload.value().object().find("execution");
            if (member == payload.value().object().end() || member->second.kind() != json::Value::Kind::String) {
                continue;
            }
            if (member->second.string() == execution.value().to_text()) {
                ++attempts;
            }
        }
    }
    std::fprintf(stdout, "result lifecycle=%s steps=%llu attempts=%llu records=%llu\n",
                 view.has_value() ? std::string{to_string(view->lifecycle)}.c_str() : "missing",
                 static_cast<unsigned long long>(view.has_value() ? view->steps.size() : 0),
                 static_cast<unsigned long long>(attempts),
                 static_cast<unsigned long long>(journal.record_count()));
    std::fflush(stdout);
    engine.shutdown();
    journal.close();
    return 0;
}
