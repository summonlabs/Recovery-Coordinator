#include "test.hpp"

#include <cstdio>
#include <string>

#include "recovery/adapters/synthetic.hpp"
#include "recovery/engine.hpp"
#include "recovery/journal.hpp"
#include "recovery/ports.hpp"

using namespace recovery;

namespace {

[[nodiscard]] std::string journal_path(const char* name) {
    return std::string{"scratch/"} + name + ".rcj";
}

[[nodiscard]] JournalOptions journal_options() {
    JournalOptions options;
    options.createIfMissing = true;
    return options;
}

// The smallest plan that exercises open, propose, publish, execute, dispatch,
// assess and complete against one synthetic authority.
[[nodiscard]] PlanDefinition single_step_plan(IdAllocator& steps, const AuthorityRef& powerAuthority) {
    PlanDefinition definition;
    definition.name = "durable-replay";
    definition.scope = "synthetic-facility/row-A";
    definition.strategy = PlanStrategy::Staged;

    const StepId stepId{*steps.allocate(IdKind::Step)};

    StepSpec step;
    step.id = stepId;
    step.name = "restore-power";
    step.evidence_domains = {Domain::Power};

    RequestBinding binding;
    binding.step = stepId;
    binding.domain = Domain::Power;
    binding.operation = "set-feed-state";
    binding.parameters = "{\"effect\":\"online\"}";
    step.requests.push_back(binding);
    step.compensable = true;
    binding.parameters = "{\"effect\":\"offline\"}";
    step.compensation_requests.push_back(binding);

    definition.steps.push_back(step);
    definition.authority_requirements.emplace(Domain::Power, powerAuthority);
    return definition;
}

void remove_journal(const std::string& path) {
    std::remove(path.c_str());
    std::remove((path + ".lock").c_str());
    std::remove((path + ".staging").c_str());
}

}  // namespace

// The engine must reconstruct its complete state from the durable records alone.
// This is the property that makes process death survivable: nothing that matters
// lives only in memory.
RC_TEST(engine_replays_a_journal_it_wrote) {
    const std::string path = journal_path("engine-replay");
    remove_journal(path);

    synthetic::World world;
    synthetic::Adapter adapter{world, synthetic::Adapter::Options{}};
    FixedClock clock{TimePoint{1000}};

    std::uint64_t firstSequence = 0;
    std::size_t firstEvidence = 0;

    {
        Journal journal;
        auto opened = journal.open(path, journal_options(), TimePoint{1000}, false, Duration{});
        RC_REQUIRE_MSG(opened.has_value(), "journal open: " + opened.error().describe());

        JournalLog log{journal};
        Engine::Options options;
        Engine engine{log, adapter, clock, options};
        auto report = engine.open(Epoch{1}, "first-incarnation");
        RC_REQUIRE_MSG(report.has_value(), "engine open: " + report.error().describe());

        const AuthorityRef power = world.authority(Domain::Power);
        auto accepted = engine.accept_authority(Domain::Power, power);
        RC_REQUIRE_MSG(accepted.has_value(), "accept authority: " + accepted.error().describe());

        IdAllocator steps;
        const PlanDefinition definition = single_step_plan(steps, power);
        auto planId = engine.propose_plan(definition);
        RC_REQUIRE_MSG(planId.has_value(), "propose plan: " + planId.error().describe());
        auto published = engine.publish_plan(planId.value());
        RC_REQUIRE_MSG(published.has_value(), "publish plan: " + published.error().describe());

        auto executionId = engine.start_execution(planId.value());
        RC_REQUIRE_MSG(executionId.has_value(), "start execution: " + executionId.error().describe());

        auto tick = engine.tick();
        RC_REQUIRE_MSG(tick.has_value(), "tick: " + tick.error().describe());

        const auto view = engine.execution(executionId.value());
        RC_REQUIRE_MSG(view.has_value(), "the execution disappeared");
        RC_REQUIRE_EQ(view->attempts.size(), static_cast<std::size_t>(1));

        firstSequence = journal.last_sequence();
        firstEvidence = engine.observations(Domain::Power).size();
        RC_REQUIRE(firstSequence > 0);
        engine.shutdown();
        journal.close();
    }

    // Reopen with a newer epoch: the durable records are the only thing carried
    // across, exactly as they would be after the first process was killed.
    Journal journal;
    auto opened = journal.open(path, journal_options(), TimePoint{2000}, false, Duration{});
    RC_REQUIRE_MSG(opened.has_value(), "second open: " + opened.error().describe());
    RC_REQUIRE_EQ(opened.value().recordCount, firstSequence);
    RC_REQUIRE(!opened.value().tornTailRepaired);
    RC_REQUIRE_EQ(opened.value().tornTailBytes, static_cast<std::uint64_t>(0));

    JournalLog log{journal};
    Engine::Options options;
    Engine engine{log, adapter, clock, options};
    auto report = engine.open(Epoch{2}, "second-incarnation");
    RC_REQUIRE_MSG(report.has_value(), "engine reopen: " + report.error().describe());

    RC_REQUIRE_EQ(report.value().records_replayed, firstSequence);
    RC_REQUIRE_EQ(report.value().plans_restored, static_cast<std::uint64_t>(1));
    RC_REQUIRE_EQ(report.value().executions_restored, static_cast<std::uint64_t>(1));
    RC_REQUIRE(report.value().evidence_fenced);
    RC_REQUIRE_EQ(engine.observations(Domain::Power).size(), firstEvidence);

    // Authority accepted by a previous incarnation is carried across as a
    // reference, never as an inherited decision: the plan binding still has to
    // match it, and every evidence view was fenced.
    const auto inherited = engine.current_authority(Domain::Power);
    RC_REQUIRE_MSG(inherited.has_value(), "authority accepted by the first incarnation was lost");
    const AuthorityRef power = world.authority(Domain::Power);
    RC_REQUIRE(*inherited == power);

    engine.shutdown();
    journal.close();
    remove_journal(path);
}
