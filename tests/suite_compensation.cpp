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
// Compensation and points of no return
// ---------------------------------------------------------------------------
// Compensation is a bounded walk over the steps whose effect was verified. Once
// a point of no return has been verified the coordinator refuses to pretend it
// can put the facility back: it says so, and the operator abandons the plan.

namespace {

struct DispatchRecord {
    std::string idempotency;
    std::string operation;
    AttemptKind kind{AttemptKind::Forward};
    bool compensating{false};
    bool point_of_no_return{false};
};

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

    [[nodiscard]] Result<AdapterResponse> inspect(const Request&) override {
        return errors::unsupported("scripted.inspect", "the compensation tests never need an inspection");
    }

    [[nodiscard]] Result<AdapterResponse> observe(const Request&, Domain) override {
        return errors::unsupported("scripted.observe", "the engine never asks a port to observe directly");
    }

    [[nodiscard]] Result<AuthorityRef> current_authority(Domain) const override { return reference_; }

    [[nodiscard]] const AuthorityRef& reference() const noexcept { return reference_; }

    std::function<Result<AdapterResponse>(const Request&)> onDispatch;
    std::uint64_t dispatchCalls{0};
    std::vector<DispatchRecord> dispatches;

private:
    AuthorityRef reference_;
};

struct Harness {
    MemoryLog log{};
    ScriptedPort port;
    FixedClock clock{TimePoint{0}};
    Engine engine;

    Harness(Engine::Options options, AuthorityRef reference)
        : port{std::move(reference)}, engine{log, port, clock, options} {}
};

[[nodiscard]] AuthorityRef scripted_authority() {
    return AuthorityRef{"power", "instance-1", 1, Digest::of("scripted-authority")};
}

[[nodiscard]] StepId mint_step(std::uint64_t counter) {
    IdAllocator allocator{counter};
    auto id = allocator.allocate(IdKind::Step);
    RC_REQUIRE_MSG(id.has_value(), "the deterministic identity allocator refused to mint a step identity");
    return StepId{*id};
}

[[nodiscard]] StepSpec forward_step(const StepId& id, std::string operation, bool pointOfNoReturn,
                                    bool compensable) {
    StepSpec spec;
    spec.id = id;
    spec.name = "forward-step";
    spec.compensable = compensable;
    RequestBinding binding;
    binding.step = id;
    binding.binding = 0;
    binding.domain = Domain::Power;
    binding.operation = std::move(operation);
    binding.parameters = "{}";
    binding.point_of_no_return = pointOfNoReturn;
    spec.requests.push_back(std::move(binding));
    if (compensable) {
        RequestBinding compensation;
        compensation.step = id;
        compensation.binding = 1;
        compensation.domain = Domain::Power;
        compensation.operation = "undo";
        compensation.parameters = "{}";
        spec.compensation_requests.push_back(std::move(compensation));
    }
    return spec;
}

[[nodiscard]] StepSpec dependent_step(const StepId& id, const StepId& predecessor, std::string operation) {
    StepSpec spec = forward_step(id, std::move(operation), false, false);
    spec.depends_on.push_back(Dependency{predecessor, EdgeOutcome::AwaitVerified});
    return spec;
}

[[nodiscard]] PlanId publish_two_step_plan(Engine& engine, const StepSpec& first, const StepSpec& second,
                                           const char* name) {
    PlanDefinition definition;
    definition.name = name;
    definition.scope = "test-scope";
    definition.strategy = PlanStrategy::Staged;
    definition.steps.push_back(first);
    definition.steps.push_back(second);
    const PlanId plan = RC_REQUIRE_OK(engine.propose_plan(definition));
    RC_REQUIRE_OK(engine.publish_plan(plan));
    return plan;
}

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
    response.add_assessment(std::move(assessment));
    return response;
}

[[nodiscard]] StepState state_of(const ExecutionView& view, const StepId& step) {
    const auto it = view.steps.find(step);
    RC_REQUIRE_MSG(it != view.steps.end(), "the execution view does not contain the step under test");
    return it->second;
}

}  // namespace

RC_TEST(compensation_walks_the_verified_steps_and_closes_as_compensated) {
    Engine::Options options;
    options.max_attempts_per_step = 1;
    Harness harness{options, scripted_authority()};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "compensation"));
    RC_REQUIRE_OK(harness.engine.accept_authority(Domain::Power, harness.port.reference()));

    harness.port.onDispatch = [&](const Request& request) -> Result<AdapterResponse> {
        if (request.operation() == "fail") {
            return AdapterResponse::failed("the scripted operation failed");
        }
        return verified_effect(request, Domain::Power, harness.port.reference());
    };

    const StepId first = mint_step(51);
    const StepId second = mint_step(52);
    const PlanId plan = publish_two_step_plan(harness.engine, forward_step(first, "power.on", false, true),
                                              dependent_step(second, first, "fail"), "compensated-plan");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_OK(harness.engine.tick());

    const auto blocked = harness.engine.execution(execution);
    RC_REQUIRE_MSG(blocked.has_value(), "the execution must exist after the failure");
    RC_REQUIRE_MSG(state_of(*blocked, first) == StepState::Verified,
                   std::string{"the first step must have produced a verified effect, but it is "} +
                       std::string{to_string(state_of(*blocked, first))});
    RC_REQUIRE_MSG(state_of(*blocked, second) == StepState::Failed,
                   std::string{"the dependent step must have failed once its budget was spent, but it is "} +
                       std::string{to_string(state_of(*blocked, second))} + " after " +
                       std::to_string(harness.port.dispatches.size()) + " dispatches");
    RC_REQUIRE_MSG(blocked->lifecycle == Lifecycle::Blocked,
                   "a failed step with no budget must block the execution before compensation starts");

    RC_REQUIRE_OK(harness.engine.begin_compensation(execution, "the operator started compensation"));
    const auto compensating = harness.engine.execution(execution);
    RC_REQUIRE_MSG(compensating.has_value(), "the execution must exist while compensating");
    RC_REQUIRE_MSG(compensating->lifecycle == Lifecycle::Compensating,
                   "begin_compensation must move the execution to Compensating");

    RC_REQUIRE_OK(harness.engine.tick());
    const auto finished = harness.engine.execution(execution);
    RC_REQUIRE_MSG(finished.has_value(), "the execution must exist after compensation");
    RC_REQUIRE_MSG(state_of(*finished, first) == StepState::Compensated,
                   "the verified reversible step must be Compensated, not left Verified");
    RC_REQUIRE_MSG(finished->lifecycle == Lifecycle::Compensated,
                   "compensation of every verified step must close the execution as Compensated");
    RC_REQUIRE_MSG(is_terminal(finished->lifecycle), "Compensated is a terminal lifecycle");

    RC_REQUIRE_MSG(harness.port.dispatches.size() == std::size_t{3},
                   "the forward steps and one compensation walk must produce exactly three dispatches");
    RC_REQUIRE_EQ(harness.port.dispatches[0].operation, std::string{"power.on"});
    RC_REQUIRE_MSG(harness.port.dispatches[0].kind == AttemptKind::Forward,
                   "the first dispatch is a forward attempt");
    RC_REQUIRE_EQ(harness.port.dispatches[1].operation, std::string{"fail"});
    RC_REQUIRE_MSG(harness.port.dispatches[1].kind == AttemptKind::Forward,
                   "the dependent dispatch is a forward attempt");
    RC_REQUIRE_EQ(harness.port.dispatches[2].operation, std::string{"undo"});
    RC_REQUIRE_MSG(harness.port.dispatches[2].kind == AttemptKind::Compensation,
                   "the compensation walk must dispatch a compensation attempt");
    RC_REQUIRE_MSG(harness.port.dispatches[2].compensating,
                   "a compensation request must be marked compensating");
    RC_REQUIRE_MSG(harness.port.dispatches[2].idempotency != harness.port.dispatches[0].idempotency,
                   "compensation must not reuse the idempotency key of the effect it undoes");

    RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_MSG(harness.port.dispatches.size() == std::size_t{3},
                   "a closed execution must not dispatch anything further");
}

RC_TEST(a_verified_point_of_no_return_makes_compensation_impossible) {
    Engine::Options options;
    options.max_attempts_per_step = 1;
    Harness harness{options, scripted_authority()};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "point-of-no-return"));
    RC_REQUIRE_OK(harness.engine.accept_authority(Domain::Power, harness.port.reference()));

    harness.port.onDispatch = [&](const Request& request) -> Result<AdapterResponse> {
        if (request.operation() == "fail") {
            return AdapterResponse::failed("the scripted operation failed");
        }
        return verified_effect(request, Domain::Power, harness.port.reference());
    };

    const StepId irreversible = mint_step(61);
    const StepId second = mint_step(62);
    const PlanId plan = publish_two_step_plan(
        harness.engine, forward_step(irreversible, "capacity.commit", true, true),
        dependent_step(second, irreversible, "fail"), "irreversible-plan");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_OK(harness.engine.tick());

    const auto blocked = harness.engine.execution(execution);
    RC_REQUIRE_MSG(blocked.has_value(), "the execution must exist after the failure");
    RC_REQUIRE_MSG(blocked->point_of_no_return_passed,
                   "a verified point-of-no-return request must be recorded on the execution");
    RC_REQUIRE_MSG(state_of(*blocked, irreversible) == StepState::Verified,
                   "the point-of-no-return step is verified");
    RC_REQUIRE_MSG(blocked->lifecycle == Lifecycle::Blocked,
                   "the execution is blocked on the failed step before the operator decides");

    const Error irreversibleError =
        RC_REQUIRE_ERR(harness.engine.begin_compensation(execution, "the operator tried to compensate"),
                       ErrorClass::PreconditionFailed);
    RC_REQUIRE_EQ(irreversibleError.code(), std::string{"execution.irreversible"});

    const auto stillBlocked = harness.engine.execution(execution);
    RC_REQUIRE_MSG(stillBlocked.has_value(), "the execution must exist after a refused compensation");
    RC_REQUIRE_MSG(stillBlocked->lifecycle == Lifecycle::Blocked,
                   "a refused compensation must not move the execution into Compensating");

    RC_REQUIRE_OK(harness.engine.abandon_execution(execution, "the operator abandoned the plan"));
    const auto abandoned = harness.engine.execution(execution);
    RC_REQUIRE_MSG(abandoned.has_value(), "the execution must exist after abandonment");
    RC_REQUIRE_MSG(abandoned->lifecycle == Lifecycle::Abandoned,
                   "abandoning an execution past a point of no return must close it as Abandoned");
    RC_REQUIRE_MSG(is_terminal(abandoned->lifecycle), "Abandoned is a terminal lifecycle");

    const Error closedError =
        RC_REQUIRE_ERR(harness.engine.begin_compensation(execution, "too late"), ErrorClass::PreconditionFailed);
    RC_REQUIRE_EQ(closedError.code(), std::string{"execution.closed"});

    RC_REQUIRE_MSG(harness.port.dispatches.size() == std::size_t{2},
                   "no compensation may ever be dispatched past a point of no return");
    for (const DispatchRecord& dispatch : harness.port.dispatches) {
        RC_REQUIRE_MSG(dispatch.kind == AttemptKind::Forward,
                       "a compensation attempt was dispatched although the plan is irreversible");
    }
}

RC_TEST(a_zero_compensation_budget_refuses_to_start_compensation) {
    Engine::Options options;
    options.max_attempts_per_step = 1;
    options.max_compensation_rounds = 0;
    Harness harness{options, scripted_authority()};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "compensation-budget"));
    RC_REQUIRE_OK(harness.engine.accept_authority(Domain::Power, harness.port.reference()));

    harness.port.onDispatch = [&](const Request& request) -> Result<AdapterResponse> {
        if (request.operation() == "fail") {
            return AdapterResponse::failed("the scripted operation failed");
        }
        return verified_effect(request, Domain::Power, harness.port.reference());
    };

    const StepId first = mint_step(71);
    const StepId second = mint_step(72);
    const PlanId plan = publish_two_step_plan(harness.engine, forward_step(first, "power.on", false, true),
                                              dependent_step(second, first, "fail"), "no-compensation-budget");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_OK(harness.engine.tick());

    const Error error = RC_REQUIRE_ERR(harness.engine.begin_compensation(execution, "the operator tried"),
                                       ErrorClass::LimitExceeded);
    RC_REQUIRE_EQ(error.code(), std::string{"execution.compensation_rounds"});

    const auto view = harness.engine.execution(execution);
    RC_REQUIRE_MSG(view.has_value(), "the execution must exist after a refused compensation");
    RC_REQUIRE_MSG(view->lifecycle == Lifecycle::Blocked,
                   "a refused compensation must leave the lifecycle untouched");
    RC_REQUIRE_MSG(view->compensation_rounds == 0,
                   "a refused compensation must not spend a compensation round");
    RC_REQUIRE_MSG(harness.port.dispatches.size() == std::size_t{2},
                   "a refused compensation must dispatch nothing");
}
