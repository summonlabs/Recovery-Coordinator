// A downstream consumer of the installed RecoveryCoordinator package.
//
// PROVENANCE: the adjacent authority this program talks to is the synthetic
// model that ships with the library. Nothing here touches facility hardware.
//
// The program exists to prove three packaging properties that source-tree
// builds cannot prove:
//
//   * the installed public headers compile outside the source tree;
//   * the exported target carries its include directories and its C++20
//     requirement transitively;
//   * the library links and runs, so a consumer can propose a plan, publish it
//     and start an execution without writing anything that the package does not
//     export.
//
// It deliberately asserts nothing about how far that execution progresses: the
// plan here is not the repository's end-to-end recovery proof, and the
// lifecycle it reaches is printed rather than assumed.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>

#include "recovery/adapters/synthetic.hpp"
#include "recovery/canonical.hpp"
#include "recovery/engine.hpp"
#include "recovery/plan.hpp"
#include "recovery/ports.hpp"

namespace {

class SteppingClock final : public recovery::Clock {
public:
    [[nodiscard]] recovery::TimePoint now() const override {
        const auto value = recovery::TimePoint{base + step * counter};
        ++counter;
        return value;
    }

private:
    mutable std::uint64_t counter{0};
    std::uint64_t base{1767225600000000000ull};  // 2026-01-01T00:00:00Z
    std::uint64_t step{1000000000ull};
};

[[nodiscard]] std::string effect_parameters(const std::string& effect) {
    recovery::CanonicalWriter writer;
    writer.raw("{\"effect\":");
    writer.quoted(effect);
    writer.raw("}");
    return writer.take();
}

[[noreturn]] void fail(const char* stage, const recovery::Error& error) {
    std::fprintf(stderr, "%s: %s\n", stage, error.describe().c_str());
    std::exit(1);
}

}  // namespace

int main() {
    using namespace recovery;

    synthetic::World world;  // in memory: this consumer owns no facility file
    synthetic::Adapter adapter{world, synthetic::Adapter::Options{}};
    MemoryLog log;
    SteppingClock clock;
    Engine::Options options;
    options.max_attempts_per_step = 2;
    Engine engine{log, adapter, clock, options};

    auto opened = engine.open(Epoch{1}, "downstream-consumer");
    if (!opened.has_value()) {
        fail("open", opened.error());
    }
    std::printf("opened %s\n", opened.value().summary().c_str());

    for (const Domain domain : {Domain::Power}) {
        auto accepted = engine.accept_authority(domain, world.authority(domain));
        if (!accepted.has_value()) {
            fail("accept_authority", accepted.error());
        }
        auto observed = adapter.observe(Request{}, domain);
        if (!observed.has_value()) {
            fail("observe", observed.error());
        }
        for (const AdapterObservation& observation : observed.value().observations()) {
            auto stored = engine.observe(observation);
            if (!stored.has_value()) {
                fail("observe(engine)", stored.error());
            }
        }
    }

    PlanDefinition definition;
    definition.name = "downstream-recovery";
    definition.scope = "downstream/consumer";
    definition.strategy = PlanStrategy::Immediate;

    IdAllocator steps{100};
    auto rawStep = steps.allocate(IdKind::Step);
    if (!rawStep.has_value()) {
        std::fprintf(stderr, "allocate: the step identity counter is exhausted\n");
        return 1;
    }
    const StepId step{*rawStep};

    StepSpec spec;
    spec.id = step;
    spec.name = "restore-power";
    spec.evidence_domains = {Domain::Power};
    RequestBinding binding;
    binding.step = step;
    binding.binding = 1;
    binding.domain = Domain::Power;
    binding.operation = "set-feed-state";
    binding.parameters = effect_parameters(R"({"domain":"power","key":"feed_state","value":"B"})");
    spec.requests.push_back(binding);
    definition.steps.push_back(spec);
    for (const Domain domain : {Domain::Power}) {
        const auto reference = engine.current_authority(domain);
        if (reference.has_value()) {
            definition.authority_requirements[domain] = *reference;
        }
    }

    auto planId = engine.propose_plan(definition);
    if (!planId.has_value()) {
        fail("propose_plan", planId.error());
    }
    auto published = engine.publish_plan(planId.value());
    if (!published.has_value()) {
        fail("publish_plan", published.error());
    }
    auto execution = engine.start_execution(planId.value());
    if (!execution.has_value()) {
        fail("start_execution", execution.error());
    }

    std::uint64_t dispatches = 0;
    for (int tickIndex = 0; tickIndex < 8; ++tickIndex) {
        auto tick = engine.tick();
        if (!tick.has_value()) {
            fail("tick", tick.error());
        }
        dispatches += tick.value().dispatches.size();
        const auto view = engine.execution(execution.value());
        if (view.has_value() && is_terminal(view->lifecycle)) {
            break;
        }
    }

    const auto view = engine.execution(execution.value());
    if (!view.has_value()) {
        std::fprintf(stderr, "execution: the execution the library returned is not visible\n");
        return 1;
    }
    if (engine.sequence() == 0) {
        std::fprintf(stderr, "durability: the engine recorded nothing\n");
        return 1;
    }

    std::printf("result plan=%s execution=%s lifecycle=%s steps=%llu dispatches=%llu records=%llu\n",
                planId.value().to_text().c_str(), execution.value().to_text().c_str(),
                std::string{to_string(view->lifecycle)}.c_str(),
                static_cast<unsigned long long>(view->steps.size()),
                static_cast<unsigned long long>(dispatches),
                static_cast<unsigned long long>(engine.sequence()));
    return 0;
}
