#ifndef RECOVERY_ENGINE_HPP
#define RECOVERY_ENGINE_HPP

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "recovery/authority.hpp"
#include "recovery/epoch.hpp"
#include "recovery/error.hpp"
#include "recovery/id.hpp"
#include "recovery/observation.hpp"
#include "recovery/plan.hpp"
#include "recovery/ports.hpp"
#include "recovery/time.hpp"

namespace recovery {

class StateApplier;

// What may happen to a request that was in flight when the previous
// incarnation died. The default never reissues a request automatically: an
// ambiguous request is reconciled against the adjacent authority, and only a
// proven "the authority has no record of this key" permits a re-dispatch.
enum class RestartPolicy : std::uint8_t {
    // Reconcile against the adjacent authority only. A request the authority
    // does not know about stays unresolved; nothing is reissued.
    ReconcileOnly = 1,
    // Reconcile, and re-dispatch only when the authority reports no record of
    // the idempotency key, which proves the request never landed.
    ReconcileThenRedispatch = 2,
    // Never contact the adjacent authority about an unresolved request. Report
    // it and wait for an operator.
    ReportOnly = 3,
};

[[nodiscard]] std::string_view to_string(RestartPolicy value);
[[nodiscard]] std::optional<RestartPolicy> restart_policy_from_string(std::string_view text);

struct AttemptView {
    AttemptId id{};
    StepId step{};
    std::uint32_t attempt_number{0};
    AttemptKind kind{AttemptKind::Forward};
    AttemptStatus status{AttemptStatus::Pending};
    Epoch epoch{};
    Digest binding{};
    std::string idempotency{};
    bool ambiguous{false};
    bool acknowledged{false};
    bool observed{false};
    EffectStatus effect{EffectStatus::NotAssessed};
    std::uint32_t dispatch_count{0};
    std::string detail{};
};

struct ExecutionView {
    ExecutionId id{};
    PlanId plan{};
    std::uint64_t plan_revision{0};
    Digest plan_content{};
    Lifecycle lifecycle{Lifecycle::Idle};
    Epoch epoch{};
    std::map<StepId, StepState> steps{};
    std::vector<AttemptView> attempts{};
    std::vector<Domain> fenced_domains{};
    std::uint32_t compensation_rounds{0};
    TimePoint started_at{};
    TimePoint updated_at{};
    std::string reason{};
    bool point_of_no_return_passed{false};
};

struct RecoveryReport {
    Epoch claimed_epoch{};
    std::string incarnation{};
    std::uint64_t records_replayed{0};
    std::uint64_t plans_restored{0};
    std::uint64_t executions_restored{0};
    std::uint64_t attempts_restored{0};
    std::uint64_t ambiguous_attempts{0};
    std::uint64_t observations_restored{0};
    std::uint64_t authority_entries_restored{0};
    // Steps whose stored state was behind their own durably recorded attempts and
    // were therefore derived forward during replay. A non-zero value is not a
    // failure: it is the coordinator closing the window between the attempt
    // record and the step record after a process died inside that window.
    std::uint64_t steps_repaired{0};
    // True when the evidence store was fenced: nothing is current until fresh
    // evidence is presented, because what the previous process knew is not
    // current authority.
    bool evidence_fenced{true};
    [[nodiscard]] std::string summary() const;
};

struct StepTransition {
    StepId step{};
    StepState before{StepState::Pending};
    StepState after{StepState::Pending};
    std::string reason{};
};

struct DispatchReport {
    ExecutionId execution{};
    StepId step{};
    AttemptId attempt{};
    std::uint32_t attempt_number{0};
    ResponseStatus response{ResponseStatus::Indeterminate};
    bool reconciled{false};
    bool reissued{false};
    bool ambiguous{false};
    std::string detail{};
};

struct TickReport {
    std::uint64_t sequence{0};
    std::vector<StepTransition> transitions{};
    std::vector<DispatchReport> dispatches{};
    std::vector<AttemptId> reconciled{};
    std::vector<ExecutionId> fenced{};
    std::vector<ExecutionId> completed{};
    std::vector<ExecutionId> failed{};
    bool idle{true};
    [[nodiscard]] std::string summary() const;
};

// ---------------------------------------------------------------------------
// Engine
// ---------------------------------------------------------------------------
// The engine is the only place that changes coordinator state. It never infers
// authority, never treats an acknowledgement as an effect, and never reissues
// an ambiguous request on its own.
//
// Concurrency model:
//
//   * One state mutex protects all coordinator state. It is never held while
//     waiting on a condition variable, joining a thread, or sleeping. Every
//     acquisition is a plain lock_guard on that one mutex, so there is exactly
//     one lock order in the process and lock inversion is impossible by
//     construction rather than by convention.
//   * Adapters are called only while the state mutex is held, and adapter code
//     must not call back into the engine. The engine enforces that: a
//     re-entrant call from inside an adapter callback is refused with an
//     internal error instead of deadlocking.
//   * The worker thread runs the same tick() entry point a caller may run, so
//     there is no second code path to audit.
//   * shutdown() signals the worker and joins it without holding the state
//     mutex, so the worker can always finish the tick it is running.
class Engine {
public:
    struct Options {
        // Attempt budget per step, including the first attempt.
        std::uint32_t max_attempts_per_step{3};
        // How long the coordinator waits for authoritative evidence before it
        // declares an attempt unverifiable. Zero means "do not give up".
        Duration observation_budget{};
        // Freshness bound applied when a plan does not name one.
        Duration default_freshness{};
        RestartPolicy restart_policy{RestartPolicy::ReconcileOnly};
        // Number of compensation rounds allowed per execution.
        std::uint32_t max_compensation_rounds{4};
        std::uint32_t max_attempts_per_execution{65536};
        // Maximum number of steps a single plan may declare.
        std::uint32_t max_steps{4096};
        // Steps dispatched per tick. The caller controls overall concurrency by
        // choosing how often it ticks.
        std::uint32_t max_parallel_dispatches{1};
        bool worker_enabled{false};
        // Delay between worker iterations. Only meaningful with worker_enabled.
        Duration worker_interval{};
    };

    Engine(DurableLog& log, AdjacentAuthorityPort& port, Clock& clock, Options options);
    ~Engine();

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    // Rebuilds state from the durable log and starts a new incarnation. The
    // claimed epoch must be strictly greater than every epoch already present
    // in durable state; a successor never inherits authority.
    [[nodiscard]] Result<RecoveryReport> open(Epoch requestedEpoch, std::string incarnation);

    // Stops the worker thread (if any) and refuses further work.
    void shutdown();

    // -----------------------------------------------------------------------
    // Evidence and authority
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<EvidenceId> observe(const AdapterObservation& observation);
    [[nodiscard]] Result<AuthorityRef> accept_authority(Domain domain, const AuthorityRef& reference);
    [[nodiscard]] Result<AuthorityRef> retire_authority(Domain domain, std::string_view reason);

    [[nodiscard]] EvidenceView view(Domain domain) const;
    [[nodiscard]] std::optional<AuthorityRef> current_authority(Domain domain) const;
    [[nodiscard]] std::vector<Observation> observations(Domain domain) const;

    // -----------------------------------------------------------------------
    // Plans
    // -----------------------------------------------------------------------
    [[nodiscard]] Result<PlanId> propose_plan(const PlanDefinition& definition);
    [[nodiscard]] Result<PlanId> publish_plan(const PlanId& plan);
    [[nodiscard]] Result<bool> supersede_plan(const PlanId& plan, std::string_view reason);
    [[nodiscard]] std::optional<Plan> plan(const PlanId& id) const;
    [[nodiscard]] std::vector<PlanId> plan_ids() const;

    // Starts an execution of a published plan against the currently accepted
    // authority. A plan whose bindings are not current is refused: the caller
    // re-plans, the coordinator never silently adapts.
    [[nodiscard]] Result<ExecutionId> start_execution(const PlanId& plan);

    [[nodiscard]] std::optional<ExecutionView> execution(const ExecutionId& id) const;
    [[nodiscard]] std::vector<ExecutionId> execution_ids() const;

    // Stops the execution without changing the facility. Effects that already
    // happened stay in place; nothing is dispatched afterwards.
    [[nodiscard]] Result<bool> cancel_execution(const ExecutionId& id, std::string_view reason);

    // Bounded compensation walk over the steps that produced a verified effect.
    [[nodiscard]] Result<bool> begin_compensation(const ExecutionId& id, std::string_view reason);

    // Acknowledges that a point of no return was passed. The execution then
    // closes as Abandoned, never as Complete.
    [[nodiscard]] Result<bool> abandon_execution(const ExecutionId& id, std::string_view reason);

    // -----------------------------------------------------------------------
    // Work
    // -----------------------------------------------------------------------
    // Runs one unit of work: assesses attempts against current evidence,
    // reconciles unresolved attempts, evaluates readiness, and dispatches ready
    // steps. It never blocks on a condition variable; an adapter may block, and
    // the caller controls that through its own adapter implementation.
    [[nodiscard]] Result<TickReport> tick();

    void start_worker();
    void stop_worker();
    [[nodiscard]] bool worker_running() const noexcept;

    [[nodiscard]] Epoch epoch() const noexcept;
    [[nodiscard]] std::string incarnation() const;
    [[nodiscard]] std::uint64_t sequence() const noexcept;

    // Canonical rendering of the durable coordinator state that decisions
    // depend on. Deterministic: two engines that applied the same records
    // produce the same bytes.
    [[nodiscard]] std::string snapshot_json() const;

    // Re-evaluates every running execution against current authority and
    // durably fences any execution whose bindings are no longer current.
    [[nodiscard]] Result<std::vector<ExecutionId>> audit_authority();

    struct Impl;

private:
    friend class AttemptCodec;
    friend class StateApplier;

    std::unique_ptr<Impl> impl_;
};

}  // namespace recovery

#endif  // RECOVERY_ENGINE_HPP