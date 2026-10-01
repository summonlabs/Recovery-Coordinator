#include "test.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "recovery/plan.hpp"

using namespace recovery;

// ---------------------------------------------------------------------------
// Plan shape and plan topology
// ---------------------------------------------------------------------------
// These tests assert the documented contract of validate_plan_shape and of the
// pure plan queries: a plan is a proposal, and a proposal that cannot be
// executed unambiguously is refused before it can ever be published.

namespace {

// A fresh deterministic allocator per call: a failure message always names the
// same identity bytes, and no test depends on another test's allocation order.
[[nodiscard]] std::vector<StepId> mint_steps(std::size_t count) {
    IdAllocator allocator{1};
    std::vector<StepId> steps;
    steps.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto id = allocator.allocate(IdKind::Step);
        RC_REQUIRE_MSG(id.has_value(), "the deterministic identity allocator refused to mint a step identity");
        steps.push_back(StepId{*id});
    }
    return steps;
}

[[nodiscard]] PlanId mint_plan_id() {
    IdAllocator allocator{1};
    auto id = allocator.allocate(IdKind::Plan);
    RC_REQUIRE_MSG(id.has_value(), "the deterministic identity allocator refused to mint a plan identity");
    return PlanId{*id};
}

[[nodiscard]] StepSpec named_step(const StepId& id, std::string name) {
    StepSpec spec;
    spec.id = id;
    spec.name = std::move(name);
    return spec;
}

[[nodiscard]] Plan make_plan(std::string name, std::vector<StepSpec> steps) {
    Plan plan{mint_plan_id(), std::move(name), "test-scope", PlanStrategy::Staged, 1, TimePoint{}};
    for (StepSpec& step : steps) {
        plan.add_step(std::move(step));
    }
    return plan;
}

[[nodiscard]] Dependency requires_verified(const StepId& predecessor) {
    return Dependency{predecessor, EdgeOutcome::AwaitVerified};
}

[[nodiscard]] RequestBinding forward_binding(const StepId& step, std::string operation) {
    RequestBinding binding;
    binding.step = step;
    binding.binding = 0;
    binding.domain = Domain::Power;
    binding.operation = std::move(operation);
    binding.parameters = "{}";
    return binding;
}

// A diamond: step0 -> {step1, step2} -> step3. Every step has one request so
// that the plan is a real recovery plan rather than a pure graph.
[[nodiscard]] std::vector<StepSpec> diamond_steps(const std::vector<StepId>& ids) {
    StepSpec first = named_step(ids[0], "restore-power");
    first.requests.push_back(forward_binding(ids[0], "power.on"));

    StepSpec second = named_step(ids[1], "restore-cooling");
    second.depends_on.push_back(requires_verified(ids[0]));
    second.requests.push_back(forward_binding(ids[1], "cooling.on"));

    StepSpec third = named_step(ids[2], "restore-capacity");
    third.depends_on.push_back(requires_verified(ids[0]));
    third.requests.push_back(forward_binding(ids[2], "capacity.on"));

    StepSpec fourth = named_step(ids[3], "verify-capacity");
    fourth.depends_on.push_back(requires_verified(ids[1]));
    fourth.depends_on.push_back(requires_verified(ids[2]));
    fourth.requests.push_back(forward_binding(ids[3], "capacity.read"));

    std::vector<StepSpec> steps;
    steps.push_back(std::move(first));
    steps.push_back(std::move(second));
    steps.push_back(std::move(third));
    steps.push_back(std::move(fourth));
    return steps;
}

}  // namespace

RC_TEST(plan_shape_accepts_a_well_formed_plan) {
    const std::vector<StepId> ids = mint_steps(4);
    Plan plan = make_plan("well-formed", diamond_steps(ids));

    const PlanId accepted = RC_REQUIRE_OK(validate_plan_shape(plan));
    RC_REQUIRE_MSG(accepted == plan.id(), "a valid plan must validate under its own identity");

    const std::vector<StepId> order = RC_REQUIRE_OK(topological_order(plan));
    RC_REQUIRE_MSG(order.size() == ids.size(), "topological_order must emit every step exactly once");
    RC_REQUIRE_MSG(order.front() == ids[0], "the only step without prerequisites must be emitted first");
    RC_REQUIRE_MSG(order.back() == ids[3], "the step that waits for both branches must be emitted last");
}

RC_TEST(plan_shape_rejects_duplicate_step_identities) {
    const std::vector<StepId> ids = mint_steps(1);
    std::vector<StepSpec> steps;
    steps.push_back(named_step(ids[0], "first"));
    steps.push_back(named_step(ids[0], "second"));

    Plan plan = make_plan("duplicate-steps", std::move(steps));
    const Error error = RC_REQUIRE_ERR(validate_plan_shape(plan), ErrorClass::IdentityConflict);
    RC_REQUIRE_EQ(error.code(), std::string{"plan.duplicate_step"});
}

RC_TEST(plan_shape_rejects_dependency_cycles) {
    const std::vector<StepId> ids = mint_steps(3);
    std::vector<StepSpec> steps;
    StepSpec first = named_step(ids[0], "first");
    first.depends_on.push_back(requires_verified(ids[1]));
    StepSpec second = named_step(ids[1], "second");
    second.depends_on.push_back(requires_verified(ids[2]));
    StepSpec third = named_step(ids[2], "third");
    third.depends_on.push_back(requires_verified(ids[0]));
    steps.push_back(std::move(first));
    steps.push_back(std::move(second));
    steps.push_back(std::move(third));

    Plan plan = make_plan("cycle", std::move(steps));
    const Error error = RC_REQUIRE_ERR(validate_plan_shape(plan), ErrorClass::IdentityConflict);
    RC_REQUIRE_EQ(error.code(), std::string{"plan.dependency_cycle"});

    const Error orderError = RC_REQUIRE_ERR(topological_order(plan), ErrorClass::IdentityConflict);
    RC_REQUIRE_EQ(orderError.code(), std::string{"plan.dependency_cycle"});
}

RC_TEST(plan_shape_rejects_self_dependency) {
    const std::vector<StepId> ids = mint_steps(1);
    StepSpec step = named_step(ids[0], "self-referential");
    step.depends_on.push_back(requires_verified(ids[0]));

    std::vector<StepSpec> steps;
    steps.push_back(std::move(step));
    Plan plan = make_plan("self-dependency", std::move(steps));

    const Error error = RC_REQUIRE_ERR(validate_plan_shape(plan), ErrorClass::InvalidArgument);
    RC_REQUIRE_EQ(error.code(), std::string{"plan.self_dependency"});
}

RC_TEST(plan_shape_rejects_unknown_predecessors) {
    const std::vector<StepId> ids = mint_steps(2);
    StepSpec step = named_step(ids[0], "waits-for-a-stranger");
    step.depends_on.push_back(requires_verified(ids[1]));

    std::vector<StepSpec> steps;
    steps.push_back(std::move(step));
    Plan plan = make_plan("unknown-predecessor", std::move(steps));

    const Error error = RC_REQUIRE_ERR(validate_plan_shape(plan), ErrorClass::InvalidArgument);
    RC_REQUIRE_EQ(error.code(), std::string{"plan.unknown_predecessor"});
}

RC_TEST(plan_shape_rejects_zero_step_identities) {
    std::vector<StepSpec> steps;
    steps.push_back(named_step(StepId{}, "no-identity"));
    Plan plan = make_plan("zero-identity", std::move(steps));

    const Error error = RC_REQUIRE_ERR(validate_plan_shape(plan), ErrorClass::InvalidArgument);
    RC_REQUIRE_EQ(error.code(), std::string{"plan.step_identity"});
}

RC_TEST(plan_shape_rejects_empty_step_names) {
    const std::vector<StepId> ids = mint_steps(1);
    std::vector<StepSpec> steps;
    steps.push_back(named_step(ids[0], ""));
    Plan plan = make_plan("empty-name", std::move(steps));

    const Error error = RC_REQUIRE_ERR(validate_plan_shape(plan), ErrorClass::InvalidArgument);
    RC_REQUIRE_EQ(error.code(), std::string{"plan.step_name"});
}

RC_TEST(plan_shape_rejects_a_compensable_step_without_compensation) {
    const std::vector<StepId> ids = mint_steps(1);
    StepSpec step = named_step(ids[0], "claims-to-be-reversible");
    step.requests.push_back(forward_binding(ids[0], "power.off"));
    step.compensable = true;

    std::vector<StepSpec> steps;
    steps.push_back(std::move(step));
    Plan plan = make_plan("compensable-without-compensation", std::move(steps));

    const Error error = RC_REQUIRE_ERR(validate_plan_shape(plan), ErrorClass::InvalidArgument);
    RC_REQUIRE_EQ(error.code(), std::string{"plan.compensation_missing"});
}

RC_TEST(plan_shape_rejects_compensation_on_a_non_compensable_step) {
    const std::vector<StepId> ids = mint_steps(1);
    StepSpec step = named_step(ids[0], "declares-an-unusable-undo");
    step.requests.push_back(forward_binding(ids[0], "power.off"));
    step.compensable = false;
    RequestBinding compensation = forward_binding(ids[0], "power.on");
    compensation.binding = 1;
    step.compensation_requests.push_back(compensation);

    std::vector<StepSpec> steps;
    steps.push_back(std::move(step));
    Plan plan = make_plan("compensation-without-compensable", std::move(steps));

    const Error error = RC_REQUIRE_ERR(validate_plan_shape(plan), ErrorClass::InvalidArgument);
    RC_REQUIRE_EQ(error.code(), std::string{"plan.compensation_unused"});
}

RC_TEST(plan_shape_rejects_oversized_plans) {
    const std::vector<StepId> ids = mint_steps(3);
    std::vector<StepSpec> steps;
    for (const StepId& id : ids) {
        steps.push_back(named_step(id, "step"));
    }
    Plan plan = make_plan("oversized", std::move(steps));

    PlanValidationOptions options;
    options.max_steps = 2;
    const Error error = RC_REQUIRE_ERR(validate_plan_shape(plan, options), ErrorClass::LimitExceeded);
    RC_REQUIRE_EQ(error.code(), std::string{"plan.step_count"});
}

RC_TEST(topological_order_is_a_pure_function_of_plan_content) {
    // Identities are minted by a bit mixer, so "ascending identity" is not
    // "allocation order". The roles are therefore assigned from the sorted
    // identities: root = smallest, join = largest.
    std::vector<StepId> ids = mint_steps(4);
    std::sort(ids.begin(), ids.end());

    std::vector<StepSpec> declaration = diamond_steps(ids);
    Plan declared = make_plan("declared-order", std::move(declaration));

    // The same four steps with the same edges, inserted in the opposite order.
    // A dependency order must not depend on how the caller happened to append
    // the steps.
    std::vector<StepSpec> reversed = diamond_steps(ids);
    std::vector<StepSpec> reverseInsertion;
    for (std::size_t index = reversed.size(); index > 0; --index) {
        reverseInsertion.push_back(std::move(reversed[index - 1]));
    }
    Plan insertedBackwards = make_plan("declared-order", std::move(reverseInsertion));

    const std::vector<StepId> first = RC_REQUIRE_OK(topological_order(declared));
    const std::vector<StepId> second = RC_REQUIRE_OK(topological_order(insertedBackwards));

    RC_REQUIRE_MSG(first.size() == std::size_t{4}, "the diamond has four steps and must emit four identities");
    RC_REQUIRE_MSG(first == second,
                   "topological_order changed when the same steps were inserted in a different order");
    RC_REQUIRE_MSG(first == ids, "the emission order is not the ascending identity order of the ready steps");
}

RC_TEST(dependents_of_agrees_with_the_declared_dependencies) {
    // Five identities are minted so that the fifth is provably not in the plan.
    std::vector<StepId> minted = mint_steps(5);
    const StepId stranger = minted.back();
    minted.pop_back();
    std::sort(minted.begin(), minted.end());
    const std::vector<StepId>& ids = minted;
    Plan plan = make_plan("dependents", diamond_steps(ids));

    const std::vector<StepId> rootDependents = dependents_of(plan, ids[0]);
    RC_REQUIRE_MSG(rootDependents.size() == std::size_t{2},
                   "the root step has exactly two declared dependents");
    RC_REQUIRE_MSG(rootDependents[0] == ids[1] && rootDependents[1] == ids[2],
                   "dependents_of must return ascending identities of the steps that declare the edge");

    const std::vector<StepId> branchDependents = dependents_of(plan, ids[1]);
    RC_REQUIRE_MSG(branchDependents.size() == std::size_t{1},
                   "a step that only one step waits for has exactly one dependent");
    RC_REQUIRE_MSG(branchDependents[0] == ids[3],
                   "the dependent of the branch step must be the join step");

    RC_REQUIRE_MSG(dependents_of(plan, ids[3]).empty(), "the join step has no dependents");

    RC_REQUIRE_MSG(dependents_of(plan, stranger).empty(), "a step that is not in the plan has no dependents");
}

RC_TEST(step_position_follows_the_declared_step_order) {
    std::vector<StepId> minted = mint_steps(5);
    const StepId stranger = minted.back();
    minted.pop_back();
    const std::vector<StepId>& ids = minted;
    std::vector<StepSpec> declaration = diamond_steps(ids);
    Plan declared = make_plan("declared-order", std::move(declaration));

    const auto root = step_position(declared, ids[0]);
    const auto join = step_position(declared, ids[3]);
    RC_REQUIRE_MSG(root.has_value() && *root == std::size_t{0}, "the first declared step is at position 0");
    RC_REQUIRE_MSG(join.has_value() && *join == std::size_t{3}, "the last declared step is at position 3");
    RC_REQUIRE_MSG(!step_position(declared, stranger).has_value(),
                   "a step that is not in the plan has no position");

    // The reverse insertion order proves the position is the declared order,
    // not the identity order.
    std::vector<StepSpec> reversed = diamond_steps(ids);
    std::vector<StepSpec> reverseInsertion;
    for (std::size_t index = reversed.size(); index > 0; --index) {
        reverseInsertion.push_back(std::move(reversed[index - 1]));
    }
    Plan insertedBackwards = make_plan("declared-order", std::move(reverseInsertion));
    const auto backwardsJoin = step_position(insertedBackwards, ids[3]);
    RC_REQUIRE_MSG(backwardsJoin.has_value() && *backwardsJoin == std::size_t{0},
                   "step_position must report the declared order, not an identity order");
}

RC_TEST(plan_content_digest_is_stable_for_the_same_content) {
    const std::vector<StepId> ids = mint_steps(4);
    Plan first = make_plan("stable", diamond_steps(ids));
    Plan second = make_plan("stable", diamond_steps(ids));

    RC_REQUIRE_MSG(first.canonical() == second.canonical(),
                   "two plans with identical content must render identical canonical bytes");
    RC_REQUIRE_MSG(first.content_digest() == second.content_digest(),
                   "two plans with identical content must have the same content digest");
    RC_REQUIRE_MSG(!first.content_digest().is_zero(), "a plan content digest is never the zero digest");

    // Content addressing must be sensitive to every field the canonical form
    // carries: a status change and a step-name change each produce new bytes and
    // therefore a new digest. (An assertion that could never be false here would
    // prove nothing about the digest at all.)
    second.set_status(PlanStatus::Superseded);
    RC_REQUIRE_MSG(first.content_digest() != second.content_digest(),
                   "the lifecycle status is part of the canonical plan content, so the digest must change");

    std::vector<StepSpec> renamed = diamond_steps(ids);
    renamed[0].name = "restore-power-renamed";
    Plan different = make_plan("stable", std::move(renamed));
    RC_REQUIRE_MSG(different.content_digest() != first.content_digest(),
                   "renaming a step must change the plan content digest");
}
