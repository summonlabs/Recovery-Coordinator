#ifndef RECOVERY_TEST_SUPPORT_REFERENCE_MODEL_HPP
#define RECOVERY_TEST_SUPPORT_REFERENCE_MODEL_HPP

// ---------------------------------------------------------------------------
// Independent reference model of the readiness semantics
// ---------------------------------------------------------------------------
// This header is an independent model of the coordinator's readiness semantics.
// It is written from the plan definition alone - the step state machine, the
// three dependency edge outcomes and the readiness gate definition that
// include/recovery/plan.hpp documents - and it is deliberately not a wrapper,
// a copy, or a friend of the dispatcher. It shares no code with
// src/engine.cpp: that translation unit holds the implementation under test
// (compute_ready_steps / compute_ready_states), while this header holds a
// second, separately written statement of what "may this step run now?" means.
//
// The model is written in a different shape from the implementation on purpose.
// The dispatcher answers the question by scanning the plan and testing each
// step against a runtime map; this model answers it by asking three named
// predicates - is this step a candidate, is every incoming edge satisfied, is
// the step even known - and only then admitting the step to the ready set. Two
// independently shaped implementations can still share a misunderstanding of
// the plan definition, which is exactly why the property suite does not merely
// compare them: it also proves that the resulting schedule is a valid
// topological layering of the generated DAG, a property that no shared mistake
// about a single predicate can satisfy by accident.
//
// The dispatcher is proven equivalent to this model by tests/suite_property.cpp:
// a locally implemented SplitMix64 with a fixed seed generates random DAGs and
// random step state assignments, and the two ready sets must be equal for every
// generated case, in the forward direction and in the compensation direction.
// On failure the suite prints the seed, the case index, the state assignment
// and every dependency of the plan, so that a counterexample is reproducible
// from the failure text alone.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "recovery/plan.hpp"

namespace recovery {
namespace reference {

// ---------------------------------------------------------------------------
// Step state classification
// ---------------------------------------------------------------------------
// Forward direction: a step is a candidate while the coordinator has not yet
// handed it to an adjacent authority. Pending means "not started", Ready means
// "the coordinator decided it may run"; Dispatched and everything after it has
// already left the coordinator and must not be dispatched twice.
[[nodiscard]] inline bool forward_candidate(StepState state) {
    return state == StepState::Pending || state == StepState::Ready;
}

// Compensation direction: only a step whose effect was verified and which the
// plan itself declares reversible may be compensated. A non-compensable step
// is never a candidate, however the caller asks.
[[nodiscard]] inline bool compensation_candidate(StepState state, bool compensable) {
    return compensable && state == StepState::Verified;
}

// A predecessor is resolved when its outcome is known and final: it either
// produced a verified effect, failed outright, was refused by the authority, or
// was skipped because a gate or an operator said so.
[[nodiscard]] inline bool resolved(StepState state) {
    return state == StepState::Verified || state == StepState::Failed || state == StepState::Refused ||
           state == StepState::Skipped;
}

// A predecessor failed when the coordinator may take a failure branch: an
// explicit failure or a refusal. Unverifiable is deliberately not a failure: it
// is an absence of knowledge, and the coordinator does not guess.
[[nodiscard]] inline bool failed(StepState state) {
    return state == StepState::Failed || state == StepState::Refused;
}

// ---------------------------------------------------------------------------
// Edge semantics
// ---------------------------------------------------------------------------
// AwaitVerified: the successor may run once the predecessor produced a
// verified effect. In the compensation direction the predecessor must itself
// have been compensated, because only then is the world back where the
// successor's own compensation expects it.
// AwaitResolved: the successor may run once the predecessor has any final
// outcome at all - this is the edge a diagnostic or follow-up step uses.
// OnFailure: the successor may run only once the predecessor failed.
[[nodiscard]] inline bool edge_satisfied(EdgeOutcome outcome, StepState predecessor, bool compensating) {
    switch (outcome) {
        case EdgeOutcome::AwaitVerified:
            return compensating ? predecessor == StepState::Compensated : predecessor == StepState::Verified;
        case EdgeOutcome::AwaitResolved:
            return resolved(predecessor);
        case EdgeOutcome::OnFailure:
            return failed(predecessor);
    }
    return false;
}

// ---------------------------------------------------------------------------
// Readiness
// ---------------------------------------------------------------------------
// One step is ready when the coordinator knows a state for it, that state is a
// candidate for the requested direction, and every incoming edge is satisfied
// by a predecessor the coordinator also knows a state for. A predecessor with
// no known state blocks its successors: the coordinator never infers a
// predecessor outcome from the absence of one.
[[nodiscard]] inline bool step_ready(const StepSpec& spec, const std::map<StepId, StepState>& states,
                                     bool compensating) {
    const auto self = states.find(spec.id);
    if (self == states.end()) {
        return false;
    }
    if (compensating) {
        if (!compensation_candidate(self->second, spec.compensable)) {
            return false;
        }
    } else if (!forward_candidate(self->second)) {
        return false;
    }
    for (const Dependency& dependency : spec.depends_on) {
        const auto predecessor = states.find(dependency.predecessor);
        if (predecessor == states.end()) {
            return false;
        }
        if (!edge_satisfied(dependency.outcome, predecessor->second, compensating)) {
            return false;
        }
    }
    return true;
}

// The ready set. Ascending StepId order, so that the answer is a pure function
// of the plan content and the state assignment and never of container or
// insertion order.
[[nodiscard]] inline std::vector<StepId> ready_set(const Plan& plan, const std::map<StepId, StepState>& states,
                                                   bool compensating) {
    std::vector<StepId> ready;
    for (const StepSpec& spec : plan.steps()) {
        if (step_ready(spec, states, compensating)) {
            ready.push_back(spec.id);
        }
    }
    std::sort(ready.begin(), ready.end());
    return ready;
}

// ---------------------------------------------------------------------------
// Expected completion schedule
// ---------------------------------------------------------------------------
// The model's expected completion order is computed by fixed point, without
// consulting the dispatcher: in every round the model completes the steps it
// finds ready, and the round in which a step completes is its depth in the
// dependency graph. The flattened order is therefore the order in which a
// coordinator that is allowed to run every ready step concurrently must observe
// the steps complete, and the round count is the longest dependency chain.
struct CompletionSchedule {
    std::vector<StepId> order{};
    std::map<StepId, std::uint32_t> round{};
    std::uint32_t rounds{0};
};

[[nodiscard]] inline CompletionSchedule completion_schedule(const Plan& plan,
                                                            const std::map<StepId, StepState>& initial,
                                                            bool compensating) {
    CompletionSchedule schedule;
    std::map<StepId, StepState> states = initial;
    std::uint32_t round = 0;
    while (true) {
        const std::vector<StepId> ready = ready_set(plan, states, compensating);
        if (ready.empty()) {
            break;
        }
        for (const StepId& step : ready) {
            states[step] = compensating ? StepState::Compensated : StepState::Verified;
            schedule.order.push_back(step);
            schedule.round[step] = round;
        }
        ++round;
    }
    schedule.rounds = round;
    return schedule;
}

// The expected completion order as a list of rounds: index i holds the steps
// the model completes in round i, in ascending StepId order.
[[nodiscard]] inline std::vector<std::vector<StepId>> completion_rounds(const Plan& plan,
                                                                        const std::map<StepId, StepState>& initial,
                                                                        bool compensating) {
    std::vector<std::vector<StepId>> rounds;
    std::map<StepId, StepState> states = initial;
    while (true) {
        const std::vector<StepId> ready = ready_set(plan, states, compensating);
        if (ready.empty()) {
            break;
        }
        for (const StepId& step : ready) {
            states[step] = compensating ? StepState::Compensated : StepState::Verified;
        }
        rounds.push_back(ready);
    }
    return rounds;
}

// A schedule is a valid topological layering when, for every AwaitVerified
// edge, the predecessor either was already verified before the schedule started
// or completed in a strictly earlier round than its successor. This is the
// property that makes the schedule a proof about the DAG rather than a
// restatement of the model.
[[nodiscard]] inline bool schedule_respects_dependencies(const Plan& plan,
                                                         const std::map<StepId, StepState>& initial,
                                                         const CompletionSchedule& schedule,
                                                         bool compensating) {
    for (const StepSpec& spec : plan.steps()) {
        const auto successorRound = schedule.round.find(spec.id);
        if (successorRound == schedule.round.end()) {
            continue;
        }
        for (const Dependency& dependency : spec.depends_on) {
            if (dependency.outcome != EdgeOutcome::AwaitVerified) {
                continue;
            }
            const auto before = initial.find(dependency.predecessor);
            const bool alreadySatisfied =
                before != initial.end() &&
                edge_satisfied(dependency.outcome, before->second, compensating);
            if (alreadySatisfied) {
                continue;
            }
            const auto predecessorRound = schedule.round.find(dependency.predecessor);
            if (predecessorRound == schedule.round.end()) {
                return false;
            }
            if (!(predecessorRound->second < successorRound->second)) {
                return false;
            }
        }
    }
    return true;
}

}  // namespace reference
}  // namespace recovery

#endif  // RECOVERY_TEST_SUPPORT_REFERENCE_MODEL_HPP
