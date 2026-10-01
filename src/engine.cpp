#include "recovery/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <set>
#include <thread>

#include "codec.hpp"
#include "engine_internal.hpp"
#include "recovery/canonical.hpp"

namespace recovery {
namespace {

constexpr const char* kReasonOperator = "operator";
constexpr const char* kReasonCompensation = "compensation";
constexpr const char* kReasonStaleEvidence = "bound evidence is no longer current for the plan";
constexpr const char* kReasonStaleAuthority = "bound authority changed after the plan was accepted";
constexpr const char* kReasonDeadline = "the step deadline expired before the effect was verified";
constexpr const char* kReasonAmbiguous =
    "an attempt was in flight when the previous coordinator incarnation ended; it is not reissued";
constexpr const char* kReasonPointOfNoReturn = "a point of no return was passed; the plan cannot be reversed";
constexpr const char* kReasonSuperseded = "the plan was superseded by a newer revision";
constexpr const char* kReasonCancelled = "an operator cancelled the execution";

[[nodiscard]] bool is_running_lifecycle(Lifecycle value) {
    switch (value) {
        case Lifecycle::Idle:
        case Lifecycle::Running:
        case Lifecycle::Blocked:
        case Lifecycle::Compensating:
            return true;
        default:
            return false;
    }
}

[[nodiscard]] std::optional<Lifecycle> lifecycle_from_text(const std::string& text) {
    for (std::uint8_t raw = 1; raw <= 10; ++raw) {
        const auto candidate = static_cast<Lifecycle>(raw);
        if (to_string(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

[[nodiscard]] std::optional<StepState> step_state_from_text(const std::string& text) {
    for (std::uint8_t raw = 1; raw <= 12; ++raw) {
        const auto candidate = static_cast<StepState>(raw);
        if (to_string(candidate) == text) {
            return candidate;
        }
    }
    return std::nullopt;
}

}  // namespace

std::string_view to_string(RestartPolicy value) {
    switch (value) {
        case RestartPolicy::ReconcileOnly:
            return "reconcile_only";
        case RestartPolicy::ReconcileThenRedispatch:
            return "reconcile_then_redispatch";
        case RestartPolicy::ReportOnly:
            return "report_only";
    }
    return "unknown";
}

std::optional<RestartPolicy> restart_policy_from_string(std::string_view text) {
    if (text == "reconcile_only") {
        return RestartPolicy::ReconcileOnly;
    }
    if (text == "reconcile_then_redispatch") {
        return RestartPolicy::ReconcileThenRedispatch;
    }
    if (text == "report_only") {
        return RestartPolicy::ReportOnly;
    }
    return std::nullopt;
}

std::string RecoveryReport::summary() const {
    CanonicalWriter writer;
    writer.raw("epoch=");
    writer.unsigned_integer(claimed_epoch.value());
    writer.raw(" incarnation=");
    writer.quoted(incarnation);
    writer.raw(" records=");
    writer.unsigned_integer(records_replayed);
    writer.raw(" plans=");
    writer.unsigned_integer(plans_restored);
    writer.raw(" executions=");
    writer.unsigned_integer(executions_restored);
    writer.raw(" attempts=");
    writer.unsigned_integer(attempts_restored);
    writer.raw(" ambiguous=");
    writer.unsigned_integer(ambiguous_attempts);
    writer.raw(" evidence_fenced=");
    writer.boolean(evidence_fenced);
    return writer.take();
}

std::string TickReport::summary() const {
    CanonicalWriter writer;
    writer.raw("sequence=");
    writer.unsigned_integer(sequence);
    writer.raw(" transitions=");
    writer.unsigned_integer(transitions.size());
    writer.raw(" dispatches=");
    writer.unsigned_integer(dispatches.size());
    writer.raw(" reconciled=");
    writer.unsigned_integer(reconciled.size());
    writer.raw(" fenced=");
    writer.unsigned_integer(fenced.size());
    writer.raw(" completed=");
    writer.unsigned_integer(completed.size());
    writer.raw(" failed=");
    writer.unsigned_integer(failed.size());
    writer.raw(" idle=");
    writer.boolean(idle);
    return writer.take();
}

// Readiness is a pure function of the plan and the step states. It is exposed
// here so that the independent reference model used by the property tests can
// be compared against exactly the function the engine uses.
std::vector<StepId> compute_ready_steps(const Plan& plan, const std::map<StepId, StepRuntime>& states,
                                        bool compensating) {
    std::vector<StepId> ready;
    for (const StepSpec& spec : plan.steps()) {
        const auto runtime = states.find(spec.id);
        if (runtime == states.end()) {
            continue;
        }
        const StepRuntime& step = runtime->second;
        if (compensating) {
            if (!step.verified() || !spec.compensable) {
                continue;
            }
        } else if (step.state != StepState::Pending && step.state != StepState::Ready) {
            continue;
        }

        bool blocked = false;
        for (const Dependency& dependency : spec.depends_on) {
            const auto predecessor = states.find(dependency.predecessor);
            if (predecessor == states.end()) {
                blocked = true;
                break;
            }
            const StepRuntime& other = predecessor->second;
            switch (dependency.outcome) {
                case EdgeOutcome::AwaitVerified:
                    if (compensating ? other.state != StepState::Compensated : other.state != StepState::Verified) {
                        blocked = true;
                    }
                    break;
                case EdgeOutcome::AwaitResolved:
                    if (!other.resolved()) {
                        blocked = true;
                    }
                    break;
                case EdgeOutcome::OnFailure:
                    if (!other.failed()) {
                        blocked = true;
                    }
                    break;
            }
            if (blocked) {
                break;
            }
        }
        if (!blocked) {
            ready.push_back(spec.id);
        }
    }
    std::sort(ready.begin(), ready.end());
    return ready;
}

std::vector<StepId> compute_ready_states(const Plan& plan, const std::map<StepId, StepState>& states,
                                         bool compensating) {
    std::map<StepId, StepRuntime> runtimes;
    for (const auto& [step, state] : states) {
        StepRuntime runtime;
        runtime.state = state;
        runtimes.emplace(step, runtime);
    }
    return compute_ready_steps(plan, runtimes, compensating);
}

namespace {

// ---------------------------------------------------------------------------
// Derived attempt view
// ---------------------------------------------------------------------------

// The coordinator derives what it knows about an attempt from its own durable
// inputs only: the status it recorded, the acknowledgements it received, and
// the evidence it accepted. Nothing here consults the world.
// The coordinator assigns evidence identity; the authority only describes what
// it saw. The identity is derived from the reported content alone, so replaying
// the same observation is an idempotent no-op rather than a second piece of
// evidence. Engine::observe, accept_adapter_observation and interpret_response
// must derive it identically, which is why it lives in exactly one place.
[[nodiscard]] EvidenceId evidence_identity(const AdapterObservation& observation) {
    CanonicalWriter writer;
    append_framed(writer, "recovery.evidence_identity.v1");
    append_framed(writer, observation.origin());
    append_framed(writer, to_string(observation.domain()));
    append_framed_u64(writer, observation.generation());
    append_framed(writer, observation.key());
    append_framed(writer, observation.reading().canonical());
    append_framed(writer, to_rfc3339(observation.observed_at()));
    append_framed(writer, observation.authority().canonical());
    const Digest identity = writer.digest_of();
    std::array<std::uint8_t, Id::kBytes> identityBytes{};
    std::copy(identity.bytes().begin(), identity.bytes().begin() + static_cast<std::ptrdiff_t>(Id::kBytes),
              identityBytes.begin());
    return EvidenceId{Id{identityBytes}};
}

// ---------------------------------------------------------------------------
// Durable state application
// ---------------------------------------------------------------------------
// One object owns every state transition. Each operation is a sequence of
// records: build them, append them to the durable log, and only then touch
// memory. If an append fails the operation aborts before memory changes, so
// memory can never be ahead of durable state.
}  // namespace

class StateApplier {
public:
    StateApplier(Engine::Impl& impl, bool replay) : impl_(&impl), replay_(replay) {}

    [[nodiscard]] Result<bool> op_begin_epoch(Epoch epoch, std::string_view incarnation);
    [[nodiscard]] Result<bool> op_publish_plan(Plan plan);
    [[nodiscard]] Result<bool> op_supersede_plan(const PlanId& plan, std::string_view reason);
    [[nodiscard]] Result<bool> op_start_execution(ExecState state);
    [[nodiscard]] Result<bool> op_accept_authority(Domain domain, const AuthorityRef& reference, TimePoint at);
    [[nodiscard]] Result<bool> op_retire_authority(Domain domain, std::string_view reason, TimePoint at);
    [[nodiscard]] Result<bool> op_accept_observation(const Observation& observation);
    [[nodiscard]] Result<bool> op_commit_attempt(const Attempt& attempt, RecordKind kind);
    [[nodiscard]] Result<bool> op_update_attempt(const Attempt& attempt, RecordKind kind);
    [[nodiscard]] Result<bool> op_step_state(ExecState& state, const StepId& step, StepState next, TimePoint at);
    [[nodiscard]] Result<bool> op_lifecycle(ExecState& state, Lifecycle next, std::string reason, TimePoint at);
    [[nodiscard]] Result<bool> op_fence(ExecState& state, std::vector<Domain> domains, std::string reason,
                                        TimePoint at);
    [[nodiscard]] Result<bool> op_close(ExecState& state, Lifecycle lifecycle, std::string reason, TimePoint at);

    [[nodiscard]] std::uint64_t sequence() const noexcept { return impl_->sequence; }

private:
    [[nodiscard]] Result<bool> emit(RecordKind kind, std::string payload);

    Engine::Impl* impl_{nullptr};
    bool replay_{false};
};

Result<bool> StateApplier::emit(RecordKind kind, std::string payload) {
    if (replay_) {
        // Replay trusts the checksum and hash chain the reader already verified.
        ++impl_->sequence;
        return true;
    }
    auto appended = impl_->log->append_record(kind, payload);
    if (!appended.has_value()) {
        return appended.error();
    }
    impl_->sequence = appended.value();
    return true;
}

Result<bool> StateApplier::op_begin_epoch(Epoch epoch, std::string_view incarnation) {
    json::Object object;
    object["epoch"] = json::Value{static_cast<unsigned long long>(epoch.value())};
    object["incarnation"] = json::Value{std::string{incarnation}};
    auto written = emit(RecordKind::BeginEpoch, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    impl_->epoch = epoch;
    impl_->incarnationName.assign(incarnation);
    return true;
}

Result<bool> StateApplier::op_publish_plan(Plan plan) {
    const Digest content = codec::plan_content_digest_of(codec::plan_to_json(plan));
    json::Object object;
    object["content_digest"] = codec::encode(content);
    object["plan"] = codec::plan_to_json(plan);
    auto written = emit(RecordKind::PlanPublished, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    const PlanId id = plan.id();
    impl_->plans[id] = std::move(plan);
    return true;
}

Result<bool> StateApplier::op_supersede_plan(const PlanId& plan, std::string_view reason) {
    json::Object object;
    object["plan"] = codec::encode(plan);
    object["reason"] = json::Value{std::string{reason}};
    auto written = emit(RecordKind::PlanSuperseded, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    impl_->supersededPlans.insert(plan);
    const auto it = impl_->plans.find(plan);
    if (it != impl_->plans.end()) {
        it->second.set_status(PlanStatus::Superseded);
    }
    return true;
}

Result<bool> StateApplier::op_start_execution(ExecState state) {
    json::Object object;
    object["content_digest"] = codec::encode(state.content);
    object["epoch"] = json::Value{static_cast<unsigned long long>(state.epoch.value())};
    object["execution"] = codec::encode(state.id);
    object["plan"] = codec::encode(state.plan);
    object["revision"] = json::Value{static_cast<unsigned long long>(state.revision)};
    object["started_at"] = codec::encode(state.startedAt);
    auto written = emit(RecordKind::ExecutionStarted, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    impl_->executions[state.id] = std::move(state);
    return true;
}

Result<bool> StateApplier::op_accept_authority(Domain domain, const AuthorityRef& reference, TimePoint at) {
    if (reference.is_zero()) {
        return errors::invalid_argument("authority.zero_reference", "authority reference is empty");
    }
    const auto existing = impl_->ledger.current(domain);
    if (existing.has_value()) {
        if (*existing == reference) {
            return true;
        }
        if (reference.generation() < existing->generation()) {
            return errors::stale_authority("authority.generation_regression",
                                           "authority generation would move backwards");
        }
        if (reference.generation() == existing->generation() && reference.digest() != existing->digest()) {
            return errors::identity_conflict("authority.digest_conflict",
                                             "the same authority generation was presented with different content");
        }
        if (!existing->same_structure(reference)) {
            return errors::identity_conflict("authority.structure_change",
                                             "authority identity changed without a retirement");
        }
    }
    json::Object object;
    object["accepted_at"] = codec::encode(at);
    object["authority"] = codec::encode(reference);
    object["domain"] = codec::encode(domain);
    auto written = emit(RecordKind::AuthorityAccepted, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    const auto accepted = impl_->ledger.accept(domain, reference, at);
    if (!accepted.has_value()) {
        return accepted.error();
    }
    return true;
}

Result<bool> StateApplier::op_retire_authority(Domain domain, std::string_view reason, TimePoint at) {
    if (!impl_->ledger.current(domain).has_value()) {
        return errors::not_found("authority.not_accepted", "cannot retire authority that is not accepted");
    }
    json::Object object;
    object["domain"] = codec::encode(domain);
    object["reason"] = json::Value{std::string{reason}};
    object["retired_at"] = codec::encode(at);
    auto written = emit(RecordKind::AuthorityRetired, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    const auto retired = impl_->ledger.retire(domain, at, reason);
    if (!retired.has_value()) {
        return retired.error();
    }
    return true;
}

Result<bool> StateApplier::op_accept_observation(const Observation& observation) {
    if (observation.id().is_zero()) {
        return errors::invalid_argument("evidence.identity", "observation identity is zero");
    }
    const auto acceptance = impl_->evidence.accept(observation);
    if (!acceptance.accepted) {
        return errors::precondition_failed("evidence.refused", acceptance.reason);
    }
    if (acceptance.replayed) {
        return true;
    }
    json::Object object;
    object["observation"] = codec::observation_to_json(observation);
    auto written = emit(RecordKind::EvidenceAccepted, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    return true;
}

Result<bool> StateApplier::op_commit_attempt(const Attempt& attempt, RecordKind kind) {
    auto written = emit(kind, json::encode(AttemptCodec::to_json(attempt)));
    if (!written.has_value()) {
        return written.error();
    }
    impl_->attempts[attempt.id()] = attempt;
    impl_->attemptIndex[attempt.execution()][std::make_pair(attempt.step(), attempt.attempt_number())] =
        attempt.id();
    impl_->dispatchOrder.push_back(attempt.id());
    const auto execution = impl_->executions.find(attempt.execution());
    if (execution != impl_->executions.end()) {
        execution->second.lastAttempt[attempt.step()] = attempt.id();
        if (attempt.status() == AttemptStatus::Dispatched) {
            execution->second.inFlight.insert(attempt.step());
        }
    }
    return true;
}

Result<bool> StateApplier::op_update_attempt(const Attempt& attempt, RecordKind kind) {
    auto written = emit(kind, json::encode(AttemptCodec::to_json(attempt)));
    if (!written.has_value()) {
        return written.error();
    }
    impl_->attempts[attempt.id()] = attempt;
    const auto execution = impl_->executions.find(attempt.execution());
    if (execution != impl_->executions.end()) {
        if (attempt.status() == AttemptStatus::Dispatched) {
            execution->second.inFlight.insert(attempt.step());
        } else {
            execution->second.inFlight.erase(attempt.step());
        }
    }
    return true;
}

Result<bool> StateApplier::op_step_state(ExecState& state, const StepId& step, StepState next, TimePoint at) {
    StepRuntime& runtime = state.steps[step];
    const StepState previous = runtime.state;
    json::Object object;
    object["attempts"] = json::Value{static_cast<unsigned long long>(runtime.attempts)};
    object["compensated"] = json::Value{runtime.compensated};
    object["compensation_attempts"] = json::Value{static_cast<unsigned long long>(runtime.compensationAttempts)};
    object["execution"] = codec::encode(state.id);
    object["previous"] = json::Value{std::string{to_string(previous)}};
    object["state"] = json::Value{std::string{to_string(next)}};
    object["step"] = codec::encode(step);
    auto written = emit(RecordKind::StepStateChanged, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    runtime.state = next;
    state.updatedAt = at;
    return true;
}

Result<bool> StateApplier::op_lifecycle(ExecState& state, Lifecycle next, std::string reason, TimePoint at) {
    // A lifecycle transition that changes nothing is not a transition. Writing a
    // record for it would make an idle coordinator grow its journal once per
    // tick, which turns a stalled execution into unbounded durable growth and
    // buries the one record that mattered under copies of itself.
    if (state.lifecycle == next && state.reason == reason) {
        return true;
    }

    json::Object object;
    object["execution"] = codec::encode(state.id);
    object["lifecycle"] = json::Value{std::string{to_string(next)}};
    object["reason"] = json::Value{reason};
    object["updated_at"] = codec::encode(at);
    auto written = emit(RecordKind::ExecutionLifecycle, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    state.lifecycle = next;
    state.reason = reason;
    state.updatedAt = at;
    return true;
}

Result<bool> StateApplier::op_fence(ExecState& state, std::vector<Domain> domains, std::string reason, TimePoint at) {
    json::Array array;
    array.reserve(domains.size());
    for (const Domain domain : domains) {
        array.push_back(codec::encode(domain));
    }
    json::Object object;
    object["domains"] = json::Value{std::move(array)};
    object["execution"] = codec::encode(state.id);
    object["fenced_at"] = codec::encode(at);
    object["reason"] = json::Value{reason};
    auto written = emit(RecordKind::ExecutionFenced, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    state.fenced = std::move(domains);
    state.updatedAt = at;
    return true;
}

Result<bool> StateApplier::op_close(ExecState& state, Lifecycle lifecycle, std::string reason, TimePoint at) {
    json::Object object;
    object["closed_at"] = codec::encode(at);
    object["execution"] = codec::encode(state.id);
    object["lifecycle"] = json::Value{std::string{to_string(lifecycle)}};
    object["reason"] = json::Value{reason};
    auto written = emit(RecordKind::ExecutionClosed, json::encode(json::Value{std::move(object)}));
    if (!written.has_value()) {
        return written.error();
    }
    state.lifecycle = lifecycle;
    state.reason = reason;
    state.updatedAt = at;
    return true;
}
// ---------------------------------------------------------------------------
// Record replay
// ---------------------------------------------------------------------------
// Replay reconstructs state from records alone. It refuses a record that
// references an object durable state has not created yet, and it refuses a
// record whose derived invariants do not hold. A refusal is fatal for open():
// an ambiguous durable generation is never adopted.
namespace {

[[nodiscard]] Result<ExecState*> replay_execution(Engine::Impl& impl, const json::Value& value) {
    auto execution = codec::want_member(value, "execution");
    if (!execution.has_value()) {
        return execution.error();
    }
    auto id = codec::decode_execution_id(*execution.value());
    if (!id.has_value()) {
        return id.error();
    }
    const auto it = impl.executions.find(*id);
    if (it == impl.executions.end()) {
        return errors::corrupt_state("record.unknown_execution", "a record references an unknown execution");
    }
    return &it->second;
}

[[nodiscard]] bool replay_record(const JournalRecord& record, Engine::Impl& impl, RecoveryReport& report) {
    auto parsed = codec::parse_canonical(record.payload);
    if (!parsed.has_value()) {
        return false;
    }
    const json::Value& value = parsed.value();

    switch (record.kind) {
        case RecordKind::BeginEpoch: {
            auto epoch = codec::want_u64(value, "epoch");
            auto incarnation = codec::want_string(value, "incarnation");
            if (!epoch.has_value() || !incarnation.has_value()) {
                return false;
            }
            if (epoch.value() < impl.epoch.value()) {
                return false;
            }
            impl.epoch = Epoch{epoch.value()};
            impl.incarnationName = incarnation.value();
            return true;
        }
        case RecordKind::PlanPublished: {
            auto planValue = codec::want_member(value, "plan");
            auto contentValue = codec::want_member(value, "content_digest");
            if (!planValue.has_value() || !contentValue.has_value()) {
                return false;
            }
            Digest content{};
            auto plan = codec::plan_from_json(*planValue.value(), content);
            if (!plan.has_value()) {
                return false;
            }
            auto storedContent = codec::decode_digest(*contentValue.value());
            if (!storedContent.has_value() || *storedContent != content) {
                return false;
            }
            if (impl.plans.find(plan->id()) != impl.plans.end()) {
                return false;
            }
            impl.plans[plan->id()] = std::move(plan.value());
            ++report.plans_restored;
            return true;
        }
        case RecordKind::PlanSuperseded: {
            auto plan = codec::want_member(value, "plan");
            if (!plan.has_value()) {
                return false;
            }
            auto id = codec::decode_plan_id(*plan.value());
            if (!id.has_value()) {
                return false;
            }
            if (impl.plans.find(*id) == impl.plans.end()) {
                return false;
            }
            impl.supersededPlans.insert(*id);
            impl.plans[*id].set_status(PlanStatus::Superseded);
            return true;
        }
        case RecordKind::ExecutionStarted: {
            auto execution = codec::want_member(value, "execution");
            auto planValue = codec::want_member(value, "plan");
            auto revision = codec::want_u64(value, "revision");
            auto epoch = codec::want_u64(value, "epoch");
            auto startedAt = codec::want_member(value, "started_at");
            auto content = codec::want_member(value, "content_digest");
            if (!execution.has_value() || !planValue.has_value() || !revision.has_value() || !epoch.has_value() ||
                !startedAt.has_value() || !content.has_value()) {
                return false;
            }
            auto id = codec::decode_execution_id(*execution.value());
            auto planId = codec::decode_plan_id(*planValue.value());
            auto started = codec::decode_time(*startedAt.value());
            auto contentDigest = codec::decode_digest(*content.value());
            if (!id.has_value() || !planId.has_value() || !started.has_value() || !contentDigest.has_value()) {
                return false;
            }
            if (impl.executions.find(*id) != impl.executions.end()) {
                return false;
            }
            const auto plan = impl.plans.find(*planId);
            if (plan == impl.plans.end()) {
                return false;
            }
            if (codec::plan_content_digest_of(codec::plan_to_json(plan->second)) != *contentDigest) {
                return false;
            }
            ExecState state;
            state.id = *id;
            state.plan = *planId;
            state.revision = revision.value();
            state.content = *contentDigest;
            state.lifecycle = Lifecycle::Running;
            state.epoch = Epoch{epoch.value()};
            state.startedAt = *started;
            state.updatedAt = *started;
            for (const StepSpec& spec : plan->second.steps()) {
                state.steps.emplace(spec.id, StepRuntime{});
            }
            impl.executions[state.id] = std::move(state);
            ++report.executions_restored;
            return true;
        }
        case RecordKind::ExecutionLifecycle: {
            auto execution = replay_execution(impl, value);
            if (!execution.has_value()) {
                return false;
            }
            auto lifecycle = codec::want_string(value, "lifecycle");
            auto reason = codec::want_string(value, "reason");
            auto updatedAt = codec::want_member(value, "updated_at");
            if (!lifecycle.has_value() || !reason.has_value() || !updatedAt.has_value()) {
                return false;
            }
            auto parsedLifecycle = lifecycle_from_text(lifecycle.value());
            auto at = codec::decode_time(*updatedAt.value());
            if (!parsedLifecycle.has_value() || !at.has_value()) {
                return false;
            }
            execution.value()->lifecycle = *parsedLifecycle;
            execution.value()->reason = reason.value();
            execution.value()->updatedAt = *at;
            return true;
        }
        case RecordKind::ExecutionFenced: {
            auto execution = replay_execution(impl, value);
            if (!execution.has_value()) {
                return false;
            }
            auto domains = codec::want_member(value, "domains");
            auto reason = codec::want_string(value, "reason");
            auto fencedAt = codec::want_member(value, "fenced_at");
            if (!domains.has_value() || !reason.has_value() || !fencedAt.has_value()) {
                return false;
            }
            if (domains.value()->kind() != json::Value::Kind::Array) {
                return false;
            }
            std::vector<Domain> parsedDomains;
            for (const json::Value& entry : domains.value()->array()) {
                auto domain = codec::decode_domain(entry);
                if (!domain.has_value()) {
                    return false;
                }
                parsedDomains.push_back(*domain);
            }
            auto at = codec::decode_time(*fencedAt.value());
            if (!at.has_value()) {
                return false;
            }
            execution.value()->fenced = std::move(parsedDomains);
            execution.value()->reason = reason.value();
            execution.value()->updatedAt = *at;
            return true;
        }
        case RecordKind::ExecutionClosed: {
            auto execution = replay_execution(impl, value);
            if (!execution.has_value()) {
                return false;
            }
            auto lifecycle = codec::want_string(value, "lifecycle");
            auto reason = codec::want_string(value, "reason");
            auto closedAt = codec::want_member(value, "closed_at");
            if (!lifecycle.has_value() || !reason.has_value() || !closedAt.has_value()) {
                return false;
            }
            auto parsedLifecycle = lifecycle_from_text(lifecycle.value());
            auto at = codec::decode_time(*closedAt.value());
            if (!parsedLifecycle.has_value() || !at.has_value()) {
                return false;
            }
            if (!is_terminal(*parsedLifecycle)) {
                return false;
            }
            execution.value()->lifecycle = *parsedLifecycle;
            execution.value()->reason = reason.value();
            execution.value()->updatedAt = *at;
            return true;
        }
        case RecordKind::AuthorityAccepted: {
            auto domain = codec::want_member(value, "domain");
            auto authority = codec::want_member(value, "authority");
            auto acceptedAt = codec::want_member(value, "accepted_at");
            if (!domain.has_value() || !authority.has_value() || !acceptedAt.has_value()) {
                return false;
            }
            auto parsedDomain = codec::decode_domain(*domain.value());
            auto parsedAuthority = codec::decode_authority(*authority.value());
            auto at = codec::decode_time(*acceptedAt.value());
            if (!parsedDomain.has_value() || !parsedAuthority.has_value() || !at.has_value()) {
                return false;
            }
            const auto accepted = impl.ledger.accept(*parsedDomain, *parsedAuthority, *at);
            if (!accepted.has_value()) {
                return false;
            }
            ++report.authority_entries_restored;
            return true;
        }
        case RecordKind::AuthorityRetired: {
            auto domain = codec::want_member(value, "domain");
            auto reason = codec::want_string(value, "reason");
            auto retiredAt = codec::want_member(value, "retired_at");
            if (!domain.has_value() || !reason.has_value() || !retiredAt.has_value()) {
                return false;
            }
            auto parsedDomain = codec::decode_domain(*domain.value());
            auto at = codec::decode_time(*retiredAt.value());
            if (!parsedDomain.has_value() || !at.has_value()) {
                return false;
            }
            const auto retired = impl.ledger.retire(*parsedDomain, *at, reason.value());
            if (!retired.has_value()) {
                return false;
            }
            return true;
        }
        case RecordKind::EvidenceAccepted: {
            auto observation = codec::want_member(value, "observation");
            if (!observation.has_value()) {
                return false;
            }
            auto parsedObservation = codec::observation_from_json(*observation.value(), EvidenceId{});
            if (!parsedObservation.has_value()) {
                return false;
            }
            const auto acceptance = impl.evidence.accept(*parsedObservation);
            if (!acceptance.accepted) {
                return false;
            }
            ++report.observations_restored;
            return true;
        }
        case RecordKind::AttemptCommitted:
        case RecordKind::AttemptDispatched:
        case RecordKind::AttemptAcknowledged:
        case RecordKind::AttemptObserved:
        case RecordKind::AttemptAssessed:
        case RecordKind::AttemptSuperseded: {
            auto attempt = AttemptCodec::from_committed_json(value);
            if (!attempt.has_value()) {
                return false;
            }
            const auto execution = impl.executions.find(attempt->execution());
            if (execution == impl.executions.end()) {
                return false;
            }
            auto runtime = execution->second.steps.find(attempt->step());
            if (runtime == execution->second.steps.end()) {
                return false;
            }
            const bool firstObservation = impl.attempts.find(attempt->id()) == impl.attempts.end();
            if (firstObservation) {
                ++report.attempts_restored;
                if (attempt->kind() == AttemptKind::Compensation) {
                    ++runtime->second.compensationAttempts;
                }
                impl.dispatchOrder.push_back(attempt->id());
            }
            if (attempt->ambiguous()) {
                ++report.ambiguous_attempts;
            }
            impl.attempts[attempt->id()] = *attempt;
            impl.attemptIndex[attempt->execution()][std::make_pair(attempt->step(), attempt->attempt_number())] =
                attempt->id();
            execution->second.lastAttempt[attempt->step()] = attempt->id();
            if (attempt->status() == AttemptStatus::Dispatched) {
                execution->second.inFlight.insert(attempt->step());
            } else {
                execution->second.inFlight.erase(attempt->step());
            }
            return true;
        }
        case RecordKind::StepStateChanged: {
            auto execution = replay_execution(impl, value);
            if (!execution.has_value()) {
                return false;
            }
            auto step = codec::want_member(value, "step");
            auto stateText = codec::want_string(value, "state");
            auto attempts = codec::want_u64(value, "attempts");
            auto compensationAttempts = codec::want_u64(value, "compensation_attempts");
            auto compensated = codec::want_bool(value, "compensated");
            if (!step.has_value() || !stateText.has_value() || !attempts.has_value() ||
                !compensationAttempts.has_value() || !compensated.has_value()) {
                return false;
            }
            auto stepId = codec::decode_step_id(*step.value());
            if (!stepId.has_value()) {
                return false;
            }
            auto runtime = execution.value()->steps.find(*stepId);
            if (runtime == execution.value()->steps.end()) {
                return false;
            }
            auto parsedState = step_state_from_text(stateText.value());
            if (!parsedState.has_value()) {
                return false;
            }
            if (attempts.value() > 0xffffffffull || compensationAttempts.value() > 0xffffffffull) {
                return false;
            }
            runtime->second.state = *parsedState;
            runtime->second.attempts = static_cast<std::uint32_t>(attempts.value());
            runtime->second.compensationAttempts = static_cast<std::uint32_t>(compensationAttempts.value());
            runtime->second.compensated = compensated.value();
            return true;
        }
        case RecordKind::CounterChanged:
        case RecordKind::Checkpoint:
            return true;
    }
    return false;
}

}  // namespace
// ---------------------------------------------------------------------------
// Construction, open and shutdown
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Replay repair
// ---------------------------------------------------------------------------

// The coordinator writes an attempt's outcome and the state of the step it
// belongs to as two records. A process that dies between them leaves a step that
// is behind its own attempt: the attempt is durably effect_present and verified
// while the step still says dispatched. Trusting the stored step state would
// leave the recovery stalled forever on work that is already proven done, and no
// later stage of a tick would ever revisit it, so on replay the step is derived
// from its attempts and the further advanced of the two wins.
//
// The derivation is a pure function of durable records, so every incarnation
// derives the same state, and a StepStateChanged record that arrives later in
// the stream still overrides it because replay applies records in order.
[[nodiscard]] StepState derive_step_state(StepState stored, const std::vector<const Attempt*>& attempts,
                                          bool compensating) {
    StepState best = stored;
    const auto advance = [&best](StepState candidate) {
        if (static_cast<std::uint8_t>(candidate) > static_cast<std::uint8_t>(best)) {
            best = candidate;
        }
    };

    for (const Attempt* attempt : attempts) {
        switch (attempt->status()) {
            case AttemptStatus::EffectPresent:
                if (attempt->effect_status() == EffectStatus::Verified) {
                    advance(compensating ? StepState::Compensated : StepState::Verified);
                }
                break;
            case AttemptStatus::Refused:
                advance(StepState::Refused);
                break;
            case AttemptStatus::Failed:
            case AttemptStatus::Unverifiable:
                advance(StepState::Failed);
                break;
            case AttemptStatus::Dispatched:
                advance(attempt->has_acknowledgement() ? StepState::Acknowledged : StepState::Dispatched);
                break;
            case AttemptStatus::Pending:
            case AttemptStatus::Superseded:
                break;
        }
    }
    return best;
}

Engine::Engine(DurableLog& log, AdjacentAuthorityPort& port, Clock& clock, Options options)
    : impl_(std::make_unique<Impl>()) {
    impl_->log = &log;
    impl_->port = &port;
    impl_->clock = &clock;
    impl_->options = std::move(options);
}

Engine::~Engine() {
    shutdown();
    impl_.reset();
}

Result<RecoveryReport> Engine::open(Epoch requestedEpoch, std::string incarnation) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (impl_->opened) {
        return errors::precondition_failed("engine.already_open", "the engine is already open");
    }
    if (impl_->log->poisoned()) {
        return errors::persistence("engine.log_poisoned", "the durable log is no longer usable");
    }
    if (requestedEpoch.is_zero()) {
        return errors::invalid_argument("engine.epoch", "the claimed epoch must be greater than zero");
    }
    if (incarnation.empty()) {
        return errors::invalid_argument("engine.incarnation", "the incarnation name must not be empty");
    }

    RecoveryReport report;
    report.claimed_epoch = requestedEpoch;
    report.incarnation = incarnation;

    auto records = impl_->log->read_records();
    if (!records.has_value()) {
        return records.error();
    }
    const auto& stream = records.value();
    std::uint64_t expected = 0;
    for (const JournalRecord& record : stream) {
        if (record.sequence != expected + 1) {
            return errors::corrupt_state("log.sequence", "the durable record sequence is not contiguous");
        }
        expected = record.sequence;
    }

    for (const JournalRecord& record : stream) {
        if (!replay_record(record, *impl_, report)) {
            return errors::corrupt_state("log.replay", "a durable record could not be reconstructed");
        }
    }
    report.records_replayed = stream.size();

    // Close the window between an attempt record and the step record that
    // follows it. A process killed inside that window leaves a step behind its
    // own durably proven attempt, which no later stage of a tick would ever
    // revisit.
    for (auto& [executionId, state] : impl_->executions) {
        const bool compensating = state.lifecycle == Lifecycle::Compensating;
        for (auto& [stepId, runtime] : state.steps) {
            std::vector<const Attempt*> attempts;
            const auto index = impl_->attemptIndex.find(executionId);
            if (index != impl_->attemptIndex.end()) {
                for (const auto& [key, attemptId] : index->second) {
                    if (!(key.first == stepId)) {
                        continue;
                    }
                    const auto attempt = impl_->attempts.find(attemptId);
                    if (attempt != impl_->attempts.end()) {
                        attempts.push_back(&attempt->second);
                    }
                }
            }
            const StepState derived = derive_step_state(runtime.state, attempts, compensating);
            if (derived != runtime.state) {
                runtime.state = derived;
                ++report.steps_repaired;
            }
        }
    }

    // The claimed epoch must advance beyond everything durable state has seen.
    if (requestedEpoch.value() <= impl_->epoch.value()) {
        return errors::stale_authority("epoch.not_advancing",
                                       "the claimed epoch must be strictly greater than every epoch already seen");
    }

    // The claim is recorded on the medium first, so that even a crash between
    // the two writes leaves the header no lower than the epoch this incarnation
    // was willing to own. A medium that cannot carry the claim out of band says
    // so, and the durable BeginEpoch record below remains authoritative.
    if (impl_->log != nullptr) {
        auto recordedOnMedium = impl_->log->record_claimed_epoch(requestedEpoch, incarnation);
        if (!recordedOnMedium.has_value() && recordedOnMedium.error().cls() != ErrorClass::Unsupported) {
            return recordedOnMedium.error();
        }
    }

    StateApplier applier{*impl_, false};
    auto claimed = applier.op_begin_epoch(requestedEpoch, incarnation);
    if (!claimed.has_value()) {
        return claimed.error();
    }

    // Evidence from a previous incarnation is history, not authority. Every
    // view is fenced until fresh evidence is presented and accepted again.
    impl_->evidence.fence_all();
    report.evidence_fenced = true;
    impl_->opened = true;
    return report;
}

void Engine::shutdown() {
    stop_worker();
    std::lock_guard<std::mutex> guard(impl_->state);
    impl_->shutdown = true;
    impl_->opened = false;
}

void Engine::start_worker() {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->workerStarted) {
        return;
    }
    impl_->workerStop = false;
    impl_->workerStarted = true;
    impl_->worker = std::thread([this] {
        while (true) {
            {
                std::unique_lock<std::mutex> inner(impl_->state);
                impl_->workerSignal.wait_for(inner, std::chrono::milliseconds(1),
                                             [this] { return impl_->workerStop; });
                if (impl_->workerStop) {
                    return;
                }
            }
            // The state lock is released before the wait above and before this
            // call: the worker never holds the state lock while waiting.
            const auto result = tick();
            (void)result;
            if (!impl_->options.worker_interval.is_zero()) {
                std::this_thread::sleep_for(std::chrono::nanoseconds(impl_->options.worker_interval.nanos()));
            }
        }
    });
}

void Engine::stop_worker() {
    {
        std::lock_guard<std::mutex> guard(impl_->state);
        if (!impl_->workerStarted) {
            return;
        }
        impl_->workerStop = true;
    }
    impl_->workerSignal.notify_all();
    // Joining happens without the state lock: the worker must be able to take
    // the lock in order to finish the tick it is running.
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    std::lock_guard<std::mutex> guard(impl_->state);
    impl_->workerStarted = false;
    impl_->worker = std::thread{};
}

bool Engine::worker_running() const noexcept {
    std::lock_guard<std::mutex> guard(impl_->state);
    return impl_->workerStarted;
}

Epoch Engine::epoch() const noexcept {
    std::lock_guard<std::mutex> guard(impl_->state);
    return impl_->epoch;
}

std::string Engine::incarnation() const {
    std::lock_guard<std::mutex> guard(impl_->state);
    return impl_->incarnationName;
}

std::uint64_t Engine::sequence() const noexcept {
    std::lock_guard<std::mutex> guard(impl_->state);
    return impl_->sequence;
}

// ---------------------------------------------------------------------------
// Evidence and authority
// ---------------------------------------------------------------------------

Result<EvidenceId> Engine::observe(const AdapterObservation& observation) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }

    const EvidenceId id = evidence_identity(observation);

    Observation stored{id,          observation.domain(),   observation.generation(),  ObservationSource::Adapter,
                       observation.origin(), observation.key(), observation.reading(), observation.observed_at(),
                       observation.authority(), observation.stale()};

    StateApplier applier{*impl_, false};
    auto accepted = applier.op_accept_observation(stored);
    if (!accepted.has_value()) {
        return accepted.error();
    }
    return id;
}

Result<AuthorityRef> Engine::accept_authority(Domain domain, const AuthorityRef& reference) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    StateApplier applier{*impl_, false};
    auto accepted = applier.op_accept_authority(domain, reference, impl_->clock->now());
    if (!accepted.has_value()) {
        return accepted.error();
    }
    return reference;
}

Result<AuthorityRef> Engine::retire_authority(Domain domain, std::string_view reason) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    const auto current = impl_->ledger.current(domain);
    if (!current.has_value()) {
        return errors::not_found("authority.not_accepted", "cannot retire authority that is not accepted");
    }
    StateApplier applier{*impl_, false};
    auto retired = applier.op_retire_authority(domain, reason, impl_->clock->now());
    if (!retired.has_value()) {
        return retired.error();
    }
    return *current;
}

EvidenceView Engine::view(Domain domain) const {
    std::lock_guard<std::mutex> guard(impl_->state);
    return impl_->evidence.view(domain);
}

std::optional<AuthorityRef> Engine::current_authority(Domain domain) const {
    std::lock_guard<std::mutex> guard(impl_->state);
    return impl_->ledger.current(domain);
}

std::vector<Observation> Engine::observations(Domain domain) const {
    std::lock_guard<std::mutex> guard(impl_->state);
    return impl_->evidence.observations_for(domain);
}

// ---------------------------------------------------------------------------
// Plans
// ---------------------------------------------------------------------------

Result<PlanId> Engine::propose_plan(const PlanDefinition& definition) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    if (definition.steps.size() > impl_->options.max_steps) {
        return errors::limit_exceeded("plan.step_count", "the plan exceeds the configured maximum step count");
    }

    auto id = impl_->ids.allocate(IdKind::Plan);
    if (!id.has_value()) {
        return errors::overflow("plan.identity", "the plan identity counter is exhausted");
    }
    const PlanId planId{*id};

    Plan plan{planId, definition.name, definition.scope, definition.strategy, 1, impl_->clock->now()};
    for (const StepSpec& spec : definition.steps) {
        plan.add_step(spec);
    }
    for (const auto& [domain, requirement] : definition.authority_requirements) {
        plan.require_authority(domain, requirement);
    }
    for (const auto& [domain, freshness] : definition.freshness_requirements) {
        plan.require_freshness(domain, freshness);
    }

    PlanValidationOptions options;
    options.max_steps = impl_->options.max_steps;
    auto shape = validate_plan_shape(plan, options);
    if (!shape.has_value()) {
        return shape.error();
    }

    impl_->plans[planId] = std::move(plan);
    return planId;
}

Result<PlanId> Engine::publish_plan(const PlanId& planId) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    const auto it = impl_->plans.find(planId);
    if (it == impl_->plans.end()) {
        return errors::not_found("plan.unknown", "no such plan");
    }
    if (impl_->supersededPlans.find(planId) != impl_->supersededPlans.end()) {
        return errors::precondition_failed("plan.superseded", "the plan was already superseded");
    }
    if (it->second.status() == PlanStatus::Published) {
        return planId;
    }

    Plan plan = it->second;
    plan.set_status(PlanStatus::Published);
    StateApplier applier{*impl_, false};
    auto published = applier.op_publish_plan(std::move(plan));
    if (!published.has_value()) {
        return published.error();
    }
    return planId;
}

Result<bool> Engine::supersede_plan(const PlanId& planId, std::string_view reason) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    if (impl_->plans.find(planId) == impl_->plans.end()) {
        return errors::not_found("plan.unknown", "no such plan");
    }
    if (impl_->supersededPlans.find(planId) != impl_->supersededPlans.end()) {
        return true;
    }

    // Publishing a newer revision fences every execution that is still bound to
    // the older authority. The executions are not silently re-pointed at the
    // new plan: they stop.
    const auto now = impl_->clock->now();
    StateApplier applier{*impl_, false};
    for (auto& [executionId, state] : impl_->executions) {
        if (state.plan != planId || !is_running_lifecycle(state.lifecycle)) {
            continue;
        }
        auto fenced = applier.op_fence(state, state.fenced, kReasonSuperseded, now);
        if (!fenced.has_value()) {
            return fenced.error();
        }
        auto closed = applier.op_lifecycle(state, Lifecycle::Stale, kReasonSuperseded, now);
        if (!closed.has_value()) {
            return closed.error();
        }
    }
    auto superseded = applier.op_supersede_plan(planId, reason);
    if (!superseded.has_value()) {
        return superseded.error();
    }
    return true;
}

std::optional<Plan> Engine::plan(const PlanId& id) const {
    std::lock_guard<std::mutex> guard(impl_->state);
    const auto it = impl_->plans.find(id);
    if (it == impl_->plans.end()) {
        return std::nullopt;
    }
    return it->second;
}

std::vector<PlanId> Engine::plan_ids() const {
    std::lock_guard<std::mutex> guard(impl_->state);
    std::vector<PlanId> ids;
    ids.reserve(impl_->plans.size());
    for (const auto& [id, value] : impl_->plans) {
        (void)value;
        ids.push_back(id);
    }
    return ids;
}

// ---------------------------------------------------------------------------
// Executions
// ---------------------------------------------------------------------------

namespace {

[[nodiscard]] ExecState build_execution_state(const Plan& plan, const ExecutionId& id, Epoch epoch, TimePoint now,
                                              const Digest& content) {
    ExecState state;
    state.id = id;
    state.plan = plan.id();
    state.revision = plan.revision();
    state.content = content;
    state.lifecycle = Lifecycle::Running;
    state.epoch = epoch;
    state.startedAt = now;
    state.updatedAt = now;
    for (const StepSpec& spec : plan.steps()) {
        state.steps.emplace(spec.id, StepRuntime{});
    }
    return state;
}

// The plan is bound to the authority the coordinator accepted. A plan whose
// requirements are not currently satisfied is refused: the coordinator never
// adapts a plan to new authority on its own.
[[nodiscard]] Result<bool> require_current_authority(const Plan& plan, const AuthorityLedger& ledger) {
    for (const auto& [domain, requirement] : plan.authority_requirements()) {
        const auto current = ledger.current(domain);
        if (!current.has_value()) {
            return errors::stale_authority("plan.authority_unestablished",
                                           "no authority is accepted for a domain the plan requires");
        }
        if (*current != requirement) {
            return errors::stale_authority("plan.authority_changed",
                                           "accepted authority no longer matches the plan binding");
        }
    }
    return true;
}

}  // namespace


Result<ExecutionId> Engine::start_execution(const PlanId& planId) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    const auto planIt = impl_->plans.find(planId);
    if (planIt == impl_->plans.end()) {
        return errors::not_found("plan.unknown", "no such plan");
    }
    if (planIt->second.status() != PlanStatus::Published) {
        return errors::precondition_failed("plan.not_published", "the plan has not been published");
    }
    auto current = require_current_authority(planIt->second, impl_->ledger);
    if (!current.has_value()) {
        return current.error();
    }

    auto id = impl_->ids.allocate(IdKind::Execution);
    if (!id.has_value()) {
        return errors::overflow("execution.identity", "the execution identity counter is exhausted");
    }
    const ExecutionId executionId{*id};

    const auto now = impl_->clock->now();
    ExecState state =
        build_execution_state(planIt->second, executionId, impl_->epoch, now, codec::plan_content_digest_of(codec::plan_to_json(planIt->second)));

    StateApplier applier{*impl_, false};
    auto started = applier.op_start_execution(std::move(state));
    if (!started.has_value()) {
        return started.error();
    }
    return executionId;
}

std::vector<ExecutionId> Engine::execution_ids() const {
    std::lock_guard<std::mutex> guard(impl_->state);
    std::vector<ExecutionId> ids;
    ids.reserve(impl_->executions.size());
    for (const auto& [id, state] : impl_->executions) {
        (void)state;
        ids.push_back(id);
    }
    return ids;
}

std::optional<ExecutionView> Engine::execution(const ExecutionId& id) const {
    std::lock_guard<std::mutex> guard(impl_->state);
    const auto it = impl_->executions.find(id);
    if (it == impl_->executions.end()) {
        return std::nullopt;
    }
    const ExecState& state = it->second;

    ExecutionView view;
    view.id = state.id;
    view.plan = state.plan;
    view.plan_revision = state.revision;
    view.plan_content = state.content;
    view.lifecycle = state.lifecycle;
    view.epoch = state.epoch;
    view.fenced_domains = state.fenced;
    view.compensation_rounds = state.compensationRounds;
    view.started_at = state.startedAt;
    view.updated_at = state.updatedAt;
    view.reason = state.reason;
    view.point_of_no_return_passed = state.pointOfNoReturn;
    for (const auto& [step, runtime] : state.steps) {
        view.steps.emplace(step, runtime.state);
    }

    const auto index = impl_->attemptIndex.find(id);
    if (index != impl_->attemptIndex.end()) {
        for (const auto& [key, attemptId] : index->second) {
            (void)key;
            const auto attempt = impl_->attempts.find(attemptId);
            if (attempt == impl_->attempts.end()) {
                continue;
            }
            AttemptView attemptView;
            attemptView.id = attempt->second.id();
            attemptView.step = attempt->second.step();
            attemptView.attempt_number = attempt->second.attempt_number();
            attemptView.kind = attempt->second.kind();
            attemptView.status = attempt->second.status();
            attemptView.epoch = attempt->second.epoch();
            attemptView.binding = attempt->second.binding_digest();
            attemptView.idempotency = attempt->second.idempotency();
            attemptView.ambiguous = attempt->second.ambiguous();
            attemptView.acknowledged = attempt->second.has_acknowledgement();
            attemptView.observed = attempt->second.has_observation();
            attemptView.effect = attempt->second.effect_status();
            attemptView.dispatch_count = attempt->second.dispatch_count();
            attemptView.detail = attempt->second.detail();
            view.attempts.push_back(std::move(attemptView));
        }
    }
    std::sort(view.attempts.begin(), view.attempts.end(), [](const AttemptView& lhs, const AttemptView& rhs) {
        if (lhs.attempt_number != rhs.attempt_number) {
            return lhs.attempt_number < rhs.attempt_number;
        }
        return lhs.id < rhs.id;
    });
    return view;
}

Result<bool> Engine::cancel_execution(const ExecutionId& id, std::string_view reason) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    const auto it = impl_->executions.find(id);
    if (it == impl_->executions.end()) {
        return errors::not_found("execution.unknown", "no such execution");
    }
    if (is_terminal(it->second.lifecycle)) {
        return errors::precondition_failed("execution.closed", "the execution is already closed");
    }
    const auto now = impl_->clock->now();
    StateApplier applier{*impl_, false};
    auto closed = applier.op_close(it->second, Lifecycle::Cancelled, std::string{reason}, now);
    if (!closed.has_value()) {
        return closed.error();
    }
    return true;
}

Result<bool> Engine::abandon_execution(const ExecutionId& id, std::string_view reason) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    const auto it = impl_->executions.find(id);
    if (it == impl_->executions.end()) {
        return errors::not_found("execution.unknown", "no such execution");
    }
    if (is_terminal(it->second.lifecycle)) {
        return errors::precondition_failed("execution.closed", "the execution is already closed");
    }
    // Abandoning is only meaningful once an irreversible effect has been
    // verified. Before that the caller should compensate or cancel instead.
    if (!it->second.pointOfNoReturn) {
        return errors::precondition_failed(
            "execution.no_point_of_no_return",
            "no point of no return has been verified; compensate or cancel instead of abandoning");
    }
    const auto now = impl_->clock->now();
    StateApplier applier{*impl_, false};
    auto closed = applier.op_close(it->second, Lifecycle::Abandoned, std::string{reason}, now);
    if (!closed.has_value()) {
        return closed.error();
    }
    return true;
}

Result<bool> Engine::begin_compensation(const ExecutionId& id, std::string_view reason) {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    const auto it = impl_->executions.find(id);
    if (it == impl_->executions.end()) {
        return errors::not_found("execution.unknown", "no such execution");
    }
    if (is_terminal(it->second.lifecycle)) {
        return errors::precondition_failed("execution.closed", "the execution is already closed");
    }
    ExecState& state = it->second;
    if (state.lifecycle == Lifecycle::Compensating) {
        return true;
    }
    if (state.pointOfNoReturn) {
        // Once an irreversible effect is verified the coordinator refuses to
        // pretend it can put the facility back. The caller decides what to do;
        // the coordinator does not invent a rollback.
        return errors::precondition_failed(
            "execution.irreversible",
            "a point of no return was passed; the plan cannot be compensated and must be abandoned");
    }
    if (state.compensationRounds >= impl_->options.max_compensation_rounds) {
        return errors::limit_exceeded("execution.compensation_rounds",
                                      "the compensation round budget for this execution is exhausted");
    }

    const auto now = impl_->clock->now();
    StateApplier applier{*impl_, false};
    ++state.compensationRounds;
    state.compensationActive = true;
    auto lifecycle = applier.op_lifecycle(state, Lifecycle::Compensating, std::string{reason}, now);
    if (!lifecycle.has_value()) {
        return lifecycle.error();
    }
    return true;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Attempt dispatch and reconciliation
// ---------------------------------------------------------------------------

namespace {

// Closes an execution. Terminal states are never reopened: a closed execution
// is a durable fact.
[[nodiscard]] Result<bool> close_execution(StateApplier& applier, ExecState& state, Lifecycle lifecycle,
                                           std::string reason, TimePoint now) {
    return applier.op_close(state, lifecycle, std::move(reason), now);
}

[[nodiscard]] Attempt make_attempt(const AttemptId& id, const ExecutionId& execution, const StepSpec& spec,
                                   const RequestBinding& binding, std::uint32_t number, AttemptKind kind, Epoch epoch,
                                   std::uint64_t revision, TimePoint now) {
    const std::string key = idempotency_key(execution, spec.id, number, binding, kind);
    return Attempt{id, execution, spec.id, number, kind, epoch, revision, binding, binding.digest(), key, now};
}

// Builds the bounded request the coordinator is allowed to hand an adjacent
// authority. It carries no plan identity, only the idempotency key and the
// epoch under which it was issued.
[[nodiscard]] Request make_request(const Attempt& attempt) {
    const std::string domain{to_string(attempt.binding().domain)};
    RequestContext context{attempt.id().to_text(), attempt.epoch(), attempt.idempotency(), domain};
    return Request{domain,
                   attempt.binding().operation,
                   attempt.binding().parameters,
                   attempt.binding_digest(),
                   attempt.idempotency(),
                   std::move(context),
                   attempt.binding().point_of_no_return,
                   attempt.kind() == AttemptKind::Compensation,
                   attempt.kind(),
                   domain};
}

// Converts an adapter response into the coordinator's own assessment of the
// attempt. This is the only place where an adapter opinion becomes coordinator
// state, and it never accepts a claim as an effect unless the claim carries an
// authority reference that is current for the domain.
struct ResponseOutcome {
    AttemptStatus status{AttemptStatus::Dispatched};
    EffectStatus effect{EffectStatus::NotAssessed};
    bool acknowledged{false};
    bool observed{false};
    std::string detail{};
    Digest observedDigest{};
    Digest expectationDigest{};
    AuthorityRef authority{};
    EvidenceId evidence{};
    bool ambiguous{false};
};

[[nodiscard]] ResponseOutcome interpret_response(Engine::Impl& impl, const Attempt& attempt,
                                                 const AdapterResponse& response) {
    ResponseOutcome outcome;
    outcome.detail = response.detail();

    // Everything below is decided from two durable inputs: the authority the
    // coordinator accepted for the domain this attempt binds to, and the
    // evidence the coordinator has itself accepted for it. Nothing is taken
    // from the adapter on trust.
    const Domain domain = attempt.binding().domain;
    const EvidenceView view = impl.evidence.view(domain);
    const auto current = impl.ledger.current(domain);
    const bool authorityCurrent = current.has_value() && view.authority() == *current;

    // An observation confirms an effect; a claim never does. The observation
    // must be about the domain this attempt binds to, and the coordinator must
    // already hold it as accepted evidence: what the authority reports now has
    // to be what the coordinator accepted, from the authority it accepted.
    const AdapterObservation* confirmation = nullptr;
    for (const AdapterObservation& observation : response.observations()) {
        if (observation.key().empty() || observation.origin().empty()) {
            continue;
        }
        if (observation.domain() != domain) {
            continue;
        }
        if (view.authority() != observation.authority()) {
            continue;
        }
        const auto acceptedReading = view.find(observation.key());
        if (!acceptedReading.has_value() || !(*acceptedReading == observation.reading())) {
            continue;
        }
        confirmation = &observation;
        break;
    }

    if (confirmation != nullptr) {
        outcome.observed = true;
        outcome.authority = confirmation->authority();
        outcome.evidence = evidence_identity(*confirmation);
        CanonicalWriter writer;
        append_framed(writer, "recovery.evidence_view.v1");
        append_framed(writer, to_string(confirmation->domain()));
        append_framed(writer, confirmation->authority().canonical());
        append_framed_u64(writer, confirmation->generation());
        append_framed(writer, confirmation->key());
        append_framed(writer, confirmation->reading().canonical());
        outcome.observedDigest = writer.digest_of();
    }

    for (const EffectAssessment& assessment : response.assessments()) {
        if (assessment.status == EffectStatus::NotAssessed) {
            continue;
        }
        if (!authorityCurrent) {
            outcome.effect = EffectStatus::Unverifiable;
            outcome.detail = "the reported assessment is not bound to current authority";
            continue;
        }
        if (!outcome.observed) {
            // An assessment is the authority's answer about the expectation it
            // was given, but it is still only a claim. Without an accepted
            // observation that confirms it, the coordinator records nothing:
            // an acknowledgement is not an effect.
            outcome.detail = "the reported assessment is not confirmed by an accepted observation";
            continue;
        }
        if (assessment.observed != assessment.expectation) {
            // The claim contradicts itself: it says the expectation was met
            // while naming a different reading as the one it observed. A
            // self-contradictory claim is not evidence of anything, and the
            // coordinator must not promote it to a verified effect merely
            // because the digests are present. The durable record keeps both
            // digests, so a later reader can see exactly what was compared and
            // why it was refused.
            outcome.effect = EffectStatus::Unverifiable;
            outcome.detail = "the reported assessment is internally inconsistent: the observed evidence is not "
                             "the evidence the expectation named";
            continue;
        }
        outcome.effect = assessment.status;
        outcome.expectationDigest = assessment.expectation;
        if (!assessment.observed.is_zero()) {
            outcome.observedDigest = assessment.observed;
        }
        if (!assessment.detail.empty()) {
            outcome.detail = assessment.detail;
        }
    }

    switch (response.status()) {
        case ResponseStatus::Completed:
            outcome.acknowledged = true;
            if (!outcome.observed) {
                // The authority says the operation completed and the
                // coordinator has no evidence of it. That is an
                // acknowledgement, and an acknowledgement is not an effect.
                outcome.status = AttemptStatus::Dispatched;
                break;
            }
            switch (outcome.effect) {
                case EffectStatus::Verified:
                    outcome.status = AttemptStatus::EffectPresent;
                    break;
                case EffectStatus::Contradicted:
                    // A definite outcome that does not satisfy the expectation
                    // is a failure, not an unresolved request.
                    outcome.status = AttemptStatus::Failed;
                    break;
                case EffectStatus::Unverifiable:
                    outcome.status = AttemptStatus::Unverifiable;
                    break;
                case EffectStatus::NotAssessed:
                    outcome.status = AttemptStatus::Dispatched;
                    break;
            }
            break;
        case ResponseStatus::InProgress:
            outcome.acknowledged = true;
            outcome.status = AttemptStatus::Dispatched;
            break;
        case ResponseStatus::Refused:
            outcome.status = AttemptStatus::Refused;
            break;
        case ResponseStatus::Failed:
            outcome.status = AttemptStatus::Failed;
            break;
        case ResponseStatus::Unknown:
            // The adjacent authority has no record of the key. That is proof
            // that no effect was produced, which is exactly what makes a
            // re-dispatch safe. Under a policy that permits a re-dispatch the
            // attempt resolves as a spent failure; under a policy that forbids
            // reissuing, the attempt stays unresolved and the coordinator only
            // reports what it learned.
            if (impl.options.restart_policy == RestartPolicy::ReconcileThenRedispatch) {
                outcome.status = AttemptStatus::Failed;
            } else {
                // The absence is proven, but this policy forbids acting on it, so
                // the attempt stays unresolved and keeps its ambiguity: nothing
                // here is knowledge the coordinator is allowed to use.
                outcome.status = AttemptStatus::Dispatched;
                outcome.ambiguous = true;
            }
            outcome.detail = response.detail().empty() ? "the adjacent authority has no record of the key"
                                                       : response.detail();
            break;
        case ResponseStatus::Indeterminate:
            outcome.status = AttemptStatus::Dispatched;
            outcome.ambiguous = true;
            break;
    }
    return outcome;
}

struct TickContext {
    StateApplier* applier{nullptr};
    Engine::Impl* impl{nullptr};
    TimePoint now{};
    TickReport* report{nullptr};
    bool didWork{false};

    [[nodiscard]] std::uint32_t max_attempts_per_step() const { return impl->options.max_attempts_per_step; }
};

// Applies a response outcome to an attempt, then propagates the resulting state
// into the step state machine. The attempt is made durable first, and the step
// is recorded against it afterwards.
[[nodiscard]] Result<bool> apply_outcome(TickContext& context, ExecState& state, const StepSpec& spec, Attempt& attempt,
                                         const ResponseOutcome& outcome, bool compensating) {
    const AttemptStatus previousStatus = attempt.status();
    const bool changed = previousStatus != outcome.status || attempt.ambiguous() != outcome.ambiguous ||
                         attempt.detail() != outcome.detail;
    AttemptMutation::assign_status(attempt, outcome.status, outcome.ambiguous, outcome.detail);
    if (outcome.acknowledged) {
        AttemptMutation::record_acknowledgement(attempt, outcome.detail, context.now);
    }
    if (outcome.observed) {
        AttemptMutation::record_assessment(attempt, outcome.effect, outcome.observedDigest,
                                           outcome.expectationDigest, outcome.authority, outcome.evidence,
                                           outcome.detail, context.now);
    }
    if (changed || outcome.observed || outcome.acknowledged) {
        auto written = context.applier->op_update_attempt(attempt, RecordKind::AttemptAssessed);
        if (!written.has_value()) {
            return written.error();
        }
    }

    StepRuntime& runtime = state.steps[spec.id];
    if (!compensating) {
        switch (outcome.status) {
            case AttemptStatus::Refused:
            case AttemptStatus::Failed:
                if (runtime.attempts >= context.max_attempts_per_step()) {
                    auto transition = context.applier->op_step_state(
                        state, spec.id,
                        outcome.status == AttemptStatus::Refused ? StepState::Refused : StepState::Failed,
                        context.now);
                    if (!transition.has_value()) {
                        return transition.error();
                    }
                } else if (runtime.state == StepState::Dispatched || runtime.state == StepState::Acknowledged) {
                    // A definite "no effect" answer spends one attempt. While the
                    // budget remains the step returns to a dispatchable state so
                    // that a later tick issues a new attempt, with a new attempt
                    // number and therefore a new idempotency key. The key of the
                    // spent attempt is never reused.
                    auto transition = context.applier->op_step_state(state, spec.id, StepState::Ready, context.now);
                    if (!transition.has_value()) {
                        return transition.error();
                    }
                }
                break;
            case AttemptStatus::Unverifiable:
                // An unverifiable outcome is uncertain, not a definite absence of
                // an effect, so the coordinator must not reissue it on its own
                // initiative. It becomes a failure only once the attempt budget
                // is spent.
                if (runtime.attempts >= context.max_attempts_per_step()) {
                    auto transition = context.applier->op_step_state(state, spec.id, StepState::Failed, context.now);
                    if (!transition.has_value()) {
                        return transition.error();
                    }
                }
                break;
            case AttemptStatus::EffectPresent: {
                runtime.authority = AttemptMutation::assessment_of(attempt).authority;
                runtime.evidence = AttemptMutation::assessment_of(attempt).observed;
                runtime.verifiedAt = context.now;
                auto transition = context.applier->op_step_state(state, spec.id, StepState::Verified, context.now);
                if (!transition.has_value()) {
                    return transition.error();
                }
                if (attempt.binding().point_of_no_return) {
                    state.pointOfNoReturn = true;
                }
                break;
            }
            case AttemptStatus::Dispatched:
            case AttemptStatus::Pending:
            case AttemptStatus::Superseded:
                break;
        }
        if (AttemptMutation::has_acknowledgement(attempt) && runtime.state == StepState::Dispatched) {
            auto transition = context.applier->op_step_state(state, spec.id, StepState::Acknowledged, context.now);
            if (!transition.has_value()) {
                return transition.error();
            }
        }
    } else {
        switch (outcome.status) {
            case AttemptStatus::EffectPresent: {
                runtime.compensated = true;
                auto transition =
                    context.applier->op_step_state(state, spec.id, StepState::Compensated, context.now);
                if (!transition.has_value()) {
                    return transition.error();
                }
                break;
            }
            case AttemptStatus::Refused:
            case AttemptStatus::Failed:
            case AttemptStatus::Unverifiable:
                if (spec.compensation_required) {
                    auto transition =
                        context.applier->op_step_state(state, spec.id, StepState::Uncompensable, context.now);
                    if (!transition.has_value()) {
                        return transition.error();
                    }
                }
                break;
            case AttemptStatus::Dispatched:
            case AttemptStatus::Pending:
            case AttemptStatus::Superseded:
                break;
        }
    }
    context.didWork = true;
    return true;
}

// Emits one attempt for a step. The attempt is durable before the adapter is
// called, which is what makes process death unable to cause a blind duplicate:
// a request is either recorded before it is sent, or it was never sent.
[[nodiscard]] Result<AttemptId> commit_attempt(TickContext& context, ExecState& state, const StepSpec& spec,
                                               const RequestBinding& binding, AttemptKind kind, bool compensating) {
    StepRuntime& runtime = state.steps[spec.id];
    const std::uint32_t number = compensating ? runtime.compensationAttempts + 1 : runtime.attempts + 1;
    if (compensating) {
        runtime.compensationAttempts = number;
    } else {
        runtime.attempts = number;
    }

    auto id = context.impl->ids.allocate(IdKind::Attempt);
    if (!id.has_value()) {
        return errors::overflow("attempt.identity", "the attempt identity counter is exhausted");
    }
    const AttemptId attemptId{*id};
    Attempt attempt = make_attempt(attemptId, state.id, spec, binding, number, kind, context.impl->epoch,
                                   state.revision, context.now);
    auto written = context.applier->op_commit_attempt(attempt, RecordKind::AttemptCommitted);
    if (!written.has_value()) {
        return written.error();
    }
    auto transition = context.applier->op_step_state(
        state, spec.id, compensating ? StepState::Compensating : StepState::Dispatched, context.now);
    if (!transition.has_value()) {
        return transition.error();
    }
    if (context.report != nullptr) {
        StepTransition record;
        record.step = spec.id;
        record.before = compensating ? StepState::Verified : StepState::Ready;
        record.after = compensating ? StepState::Compensating : StepState::Dispatched;
        record.reason = "attempt committed";
        context.report->transitions.push_back(std::move(record));
    }
    context.didWork = true;
    return attemptId;
}

// Calls the adapter with the state lock held and marks adapter execution so
// that a re-entrant engine call is refused rather than deadlocked.
[[nodiscard]] Result<AdapterResponse> invoke_adapter(Engine::Impl& impl, const Request& request, bool dispatch) {
    impl.adapterActive = true;
    struct Guard {
        Engine::Impl* impl{nullptr};
        ~Guard() { impl->adapterActive = false; }
    } guard{&impl};

    if (dispatch) {
        return impl.port->dispatch(request);
    }
    return impl.port->inspect(request);
}

[[nodiscard]] Result<bool> accept_adapter_observation(TickContext& context,
                                                      const AdapterObservation& observation) {
    if (observation.key().empty() || observation.origin().empty()) {
        return true;
    }
    Observation stored{evidence_identity(observation),
                       observation.domain(),
                       observation.generation(),
                       ObservationSource::Adapter,
                       observation.origin(),
                       observation.key(),
                       observation.reading(),
                       observation.observed_at(),
                       observation.authority(),
                       observation.stale()};
    auto accepted = context.applier->op_accept_observation(stored);
    if (!accepted.has_value()) {
        return accepted.error();
    }
    context.didWork = true;
    return true;
}

[[nodiscard]] Result<bool> dispatch_attempt(TickContext& context, ExecState& state, const StepSpec& spec,
                                            const RequestBinding& binding, bool compensating) {
    auto attemptId = commit_attempt(context, state, spec, binding,
                                    compensating ? AttemptKind::Compensation : AttemptKind::Forward, compensating);
    if (!attemptId.has_value()) {
        return attemptId.error();
    }

    Attempt& attempt = context.impl->attempts[attemptId.value()];
    AttemptMutation::count_dispatch(attempt);
    AttemptMutation::set_dispatch_intent(attempt, context.now);
    const Request request = make_request(attempt);

    auto response = invoke_adapter(*context.impl, request, true);
    if (!response.has_value()) {
        // A transport failure is not knowledge about the world. The attempt
        // stays ambiguous and is never reissued on the coordinator's own
        // initiative.
        AttemptMutation::assign_status(attempt, AttemptStatus::Dispatched, true,
                                      "the adjacent authority could not be reached: " +
                                          response.error().describe());
        auto written = context.applier->op_update_attempt(attempt, RecordKind::AttemptAssessed);
        if (!written.has_value()) {
            return written.error();
        }
        if (context.report != nullptr) {
            DispatchReport record;
            record.execution = state.id;
            record.step = spec.id;
            record.attempt = attempt.id();
            record.attempt_number = attempt.attempt_number();
            record.response = ResponseStatus::Indeterminate;
            record.ambiguous = true;
            record.detail = attempt.detail();
            context.report->dispatches.push_back(std::move(record));
        }
        return true;
    }

    // Observations the adapter volunteered become evidence before the response
    // is interpreted, so that an assessment carried by the same response can be
    // checked against the evidence that arrived with it.
    for (const AdapterObservation& observation : response.value().observations()) {
        auto accepted = accept_adapter_observation(context, observation);
        if (!accepted.has_value()) {
            return accepted.error();
        }
    }

    const ResponseOutcome outcome = interpret_response(*context.impl, attempt, response.value());
    auto applied = apply_outcome(context, state, spec, attempt, outcome, compensating);
    if (!applied.has_value()) {
        return applied.error();
    }
    if (context.report != nullptr) {
        DispatchReport record;
        record.execution = state.id;
        record.step = spec.id;
        record.attempt = attempt.id();
        record.attempt_number = attempt.attempt_number();
        record.response = response.value().status();
        record.ambiguous = outcome.ambiguous;
        record.detail = outcome.detail;
        context.report->dispatches.push_back(std::move(record));
    }
    return true;
}

// Reconciles one unresolved attempt through idempotent replay. The coordinator
// asks the adjacent authority what happened; it never assumes.
[[nodiscard]] Result<bool> reconcile_attempt(TickContext& context, ExecState& state, const StepSpec& spec,
                                             Attempt& attempt) {
    if (context.impl->options.restart_policy == RestartPolicy::ReportOnly) {
        return false;
    }
    const Request request = make_request(attempt);
    auto response = invoke_adapter(*context.impl, request, false);
    if (!response.has_value()) {
        const std::string reconcileDetail =
            "reconciliation could not reach the adjacent authority: " + response.error().describe();
        AttemptMutation::assign_status(attempt, attempt.status(), true, reconcileDetail);
        auto written = context.applier->op_update_attempt(attempt, RecordKind::AttemptAssessed);
        if (!written.has_value()) {
            return written.error();
        }
        return false;
    }

    // Observations the adapter volunteered become evidence before the response
    // is interpreted, exactly as on the dispatch path, so that an assessment
    // carried by the same response can be checked against the evidence that
    // arrived with it rather than against nothing.
    for (const AdapterObservation& observation : response.value().observations()) {
        auto accepted = accept_adapter_observation(context, observation);
        if (!accepted.has_value()) {
            return accepted.error();
        }
    }

    const ResponseOutcome outcome = interpret_response(*context.impl, attempt, response.value());
    // Reconciliation resolves the ambiguity: the coordinator now knows what the
    // authority recorded, whether or not that is a definite effect.
    AttemptMutation::set_ambiguity(attempt, false);
    auto applied =
        apply_outcome(context, state, spec, attempt, outcome, attempt.kind() == AttemptKind::Compensation);
    if (!applied.has_value()) {
        return applied.error();
    }
    if (context.report != nullptr) {
        DispatchReport record;
        record.execution = state.id;
        record.step = spec.id;
        record.attempt = attempt.id();
        record.attempt_number = attempt.attempt_number();
        record.response = response.value().status();
        record.reconciled = true;
        record.detail = outcome.detail;
        context.report->dispatches.push_back(std::move(record));
    }

    // Only a proven "the authority has no record of the key" permits a
    // re-dispatch, and only when the policy allows it. The re-dispatch uses a
    // new attempt number and therefore a new idempotency key.
    if (response.value().status() != ResponseStatus::Unknown ||
        context.impl->options.restart_policy != RestartPolicy::ReconcileThenRedispatch) {
        return true;
    }
    StepRuntime& runtime = state.steps[spec.id];
    const bool compensating = attempt.kind() == AttemptKind::Compensation;
    const std::uint32_t used = compensating ? runtime.compensationAttempts : runtime.attempts;
    if (used >= context.max_attempts_per_step()) {
        return true;
    }
    auto redispatch = dispatch_attempt(context, state, spec, attempt.binding(), compensating);
    if (!redispatch.has_value()) {
        return redispatch.error();
    }
    return true;
}

// Evaluates one execution. The stages run in a fixed order, so a tick is
// reproducible for the same durable state:
//   1. plan binding check against current authority;
//   2. reconciliation of unresolved attempts;
//   3. assessment of in-flight attempts against current evidence;
//   4. readiness evaluation, gate evaluation and dispatch;
//   5. completion, failure and deadline derivation.
[[nodiscard]] Result<bool> execute_plan(TickContext& context, ExecState& state) {
    const auto planIt = context.impl->plans.find(state.plan);
    if (planIt == context.impl->plans.end()) {
        return errors::internal("execution.plan_missing", "the execution refers to a plan that is not loaded");
    }
    const Plan& plan = planIt->second;

    // Stage 1: the plan binding is checked on every tick. A binding that is no
    // longer current fences the execution rather than adapting the plan.
    for (const auto& [domain, requirement] : plan.authority_requirements()) {
        const auto current = context.impl->ledger.current(domain);
        if (!current.has_value() || *current != requirement) {
            auto fenced = context.applier->op_fence(state, state.fenced, kReasonStaleAuthority, context.now);
            if (!fenced.has_value()) {
                return fenced.error();
            }
            auto lifecycle =
                context.applier->op_lifecycle(state, Lifecycle::Stale, kReasonStaleAuthority, context.now);
            if (!lifecycle.has_value()) {
                return lifecycle.error();
            }
            if (context.report != nullptr) {
                context.report->fenced.push_back(state.id);
            }
            context.didWork = true;
            return true;
        }
    }

    // Stage 2: unresolved attempts. An attempt still marked Dispatched while
    // the current epoch is newer than the one it was issued under was in flight
    // when the previous incarnation ended.
    std::vector<AttemptId> unresolved;
    for (const auto& [key, attemptId] : context.impl->attemptIndex[state.id]) {
        (void)key;
        const auto attempt = context.impl->attempts.find(attemptId);
        if (attempt == context.impl->attempts.end()) {
            continue;
        }
        if (attempt->second.status() != AttemptStatus::Dispatched) {
            continue;
        }
        unresolved.push_back(attemptId);
    }
    std::sort(unresolved.begin(), unresolved.end());
    for (const AttemptId& attemptId : unresolved) {
        Attempt& attempt = context.impl->attempts[attemptId];
        const StepSpec* spec = plan.find_step(attempt.step());
        if (spec == nullptr) {
            return errors::internal("execution.step_missing",
                                    "the attempt refers to a step that is not in the plan");
        }
        const bool staleIncarnation = attempt.epoch().value() != context.impl->epoch.value();
        if (staleIncarnation) {
            AttemptMutation::set_ambiguity(attempt, true);
        }
        auto reconciled = reconcile_attempt(context, state, *spec, attempt);
        if (!reconciled.has_value()) {
            return reconciled.error();
        }
        if (staleIncarnation && context.report != nullptr) {
            context.report->reconciled.push_back(attemptId);
        }
    }

    // Stage 3: assessment. An attempt whose observation budget expired without
    // authoritative evidence is declared unverifiable; it is never silently
    // treated as a success.
    std::vector<AttemptId> inflight;
    for (const auto& [key, attemptId] : context.impl->attemptIndex[state.id]) {
        (void)key;
        const auto attempt = context.impl->attempts.find(attemptId);
        if (attempt == context.impl->attempts.end()) {
            continue;
        }
        if (attempt->second.status() != AttemptStatus::Dispatched || attempt->second.ambiguous()) {
            continue;
        }
        inflight.push_back(attemptId);
    }
    std::sort(inflight.begin(), inflight.end());
    for (const AttemptId& attemptId : inflight) {
        Attempt& attempt = context.impl->attempts[attemptId];
        const StepSpec* spec = plan.find_step(attempt.step());
        if (spec == nullptr) {
            return errors::internal("execution.step_missing",
                                    "the attempt refers to a step that is not in the plan");
        }
        const auto dispatchedAt = attempt.dispatch_intent_at().value_or(attempt.committed_at());
        const auto age = context.now.since(dispatchedAt);
        if (!age.has_value()) {
            continue;
        }
        if (context.impl->options.observation_budget.is_zero() ||
            !(context.impl->options.observation_budget < *age)) {
            continue;
        }
        ResponseOutcome outcome;
        outcome.status = AttemptStatus::Unverifiable;
        outcome.effect = EffectStatus::Unverifiable;
        outcome.detail = "the observation budget expired without authoritative evidence";
        auto applied = apply_outcome(context, state, *spec, attempt, outcome,
                                     attempt.kind() == AttemptKind::Compensation);
        if (!applied.has_value()) {
            return applied.error();
        }
    }

    if (!is_running_lifecycle(state.lifecycle)) {
        return true;
    }

    // Stage 4: readiness, gates and dispatch.
    const bool compensating = state.lifecycle == Lifecycle::Compensating;
    const std::vector<StepId> ready = compute_ready_steps(plan, state.steps, compensating);
    std::uint32_t dispatchedThisTick = 0;
    for (const StepId& stepId : ready) {
        if (dispatchedThisTick >= context.impl->options.max_parallel_dispatches) {
            break;
        }
        const StepSpec* spec = plan.find_step(stepId);
        if (spec == nullptr) {
            continue;
        }
        StepRuntime& runtime = state.steps[stepId];
        if (compensating) {
            if (spec->compensation_requests.empty()) {
                runtime.compensated = true;
                auto transition =
                    context.applier->op_step_state(state, stepId, StepState::Compensated, context.now);
                if (!transition.has_value()) {
                    return transition.error();
                }
                continue;
            }
            if (runtime.compensationAttempts >= context.max_attempts_per_step()) {
                if (spec->compensation_required && runtime.state != StepState::Uncompensable) {
                    auto transition =
                        context.applier->op_step_state(state, stepId, StepState::Uncompensable, context.now);
                    if (!transition.has_value()) {
                        return transition.error();
                    }
                }
                continue;
            }
            auto dispatched = dispatch_attempt(context, state, *spec, spec->compensation_requests.front(), true);
            if (!dispatched.has_value()) {
                return dispatched.error();
            }
            ++dispatchedThisTick;
            continue;
        }

        // Readiness gates are evaluated against accepted evidence only. A gate
        // that cannot be evaluated is a gate that is not satisfied.
        bool gatesSatisfied = true;
        std::string gateReason;
        for (const ReadinessGate& gate : spec->gates) {
            const EvidenceView view = context.impl->evidence.view(gate.domain);
            if (!view.established()) {
                gatesSatisfied = false;
                gateReason = "no accepted evidence for a gate domain";
                break;
            }
            if (gate.require_domain_authority) {
                const auto current = context.impl->ledger.current(gate.domain);
                if (!current.has_value() || view.authority() != *current) {
                    gatesSatisfied = false;
                    gateReason = "gate evidence is not bound to current authority";
                    break;
                }
            }
            if (!view.is_current(context.now, gate.max_age)) {
                gatesSatisfied = false;
                gateReason = "gate evidence is older than the gate allows";
                break;
            }
            for (const std::string& key : gate.required_keys) {
                if (!view.find(key).has_value()) {
                    gatesSatisfied = false;
                    gateReason = "a required reading is absent";
                    break;
                }
            }
            if (!gatesSatisfied) {
                break;
            }
            for (const auto& [key, expected] : gate.expected) {
                const auto actual = view.find(key);
                if (!actual.has_value() || !(*actual == expected)) {
                    gatesSatisfied = false;
                    gateReason = "a required reading does not have the expected value";
                    break;
                }
            }
            if (!gatesSatisfied) {
                break;
            }
        }
        if (!gatesSatisfied) {
            if (context.report != nullptr) {
                StepTransition record;
                record.step = stepId;
                record.before = runtime.state;
                record.after = runtime.state;
                record.reason = "readiness gate not satisfied: " + gateReason;
                context.report->transitions.push_back(std::move(record));
            }
            continue;
        }

        if (spec->requests.empty()) {
            // An assessment step with no request is satisfied by the evidence
            // the gates just validated. It produces no effect.
            runtime.verifiedAt = context.now;
            auto transition = context.applier->op_step_state(state, stepId, StepState::Verified, context.now);
            if (!transition.has_value()) {
                return transition.error();
            }
            context.didWork = true;
            continue;
        }
        if (runtime.attempts >= context.max_attempts_per_step()) {
            auto transition = context.applier->op_step_state(state, stepId, StepState::Failed, context.now);
            if (!transition.has_value()) {
                return transition.error();
            }
            continue;
        }
        auto dispatched = dispatch_attempt(context, state, *spec, spec->requests.front(), false);
        if (!dispatched.has_value()) {
            return dispatched.error();
        }
        ++dispatchedThisTick;
    }

    // Stage 5: completion, failure and deadline derivation.
    if (compensating) {
        bool allCompensated = true;
        for (const StepSpec& spec : plan.steps()) {
            const StepRuntime& runtime = state.steps[spec.id];
            if (!runtime.verified()) {
                continue;
            }
            if (runtime.state != StepState::Compensated && runtime.state != StepState::Uncompensable) {
                allCompensated = false;
                break;
            }
        }
        if (allCompensated) {
            state.compensationActive = false;
            auto closed = close_execution(*context.applier, state, Lifecycle::Compensated,
                                          "compensation finished", context.now);
            if (!closed.has_value()) {
                return closed.error();
            }
            context.didWork = true;
        }
        return true;
    }

    bool allVerified = true;
    bool anyFailed = false;
    for (const StepSpec& spec : plan.steps()) {
        const StepRuntime& runtime = state.steps[spec.id];
        if (runtime.state != StepState::Verified) {
            allVerified = false;
        }
        if (runtime.failed()) {
            anyFailed = true;
        }
    }
    if (allVerified) {
        auto closed =
            close_execution(*context.applier, state, Lifecycle::Complete, "every step verified", context.now);
        if (!closed.has_value()) {
            return closed.error();
        }
        if (context.report != nullptr) {
            context.report->completed.push_back(state.id);
        }
        context.didWork = true;
        return true;
    }
    if (anyFailed) {
        // A step that cannot make progress stops the execution. The coordinator
        // does not choose compensation or abandonment on its own: it reports
        // the decision the operator has to make.
        auto blocked = context.applier->op_lifecycle(state, Lifecycle::Blocked,
                                                     "a step failed and no attempt budget remains", context.now);
        if (!blocked.has_value()) {
            return blocked.error();
        }
        if (context.report != nullptr) {
            context.report->failed.push_back(state.id);
        }
        context.didWork = true;
        return true;
    }

    // A step whose deadline passed can never be verified. It fences the
    // execution rather than quietly completing it.
    for (const StepSpec& spec : plan.steps()) {
        StepRuntime& runtime = state.steps[spec.id];
        if (spec.deadline.is_zero() || runtime.state == StepState::Verified) {
            continue;
        }
        if (!spec.deadline.is_after(context.now)) {
            auto fenced = context.applier->op_fence(state, state.fenced, kReasonDeadline, context.now);
            if (!fenced.has_value()) {
                return fenced.error();
            }
            auto lifecycle =
                context.applier->op_lifecycle(state, Lifecycle::Stale, kReasonDeadline, context.now);
            if (!lifecycle.has_value()) {
                return lifecycle.error();
            }
            if (context.report != nullptr) {
                context.report->fenced.push_back(state.id);
            }
            context.didWork = true;
            return true;
        }
    }

    // Blocked detection: nothing ready, nothing in flight, no failure. The
    // execution is stalled on a dependency that cannot become satisfied.
    if (ready.empty() && state.inFlight.empty()) {
        bool progressPossible = false;
        for (const StepSpec& spec : plan.steps()) {
            const StepRuntime& runtime = state.steps[spec.id];
            if (runtime.state == StepState::Pending || runtime.state == StepState::Ready) {
                progressPossible = true;
                break;
            }
        }
        if (progressPossible) {
            auto blocked = context.applier->op_lifecycle(
                state, Lifecycle::Blocked, "no step can become ready with the evidence available", context.now);
            if (!blocked.has_value()) {
                return blocked.error();
            }
            context.didWork = true;
        }
    }
    return true;
}

}  // namespace

Result<TickReport> Engine::tick() {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (impl_->log->poisoned()) {
        return errors::persistence("engine.log_poisoned", "the durable log is no longer usable");
    }
    TickReport report;
    if (!impl_->opened || impl_->shutdown) {
        report.sequence = impl_->sequence;
        return report;
    }

    StateApplier applier{*impl_, false};
    TickContext context;
    context.applier = &applier;
    context.impl = impl_.get();
    context.now = impl_->clock->now();
    context.report = &report;
    context.didWork = false;

    for (auto& [executionId, state] : impl_->executions) {
        (void)executionId;
        if (!is_running_lifecycle(state.lifecycle)) {
            continue;
        }
        auto result = execute_plan(context, state);
        if (!result.has_value()) {
            return result.error();
        }
    }

    report.sequence = impl_->sequence;
    report.idle = !context.didWork;
    return report;
}

Result<std::vector<ExecutionId>> Engine::audit_authority() {
    std::lock_guard<std::mutex> guard(impl_->state);
    if (impl_->adapterActive) {
        return errors::internal("engine.reentrant", "the engine was called from inside an adapter callback");
    }
    if (!impl_->opened) {
        return errors::precondition_failed("engine.closed", "the engine is not open");
    }
    std::vector<ExecutionId> fenced;
    const auto now = impl_->clock->now();
    StateApplier applier{*impl_, false};
    for (auto& [executionId, state] : impl_->executions) {
        if (!is_running_lifecycle(state.lifecycle)) {
            continue;
        }
        const auto planIterator = impl_->plans.find(state.plan);
        if (planIterator == impl_->plans.end()) {
            continue;
        }
        bool current = true;
        std::vector<Domain> changed;
        for (const auto& [domain, requirement] : planIterator->second.authority_requirements()) {
            const auto accepted = impl_->ledger.current(domain);
            if (!accepted.has_value() || *accepted != requirement) {
                current = false;
                changed.push_back(domain);
            }
        }
        if (current) {
            continue;
        }
        auto fencedResult = applier.op_fence(state, changed, kReasonStaleAuthority, now);
        if (!fencedResult.has_value()) {
            return fencedResult.error();
        }
        auto lifecycle = applier.op_lifecycle(state, Lifecycle::Stale, kReasonStaleAuthority, now);
        if (!lifecycle.has_value()) {
            return lifecycle.error();
        }
        fenced.push_back(executionId);
    }
    return fenced;
}

std::string Engine::snapshot_json() const {
    std::lock_guard<std::mutex> guard(impl_->state);
    json::Object root;
    root["epoch"] = json::Value{static_cast<unsigned long long>(impl_->epoch.value())};
    root["incarnation"] = json::Value{impl_->incarnationName};
    root["sequence"] = json::Value{static_cast<unsigned long long>(impl_->sequence)};

    // Authority, plans, executions, attempts and evidence are all emitted in
    // ascending identity order, so two engines that applied the same records
    // produce the same bytes.
    json::Object authority;
    for (const auto& [domain, entry] : impl_->ledger.entries()) {
        json::Object record;
        record["accepted_at"] = codec::encode(entry.acceptedAt);
        record["reference"] = codec::encode(entry.reference);
        record["retired"] = json::Value{entry.retired};
        authority[std::string{to_string(domain)}] = json::Value{std::move(record)};
    }
    root["authority"] = json::Value{std::move(authority)};

    json::Array plans;
    for (const auto& [id, value] : impl_->plans) {
        (void)id;
        json::Object entry;
        entry["content_digest"] = codec::encode(codec::plan_content_digest_of(codec::plan_to_json(value)));
        entry["plan"] = codec::plan_to_json(value);
        plans.push_back(json::Value{std::move(entry)});
    }
    root["plans"] = json::Value{std::move(plans)};

    json::Array executions;
    for (const auto& [id, state] : impl_->executions) {
        json::Object entry;
        entry["content_digest"] = codec::encode(state.content);
        entry["epoch"] = json::Value{static_cast<unsigned long long>(state.epoch.value())};
        entry["execution"] = codec::encode(id);
        entry["lifecycle"] = json::Value{std::string{to_string(state.lifecycle)}};
        entry["plan"] = codec::encode(state.plan);
        entry["revision"] = json::Value{static_cast<unsigned long long>(state.revision)};
        entry["started_at"] = codec::encode(state.startedAt);
        json::Array steps;
        for (const auto& [step, runtime] : state.steps) {
            json::Object stepRecord;
            stepRecord["attempts"] = json::Value{static_cast<unsigned long long>(runtime.attempts)};
            stepRecord["compensated"] = json::Value{runtime.compensated};
            stepRecord["compensation_attempts"] =
                json::Value{static_cast<unsigned long long>(runtime.compensationAttempts)};
            stepRecord["state"] = json::Value{std::string{to_string(runtime.state)}};
            stepRecord["step"] = codec::encode(step);
            steps.push_back(json::Value{std::move(stepRecord)});
        }
        entry["steps"] = json::Value{std::move(steps)};
        executions.push_back(json::Value{std::move(entry)});
    }
    root["executions"] = json::Value{std::move(executions)};

    json::Array attempts;
    for (const auto& [id, attempt] : impl_->attempts) {
        (void)id;
        attempts.push_back(AttemptCodec::to_json(attempt));
    }
    root["attempts"] = json::Value{std::move(attempts)};

    json::Array evidence;
    for (const Domain domain : impl_->evidence.domains()) {
        for (const Observation& observation : impl_->evidence.observations_for(domain)) {
            evidence.push_back(codec::observation_to_json(observation));
        }
    }
    root["evidence"] = json::Value{std::move(evidence)};
    return json::encode(json::Value{std::move(root)});
}

}  // namespace recovery
