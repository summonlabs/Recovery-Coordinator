#include "test.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "engine_internal.hpp"
#include "reference_model.hpp"

#include "recovery/id.hpp"
#include "recovery/plan.hpp"

using namespace recovery;

namespace {

// ---------------------------------------------------------------------------
// Deterministic randomness
// ---------------------------------------------------------------------------
// SplitMix64, implemented here rather than taken from <random>, with a fixed
// seed. The generator is part of the proof: the same seed always produces the
// same sequence of generated DAGs on every platform and every build, and the
// seed is printed both before the run and inside every failure message, so a
// counterexample can be replayed exactly. std::random_device is deliberately
// never consulted: a proof that cannot be reproduced is not a proof.
class SplitMix64 {
public:
    explicit SplitMix64(std::uint64_t seed) : state_(seed) {}

    [[nodiscard]] std::uint64_t next() {
        state_ += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = state_;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform-ish value in [0, bound). bound == 0 is treated as 1.
    [[nodiscard]] std::uint64_t below(std::uint64_t bound) {
        if (bound == 0) {
            return 0;
        }
        return next() % bound;
    }

    [[nodiscard]] bool chance(std::uint64_t percent) { return below(100) < percent; }

private:
    std::uint64_t state_{0};
};

constexpr std::uint64_t kReadinessSeed = 0x9E3779B97F4A7C15ull;
constexpr std::uint64_t kScheduleSeed = 0x0123456789ABCDEFull;
constexpr int kReadySetCases = 200;
constexpr int kScheduleCases = 200;
constexpr int kAssignmentsPerCase = 6;

[[nodiscard]] std::string hex_seed(std::uint64_t seed) {
    std::ostringstream out;
    out << "0x" << std::hex << seed << std::dec;
    return out.str();
}

// ---------------------------------------------------------------------------
// Case generation
// ---------------------------------------------------------------------------
// Every generated plan is a DAG by construction: a step may depend only on
// steps declared before it. Edge outcomes, compensation flags and duplicate
// edges are randomised, and one dependency may name an identity that is not a
// step of the plan at all, which is how the "unknown predecessor blocks" rule
// is exercised without hand-written cases.
struct GeneratedCase {
    Plan plan{};
    std::vector<StepId> steps{};
    std::map<StepId, std::string> labels{};
    std::vector<StepId> unknown{};
    // Coverage of the generator, so that a property test cannot pass by
    // generating only trivial cases: the suite asserts that every branch below
    // was exercised at least once.
    std::size_t awaitVerifiedEdges{0};
    std::size_t awaitResolvedEdges{0};
    std::size_t onFailureEdges{0};
    std::size_t duplicateEdges{0};
};

// What the generated corpus actually exercised. Asserted at the end of every
// property test; a corpus that never produced a ready step, never produced a
// multi-round schedule or never exercised an edge outcome would make the
// comparison vacuous.
struct Coverage {
    std::size_t forwardReadyNonEmpty{0};
    std::size_t compensationReadyNonEmpty{0};
    std::size_t missingSteps{0};
    std::size_t multiRoundSchedules{0};
    std::size_t awaitVerifiedEdges{0};
    std::size_t awaitResolvedEdges{0};
    std::size_t onFailureEdges{0};
    std::size_t duplicateEdges{0};
    std::size_t phantomEdges{0};
    std::size_t plans{0};
};

void account(const GeneratedCase& generated, Coverage& coverage) {
    ++coverage.plans;
    coverage.awaitVerifiedEdges += generated.awaitVerifiedEdges;
    coverage.awaitResolvedEdges += generated.awaitResolvedEdges;
    coverage.onFailureEdges += generated.onFailureEdges;
    coverage.duplicateEdges += generated.duplicateEdges;
    coverage.phantomEdges += generated.unknown.size();
}

[[nodiscard]] std::string label_of(const GeneratedCase& generated, const StepId& id) {
    const auto it = generated.labels.find(id);
    if (it != generated.labels.end()) {
        return it->second;
    }
    return std::string{"<unknown:"} + id.hex().substr(0, 8) + ">";
}

[[nodiscard]] StepId allocate_step(IdAllocator& allocator) {
    auto id = allocator.allocate(IdKind::Step);
    if (!id.has_value()) {
        throw ::rectest::Failure("the step identity allocator was exhausted");
    }
    return StepId{*id};
}

[[nodiscard]] GeneratedCase generate_case(SplitMix64& rng, std::uint64_t identityBase,
                                           std::uint64_t compensablePercent = 50, std::uint64_t edgePercent = 30) {
    GeneratedCase generated;
    generated.plan = Plan{PlanId{}, "property-readiness", "tests/readiness", PlanStrategy::Staged, 1, TimePoint{}};

    IdAllocator allocator{identityBase};
    const std::size_t count = 1 + static_cast<std::size_t>(rng.below(10));
    for (std::size_t index = 0; index < count; ++index) {
        const StepId id = allocate_step(allocator);
        generated.steps.push_back(id);
        generated.labels[id] = "s" + std::to_string(index);
    }

    for (std::size_t index = 0; index < count; ++index) {
        StepSpec spec;
        spec.id = generated.steps[index];
        spec.name = "step-" + std::to_string(index);
        spec.compensable = rng.chance(compensablePercent);
        spec.compensation_required = spec.compensable && rng.chance(30);
        for (std::size_t earlier = 0; earlier < index; ++earlier) {
            if (!rng.chance(edgePercent)) {
                continue;
            }
            Dependency dependency;
            dependency.predecessor = generated.steps[earlier];
            const std::uint64_t outcome = rng.below(3);
            dependency.outcome = outcome == 0   ? EdgeOutcome::AwaitVerified
                                 : outcome == 1 ? EdgeOutcome::AwaitResolved
                                                : EdgeOutcome::OnFailure;
            spec.depends_on.push_back(dependency);
            if (dependency.outcome == EdgeOutcome::AwaitVerified) {
                ++generated.awaitVerifiedEdges;
            } else if (dependency.outcome == EdgeOutcome::AwaitResolved) {
                ++generated.awaitResolvedEdges;
            } else {
                ++generated.onFailureEdges;
            }
            // A duplicate predecessor with a different edge outcome. The plan
            // definition allows it, and readiness must require both edges.
            if (rng.chance(15)) {
                Dependency second = dependency;
                second.outcome = outcome == 0 ? EdgeOutcome::OnFailure : EdgeOutcome::AwaitVerified;
                spec.depends_on.push_back(second);
                ++generated.duplicateEdges;
            }
        }
        // Rarely, depend on an identity that is not a step of this plan. The
        // coordinator must treat that predecessor as blocking, never as
        // satisfied by absence.
        if (rng.chance(10)) {
            const StepId phantom = allocate_step(allocator);
            generated.unknown.push_back(phantom);
            Dependency dependency;
            dependency.predecessor = phantom;
            dependency.outcome = EdgeOutcome::AwaitVerified;
            spec.depends_on.push_back(dependency);
        }
        generated.plan.add_step(spec);
    }
    return generated;
}

// A random assignment of step states. A step may be missing from the map
// entirely, which models a plan step the coordinator has no runtime for; such a
// step is never ready and its successors are blocked by it.
[[nodiscard]] std::map<StepId, StepState> random_states(SplitMix64& rng, const GeneratedCase& generated) {
    std::map<StepId, StepState> states;
    for (const StepId& step : generated.steps) {
        if (rng.chance(15)) {
            continue;
        }
        states[step] = static_cast<StepState>(1 + rng.below(12));
    }
    return states;
}

// A state assignment biased for the schedule properties. A schedule needs
// steps that can actually run, so this assignment concentrates on the states
// the requested direction admits: the forward direction on the states that may
// still be dispatched, the compensation direction on verified, compensable
// steps. The uniform assignment above is used for the ready set property, where
// every state matters.
[[nodiscard]] std::map<StepId, StepState> schedule_states(SplitMix64& rng, const GeneratedCase& generated,
                                                         bool compensating) {
    static const StepState forwardPool[] = {StepState::Pending, StepState::Pending, StepState::Ready,
                                            StepState::Verified, StepState::Verified, StepState::Failed,
                                            StepState::Skipped, StepState::Dispatched};
    static const StepState compensationPool[] = {StepState::Verified, StepState::Verified, StepState::Verified,
                                                 StepState::Compensated, StepState::Failed, StepState::Pending};
    const std::size_t forwardCount = sizeof(forwardPool) / sizeof(forwardPool[0]);
    const std::size_t compensationCount = sizeof(compensationPool) / sizeof(compensationPool[0]);
    std::map<StepId, StepState> states;
    for (const StepId& step : generated.steps) {
        if (rng.chance(10)) {
            continue;
        }
        states[step] = compensating ? compensationPool[rng.below(compensationCount)]
                                    : forwardPool[rng.below(forwardCount)];
    }
    return states;
}

// ---------------------------------------------------------------------------
// Failure rendering
// ---------------------------------------------------------------------------
[[nodiscard]] std::string state_name(const std::map<StepId, StepState>& states, const StepId& step) {
    const auto it = states.find(step);
    if (it == states.end()) {
        return "absent";
    }
    return std::string{to_string(it->second)};
}

[[nodiscard]] std::string describe_state_assignment(const GeneratedCase& generated,
                                                    const std::map<StepId, StepState>& states) {
    std::ostringstream out;
    for (const StepSpec& spec : generated.plan.steps()) {
        out << "    " << label_of(generated, spec.id) << "=" << state_name(states, spec.id)
            << " compensable=" << (spec.compensable ? "true" : "false") << " deps=[";
        bool first = true;
        for (const Dependency& dependency : spec.depends_on) {
            if (!first) {
                out << ", ";
            }
            first = false;
            out << label_of(generated, dependency.predecessor) << ":" << to_string(dependency.outcome);
        }
        out << "]\n";
    }
    for (const StepId& phantom : generated.unknown) {
        out << "    " << label_of(generated, phantom) << " is referenced but is not a step of the plan\n";
    }
    return out.str();
}

[[nodiscard]] std::string describe_steps(const GeneratedCase& generated, const std::vector<StepId>& steps) {
    std::ostringstream out;
    out << "[";
    for (std::size_t index = 0; index < steps.size(); ++index) {
        if (index != 0) {
            out << ", ";
        }
        out << label_of(generated, steps[index]);
    }
    out << "]";
    return out.str();
}

[[nodiscard]] std::string case_header(std::uint64_t seed, int caseIndex, int assignmentIndex,
                                      const char* direction) {
    std::ostringstream out;
    out << "seed=" << hex_seed(seed) << " case=" << caseIndex << " assignment=" << assignmentIndex
        << " direction=" << direction << "\n";
    return out.str();
}

// ---------------------------------------------------------------------------
// One property check
// ---------------------------------------------------------------------------
// Compares the dispatcher's ready set against the reference model's ready set
// for one generated case and one state assignment, in one direction. The
// dispatcher's answer must also be ascending and stable: readiness is a pure
// function of plan content and step states, so two calls with the same inputs
// must produce the same bytes.
void check_ready_set(std::uint64_t seed, int caseIndex, int assignmentIndex, const GeneratedCase& generated,
                     const std::map<StepId, StepState>& states, bool compensating, Coverage& coverage) {
    const char* direction = compensating ? "compensation" : "forward";
    const std::vector<StepId> dispatcher = compute_ready_states(generated.plan, states, compensating);
    const std::vector<StepId> model = reference::ready_set(generated.plan, states, compensating);
    if (compensating) {
        if (!model.empty()) {
            ++coverage.compensationReadyNonEmpty;
        }
    } else if (!model.empty()) {
        ++coverage.forwardReadyNonEmpty;
    }
    coverage.missingSteps += generated.steps.size() - states.size();
    const std::string context = case_header(seed, caseIndex, assignmentIndex, direction) +
                                describe_state_assignment(generated, states);

    if (!std::is_sorted(dispatcher.begin(), dispatcher.end())) {
        RC_REQUIRE_MSG(false, context + "  the dispatcher's ready set is not in ascending StepId order: " +
                                  describe_steps(generated, dispatcher));
    }
    const std::vector<StepId> again = compute_ready_states(generated.plan, states, compensating);
    if (again != dispatcher) {
        RC_REQUIRE_MSG(false, context + "  two identical calls disagreed: " + describe_steps(generated, dispatcher) +
                                  " vs " + describe_steps(generated, again));
    }
    if (dispatcher != model) {
        RC_REQUIRE_MSG(false, context + "  the dispatcher and the reference model disagree\n    dispatcher: " +
                                  describe_steps(generated, dispatcher) + "\n    reference:  " +
                                  describe_steps(generated, model));
    }
}

// Drives the dispatcher and the model through the same fixed point and proves
// that the resulting completion schedule is the model's expected schedule and a
// valid topological layering of the generated DAG.
void check_completion_schedule(std::uint64_t seed, int caseIndex, const GeneratedCase& generated,
                               const std::map<StepId, StepState>& states, bool compensating, Coverage& coverage) {
    const char* direction = compensating ? "compensation" : "forward";
    const std::string context = case_header(seed, caseIndex, 0, direction) +
                                describe_state_assignment(generated, states);

    const std::vector<std::vector<StepId>> expected = reference::completion_rounds(generated.plan, states, compensating);
    const reference::CompletionSchedule schedule = reference::completion_schedule(generated.plan, states, compensating);
    if (expected.size() >= 2) {
        ++coverage.multiRoundSchedules;
    }
    if (!expected.empty()) {
        if (compensating) {
            ++coverage.compensationReadyNonEmpty;
        } else {
            ++coverage.forwardReadyNonEmpty;
        }
    }
    coverage.missingSteps += generated.steps.size() - states.size();

    if (!reference::schedule_respects_dependencies(generated.plan, states, schedule, compensating)) {
        RC_REQUIRE_MSG(false, context + "  the reference schedule is not a valid topological layering: " +
                                  describe_steps(generated, schedule.order));
    }

    // The dispatcher drives its own fixed point: in every round it completes
    // exactly the steps its ready set names, and the flattened order must equal
    // the model's expected order round for round.
    std::map<StepId, StepState> driving = states;
    std::vector<StepId> order;
    std::size_t round = 0;
    while (true) {
        const std::vector<StepId> ready = compute_ready_states(generated.plan, driving, compensating);
        if (ready.empty()) {
            break;
        }
        if (round >= expected.size()) {
            RC_REQUIRE_MSG(false, context + "  the dispatcher produced more rounds than the model: round " +
                                      std::to_string(round) + " ready " + describe_steps(generated, ready));
        }
        if (ready != expected[round]) {
            RC_REQUIRE_MSG(false, context + "  round " + std::to_string(round) +
                                      " disagrees with the model\n    dispatcher: " + describe_steps(generated, ready) +
                                      "\n    reference:  " + describe_steps(generated, expected[round]));
        }
        for (const StepId& step : ready) {
            driving[step] = compensating ? StepState::Compensated : StepState::Verified;
            order.push_back(step);
        }
        ++round;
    }
    if (round != expected.size()) {
        RC_REQUIRE_MSG(false, context + "  the dispatcher stopped after " + std::to_string(round) +
                                  " rounds while the model expected " + std::to_string(expected.size()));
    }
    if (order != schedule.order) {
        RC_REQUIRE_MSG(false, context + "  the completion order disagrees with the model\n    dispatcher: " +
                                  describe_steps(generated, order) + "\n    reference:  " +
                                  describe_steps(generated, schedule.order));
    }
    if (schedule.rounds != static_cast<std::uint32_t>(expected.size())) {
        RC_REQUIRE_MSG(false, context + "  the model's own round count is inconsistent");
    }
}

}  // namespace

RC_TEST(property_ready_set_matches_the_reference_model) {
    std::cout << "[ seed ] readiness property seed=" << hex_seed(kReadinessSeed) << " cases=" << kReadySetCases
              << " assignments=" << kAssignmentsPerCase << " directions=forward,compensation" << std::endl;
    SplitMix64 rng{kReadinessSeed};
    Coverage coverage;
    std::uint64_t identityBase = 0x1000;
    for (int caseIndex = 0; caseIndex < kReadySetCases; ++caseIndex) {
        const GeneratedCase generated = generate_case(rng, identityBase);
        identityBase += 0x100;
        account(generated, coverage);
        for (int assignmentIndex = 0; assignmentIndex < kAssignmentsPerCase; ++assignmentIndex) {
            const std::map<StepId, StepState> states = random_states(rng, generated);
            check_ready_set(kReadinessSeed, caseIndex, assignmentIndex, generated, states, false, coverage);
            check_ready_set(kReadinessSeed, caseIndex, assignmentIndex, generated, states, true, coverage);
        }
    }
    std::cout << "[ cover ] plans=" << coverage.plans << " forwardReady=" << coverage.forwardReadyNonEmpty
              << " compensationReady=" << coverage.compensationReadyNonEmpty
              << " missingSteps=" << coverage.missingSteps << " awaitVerified=" << coverage.awaitVerifiedEdges
              << " awaitResolved=" << coverage.awaitResolvedEdges << " onFailure=" << coverage.onFailureEdges
              << " duplicateEdges=" << coverage.duplicateEdges << " phantomEdges=" << coverage.phantomEdges
              << std::endl;
    RC_REQUIRE_MSG(coverage.plans == static_cast<std::size_t>(kReadySetCases), "the generator produced fewer plans");
    RC_REQUIRE_MSG(coverage.forwardReadyNonEmpty > 0, "no generated case ever had a ready step: the comparison is vacuous");
    RC_REQUIRE_MSG(coverage.compensationReadyNonEmpty > 0,
                   "no generated case ever had a compensable verified step: the compensation comparison is vacuous");
    RC_REQUIRE_MSG(coverage.missingSteps > 0, "no generated case ever omitted a step state");
    RC_REQUIRE_MSG(coverage.awaitVerifiedEdges > 0 && coverage.awaitResolvedEdges > 0 &&
                       coverage.onFailureEdges > 0,
                   "the corpus did not exercise every edge outcome");
    RC_REQUIRE_MSG(coverage.duplicateEdges > 0, "the corpus never produced a duplicated predecessor");
    RC_REQUIRE_MSG(coverage.phantomEdges > 0, "the corpus never produced a dependency on an unknown identity");
}

RC_TEST(property_forward_completion_order_matches_the_reference_model) {
    std::cout << "[ seed ] forward schedule property seed=" << hex_seed(kScheduleSeed) << " cases=" << kScheduleCases
              << " assignments=" << kAssignmentsPerCase << std::endl;
    SplitMix64 rng{kScheduleSeed};
    Coverage coverage;
    std::uint64_t identityBase = 0x900000;
    for (int caseIndex = 0; caseIndex < kScheduleCases; ++caseIndex) {
        const GeneratedCase generated = generate_case(rng, identityBase, 50, 45);
        identityBase += 0x100;
        account(generated, coverage);
        for (int assignmentIndex = 0; assignmentIndex < kAssignmentsPerCase; ++assignmentIndex) {
            const std::map<StepId, StepState> states = schedule_states(rng, generated, false);
            check_completion_schedule(kScheduleSeed, caseIndex, generated, states, false, coverage);
        }
    }
    std::cout << "[ cover ] plans=" << coverage.plans << " multiRoundSchedules=" << coverage.multiRoundSchedules
              << " forwardReady=" << coverage.forwardReadyNonEmpty << " missingSteps=" << coverage.missingSteps
              << std::endl;
    RC_REQUIRE_MSG(coverage.multiRoundSchedules > 0,
                   "no generated case ever completed in more than one round: the order comparison is vacuous");
    RC_REQUIRE_MSG(coverage.plans == static_cast<std::size_t>(kScheduleCases), "the generator produced fewer plans");
}

RC_TEST(property_compensation_completion_order_matches_the_reference_model) {
    std::cout << "[ seed ] compensation schedule property seed=" << hex_seed(kScheduleSeed + 1)
              << " cases=" << kScheduleCases << " assignments=" << kAssignmentsPerCase << std::endl;
    SplitMix64 rng{kScheduleSeed + 1};
    Coverage coverage;
    std::uint64_t identityBase = 0xA00000;
    for (int caseIndex = 0; caseIndex < kScheduleCases; ++caseIndex) {
        const GeneratedCase generated = generate_case(rng, identityBase, 90, 45);
        identityBase += 0x100;
        account(generated, coverage);
        for (int assignmentIndex = 0; assignmentIndex < kAssignmentsPerCase; ++assignmentIndex) {
            const std::map<StepId, StepState> states = schedule_states(rng, generated, true);
            check_completion_schedule(kScheduleSeed + 1, caseIndex, generated, states, true, coverage);
        }
    }
    std::cout << "[ cover ] plans=" << coverage.plans << " multiRoundSchedules=" << coverage.multiRoundSchedules
              << " compensationReady=" << coverage.compensationReadyNonEmpty
              << " missingSteps=" << coverage.missingSteps << std::endl;
    RC_REQUIRE_MSG(coverage.multiRoundSchedules > 0,
                   "no compensation schedule ever completed in more than one round: the comparison is vacuous");
    RC_REQUIRE_MSG(coverage.compensationReadyNonEmpty > 0,
                   "no compensation schedule ever had a ready step: the comparison is vacuous");
}
