#include "test.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "recovery/engine.hpp"
#include "recovery/plan.hpp"
#include "recovery/ports.hpp"

using namespace recovery;

// ---------------------------------------------------------------------------
// Attempt lifecycle
// ---------------------------------------------------------------------------
// An attempt is the durable unit of external effect. These tests drive the
// engine through a scripted adjacent authority so that every assertion is about
// the coordinator's own rules:
//
//   * a definite "no effect" answer spends one attempt and is retried until the
//     budget is spent, and the step then fails in the way it was refused;
//   * a request that may have landed is never reissued on the coordinator's own
//     initiative;
//   * an indeterminate answer is never a success;
//   * only a proven "the authority has no record of this key" permits a
//     re-dispatch, and the re-dispatch carries a new idempotency key.

namespace {

struct DispatchRecord {
    std::string idempotency;
    std::string operation;
    AttemptKind kind{AttemptKind::Forward};
    bool compensating{false};
    bool point_of_no_return{false};
};

// PROVENANCE: SYNTHETIC. The scripted authority replays a fixed answer; it does
// not model any real facility or controller.
class ScriptedPort final : public AdjacentAuthorityPort {
public:
    explicit ScriptedPort(AuthorityRef reference) : reference_(std::move(reference)) {}

    [[nodiscard]] std::string authority_name() const override { return "scripted-adjacent-authority"; }

    [[nodiscard]] Result<AdapterResponse> dispatch(const Request& request) override {
        ++dispatchCalls;
        dispatches.push_back(DispatchRecord{request.idempotency_key(), request.operation(), request.kind(),
                                            request.compensating(), request.point_of_no_return()});
        if (!onDispatch) {
            return errors::internal("scripted.dispatch", "the test did not script a dispatch response");
        }
        return onDispatch(request);
    }

    [[nodiscard]] Result<AdapterResponse> inspect(const Request& request) override {
        ++inspectCalls;
        if (!onInspect) {
            return errors::unsupported("scripted.inspect", "the test did not script an inspect response");
        }
        return onInspect(request);
    }

    [[nodiscard]] Result<AdapterResponse> observe(const Request&, Domain) override {
        return errors::unsupported("scripted.observe", "the engine never asks a port to observe directly");
    }

    [[nodiscard]] Result<AuthorityRef> current_authority(Domain) const override { return reference_; }

    [[nodiscard]] const AuthorityRef& reference() const noexcept { return reference_; }

    std::function<Result<AdapterResponse>(const Request&)> onDispatch;
    std::function<Result<AdapterResponse>(const Request&)> onInspect;
    std::uint64_t dispatchCalls{0};
    std::uint64_t inspectCalls{0};
    std::vector<DispatchRecord> dispatches;

private:
    AuthorityRef reference_;
};

struct Harness {
    MemoryLog log{};
    ScriptedPort port;
    FixedClock clock{TimePoint{0}};
    Engine engine;

    Harness(Engine::Options options, AuthorityRef reference, TimePoint start)
        : port{std::move(reference)}, clock{start}, engine{log, port, clock, std::move(options)} {}
};

[[nodiscard]] AuthorityRef scripted_authority() {
    return AuthorityRef{"power", "instance-1", 3, Digest::of("scripted-authority")};
}

[[nodiscard]] StepId mint_step(std::uint64_t counter) {
    IdAllocator allocator{counter};
    auto id = allocator.allocate(IdKind::Step);
    RC_REQUIRE_MSG(id.has_value(), "the deterministic identity allocator refused to mint a step identity");
    return StepId{*id};
}

[[nodiscard]] StepSpec single_request_step(const StepId& id, std::string operation) {
    StepSpec spec;
    spec.id = id;
    spec.name = "scripted-step";
    RequestBinding binding;
    binding.step = id;
    binding.binding = 0;
    binding.domain = Domain::Power;
    binding.operation = std::move(operation);
    binding.parameters = "{}";
    spec.requests.push_back(std::move(binding));
    return spec;
}

[[nodiscard]] PlanId publish_single_step_plan(Engine& engine, const StepSpec& step, const char* name) {
    PlanDefinition definition;
    definition.name = name;
    definition.scope = "test-scope";
    definition.strategy = PlanStrategy::Staged;
    definition.steps.push_back(step);
    const PlanId plan = RC_REQUIRE_OK(engine.propose_plan(definition));
    RC_REQUIRE_OK(engine.publish_plan(plan));
    return plan;
}

// A completed answer that carries both an observation and an assessment bound to
// the authority the coordinator accepted. This is what a real adjacent
// authority must present before the coordinator may call an effect verified.
[[nodiscard]] AdapterResponse verified_effect(const Request& request, Domain domain, const AuthorityRef& authority) {
    AdapterResponse response = AdapterResponse::completed("the scripted authority reports the effect");
    response.add_observation(AdapterObservation{domain, authority.generation(), "scripted/observation", "effect",
                                                Reading::flag(true), TimePoint{}, authority, false});
    EffectAssessment assessment;
    assessment.status = EffectStatus::Verified;
    assessment.expectation = request.binding_digest();
    assessment.observed = request.binding_digest();
    assessment.authority = authority;
    assessment.detail = "the scripted authority confirms the expected effect";
    response.add_assessment(assessment);
    return response;
}

[[nodiscard]] const AttemptView& attempt_at(const ExecutionView& view, std::size_t index) {
    RC_REQUIRE_MSG(index < view.attempts.size(),
                   "the execution does not hold the attempt the test is looking for");
    return view.attempts[index];
}

[[nodiscard]] StepState state_of(const ExecutionView& view, const StepId& step) {
    const auto it = view.steps.find(step);
    RC_REQUIRE_MSG(it != view.steps.end(), "the execution view does not contain the step under test");
    return it->second;
}

}  // namespace

RC_TEST(refused_attempts_are_retried_until_the_attempt_budget_is_spent) {
    Engine::Options options;
    options.max_attempts_per_step = 3;
    Harness harness{options, scripted_authority(), TimePoint{0}};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "attempt-lifecycle"));
    harness.port.onDispatch = [](const Request&) -> Result<AdapterResponse> {
        return AdapterResponse::refused("the scripted authority declines the request");
    };
    harness.port.onInspect = [](const Request&) -> Result<AdapterResponse> {
        return AdapterResponse::refused("the scripted authority declines the request");
    };

    const StepId step = mint_step(11);
    const PlanId plan = publish_single_step_plan(harness.engine, single_request_step(step, "power.on"), "refused");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    for (std::uint32_t attempt = 1; attempt <= 3; ++attempt) {
        RC_REQUIRE_OK(harness.engine.tick());
        const auto view = harness.engine.execution(execution);
        RC_REQUIRE_MSG(view.has_value(), "the execution must exist while it is running");
        RC_REQUIRE_MSG(view->attempts.size() == static_cast<std::size_t>(attempt),
                       "each tick with budget remaining must commit exactly one further attempt");
        RC_REQUIRE_MSG(attempt_at(*view, attempt - 1).attempt_number == attempt,
                       "attempt numbers must advance one at a time");
        RC_REQUIRE_MSG(attempt_at(*view, attempt - 1).status == AttemptStatus::Refused,
                       "a refused request must be recorded as a refused attempt");
        RC_REQUIRE_MSG(attempt_at(*view, attempt - 1).effect == EffectStatus::NotAssessed,
                       "a refusal is not an assessed effect");
        if (attempt < 3) {
            RC_REQUIRE_MSG(view->steps.at(step) == StepState::Ready,
                           "with attempt budget remaining a refused step must return to a dispatchable state");
        }
    }

    RC_REQUIRE_EQ(harness.port.dispatchCalls, std::uint64_t{3});
    const auto spent = harness.engine.execution(execution);
    RC_REQUIRE_MSG(spent.has_value(), "the execution must exist after the budget was spent");
    RC_REQUIRE_MSG(spent->steps.at(step) == StepState::Refused,
                   "once the attempt budget is spent a refused step must be Refused");
    RC_REQUIRE_MSG(spent->lifecycle == Lifecycle::Blocked,
                   "a step that cannot make progress must stop the execution as Blocked");

    RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_EQ(harness.port.dispatchCalls, std::uint64_t{3});
    const auto after = harness.engine.execution(execution);
    RC_REQUIRE_MSG(after.has_value() && after->attempts.size() == std::size_t{3},
                   "no attempt may be committed after the budget is spent");
    RC_REQUIRE_MSG(after->lifecycle == Lifecycle::Blocked,
                   "a blocked execution must stay blocked until an operator decides");
}

RC_TEST(an_unreachable_adapter_leaves_the_attempt_dispatched_and_ambiguous) {
    Engine::Options options;
    options.max_attempts_per_step = 3;
    Harness harness{options, scripted_authority(), TimePoint{0}};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "attempt-lifecycle"));
    harness.port.onDispatch = [](const Request&) -> Result<AdapterResponse> {
        return errors::operation_failed("scripted.transport", "the scripted transport failed");
    };
    harness.port.onInspect = [](const Request&) -> Result<AdapterResponse> {
        return errors::operation_failed("scripted.transport", "the scripted transport failed");
    };

    const StepId step = mint_step(21);
    const PlanId plan =
        publish_single_step_plan(harness.engine, single_request_step(step, "power.on"), "unreachable");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    RC_REQUIRE_OK(harness.engine.tick());
    const auto first = harness.engine.execution(execution);
    RC_REQUIRE_MSG(first.has_value(), "the execution must exist after a failed dispatch");
    RC_REQUIRE_MSG(first->attempts.size() == std::size_t{1}, "exactly one attempt was committed");
    RC_REQUIRE_MSG(attempt_at(*first, 0).status == AttemptStatus::Dispatched,
                   "an unreachable authority produces no definite outcome, so the attempt stays Dispatched");
    RC_REQUIRE_MSG(attempt_at(*first, 0).ambiguous,
                   "an attempt that may have reached the authority must be marked ambiguous");
    RC_REQUIRE_MSG(first->steps.at(step) == StepState::Dispatched,
                   "the step must stay Dispatched while its attempt is unresolved");

    for (int tick = 0; tick < 3; ++tick) {
        RC_REQUIRE_OK(harness.engine.tick());
    }
    RC_REQUIRE_EQ(harness.port.dispatchCalls, std::uint64_t{1});
    RC_REQUIRE_MSG(harness.port.inspectCalls >= std::uint64_t{1},
                   "an unresolved attempt must be reconciled against the authority, never blindly reissued");

    const auto settled = harness.engine.execution(execution);
    RC_REQUIRE_MSG(settled.has_value(), "the execution must exist after reconciliation attempts");
    RC_REQUIRE_MSG(settled->attempts.size() == std::size_t{1},
                   "no second attempt may be committed for an ambiguous request");
    RC_REQUIRE_MSG(attempt_at(*settled, 0).status == AttemptStatus::Dispatched,
                   "an unreachable authority leaves the attempt unresolved");
    RC_REQUIRE_MSG(settled->lifecycle == Lifecycle::Running,
                   "an unresolved in-flight attempt keeps the execution running");
    RC_REQUIRE_MSG(!is_terminal(settled->lifecycle),
                   "an unresolved attempt must never close the execution");
}

RC_TEST(an_indeterminate_response_is_never_treated_as_success) {
    Engine::Options options;
    options.max_attempts_per_step = 3;
    options.restart_policy = RestartPolicy::ReconcileOnly;
    Harness harness{options, scripted_authority(), TimePoint{0}};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "attempt-lifecycle"));
    harness.port.onDispatch = [](const Request&) -> Result<AdapterResponse> {
        return AdapterResponse::indeterminate("the scripted outcome is not known");
    };
    harness.port.onInspect = [](const Request&) -> Result<AdapterResponse> {
        return AdapterResponse::unknown("the authority has no record of the key");
    };

    const StepId step = mint_step(31);
    const PlanId plan =
        publish_single_step_plan(harness.engine, single_request_step(step, "power.on"), "indeterminate");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    RC_REQUIRE_OK(harness.engine.tick());
    const auto first = harness.engine.execution(execution);
    RC_REQUIRE_MSG(first.has_value(), "the execution must exist after an indeterminate response");
    RC_REQUIRE_MSG(attempt_at(*first, 0).status == AttemptStatus::Dispatched,
                   "an indeterminate response is not a definite outcome");
    RC_REQUIRE_MSG(attempt_at(*first, 0).ambiguous,
                   "an indeterminate response leaves it unknown whether the request landed");
    RC_REQUIRE_MSG(attempt_at(*first, 0).effect == EffectStatus::NotAssessed,
                   "an indeterminate response must not carry an assessed effect");
    RC_REQUIRE_MSG(first->steps.at(step) == StepState::Dispatched,
                   "an indeterminate response must not advance the step past Dispatched");
    RC_REQUIRE_MSG(!first->point_of_no_return_passed,
                   "an indeterminate response must not record a point of no return");

    for (int tick = 0; tick < 4; ++tick) {
        RC_REQUIRE_OK(harness.engine.tick());
    }
    const auto settled = harness.engine.execution(execution);
    RC_REQUIRE_MSG(settled.has_value(), "the execution must exist after reconciliation");
    RC_REQUIRE_MSG(settled->attempts.size() == std::size_t{1},
                   "under ReconcileOnly nothing may be reissued, so no second attempt may exist");
    RC_REQUIRE_EQ(harness.port.dispatchCalls, std::uint64_t{1});
    RC_REQUIRE_MSG(attempt_at(*settled, 0).status == AttemptStatus::Dispatched,
                   "a request the authority has no record of stays unresolved under ReconcileOnly");
    RC_REQUIRE_MSG(settled->steps.at(step) != StepState::Verified,
                   "an indeterminate response must never verify a step");
    RC_REQUIRE_MSG(!is_terminal(settled->lifecycle),
                   "an indeterminate response must never close the execution as success");
    RC_REQUIRE_MSG(settled->lifecycle == Lifecycle::Running,
                   "an unresolved attempt keeps the execution running");
}

RC_TEST(an_unknown_response_permits_a_redispatch_with_a_new_idempotency_key) {
    Engine::Options options;
    options.max_attempts_per_step = 3;
    options.restart_policy = RestartPolicy::ReconcileThenRedispatch;
    Harness harness{options, scripted_authority(), TimePoint{0}};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "attempt-lifecycle"));
    RC_REQUIRE_OK(harness.engine.accept_authority(Domain::Power, harness.port.reference()));

    std::uint64_t dispatches = 0;
    harness.port.onDispatch = [&](const Request& request) -> Result<AdapterResponse> {
        ++dispatches;
        if (dispatches == 1) {
            return AdapterResponse::indeterminate("the first response was lost");
        }
        return verified_effect(request, Domain::Power, harness.port.reference());
    };
    harness.port.onInspect = [](const Request&) -> Result<AdapterResponse> {
        return AdapterResponse::unknown("the authority has no record of the key");
    };

    const StepId step = mint_step(41);
    const PlanId plan =
        publish_single_step_plan(harness.engine, single_request_step(step, "power.on"), "unknown-redispatch");
    const auto stored = harness.engine.plan(plan);
    RC_REQUIRE_MSG(stored.has_value(), "the published plan must be retrievable");
    const StepSpec* spec = stored->find_step(step);
    RC_REQUIRE_MSG(spec != nullptr, "the published plan must contain the step that was proposed");
    RC_REQUIRE_MSG(spec->requests.size() == std::size_t{1}, "the step declares exactly one request binding");

    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));
    const std::string firstKey = idempotency_key(execution, step, 1, spec->requests.front(), AttemptKind::Forward);
    const std::string secondKey = idempotency_key(execution, step, 2, spec->requests.front(), AttemptKind::Forward);
    RC_REQUIRE_MSG(firstKey != secondKey,
                   "a new attempt number must derive a new idempotency key from the same binding");

    RC_REQUIRE_OK(harness.engine.tick());
    const auto ambiguous = harness.engine.execution(execution);
    RC_REQUIRE_MSG(ambiguous.has_value(), "the execution must exist after the first dispatch");
    RC_REQUIRE_MSG(ambiguous->attempts.size() == std::size_t{1}, "the first tick commits one attempt");
    RC_REQUIRE_MSG(attempt_at(*ambiguous, 0).status == AttemptStatus::Dispatched,
                   "the lost response leaves the first attempt unresolved");
    RC_REQUIRE_MSG(attempt_at(*ambiguous, 0).idempotency == firstKey,
                   "the first attempt must carry the key derived from attempt number one");

    const TickReport second = RC_REQUIRE_OK(harness.engine.tick());
    const auto redispatched = harness.engine.execution(execution);
    RC_REQUIRE_MSG(redispatched.has_value(), "the execution must exist after reconciliation");
    RC_REQUIRE_MSG(redispatched->attempts.size() == std::size_t{2},
                   "the proven absence of the key must permit exactly one re-dispatch");
    RC_REQUIRE_MSG(redispatched->attempts[0].idempotency != redispatched->attempts[1].idempotency,
                   "a re-dispatch must never reuse the idempotency key of the attempt it replaces");
    RC_REQUIRE_MSG(redispatched->attempts[1].attempt_number == 2,
                   "the re-dispatch is a new attempt number");
    RC_REQUIRE_MSG(redispatched->attempts[1].idempotency == secondKey,
                   "the re-dispatch must carry the key derived from attempt number two");
    RC_REQUIRE_MSG(redispatched->attempts[1].status == AttemptStatus::EffectPresent,
                   "the verified re-dispatch must be recorded as a present effect");
    RC_REQUIRE_MSG(redispatched->steps.at(step) == StepState::Verified,
                   "a verified effect must verify the step");
    RC_REQUIRE_MSG(redispatched->lifecycle == Lifecycle::Complete,
                   "the only step is verified, so the execution must be Complete");
    RC_REQUIRE_MSG(second.dispatches.size() >= std::size_t{2},
                   "the tick must report both the reconciliation and the re-dispatch");
    RC_REQUIRE_MSG(second.dispatches.front().reconciled,
                   "the first report of the tick must be the reconciliation against the authority");
    RC_REQUIRE_EQ(harness.port.dispatchCalls, std::uint64_t{2});
}

// ---------------------------------------------------------------------------
// Negative cases: a claim is never an effect
// ---------------------------------------------------------------------------
// "Verified" always means that the expectation bound to the attempt was
// satisfied by an observation the coordinator accepted, from an authority the
// coordinator currently accepts. These two tests are the ones that keep an
// adapter from talking the coordinator into an effect: a verified claim bound to
// a superseded authority reference, and a verified claim whose own observed
// digest does not match the expectation it was checked against.

RC_TEST(a_verified_claim_bound_to_a_superseded_authority_is_never_verified) {
    Engine::Options options;
    options.max_attempts_per_step = 1;
    Harness harness{options, scripted_authority(), TimePoint{0}};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "superseded-authority"));

    // The coordinator accepted one authority reference, and then accepted a
    // strictly newer generation of the same authority. The first reference is
    // now superseded; the scripted authority keeps answering under it.
    const AuthorityRef superseded = harness.port.reference();
    RC_REQUIRE_OK(harness.engine.accept_authority(Domain::Power, superseded));
    const AuthorityRef current{"power", "instance-1", superseded.generation() + 1,
                               Digest::of("scripted-authority-next-generation")};
    RC_REQUIRE_MSG(current != superseded, "the test needs a reference that is strictly newer");
    RC_REQUIRE_OK(harness.engine.accept_authority(Domain::Power, current));
    RC_REQUIRE_MSG(harness.engine.current_authority(Domain::Power).has_value() &&
                       *harness.engine.current_authority(Domain::Power) == current,
                   "the ledger must currently hold the newer reference");

    harness.port.onDispatch = [&](const Request& request) -> Result<AdapterResponse> {
        return verified_effect(request, Domain::Power, superseded);
    };

    const StepId step = mint_step(81);
    const PlanId plan = publish_single_step_plan(harness.engine, single_request_step(step, "power.on"),
                                                 "superseded-authority");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    RC_REQUIRE_OK(harness.engine.tick());
    const auto view = harness.engine.execution(execution);
    RC_REQUIRE_MSG(view.has_value(), "the execution must exist after the dispatch");
    RC_REQUIRE_MSG(view->attempts.size() == std::size_t{1}, "exactly one attempt was committed");
    RC_REQUIRE_MSG(attempt_at(*view, 0).status == AttemptStatus::Unverifiable,
                   "a verified claim bound to a superseded authority must end Unverifiable");
    RC_REQUIRE_MSG(attempt_at(*view, 0).effect == EffectStatus::Unverifiable,
                   "the recorded assessment must not be Verified when its authority is superseded");
    RC_REQUIRE_MSG(state_of(*view, step) != StepState::Verified,
                   "a claim under superseded authority must never verify the step");
    RC_REQUIRE_MSG(view->lifecycle != Lifecycle::Complete,
                   "an unverifiable effect must never complete the execution");

    // No later tick may recover a verification that never happened.
    RC_REQUIRE_OK(harness.engine.tick());
    const auto settled = harness.engine.execution(execution);
    RC_REQUIRE_MSG(settled.has_value(), "the execution must exist after a second tick");
    RC_REQUIRE_MSG(state_of(*settled, step) != StepState::Verified,
                   "no later tick may verify a step whose evidence came from superseded authority");
    RC_REQUIRE_MSG(settled->lifecycle != Lifecycle::Complete,
                   "no later tick may complete an execution whose effect was never verified");
    RC_REQUIRE_EQ(harness.port.dispatchCalls, std::uint64_t{1});
}

RC_TEST(a_verified_claim_whose_observed_digest_differs_from_its_expectation_is_never_verified) {
    Engine::Options options;
    options.max_attempts_per_step = 1;
    Harness harness{options, scripted_authority(), TimePoint{0}};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "diverging-digests"));
    RC_REQUIRE_OK(harness.engine.accept_authority(Domain::Power, harness.port.reference()));

    // The authority answers with everything the happy path carries - a current
    // authority reference, a completed status, and an observation the
    // coordinator accepts - except that the observation it says it made does not
    // have the identity of the expectation it says it checked. The claim is
    // internally inconsistent, so no part of it may become a verified effect.
    harness.port.onDispatch = [&](const Request& request) -> Result<AdapterResponse> {
        AdapterResponse response = AdapterResponse::completed("the scripted authority reports a diverging effect");
        response.add_observation(AdapterObservation{Domain::Power, harness.port.reference().generation(),
                                                    "scripted/observation", "effect", Reading::flag(true),
                                                    TimePoint{}, harness.port.reference(), false});
        EffectAssessment assessment;
        assessment.status = EffectStatus::Verified;
        assessment.expectation = request.binding_digest();
        assessment.observed = Digest::of("a statement the attempt was never bound to");
        assessment.authority = harness.port.reference();
        assessment.detail = "the authority claims the expectation was met by different content";
        response.add_assessment(std::move(assessment));
        return response;
    };

    const StepId step = mint_step(91);
    const PlanId plan = publish_single_step_plan(harness.engine, single_request_step(step, "power.on"),
                                                 "diverging-digests");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    RC_REQUIRE_OK(harness.engine.tick());
    const auto view = harness.engine.execution(execution);
    RC_REQUIRE_MSG(view.has_value(), "the execution must exist after the dispatch");
    RC_REQUIRE_MSG(view->attempts.size() == std::size_t{1}, "exactly one attempt was committed");
    RC_REQUIRE_MSG(attempt_at(*view, 0).effect != EffectStatus::Verified,
                   "an assessment whose observed digest differs from its expectation must not be Verified");
    RC_REQUIRE_MSG(state_of(*view, step) != StepState::Verified,
                   "an inconsistent claim must never verify the step");
    RC_REQUIRE_MSG(view->lifecycle != Lifecycle::Complete,
                   "an inconsistent claim must never complete the execution");

    // And the refusal is durable in the sense that matters: no later tick turns
    // the same claim into a success.
    RC_REQUIRE_OK(harness.engine.tick());
    const auto settled = harness.engine.execution(execution);
    RC_REQUIRE_MSG(settled.has_value(), "the execution must exist after a second tick");
    RC_REQUIRE_MSG(state_of(*settled, step) != StepState::Verified,
                   "no later tick may verify a step on an inconsistent claim");
    RC_REQUIRE_MSG(settled->lifecycle != Lifecycle::Complete,
                   "no later tick may complete an execution whose effect was never verified");
    RC_REQUIRE_EQ(harness.port.dispatchCalls, std::uint64_t{1});
}
