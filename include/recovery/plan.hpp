#ifndef RECOVERY_PLAN_HPP
#define RECOVERY_PLAN_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "recovery/epoch.hpp"
#include "recovery/error.hpp"
#include "recovery/id.hpp"
#include "recovery/observation.hpp"
#include "recovery/time.hpp"

namespace recovery {

struct AttemptMutation;

// ---------------------------------------------------------------------------
// Recovery plan: a proposal, never authority
// ---------------------------------------------------------------------------
// A plan is a proposal. Publishing a plan, marking a step ready, or dispatching
// a request never changes a facility. Only an adjacent authority acting on a
// bounded request changes the facility, and even then the coordinator treats
// the change as unproven until an effect is verified against the exact
// expectation bound to that attempt.
//
// Every field of a plan is either derived from authoritative evidence the
// coordinator accepted, or is an explicit proposal by the caller. Nothing is
// inferred from existence, naming, apparent health, or topology.

enum class PlanStrategy : std::uint8_t {
    // Read-only assessment steps. Useful before any mutation is proposed.
    Assessment = 1,
    // Single-step recovery.
    Immediate = 2,
    // Dependency ordered recovery across domains.
    Staged = 3,
    // Dependency ordered recovery that retries refusable requests under a
    // bounded attempt budget before failing the step.
    StagedWithRetry = 4,
};

[[nodiscard]] std::string_view to_string(PlanStrategy value);
[[nodiscard]] std::optional<PlanStrategy> plan_strategy_from_string(std::string_view text);

enum class PlanStatus : std::uint8_t {
    Draft = 1,
    Published = 2,
    Superseded = 3,
};

[[nodiscard]] std::string_view to_string(PlanStatus value);

// Lifecycle of one execution of a plan. "Execution" exists because the same
// plan content may be run under a new authority generation; the execution is
// what carries the fencing decision.
enum class Lifecycle : std::uint8_t {
    // Accepted, nothing the coordinator must do right now.
    Idle = 1,
    // The coordinator has runnable work (ready steps or outstanding attempts).
    Running = 2,
    // Dependency stalled: no step is ready because prerequisites did not
    // succeed. The coordinator refuses to guess and waits for a replan.
    Blocked = 3,
    // An unrecoverable precondition no longer holds: bound authority changed.
    Stale = 4,
    // Every step completed with a verified effect.
    Complete = 5,
    // A step failed with no remaining attempt budget.
    Failed = 6,
    // Operator cancelled the execution.
    Cancelled = 7,
    // A step failed after a point of no return was passed: the facility is in a
    // state the coordinator cannot reverse and must not pretend to.
    Abandoned = 8,
    // Compensation is in progress after a failure.
    Compensating = 9,
    // Compensation finished and the plan's reversible effects were undone.
    Compensated = 10,
};

[[nodiscard]] std::string_view to_string(Lifecycle value);
[[nodiscard]] bool is_terminal(Lifecycle value);

enum class StepState : std::uint8_t {
    Pending = 1,
    Ready = 2,
    Dispatched = 3,
    Acknowledged = 4,
    Executing = 5,
    Verified = 6,
    Failed = 7,
    Refused = 8,
    Skipped = 9,
    Compensating = 10,
    Compensated = 11,
    Uncompensable = 12,
};

[[nodiscard]] std::string_view to_string(StepState value);

// Outcome of one prerequisite edge.
enum class EdgeOutcome : std::uint8_t {
    // Satisfied when the predecessor reached Verified (or Compensated, when the
    // edge is evaluated during compensation).
    AwaitVerified = 1,
    // Satisfied when the predecessor reached Verified, Failed, or Refused
    // (used for diagnostic and observation steps that must still run).
    AwaitResolved = 2,
    // Satisfied when the predecessor failed or was refused.
    OnFailure = 3,
};

[[nodiscard]] std::string_view to_string(EdgeOutcome value);
[[nodiscard]] std::optional<EdgeOutcome> edge_outcome_from_string(std::string_view text);

struct Dependency {
    StepId predecessor{};
    EdgeOutcome outcome{EdgeOutcome::AwaitVerified};

    friend bool operator==(const Dependency& lhs, const Dependency& rhs) noexcept {
        return lhs.predecessor == rhs.predecessor && lhs.outcome == rhs.outcome;
    }
    friend bool operator<(const Dependency& lhs, const Dependency& rhs) noexcept {
        if (lhs.predecessor != rhs.predecessor) {
            return lhs.predecessor < rhs.predecessor;
        }
        return lhs.outcome < rhs.outcome;
    }
};

// What a step requires of the authoritative world before it may be dispatched.
struct ReadinessGate {
    // Named readings that must be present in the current evidence view of the
    // given domain. A missing reading is a failed gate, never an assumed one.
    Domain domain{Domain::Safety};
    std::vector<std::string> required_keys{};
    // Optional expected value for each required key. Keys absent from this map
    // only need to be present, not equal to a specific value.
    std::map<std::string, Reading> expected{};
    // The view must be no older than this at dispatch time.
    Duration max_age{};
    // The view's authority reference must also satisfy the plan level
    // requirement for this domain. Defaults to true.
    bool require_domain_authority{true};

    friend bool operator==(const ReadinessGate& lhs, const ReadinessGate& rhs) noexcept;
};

// A bounded request the coordinator may ask an adjacent authority to perform.
// A request is a request: it is not authority, and dispatching it is not an
// effect.
struct RequestBinding {
    StepId step{};
    std::uint32_t binding{0};
    Domain domain{Domain::Power};
    // Operation name as understood by the adjacent authority adapter.
    std::string operation{};
    // Canonical parameter bytes. The coordinator never interprets them; it
    // binds them by digest and compares them byte for byte.
    std::string parameters{};
    // After an effect for this request is verified, the coordinator must not
    // attempt to compensate the step: the world cannot be put back.
    bool point_of_no_return{false};

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] Digest digest() const;

    friend bool operator==(const RequestBinding& lhs, const RequestBinding& rhs) noexcept;
    friend bool operator<(const RequestBinding& lhs, const RequestBinding& rhs) noexcept;
};

// An attempt is the durable unit of external effect. It is recorded before any
// consequential dispatch so that process death cannot cause a blind duplicate.
enum class AttemptStatus : std::uint8_t {
    // Recorded durably, not yet handed to an adapter.
    Pending = 1,
    // Handed to an adapter; the outcome is not known here yet.
    Dispatched = 2,
    // The adapter reported a definite effect that satisfied the expectation.
    EffectPresent = 3,
    // The adapter reported a definite outcome that did not satisfy the
    // expectation (for example, the operation completed but the world does not
    // look like the expectation required).
    Failed = 4,
    // The adjacent authority declined to perform the request.
    Refused = 5,
    // The coordinator could not obtain a definite outcome within the configured
    // observation budget. This is an uncertain state, not a failure.
    Unverifiable = 6,
    // The attempt was abandoned because a newer attempt for the same step
    // exists, or because the step was compensated.
    Superseded = 7,
};

[[nodiscard]] std::string_view to_string(AttemptStatus value);
[[nodiscard]] std::optional<AttemptStatus> attempt_status_from_string(std::string_view text);
[[nodiscard]] bool is_resolved(AttemptStatus value);

enum class AttemptKind : std::uint8_t {
    Forward = 1,
    Compensation = 2,
};

[[nodiscard]] std::string_view to_string(AttemptKind value);
[[nodiscard]] std::optional<AttemptKind> attempt_kind_from_string(std::string_view text);

// How an acknowledgment or an observation reached the coordinator. The origin
// is never inferred from the value.
enum class AttestationOrigin : std::uint8_t {
    // The adapter returned it as part of the dispatch call.
    DispatchResponse = 1,
    // The adapter returned it during an explicit inspect call.
    Inspection = 2,
    // An operator or control plane presented it.
    Presented = 3,
    // Rebuilt from the durable journal during replay.
    Replay = 4,
};

[[nodiscard]] std::string_view to_string(AttestationOrigin value);
[[nodiscard]] std::optional<AttestationOrigin> attestation_origin_from_string(std::string_view text);

// An acknowledgment is not a verified effect. It is recorded so that the
// coordinator can reason about who claimed what, and when.
struct Acknowledgement {
    bool present{false};
    AttestationOrigin origin{AttestationOrigin::DispatchResponse};
    std::string actor{};
    std::string detail{};
    TimePoint at{};

    friend bool operator==(const Acknowledgement& lhs, const Acknowledgement& rhs) noexcept;
};

struct ObservationAttestation {
    bool present{false};
    AttestationOrigin origin{AttestationOrigin::Inspection};
    EvidenceId evidence{};
    TimePoint at{};

    friend bool operator==(const ObservationAttestation& lhs, const ObservationAttestation& rhs) noexcept;
};

// The coordinator's assessment of an attempt. "Verified" always means: the
// expectation bound to this attempt was satisfied by an observation that
// carried the same request identity, the same plan revision, and an authority
// reference that is still current for the domains the expectation touches.
enum class EffectStatus : std::uint8_t {
    NotAssessed = 1,
    Verified = 2,
    Contradicted = 3,
    Unverifiable = 4,
};

[[nodiscard]] std::string_view to_string(EffectStatus value);
[[nodiscard]] std::optional<EffectStatus> effect_status_from_string(std::string_view text);

struct EffectAssessment {
    EffectStatus status{EffectStatus::NotAssessed};
    Digest expectation{};
    Digest observed{};
    AuthorityRef authority{};
    EvidenceId evidence{};
    TimePoint at{};
    std::string detail{};

    friend bool operator==(const EffectAssessment& lhs, const EffectAssessment& rhs) noexcept;
};

// An idempotency key: the adjacent authority uses it to recognise a repeated
// request as the same operation. It is derived deterministically from the plan,
// the step, the attempt number, and the exact request binding, so that a
// replayed dispatch after a restart carries the identical key.
[[nodiscard]] std::string idempotency_key(const ExecutionId& execution, const StepId& step,
                                          std::uint32_t attempt_number, const RequestBinding& binding,
                                          AttemptKind kind);

class Attempt {
public:
    Attempt() = default;

    Attempt(AttemptId id, ExecutionId execution, StepId step, std::uint32_t attempt_number, AttemptKind kind,
            Epoch epoch,
            std::uint64_t plan_revision, RequestBinding binding, Digest binding_digest, std::string idempotency,
            TimePoint committed_at);

    [[nodiscard]] const AttemptId& id() const noexcept { return id_; }
    // The execution this attempt belongs to. An attempt is bound to one run of
    // one plan revision, never to a plan in the abstract.
    [[nodiscard]] const ExecutionId& execution() const noexcept { return execution_; }
    [[nodiscard]] const StepId& step() const noexcept { return step_; }
    [[nodiscard]] std::uint32_t attempt_number() const noexcept { return attemptNumber_; }
    [[nodiscard]] AttemptKind kind() const noexcept { return kind_; }
    [[nodiscard]] Epoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::uint64_t plan_revision() const noexcept { return planRevision_; }
    [[nodiscard]] const RequestBinding& binding() const noexcept { return binding_; }
    [[nodiscard]] const Digest& binding_digest() const noexcept { return bindingDigest_; }
    [[nodiscard]] const std::string& idempotency() const noexcept { return idempotency_; }
    [[nodiscard]] AttemptStatus status() const noexcept { return status_; }
    [[nodiscard]] const Acknowledgement& acknowledgement() const noexcept { return acknowledgement_; }
    [[nodiscard]] const ObservationAttestation& observation() const noexcept { return observation_; }
    [[nodiscard]] const EffectAssessment& assessment() const noexcept { return assessment_; }
    [[nodiscard]] TimePoint committed_at() const noexcept { return committedAt_; }
    [[nodiscard]] std::optional<TimePoint> dispatch_intent_at() const noexcept { return dispatchIntentAt_; }
    [[nodiscard]] std::uint32_t dispatch_count() const noexcept { return dispatchCount_; }
    // True when the coordinator cannot prove whether the request reached the
    // adjacent authority. Such an attempt is never automatically reissued.
    [[nodiscard]] bool ambiguous() const noexcept { return ambiguous_; }
    [[nodiscard]] std::string detail() const { return detail_; }
    [[nodiscard]] EffectStatus effect_status() const noexcept { return assessment_.status; }
    [[nodiscard]] bool has_observation() const noexcept { return observation_.present; }
    [[nodiscard]] bool has_acknowledgement() const noexcept { return acknowledgement_.present; }

    // Mutators are private to the engine and to the durable codec so that a
    // caller can never advance an attempt by hand: the engine routes every
    // change through the durable journal first, and the codec reconstructs
    // exactly the state that was recorded.
    friend class Engine;
    friend class Journal;
    friend class AttemptCodec;
    // The dispatch and reconciliation helpers are the only other code allowed
    // to move an attempt forward. Every mutation they perform is routed through
    // the durable journal before it becomes visible.
    friend struct AttemptMutation;

private:
    AttemptId id_{};
    ExecutionId execution_{};
    StepId step_{};
    std::uint32_t attemptNumber_{0};
    AttemptKind kind_{AttemptKind::Forward};
    Epoch epoch_{};
    std::uint64_t planRevision_{0};
    RequestBinding binding_{};
    Digest bindingDigest_{};
    std::string idempotency_;
    AttemptStatus status_{AttemptStatus::Pending};
    Acknowledgement acknowledgement_{};
    ObservationAttestation observation_{};
    EffectAssessment assessment_{};
    TimePoint committedAt_{};
    std::optional<TimePoint> dispatchIntentAt_{};
    std::uint32_t dispatchCount_{0};
    bool ambiguous_{false};
    std::string detail_;
};

struct StepSpec {
    StepId id{};
    std::string name{};
    std::vector<Dependency> depends_on{};
    std::vector<RequestBinding> requests{};
    std::vector<ReadinessGate> gates{};
    // Domains this step's expectation depends on. An expectation is only
    // verifiable while every listed domain still carries the authority
    // generation the plan was bound to.
    std::vector<Domain> evidence_domains{};
    TimePoint deadline{};
    bool compensable{false};
    std::vector<RequestBinding> compensation_requests{};
    // When true, a compensation whose own request fails is a defect the
    // coordinator must surface, not a condition it may ignore.
    bool compensation_required{false};

    friend bool operator<(const StepSpec& lhs, const StepSpec& rhs) noexcept { return lhs.id < rhs.id; }
};

class Plan {
public:
    Plan() = default;

    Plan(PlanId id, std::string name, std::string scope, PlanStrategy strategy, std::uint64_t revision,
         TimePoint created_at);

    [[nodiscard]] const PlanId& id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] const std::string& scope() const noexcept { return scope_; }
    [[nodiscard]] PlanStrategy strategy() const noexcept { return strategy_; }
    [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
    [[nodiscard]] TimePoint created_at() const noexcept { return createdAt_; }
    [[nodiscard]] PlanStatus status() const noexcept { return status_; }
    [[nodiscard]] const std::vector<StepSpec>& steps() const noexcept { return steps_; }
    [[nodiscard]] const std::map<Domain, AuthorityRef>& authority_requirements() const noexcept {
        return authorityRequirements_;
    }
    [[nodiscard]] const std::map<Domain, Duration>& freshness_requirements() const noexcept {
        return freshnessRequirements_;
    }

    void set_status(PlanStatus status) { status_ = status; }
    void add_step(StepSpec spec);
    void require_authority(Domain domain, AuthorityRef reference);
    void require_freshness(Domain domain, Duration freshness);

    [[nodiscard]] const StepSpec* find_step(const StepId& id) const;
    [[nodiscard]] Digest content_digest() const;
    [[nodiscard]] std::string canonical() const;

private:
    friend class Engine;

    PlanId id_{};
    std::string name_;
    std::string scope_;
    PlanStrategy strategy_{PlanStrategy::Immediate};
    std::uint64_t revision_{1};
    TimePoint createdAt_{};
    PlanStatus status_{PlanStatus::Draft};
    std::vector<StepSpec> steps_;
    std::map<Domain, AuthorityRef> authorityRequirements_;
    std::map<Domain, Duration> freshnessRequirements_;
};

// Definition used to validate and construct a plan.
struct PlanDefinition {
    std::string name{};
    std::string scope{};
    PlanStrategy strategy{PlanStrategy::Staged};
    std::vector<StepSpec> steps{};
    std::map<Domain, AuthorityRef> authority_requirements{};
    std::map<Domain, Duration> freshness_requirements{};
    // Retry budgets, observation budgets and compensation round limits are
    // deliberately NOT here. They are coordinator policy, not plan content: they
    // live in Engine::Options, they are not persisted with the plan, and a
    // restart must not silently change them. A knob on the plan that the engine
    // ignored would be a promise the coordinator does not keep.
};

struct PlanValidationOptions {
    std::uint32_t max_steps{4096};
    std::uint32_t max_requests_per_step{32};
    std::uint32_t max_compensation_requests_per_step{32};
    std::uint32_t max_dependencies_per_step{1024};
    std::uint32_t max_parameters_bytes{65536};
    std::uint32_t max_evidence_domains_per_step{16};
};

// Structural validation. Every rejection names the step and the rule.
[[nodiscard]] Result<PlanId> validate_plan_shape(Plan& plan,
                                                 const PlanValidationOptions& options = PlanValidationOptions{});
[[nodiscard]] Result<Digest> validate_plan_authority(const Plan& plan, const ObservationStore& evidence,
                                                     TimePoint now);

// Deterministic topological order of the steps. Steps are emitted in ascending
// StepId order among all steps whose prerequisites were already emitted, which
// makes the sequence a pure function of the plan content. Returns an
// IdentityConflict error when the dependency graph contains a cycle.
[[nodiscard]] Result<std::vector<StepId>> topological_order(const Plan& plan);

// Dependency closure of one step, used by compensation planning.
[[nodiscard]] std::vector<StepId> dependents_of(const Plan& plan, const StepId& step);

// Position of a step in the plan's declared step order, or nullopt.
[[nodiscard]] std::optional<std::size_t> step_position(const Plan& plan, const StepId& step);

}  // namespace recovery

#endif  // RECOVERY_PLAN_HPP
