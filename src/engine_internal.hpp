#ifndef RECOVERY_ENGINE_INTERNAL_HPP
#define RECOVERY_ENGINE_INTERNAL_HPP

// Engine internals. Not installed, not part of the public surface. The state
// types live here so that the decision logic, the durable codec and the tests
// all reason about the same definitions.

#include <condition_variable>
#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "recovery/authority.hpp"
#include "recovery/engine.hpp"
#include "recovery/id.hpp"
#include "recovery/observation.hpp"
#include "recovery/plan.hpp"

namespace recovery {

struct StepRuntime {
    StepState state{StepState::Pending};
    std::uint32_t attempts{0};
    std::uint32_t compensationAttempts{0};
    bool compensated{false};
    // Values observed at the instant the effect was verified. They are the
    // evidence the verification is bound to; nothing else may be substituted.
    AuthorityRef authority{};
    Digest evidence{};
    TimePoint verifiedAt{};

    [[nodiscard]] bool verified() const noexcept { return state == StepState::Verified; }
    [[nodiscard]] bool resolved() const noexcept {
        return state == StepState::Verified || state == StepState::Failed || state == StepState::Refused ||
               state == StepState::Skipped;
    }
    [[nodiscard]] bool failed() const noexcept {
        return state == StepState::Failed || state == StepState::Refused;
    }
};

struct ExecState {
    ExecutionId id{};
    PlanId plan{};
    std::uint64_t revision{0};
    Digest content{};
    Lifecycle lifecycle{Lifecycle::Idle};
    Epoch epoch{};
    TimePoint startedAt{};
    TimePoint updatedAt{};
    std::string reason{};
    std::map<StepId, StepRuntime> steps{};
    std::map<StepId, AttemptId> lastAttempt{};
    std::set<StepId> inFlight{};
    std::vector<Domain> fenced{};
    std::uint32_t compensationRounds{0};
    bool compensationActive{false};
    bool pointOfNoReturn{false};
};

struct Engine::Impl {
    DurableLog* log{nullptr};
    AdjacentAuthorityPort* port{nullptr};
    Clock* clock{nullptr};
    Options options{};
    mutable std::mutex state{};
    Epoch epoch{};
    std::string incarnationName{};
    bool opened{false};
    bool shutdown{false};
    bool insideAdapter{false};
    bool workerStarted{false};
    bool workerStop{false};
    std::condition_variable workerSignal{};
    std::thread worker{};
    std::uint64_t sequence{0};
    IdAllocator ids{};
    ObservationStore evidence{};
    AuthorityLedger ledger{};
    std::map<PlanId, Plan> plans{};
    std::set<PlanId> supersededPlans{};
    std::map<ExecutionId, ExecState> executions{};
    std::map<AttemptId, Attempt> attempts{};
    // Attempt numbers are per step, so the index is keyed by (step, attempt
    // number). Keying by attempt number alone made every step's first attempt
    // overwrite the previous step's, which hid attempts from the execution view
    // and from reconciliation.
    std::map<ExecutionId, std::map<std::pair<StepId, std::uint32_t>, AttemptId>> attemptIndex{};
    std::vector<AttemptId> dispatchOrder{};
    std::uint64_t requestCounter{0};
    // Re-entrancy guard for the public API. Set while adapter code runs.
    bool adapterActive{false};
};

// The engine's only sanctioned way to move an attempt forward. Declaring the
// mutation surface as one named type keeps the friend declaration in Attempt
// narrow: exactly one type can write an attempt field, and it is the same type
// that routes every change through the durable journal first. It lives in the
// recovery namespace (not an anonymous one) so that the friend declaration in
// Attempt names exactly this type.
struct AttemptMutation {
    static void assign_status(Attempt& attempt, AttemptStatus status, bool ambiguous, std::string detail) {
        attempt.status_ = status;
        attempt.ambiguous_ = ambiguous;
        attempt.detail_ = std::move(detail);
    }
    static void set_ambiguity(Attempt& attempt, bool ambiguous) { attempt.ambiguous_ = ambiguous; }
    static void count_dispatch(Attempt& attempt) { attempt.dispatchCount_ += 1; }
    static void set_dispatch_intent(Attempt& attempt, TimePoint at) { attempt.dispatchIntentAt_ = at; }

    // Records that the coordinator reached the authority and the authority
    // answered. An acknowledgment is recorded as an acknowledgment; it is never
    // promoted to an effect here.
    static void record_acknowledgement(Attempt& attempt, std::string detail, TimePoint at) {
        if (attempt.acknowledgement_.present) {
            return;
        }
        attempt.acknowledgement_.present = true;
        attempt.acknowledgement_.origin = AttestationOrigin::DispatchResponse;
        attempt.acknowledgement_.actor = std::string{};
        attempt.acknowledgement_.detail = std::move(detail);
        attempt.acknowledgement_.at = at;
    }

    // Records the assessment the coordinator formed for this attempt. The
    // assessment carries the authority reference it was checked against and the
    // digests of the expected and observed evidence, so a later reader can see
    // exactly what was compared.
    static void record_assessment(Attempt& attempt, EffectStatus effect, Digest observed, Digest expectation,
                                  AuthorityRef authority, EvidenceId evidence, std::string detail, TimePoint at) {
        attempt.observation_.present = true;
        attempt.observation_.origin = AttestationOrigin::DispatchResponse;
        attempt.observation_.evidence = evidence;
        attempt.observation_.at = at;
        attempt.assessment_.status = effect == EffectStatus::NotAssessed ? EffectStatus::Unverifiable : effect;
        attempt.assessment_.observed = observed;
        attempt.assessment_.expectation = expectation;
        attempt.assessment_.authority = std::move(authority);
        attempt.assessment_.evidence = evidence;
        attempt.assessment_.at = at;
        attempt.assessment_.detail = std::move(detail);
    }

    [[nodiscard]] static bool has_acknowledgement(const Attempt& attempt) {
        return attempt.acknowledgement_.present;
    }
    [[nodiscard]] static bool has_observation(const Attempt& attempt) { return attempt.observation_.present; }
    [[nodiscard]] static const EffectAssessment& assessment_of(const Attempt& attempt) {
        return attempt.assessment_;
    }
};

// Pure readiness computation, shared with the independent reference model used
// by the property tests. It is a function of the plan and step states only:
// never of container order, timing, or adapter behaviour.
[[nodiscard]] std::vector<StepId> compute_ready_steps(const Plan& plan,
                                                      const std::map<StepId, StepRuntime>& states,
                                                      bool compensating);
[[nodiscard]] std::vector<StepId> compute_ready_states(const Plan& plan, const std::map<StepId, StepState>& states,
                                                       bool compensating);

}  // namespace recovery

#endif  // RECOVERY_ENGINE_INTERNAL_HPP
