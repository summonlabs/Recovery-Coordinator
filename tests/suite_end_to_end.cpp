#include "test.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "recovery/adapters/synthetic.hpp"
#include "recovery/canonical.hpp"
#include "recovery/engine.hpp"
#include "recovery/plan.hpp"
#include "recovery/ports.hpp"

using namespace recovery;

// ---------------------------------------------------------------------------
// End to end: a staged recovery plan against a synthetic adjacent authority
// ---------------------------------------------------------------------------
// PROVENANCE: SYNTHETIC. The adjacent authority is the in-process model in
// src/adapters/synthetic.cpp; it talks to nothing. The plan, the dependency
// graph, the readiness gates, the attempt state machine, the evidence store and
// the durable log are the real ones.
//
// What these tests prove:
//
//   * a four-step staged plan (restore power, restore cooling, restore
//     capacity, verify capacity behind a readiness gate) reaches
//     Lifecycle::Complete, which is only possible if every step produced an
//     effect the coordinator could verify;
//   * every idempotency key was applied exactly once, and no step was
//     dispatched twice;
//   * applying an effect advances the domain's *state* generation and never the
//     *authority* generation, which is what keeps one accepted authority
//     reference usable for the whole plan;
//   * an effect whose evidence arrives under a superseded authority reference is
//     never recorded as verified;
//   * a completed claim that no observation confirms is never a verified
//     effect.

namespace {

constexpr std::uint64_t kBase = 1767225600000000000ull;  // 2026-01-01T00:00:00Z

// The canonical parameter text the synthetic adapter understands.
[[nodiscard]] std::string effect_parameters(const char* effect) {
    CanonicalWriter writer;
    writer.raw("{\"effect\":");
    writer.quoted(effect);
    writer.raw("}");
    return writer.take();
}

struct StagedPlan {
    PlanDefinition definition{};
    StepId power{};
    StepId cooling{};
    StepId capacity{};
    StepId verify{};
};

// The four-step plan the child process drives, built in process so that a
// failure here is a coordinator defect and not a tool defect.
[[nodiscard]] StagedPlan build_staged_plan() {
    StagedPlan plan;
    plan.definition.name = "end-to-end-recovery";
    plan.definition.scope = "synthetic-facility/row-A";
    plan.definition.strategy = PlanStrategy::Staged;

    IdAllocator steps{7000};
    plan.power = StepId{*steps.allocate(IdKind::Step)};
    plan.cooling = StepId{*steps.allocate(IdKind::Step)};
    plan.capacity = StepId{*steps.allocate(IdKind::Step)};
    plan.verify = StepId{*steps.allocate(IdKind::Step)};

    StepSpec powerStep;
    powerStep.id = plan.power;
    powerStep.name = "restore-power";
    powerStep.evidence_domains = {Domain::Power};
    RequestBinding powerBinding;
    powerBinding.step = plan.power;
    powerBinding.binding = 1;
    powerBinding.domain = Domain::Power;
    powerBinding.operation = "set-feed-state";
    powerBinding.parameters = effect_parameters(R"({"domain":"power","key":"feed_state","value":"B"})");
    powerStep.requests.push_back(powerBinding);
    powerStep.compensable = true;
    RequestBinding powerCompensation = powerBinding;
    powerCompensation.parameters = effect_parameters(R"({"domain":"power","key":"feed_state","value":"A"})");
    powerStep.compensation_requests.push_back(powerCompensation);
    plan.definition.steps.push_back(powerStep);

    StepSpec coolingStep;
    coolingStep.id = plan.cooling;
    coolingStep.name = "restore-cooling";
    coolingStep.depends_on.push_back(Dependency{plan.power, EdgeOutcome::AwaitVerified});
    coolingStep.evidence_domains = {Domain::Cooling};
    RequestBinding coolingBinding;
    coolingBinding.step = plan.cooling;
    coolingBinding.binding = 1;
    coolingBinding.domain = Domain::Cooling;
    coolingBinding.operation = "set-loop-state";
    coolingBinding.parameters = effect_parameters(R"({"domain":"cooling","key":"loop_state","value":"recovering"})");
    coolingStep.requests.push_back(coolingBinding);
    plan.definition.steps.push_back(coolingStep);

    StepSpec capacityStep;
    capacityStep.id = plan.capacity;
    capacityStep.name = "restore-capacity";
    capacityStep.depends_on.push_back(Dependency{plan.cooling, EdgeOutcome::AwaitVerified});
    capacityStep.evidence_domains = {Domain::Capacity};
    RequestBinding capacityBinding;
    capacityBinding.step = plan.capacity;
    capacityBinding.binding = 1;
    capacityBinding.domain = Domain::Capacity;
    capacityBinding.operation = "set-usable-racks";
    capacityBinding.parameters = effect_parameters(R"({"domain":"capacity","key":"usable_racks","value":16})");
    capacityStep.requests.push_back(capacityBinding);
    plan.definition.steps.push_back(capacityStep);

    StepSpec verifyStep;
    verifyStep.id = plan.verify;
    verifyStep.name = "verify-capacity";
    verifyStep.depends_on.push_back(Dependency{plan.capacity, EdgeOutcome::AwaitVerified});
    verifyStep.evidence_domains = {Domain::Capacity};
    ReadinessGate gate;
    gate.domain = Domain::Capacity;
    gate.required_keys = {"usable_racks"};
    gate.expected.emplace("usable_racks", Reading::count(16));
    // No freshness bound is named, so any accepted capacity reading will do.
    gate.max_age = Duration{};
    verifyStep.gates.push_back(gate);
    plan.definition.steps.push_back(verifyStep);

    return plan;
}

// The baseline facility the coordinator is shown before the plan starts. It is
// the same baseline the child process writes.
void seed_world(synthetic::World& world) {
    world.set_reading(Domain::Safety, "evacuated", Reading::flag(true));
    world.set_reading(Domain::Capacity, "usable_racks", Reading::count(8));
    world.set_reading(Domain::Power, "feed_state", Reading::text("A"));
    world.set_reading(Domain::Cooling, "loop_state", Reading::text("running"));
}

// Every domain the plan depends on is accepted and observed, exactly as an
// operator would do it.
void establish_authority(Engine& engine, synthetic::Adapter& adapter) {
    for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity, Domain::Safety}) {
        auto reference = adapter.current_authority(domain);
        RC_REQUIRE_MSG(reference.has_value(), "the synthetic authority must speak for every domain");
        RC_REQUIRE_OK(engine.accept_authority(domain, reference.value()));
        auto observation = adapter.observe(Request{}, domain);
        RC_REQUIRE_MSG(observation.has_value(), "the synthetic authority must be able to observe every domain");
        for (const AdapterObservation& value : observation.value().observations()) {
            RC_REQUIRE_OK(engine.observe(value));
        }
    }
}

[[nodiscard]] std::size_t count_records(const MemoryLog& log, RecordKind kind) {
    std::size_t count = 0;
    for (const JournalRecord& record : log.records()) {
        if (record.kind == kind) {
            ++count;
        }
    }
    return count;
}

// A scripted authority used only by the negative test below. PROVENANCE:
// SYNTHETIC; it models an authority that answers with a claim.
class ClaimOnlyPort final : public AdjacentAuthorityPort {
public:
    explicit ClaimOnlyPort(AuthorityRef reference) : reference_(std::move(reference)) {}

    [[nodiscard]] std::string authority_name() const override { return "claim-only-synthetic-authority"; }

    [[nodiscard]] Result<AdapterResponse> dispatch(const Request& request) override {
        ++dispatchCalls;
        return claim(request);
    }

    [[nodiscard]] Result<AdapterResponse> inspect(const Request& request) override { return claim(request); }

    [[nodiscard]] Result<AdapterResponse> observe(const Request&, Domain) override {
        return errors::unsupported("claim-only.observe", "the coordinator never asks a port to observe directly");
    }

    [[nodiscard]] Result<AuthorityRef> current_authority(Domain) const override { return reference_; }

    [[nodiscard]] const AuthorityRef& reference() const noexcept { return reference_; }
    [[nodiscard]] std::uint64_t dispatch_calls() const noexcept { return dispatchCalls; }

private:
    // A completed answer that carries a Verified assessment and no observation
    // at all: the authority says the effect happened and shows nothing.
    [[nodiscard]] Result<AdapterResponse> claim(const Request& request) {
        AdapterResponse response = AdapterResponse::completed("the authority claims the effect is present");
        EffectAssessment assessment;
        assessment.status = EffectStatus::Verified;
        assessment.expectation = request.binding_digest();
        assessment.observed = request.binding_digest();
        assessment.authority = reference_;
        assessment.detail = "a claim with nothing behind it";
        response.add_assessment(std::move(assessment));
        return response;
    }

    AuthorityRef reference_;
    std::uint64_t dispatchCalls{0};
};

[[nodiscard]] StepSpec single_step(const StepId& id, const RequestBinding& binding) {
    StepSpec spec;
    spec.id = id;
    spec.name = "scripted-step";
    spec.evidence_domains = {Domain::Power};
    spec.requests.push_back(binding);
    return spec;
}

[[nodiscard]] RequestBinding power_binding(const StepId& id) {
    RequestBinding binding;
    binding.step = id;
    binding.binding = 1;
    binding.domain = Domain::Power;
    binding.operation = "set-feed-state";
    binding.parameters = effect_parameters(R"({"domain":"power","key":"feed_state","value":"B"})");
    return binding;
}

}  // namespace

// The proof obligation for R1: a staged plan of four steps reaches Complete.
RC_TEST(four_step_staged_plan_reaches_complete_in_one_pass) {
    synthetic::World world;
    synthetic::Adapter adapter{world, synthetic::Adapter::Options{}};
    MemoryLog log;
    FixedClock clock{TimePoint{kBase}};
    Engine::Options options;
    options.max_attempts_per_step = 2;
    Engine engine{log, adapter, clock, options};
    RC_REQUIRE_OK(engine.open(Epoch{1}, "end-to-end"));

    seed_world(world);
    establish_authority(engine, adapter);

    StagedPlan staged = build_staged_plan();
    for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity, Domain::Safety}) {
        const auto reference = engine.current_authority(domain);
        RC_REQUIRE_MSG(reference.has_value(), "every domain of the plan must carry accepted authority");
        staged.definition.authority_requirements[domain] = *reference;
    }

    const PlanId plan = RC_REQUIRE_OK(engine.propose_plan(staged.definition));
    RC_REQUIRE_OK(engine.publish_plan(plan));
    const ExecutionId execution = RC_REQUIRE_OK(engine.start_execution(plan));

    std::uint32_t ticks = 0;
    auto view = engine.execution(execution);
    while (view.has_value() && !is_terminal(view->lifecycle) && ticks < 32) {
        RC_REQUIRE_OK(engine.tick());
        view = engine.execution(execution);
        ++ticks;
    }

    RC_REQUIRE_MSG(view.has_value(), "the execution must exist after the plan finished");
    RC_REQUIRE_MSG(view->lifecycle == Lifecycle::Complete,
                   "a four-step staged plan whose effects are all confirmed must complete");
    RC_REQUIRE_EQ(view->steps.size(), std::size_t{4});
    RC_REQUIRE_MSG(view->steps.at(staged.power) == StepState::Verified, "restore-power must be Verified");
    RC_REQUIRE_MSG(view->steps.at(staged.cooling) == StepState::Verified, "restore-cooling must be Verified");
    RC_REQUIRE_MSG(view->steps.at(staged.capacity) == StepState::Verified, "restore-capacity must be Verified");
    RC_REQUIRE_MSG(view->steps.at(staged.verify) == StepState::Verified,
                   "the gated verify step must be Verified by the evidence the gates were given");
    RC_REQUIRE_MSG(!view->point_of_no_return_passed, "this plan names no point of no return");

    // Exactly one attempt per effect step, each of them verified, and the
    // verify step needs no attempt at all because it dispatches nothing.
    RC_REQUIRE_EQ(view->attempts.size(), std::size_t{3});
    for (const AttemptView& attempt : view->attempts) {
        RC_REQUIRE_MSG(attempt.status == AttemptStatus::EffectPresent,
                       "every attempt in a completed plan must be a present effect");
        RC_REQUIRE_MSG(attempt.effect == EffectStatus::Verified, "every attempt must carry a verified assessment");
        RC_REQUIRE_EQ(attempt.dispatch_count, std::uint32_t{1});
        RC_REQUIRE_MSG(!attempt.ambiguous, "no attempt in a completed plan may stay ambiguous");
    }
    RC_REQUIRE_EQ(adapter.dispatch_calls(), std::uint64_t{3});

    // Exactly one applied effect per idempotency key. The effect journal is the
    // authority's own record: an entry appears only when the key was applied for
    // the first time, and the apply count can only exceed one if the authority
    // applied a repeated key.
    RC_REQUIRE_EQ(world.effect_journal().size(), std::size_t{3});
    RC_REQUIRE_EQ(world.accepted_key_count(), std::size_t{3});
    RC_REQUIRE_EQ(world.applied_operation_count(), std::size_t{3});
    for (std::size_t index = 0; index < world.effect_journal().size(); ++index) {
        const std::string& key = world.effect_journal()[index];
        RC_REQUIRE_MSG(world.apply_count(key) == 1, "an idempotency key must be applied exactly once");
        for (std::size_t other = index + 1; other < world.effect_journal().size(); ++other) {
            RC_REQUIRE_MSG(world.effect_journal()[other] != key,
                           "two attempts must never share one idempotency key");
        }
    }

    // The facility really moved.
    RC_REQUIRE_MSG(world.readings(Domain::Power).at("feed_state") == Reading::text("B"),
                   "the power feed state must hold the reading the request named");
    RC_REQUIRE_MSG(world.readings(Domain::Cooling).at("loop_state") == Reading::text("recovering"),
                   "the cooling loop state must hold the reading the request named");
    RC_REQUIRE_MSG(world.readings(Domain::Capacity).at("usable_racks") == Reading::count(16),
                   "the capacity reading must hold the value the gate expects");
    RC_REQUIRE_MSG(world.readings(Domain::Safety).at("evacuated") == Reading::flag(true),
                   "an unrelated domain must be untouched by the plan");

    // Applying effects moved the state generation of every affected domain and
    // moved no authority generation at all: one accepted authority reference
    // stayed current for the whole plan.
    for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity}) {
        RC_REQUIRE_MSG(world.authority_generation(domain) == 1,
                       "applying an effect must not move the authority generation");
        RC_REQUIRE_MSG(world.generation(domain) > 1, "applying an effect must advance the state generation");
    }
    for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity, Domain::Safety}) {
        const auto accepted = engine.current_authority(domain);
        RC_REQUIRE_MSG(accepted.has_value(), "authority for a domain of the plan must stay accepted");
        RC_REQUIRE_MSG(*accepted == world.authority(domain),
                       "the accepted authority must still be the authority the world presents");
    }

    // The durable log records exactly one committed attempt per effect step.
    RC_REQUIRE_EQ(count_records(log, RecordKind::AttemptCommitted), std::size_t{3});
    RC_REQUIRE_EQ(count_records(log, RecordKind::AttemptSuperseded), std::size_t{0});
    RC_REQUIRE_EQ(count_records(log, RecordKind::ExecutionClosed), std::size_t{1});
}

// Invariant 4, negative half: an effect whose evidence arrives under an
// authority reference the coordinator has not accepted is not a verified
// effect, however definite the authority's own claim looks.
RC_TEST(effect_evidence_under_a_superseded_authority_is_never_verified) {
    synthetic::World world;
    synthetic::Adapter adapter{world, synthetic::Adapter::Options{}};
    MemoryLog log;
    FixedClock clock{TimePoint{kBase}};
    Engine::Options options;
    options.max_attempts_per_step = 2;
    Engine engine{log, adapter, clock, options};
    RC_REQUIRE_OK(engine.open(Epoch{1}, "superseded-authority"));

    seed_world(world);
    const auto accepted = adapter.current_authority(Domain::Power);
    RC_REQUIRE_MSG(accepted.has_value(), "the synthetic authority must speak for power");
    RC_REQUIRE_OK(engine.accept_authority(Domain::Power, accepted.value()));
    auto baseline = adapter.observe(Request{}, Domain::Power);
    RC_REQUIRE_MSG(baseline.has_value(), "the synthetic authority must be able to observe power");
    for (const AdapterObservation& value : baseline.value().observations()) {
        RC_REQUIRE_OK(engine.observe(value));
    }

    // The authority moves to a new generation before the plan is dispatched.
    // The coordinator still holds the reference it accepted, and that reference
    // is now superseded.
    world.replace_authority(Domain::Power, "authority-1", 2);
    const auto superseded = engine.current_authority(Domain::Power);
    RC_REQUIRE_MSG(superseded.has_value(), "the accepted authority must still be recorded");
    RC_REQUIRE_MSG(*superseded != world.authority(Domain::Power),
                   "the world must present a reference the coordinator has not accepted");

    const StepId step = StepId{*IdAllocator{7100}.allocate(IdKind::Step)};
    PlanDefinition definition;
    definition.name = "superseded-authority";
    definition.scope = "synthetic-facility/row-A";
    definition.strategy = PlanStrategy::Immediate;
    definition.steps.push_back(single_step(step, power_binding(step)));
    definition.authority_requirements[Domain::Power] = *superseded;
    const PlanId plan = RC_REQUIRE_OK(engine.propose_plan(definition));
    RC_REQUIRE_OK(engine.publish_plan(plan));
    const ExecutionId execution = RC_REQUIRE_OK(engine.start_execution(plan));

    for (int tick = 0; tick < 4; ++tick) {
        RC_REQUIRE_OK(engine.tick());
    }

    const auto view = engine.execution(execution);
    RC_REQUIRE_MSG(view.has_value(), "the execution must exist while the authority is superseded");
    RC_REQUIRE_MSG(view->lifecycle != Lifecycle::Complete,
                   "an effect confirmed only by a superseded authority must never complete the plan");
    RC_REQUIRE_MSG(view->steps.at(step) != StepState::Verified,
                   "a step whose evidence is under a superseded authority must not be Verified");
    RC_REQUIRE_MSG(!view->point_of_no_return_passed,
                   "a superseded authority must not be able to record a point of no return");
    for (const AttemptView& attempt : view->attempts) {
        RC_REQUIRE_MSG(attempt.effect != EffectStatus::Verified,
                       "an assessment bound to a superseded authority must never be recorded as verified");
    }
    // The effect itself did land in the world; the coordinator simply refuses to
    // call it verified, which is the whole point of the check.
    RC_REQUIRE_EQ(world.effect_journal().size(), std::size_t{1});
}

// A claim is not an observation. A completed answer that carries a Verified
// assessment and no evidence at all must not verify the step.
RC_TEST(a_completed_claim_without_evidence_is_never_verified) {
    const AuthorityRef reference = AuthorityRef{"power", "instance-1", 3, Digest::of("claim-only-authority")};
    ClaimOnlyPort port{reference};
    MemoryLog log;
    FixedClock clock{TimePoint{kBase}};
    Engine::Options options;
    options.max_attempts_per_step = 2;
    Engine engine{log, port, clock, options};
    RC_REQUIRE_OK(engine.open(Epoch{1}, "claim-only"));
    RC_REQUIRE_OK(engine.accept_authority(Domain::Power, reference));
    // The coordinator holds accepted evidence for the domain, so the only thing
    // missing from the answer below is the observation itself.
    RC_REQUIRE_OK(engine.observe(AdapterObservation{Domain::Power, 3, "claim-only/observation", "feed_state",
                                                     Reading::text("A"), TimePoint{}, reference, false}));

    const StepId step = StepId{*IdAllocator{7200}.allocate(IdKind::Step)};
    PlanDefinition definition;
    definition.name = "claim-only";
    definition.scope = "synthetic-facility/row-A";
    definition.strategy = PlanStrategy::Immediate;
    definition.steps.push_back(single_step(step, power_binding(step)));
    definition.authority_requirements[Domain::Power] = reference;
    const PlanId plan = RC_REQUIRE_OK(engine.propose_plan(definition));
    RC_REQUIRE_OK(engine.publish_plan(plan));
    const ExecutionId execution = RC_REQUIRE_OK(engine.start_execution(plan));

    RC_REQUIRE_OK(engine.tick());
    const auto view = engine.execution(execution);
    RC_REQUIRE_MSG(view.has_value(), "the execution must exist after the claim");
    RC_REQUIRE_MSG(view->steps.at(step) != StepState::Verified,
                   "a claim that no observation confirms must never verify a step");
    RC_REQUIRE_MSG(view->lifecycle != Lifecycle::Complete, "a claim alone must never complete a plan");
    RC_REQUIRE_EQ(view->attempts.size(), std::size_t{1});
    RC_REQUIRE_MSG(view->attempts[0].status == AttemptStatus::Dispatched,
                   "a claim without evidence leaves the attempt unresolved");
    RC_REQUIRE_MSG(view->attempts[0].effect == EffectStatus::NotAssessed,
                   "a claim without evidence must not be recorded as an assessed effect");
    RC_REQUIRE_EQ(port.dispatch_calls(), std::uint64_t{1});
}
