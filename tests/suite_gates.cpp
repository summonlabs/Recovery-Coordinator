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
// Readiness gates
// ---------------------------------------------------------------------------
// A gate is evaluated against accepted evidence only. A gate that cannot be
// evaluated is a gate that is not satisfied: no dispatch, no state change, and a
// report that says why. The last test is the control case: when every condition
// holds, the very same plan dispatches.

namespace {

constexpr std::uint64_t kSecond = 1000000000ull;

class ScriptedPort final : public AdjacentAuthorityPort {
public:
    explicit ScriptedPort(AuthorityRef reference) : reference_(std::move(reference)) {}

    [[nodiscard]] std::string authority_name() const override { return "scripted-adjacent-authority"; }

    [[nodiscard]] Result<AdapterResponse> dispatch(const Request& request) override {
        ++dispatchCalls;
        if (!onDispatch) {
            return errors::internal("scripted.dispatch", "the test did not script a dispatch response");
        }
        return onDispatch(request);
    }

    [[nodiscard]] Result<AdapterResponse> inspect(const Request&) override {
        return errors::unsupported("scripted.inspect", "the gate tests never need an inspection");
    }

    [[nodiscard]] Result<AdapterResponse> observe(const Request&, Domain) override {
        return errors::unsupported("scripted.observe", "the engine never asks a port to observe directly");
    }

    [[nodiscard]] Result<AuthorityRef> current_authority(Domain) const override { return reference_; }

    [[nodiscard]] const AuthorityRef& reference() const noexcept { return reference_; }

    std::function<Result<AdapterResponse>(const Request&)> onDispatch;
    std::uint64_t dispatchCalls{0};

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

[[nodiscard]] StepId mint_step() {
    IdAllocator allocator{7};
    auto id = allocator.allocate(IdKind::Step);
    RC_REQUIRE_MSG(id.has_value(), "the deterministic identity allocator refused to mint a step identity");
    return StepId{*id};
}

[[nodiscard]] StepSpec gated_step(const StepId& id, const ReadinessGate& gate) {
    StepSpec spec;
    spec.id = id;
    spec.name = "gated-step";
    spec.gates.push_back(gate);
    RequestBinding binding;
    binding.step = id;
    binding.binding = 0;
    binding.domain = Domain::Power;
    binding.operation = "power.on";
    binding.parameters = "{}";
    spec.requests.push_back(std::move(binding));
    return spec;
}

[[nodiscard]] PlanId publish_step(Engine& engine, const StepSpec& step, const char* name) {
    PlanDefinition definition;
    definition.name = name;
    definition.scope = "test-scope";
    definition.strategy = PlanStrategy::Staged;
    definition.steps.push_back(step);
    const PlanId plan = RC_REQUIRE_OK(engine.propose_plan(definition));
    RC_REQUIRE_OK(engine.publish_plan(plan));
    return plan;
}

void establish_authority(Harness& harness, Domain domain) {
    RC_REQUIRE_OK(harness.engine.accept_authority(domain, harness.port.reference()));
}

void observe_reading(Harness& harness, Domain domain, std::string key, Reading reading, TimePoint at) {
    const AuthorityRef authority = harness.port.reference();
    RC_REQUIRE_OK(harness.engine.observe(AdapterObservation{domain, authority.generation(), "scripted/evidence",
                                                            std::move(key), std::move(reading), at, authority,
                                                            false}));
}

[[nodiscard]] StepState state_of(const ExecutionView& view, const StepId& step) {
    const auto it = view.steps.find(step);
    RC_REQUIRE_MSG(it != view.steps.end(), "the execution view does not contain the step under test");
    return it->second;
}

// Asserts that the gate refused to dispatch, that nothing about the step
// changed, and that the tick named the exact condition that failed. Naming the
// condition matters: otherwise any gate failure would satisfy every case, and
// the suite would prove only that "some gate rule" works.
void require_gate_blocks(Harness& harness, const StepId& step, const ExecutionId& execution,
                         const char* condition, const char* expectedReason) {
    const TickReport report = RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_MSG(harness.port.dispatchCalls == 0,
                   std::string{"the readiness gate dispatched a request although "} + condition);
    const auto view = harness.engine.execution(execution);
    RC_REQUIRE_MSG(view.has_value(), "the execution must exist while its gate is unsatisfied");
    RC_REQUIRE_MSG(state_of(*view, step) == StepState::Pending,
                   std::string{"a step advanced although "} + condition);
    std::string reported;
    bool statesUnchanged = false;
    for (const StepTransition& transition : report.transitions) {
        if (transition.step != step) {
            continue;
        }
        reported = transition.reason;
        statesUnchanged = transition.before == transition.after;
    }
    RC_REQUIRE_MSG(statesUnchanged,
                   std::string{"a gate that is not satisfied must not change the step state; the tick said: "} +
                       reported);
    RC_REQUIRE_MSG(reported.find(expectedReason) != std::string::npos,
                   std::string{"the gate was withheld for the wrong reason: expected \""} + expectedReason +
                       "\" but the tick said: " + reported);
}

}  // namespace

RC_TEST(a_readiness_gate_blocks_dispatch_without_accepted_evidence) {
    Harness harness{Engine::Options{}, scripted_authority()};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "gate-evidence"));
    establish_authority(harness, Domain::Power);

    ReadinessGate gate;
    gate.domain = Domain::Power;
    gate.required_keys.push_back("power_ok");
    gate.max_age = Duration{10 * kSecond};

    const StepId step = mint_step();
    const PlanId plan = publish_step(harness.engine, gated_step(step, gate), "gate-without-evidence");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    RC_REQUIRE_MSG(!harness.engine.view(Domain::Power).established(),
                   "the test needs an unestablished view for this case");
    require_gate_blocks(harness, step, execution, "the domain has no accepted evidence",
                        "no accepted evidence for a gate domain");
}

RC_TEST(a_readiness_gate_blocks_dispatch_when_evidence_is_older_than_max_age) {
    Harness harness{Engine::Options{}, scripted_authority()};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "gate-freshness"));
    establish_authority(harness, Domain::Power);
    observe_reading(harness, Domain::Power, "power_ok", Reading::flag(true), TimePoint{0});

    ReadinessGate gate;
    gate.domain = Domain::Power;
    gate.required_keys.push_back("power_ok");
    gate.max_age = Duration{kSecond};

    const StepId step = mint_step();
    const PlanId plan = publish_step(harness.engine, gated_step(step, gate), "gate-too-old");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    // The evidence is one second old when the gate allows exactly one second,
    // and two seconds old when it is evaluated.
    harness.clock.set(TimePoint{2 * kSecond});
    RC_REQUIRE_MSG(harness.engine.view(Domain::Power).established(),
                   "the test needs an established view for this case");
    require_gate_blocks(harness, step, execution, "the evidence is older than the gate allows",
                        "gate evidence is older than the gate allows");
}

RC_TEST(a_readiness_gate_blocks_dispatch_when_a_required_key_is_absent) {
    Harness harness{Engine::Options{}, scripted_authority()};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "gate-missing-key"));
    establish_authority(harness, Domain::Power);
    observe_reading(harness, Domain::Power, "cooling_ok", Reading::flag(true), TimePoint{0});

    ReadinessGate gate;
    gate.domain = Domain::Power;
    gate.required_keys.push_back("power_ok");
    gate.max_age = Duration{10 * kSecond};

    const StepId step = mint_step();
    const PlanId plan = publish_step(harness.engine, gated_step(step, gate), "gate-missing-key");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    require_gate_blocks(harness, step, execution, "a required reading is absent",
                        "a required reading is absent");
}

RC_TEST(a_readiness_gate_blocks_dispatch_when_an_expected_value_does_not_match) {
    Harness harness{Engine::Options{}, scripted_authority()};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "gate-wrong-value"));
    establish_authority(harness, Domain::Power);
    observe_reading(harness, Domain::Power, "power_ok", Reading::flag(false), TimePoint{0});

    ReadinessGate gate;
    gate.domain = Domain::Power;
    gate.required_keys.push_back("power_ok");
    gate.expected["power_ok"] = Reading::flag(true);
    gate.max_age = Duration{10 * kSecond};

    const StepId step = mint_step();
    const PlanId plan = publish_step(harness.engine, gated_step(step, gate), "gate-wrong-value");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    require_gate_blocks(harness, step, execution, "a required reading does not have the expected value",
                        "a required reading does not have the expected value");
}

RC_TEST(a_readiness_gate_passes_when_every_condition_holds) {
    Harness harness{Engine::Options{}, scripted_authority()};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "gate-satisfied"));
    establish_authority(harness, Domain::Power);
    observe_reading(harness, Domain::Power, "power_ok", Reading::flag(true), TimePoint{0});

    ReadinessGate gate;
    gate.domain = Domain::Power;
    gate.required_keys.push_back("power_ok");
    gate.expected["power_ok"] = Reading::flag(true);
    gate.max_age = Duration{10 * kSecond};
    gate.require_domain_authority = true;

    const StepId step = mint_step();
    const PlanId plan = publish_step(harness.engine, gated_step(step, gate), "gate-satisfied");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    harness.port.onDispatch = [&](const Request& request) -> Result<AdapterResponse> {
        AdapterResponse response = AdapterResponse::completed("the scripted authority applied the request");
        response.add_observation(AdapterObservation{Domain::Power, harness.port.reference().generation(),
                                                    "scripted/observation", "power_ok", Reading::flag(true),
                                                    TimePoint{0}, harness.port.reference(), false});
        EffectAssessment assessment;
        assessment.status = EffectStatus::Verified;
        assessment.expectation = request.binding_digest();
        assessment.observed = request.binding_digest();
        assessment.authority = harness.port.reference();
        assessment.detail = "the scripted authority confirms the effect";
        response.add_assessment(std::move(assessment));
        return response;
    };

    RC_REQUIRE_OK(harness.engine.tick());
    RC_REQUIRE_EQ(harness.port.dispatchCalls, std::uint64_t{1});

    const auto view = harness.engine.execution(execution);
    RC_REQUIRE_MSG(view.has_value(), "the execution must exist after a satisfied gate");
    RC_REQUIRE_MSG(view->attempts.size() == std::size_t{1},
                   "a satisfied gate must let the step commit exactly one attempt");
    RC_REQUIRE_MSG(view->attempts[0].status == AttemptStatus::EffectPresent,
                   "the scripted verified effect must be recorded as a present effect");
    RC_REQUIRE_MSG(state_of(*view, step) == StepState::Verified,
                   "a satisfied gate must let a verified effect verify the step");
    RC_REQUIRE_MSG(view->lifecycle == Lifecycle::Complete,
                   "the only step is verified, so the execution must be Complete");
}

RC_TEST(a_readiness_gate_requires_authority_for_its_domain) {
    Harness harness{Engine::Options{}, scripted_authority()};
    RC_REQUIRE_OK(harness.engine.open(Epoch{1}, "gate-authority"));
    // Evidence is accepted, but no authority reference was accepted for the
    // domain: the reading exists and is fresh, and the gate still refuses.
    const AuthorityRef authority = harness.port.reference();
    RC_REQUIRE_OK(harness.engine.observe(AdapterObservation{Domain::Power, authority.generation(),
                                                            "scripted/evidence", "power_ok", Reading::flag(true),
                                                            TimePoint{0}, authority, false}));
    RC_REQUIRE_MSG(harness.engine.view(Domain::Power).established(),
                   "the test needs an established view for this case");
    RC_REQUIRE_MSG(!harness.engine.current_authority(Domain::Power).has_value(),
                   "the test needs a domain with no accepted authority");

    ReadinessGate gate;
    gate.domain = Domain::Power;
    gate.required_keys.push_back("power_ok");
    gate.max_age = Duration{10 * kSecond};
    gate.require_domain_authority = true;

    const StepId step = mint_step();
    const PlanId plan = publish_step(harness.engine, gated_step(step, gate), "gate-without-authority");
    const ExecutionId execution = RC_REQUIRE_OK(harness.engine.start_execution(plan));

    require_gate_blocks(harness, step, execution, "the gate evidence is not bound to current authority",
                        "gate evidence is not bound to current authority");
}
