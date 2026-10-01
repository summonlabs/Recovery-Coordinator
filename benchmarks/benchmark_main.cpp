// Benchmarks for Recovery Coordinator.
//
// Every benchmark reports what it measured, at what scale, and with what
// provenance. A durable benchmark includes the durability cost it claims: the
// journal benchmarks below publish every record through a device flush, so the
// number they print is the cost of a completed, durable operation rather than
// the cost of handing bytes to the operating system.
//
// Provenance is honest about the *workload*, not about the implementation:
//
//   * REAL      - the measured operation is the shipped implementation and its
//                 input is produced by this program (in-process data, files,
//                 generated plans and DAGs);
//   * SYNTHETIC - the measured operation ends in an adjacent authority that is
//                 the in-process synthetic model, not a real facility.
//
// A scenario that fails to complete is printed as FAILED with its scale, its
// provenance and the state it reached, and the process exits non-zero. The
// benchmark never converts a failure into a smaller measurement.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "engine_internal.hpp"
#include "recovery/adapters/synthetic.hpp"
#include "recovery/canonical.hpp"
#include "recovery/engine.hpp"
#include "recovery/journal.hpp"
#include "recovery/plan.hpp"
#include "recovery/ports.hpp"

namespace {

class StepClock final : public recovery::Clock {
public:
    [[nodiscard]] recovery::TimePoint now() const override {
        const auto value = recovery::TimePoint{base_ + step_ * counter_};
        ++counter_;
        return value;
    }

private:
    mutable std::uint64_t counter_{0};
    std::uint64_t base_{1767225600000000000ull};
    std::uint64_t step_{1000000ull};
};

[[nodiscard]] double millis_since(const std::chrono::steady_clock::time_point& start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return std::chrono::duration<double, std::milli>(elapsed).count();
}

void report(const char* name, std::uint64_t operations, double millis, const char* provenance, const char* scale) {
    const double perOperation = operations == 0 ? 0.0 : millis / static_cast<double>(operations);
    std::fprintf(stdout, "%-34s ops=%8llu total_ms=%9.3f per_op_ms=%9.6f throughput_per_s=%12.1f provenance=%s scale=%s\n",
                 name, static_cast<unsigned long long>(operations), millis, perOperation,
                 perOperation <= 0.0 ? 0.0 : 1000.0 / perOperation, provenance, scale);
}

// A measured scenario that did not complete. The detail lines are printed
// immediately, in full, so the failure is diagnosable from this output alone:
// no other artifact is needed to see what state the engine reached.
void report_failure(const char* name, const char* provenance, const char* scale,
                    const std::vector<std::string>& detail) {
    std::fprintf(stdout, "%-34s FAILED provenance=%s scale=%s\n", name, provenance, scale);
    for (const std::string& line : detail) {
        std::fprintf(stdout, "  %s\n", line.c_str());
    }
}

// The complete observable state of one execution: its lifecycle, how many steps
// are in each step state, and the state of every step that is not Pending (the
// Pending population is already counted). Step names come from the plan, so a
// reader does not have to decode identities to see which step stalled.
[[nodiscard]] std::vector<std::string> describe_execution(const recovery::Engine& engine,
                                                          const recovery::ExecutionId& id) {
    using namespace recovery;
    std::vector<std::string> lines;
    const auto view = engine.execution(id);
    if (!view.has_value()) {
        lines.push_back("the execution is not visible in the engine");
        return lines;
    }
    const auto plan = engine.plan(view->plan);
    lines.push_back(std::string{"lifecycle="} + std::string{to_string(view->lifecycle)} +
                    " plan_revision=" + std::to_string(view->plan_revision) +
                    " steps=" + std::to_string(view->steps.size()) +
                    " attempts=" + std::to_string(view->attempts.size()) +
                    " point_of_no_return=" + (view->point_of_no_return_passed ? "true" : "false") +
                    " reason=\"" + view->reason + "\"");

    std::map<std::string, std::uint64_t> counts;
    std::vector<std::string> nonPending;
    for (const auto& [step, state] : view->steps) {
        const std::string stateName{to_string(state)};
        counts[stateName] += 1;
        if (state == StepState::Pending) {
            continue;
        }
        std::string name = step.to_text();
        if (plan.has_value()) {
            const StepSpec* spec = plan->find_step(step);
            if (spec != nullptr) {
                name = spec->name;
            }
        }
        nonPending.push_back(name + "=" + stateName);
    }
    std::string counted = "step states:";
    for (const auto& [stateName, count] : counts) {
        counted += " " + stateName + "=" + std::to_string(count);
    }
    lines.push_back(std::move(counted));

    std::string listed = "non-pending steps:";
    if (nonPending.empty()) {
        listed += " (none)";
    } else {
        for (const std::string& entry : nonPending) {
            listed += " " + entry;
        }
    }
    lines.push_back(std::move(listed));
    return lines;
}

// A generated DAG plus the identities it was built from, so a caller can reason
// about the expected ready set instead of trusting the benchmark's own count.
struct DagShape {
    recovery::Plan plan;
    std::vector<recovery::StepId> steps;
};

// Builds a generated plan with `count` steps. Each step depends on the previous
// `fanIn` steps (fanIn 0, 1 or 2), so the dependency edge count is exact and
// known to the caller.
[[nodiscard]] std::optional<DagShape> build_chain_dag(std::uint64_t count, std::uint64_t fanIn,
                                                      std::uint64_t seed) {
    using namespace recovery;
    IdAllocator ids{seed};
    auto rawPlan = ids.allocate(IdKind::Plan);
    if (!rawPlan.has_value()) {
        return std::nullopt;
    }
    DagShape shape;
    shape.plan = Plan{PlanId{*rawPlan}, "benchmark-dag", "synthetic/benchmark", PlanStrategy::Staged, 1,
                      TimePoint{1767225600000000000ull}};
    shape.steps.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t i = 0; i < count; ++i) {
        auto rawStep = ids.allocate(IdKind::Step);
        if (!rawStep.has_value()) {
            return std::nullopt;
        }
        shape.steps.push_back(StepId{*rawStep});
    }
    for (std::uint64_t i = 0; i < count; ++i) {
        StepSpec spec;
        spec.id = shape.steps[static_cast<std::size_t>(i)];
        spec.name = "dag-step-" + std::to_string(i);
        for (std::uint64_t back = 1; back <= fanIn && back <= i; ++back) {
            spec.depends_on.push_back(
                Dependency{shape.steps[static_cast<std::size_t>(i - back)], EdgeOutcome::AwaitVerified});
        }
        shape.plan.add_step(std::move(spec));
    }
    return shape;
}

// Builds a fan-out plan: one root step with no dependencies and `width` steps
// that depend only on the root. Once the root is Verified the ready set is the
// whole width, which is what makes this shape a ready-set measurement rather
// than a chain measurement.
[[nodiscard]] std::optional<DagShape> build_fan_out_dag(std::uint64_t width, std::uint64_t seed) {
    using namespace recovery;
    IdAllocator ids{seed};
    auto rawPlan = ids.allocate(IdKind::Plan);
    if (!rawPlan.has_value()) {
        return std::nullopt;
    }
    DagShape shape;
    shape.plan = Plan{PlanId{*rawPlan}, "benchmark-fan-out", "synthetic/benchmark", PlanStrategy::Staged, 1,
                      TimePoint{1767225600000000000ull}};
    auto rawRoot = ids.allocate(IdKind::Step);
    if (!rawRoot.has_value()) {
        return std::nullopt;
    }
    const StepId root{*rawRoot};
    StepSpec rootSpec;
    rootSpec.id = root;
    rootSpec.name = "root";
    shape.plan.add_step(std::move(rootSpec));
    shape.steps.push_back(root);

    for (std::uint64_t i = 0; i < width; ++i) {
        auto rawStep = ids.allocate(IdKind::Step);
        if (!rawStep.has_value()) {
            return std::nullopt;
        }
        const StepId step{*rawStep};
        StepSpec spec;
        spec.id = step;
        spec.name = "fan-" + std::to_string(i);
        spec.depends_on.push_back(Dependency{root, EdgeOutcome::AwaitVerified});
        shape.plan.add_step(std::move(spec));
        shape.steps.push_back(step);
    }
    return shape;
}

}  // namespace

int main(int argc, char** argv) {
    using namespace recovery;
    std::string directory = ".";
    // The end-to-end scenario's tick budget is a workload parameter, not a
    // hidden timeout: it is printed in that line's scale, and lowering it is how
    // a reader can see for themselves that a scenario which cannot complete is
    // reported with its state and fails the run.
    std::uint64_t tickBudgetOverride = 0;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string{argv[i]} == "--directory") {
            directory = argv[i + 1];
        }
        if (std::string{argv[i]} == "--tick-budget") {
            tickBudgetOverride = std::strtoull(argv[i + 1], nullptr, 10);
        }
    }
    std::vector<std::string> failures;

    // -----------------------------------------------------------------------
    // Canonical encoding (REAL: pure in-process computation)
    // -----------------------------------------------------------------------
    {
        constexpr std::uint64_t kOperations = 200000;
        const auto start = std::chrono::steady_clock::now();
        Digest accumulator{};
        for (std::uint64_t i = 0; i < kOperations; ++i) {
            CanonicalWriter writer;
            append_framed(writer, "recovery.benchmark.v1");
            append_framed_u64(writer, i);
            accumulator = writer.digest_of();
        }
        report("canonical_encode_and_sha256", kOperations, millis_since(start), "REAL",
               "200000 framed+digested values, in-process");
        if (accumulator.is_zero()) {
            report_failure("canonical_encode_and_sha256", "REAL", "200000 framed+digested values, in-process",
                           {"the digest accumulator is zero, so nothing was hashed"});
            failures.push_back("canonical_encode_and_sha256 produced a zero digest");
        }
    }

    // -----------------------------------------------------------------------
    // Durable journal publication (REAL: every record is flushed to the device)
    // -----------------------------------------------------------------------
    {
        const std::string path = directory + "/benchmark-journal.rcj";
        std::remove(path.c_str());
        Journal journal;
        JournalOptions options;
        options.createIfMissing = true;
        StepClock clock;
        auto opened = journal.open(path, options, clock.now(), false, Duration{});
        if (!opened.has_value()) {
            report_failure("durable_journal_append_flush", "REAL", "single file, device flush per record",
                           {"the journal could not be opened: " + opened.error().describe()});
            failures.push_back("durable_journal_append_flush could not open a journal");
        } else {
            constexpr std::uint64_t kRecords = 2000;
            const auto start = std::chrono::steady_clock::now();
            bool appendFailed = false;
            std::string appendDetail;
            for (std::uint64_t i = 0; i < kRecords; ++i) {
                CanonicalWriter writer;
                writer.raw("{\"sequence\":");
                writer.unsigned_integer(i);
                writer.raw(",\"payload\":");
                writer.quoted("benchmark-record-with-a-realistic-payload-length-0123456789");
                writer.raw("}");
                auto appended = journal.append(RecordKind::Checkpoint, writer.str());
                if (!appended.has_value()) {
                    appendFailed = true;
                    appendDetail = appended.error().describe();
                    break;
                }
            }
            if (appendFailed) {
                report_failure("durable_journal_append_flush", "REAL", "single file, device flush per record",
                               {"the append failed: " + appendDetail});
                failures.push_back("durable_journal_append_flush did not complete " +
                                   std::to_string(kRecords) + " durable appends");
            } else {
                report("durable_journal_append_flush", kRecords, millis_since(start), "REAL",
                       "single file, device flush per record");
            }

            const auto readStart = std::chrono::steady_clock::now();
            auto records = journal.read_all();
            if (!records.has_value()) {
                report_failure("durable_journal_verify_read", "REAL", "single file, chain verified",
                               {"the read-back failed: " + records.error().describe()});
                failures.push_back("durable_journal_verify_read could not verify the chain");
            } else {
                report("durable_journal_verify_read", static_cast<std::uint64_t>(records.value().size()),
                       millis_since(readStart), "REAL", "single file, chain verified");
                if (records.value().size() != kRecords) {
                    failures.push_back("durable_journal_verify_read returned " +
                                       std::to_string(records.value().size()) + " of " + std::to_string(kRecords) +
                                       " committed records");
                }
            }
            journal.close();
        }
        std::remove(path.c_str());
        std::remove((path + ".lock").c_str());
    }

    // -----------------------------------------------------------------------
    // Plan shape validation for a large DAG (REAL: the shipped validator)
    // -----------------------------------------------------------------------
    {
        constexpr std::uint64_t kSteps = 2000;
        constexpr std::uint64_t kFanIn = 2;
        constexpr std::uint64_t kEdges = kSteps * kFanIn - 3;  // 1 + 2 for the first two steps are absent
        constexpr std::uint64_t kIterations = 32;
        auto shape = build_chain_dag(kSteps, kFanIn, 1000);
        if (!shape.has_value()) {
            report_failure("plan_shape_validation_large_dag", "REAL", "2000-step DAG", {"the DAG could not be built"});
            failures.push_back("plan_shape_validation_large_dag could not build its DAG");
        } else {
            bool validated = true;
            std::string detail;
            const auto start = std::chrono::steady_clock::now();
            for (std::uint64_t iteration = 0; iteration < kIterations; ++iteration) {
                PlanValidationOptions options;
                auto result = validate_plan_shape(shape->plan, options);
                if (!result.has_value()) {
                    validated = false;
                    detail = result.error().describe();
                    break;
                }
                if (result.value() != shape->plan.id()) {
                    validated = false;
                    detail = "the validator returned a different plan identity";
                    break;
                }
            }
            const double millis = millis_since(start);
            const std::string scale = std::to_string(kSteps) + "-step DAG, " + std::to_string(kEdges) +
                                      " edges, default limits";
            if (!validated) {
                report_failure("plan_shape_validation_large_dag", "REAL", scale.c_str(),
                               {"the validator refused a DAG it built: " + detail});
                failures.push_back("plan_shape_validation_large_dag refused a valid " + std::to_string(kSteps) +
                                   "-step DAG");
            } else {
                report("plan_shape_validation_large_dag", kIterations, millis, "REAL", scale.c_str());
            }
        }
    }

    // -----------------------------------------------------------------------
    // Ready-set computation for a wide DAG (REAL: the shipped readiness
    // function, the same one the engine dispatches from)
    // -----------------------------------------------------------------------
    {
        constexpr std::uint64_t kWidth = 2999;
        constexpr std::uint64_t kIterations = 256;
        auto shape = build_fan_out_dag(kWidth, 2000);
        if (!shape.has_value()) {
            report_failure("ready_set_wide_dag", "REAL", "fan-out DAG", {"the DAG could not be built"});
            failures.push_back("ready_set_wide_dag could not build its DAG");
        } else {
            std::map<StepId, StepState> states;
            for (const StepId& step : shape->steps) {
                states[step] = StepState::Pending;
            }
            states[shape->steps.front()] = StepState::Verified;
            std::uint64_t observed = 0;
            const auto start = std::chrono::steady_clock::now();
            for (std::uint64_t iteration = 0; iteration < kIterations; ++iteration) {
                auto ready = compute_ready_states(shape->plan, states, false);
                observed = static_cast<std::uint64_t>(ready.size());
                if (observed != kWidth) {
                    break;
                }
            }
            const double millis = millis_since(start);
            const std::string scale = "fan-out DAG root+" + std::to_string(kWidth) + " dependents, ready width " +
                                      std::to_string(kWidth) + ", " + std::to_string(kIterations) +
                                      " computations";
            if (observed != kWidth) {
                report_failure("ready_set_wide_dag", "REAL", scale.c_str(),
                               {"the ready set was " + std::to_string(observed) + " instead of " +
                                std::to_string(kWidth) + " after the root was Verified"});
                failures.push_back("ready_set_wide_dag computed " + std::to_string(observed) + " ready steps");
            } else {
                report("ready_set_wide_dag", kIterations, millis, "REAL", scale.c_str());
            }
        }
    }

    // -----------------------------------------------------------------------
    // End to end synthetic recovery (SYNTHETIC: the adjacent authority is the
    // in-process model; the dependency graph, the durable log, the evidence
    // store and the attempt state machine are the real ones)
    // -----------------------------------------------------------------------
    {
        constexpr std::uint64_t kSteps = 200;
        const std::uint64_t tickBudget =
            tickBudgetOverride != 0 ? tickBudgetOverride : kSteps * 4 + 64;
        synthetic::World world;
        synthetic::Adapter adapter{world, synthetic::Adapter::Options{}};
        MemoryLog log;
        StepClock clock;
        Engine::Options options;
        options.max_attempts_per_step = 2;
        options.max_steps = 4096;
        Engine engine{log, adapter, clock, options};
        auto opened = engine.open(Epoch{1}, "benchmark");
        if (!opened.has_value()) {
            report_failure("end_to_end_staged_recovery", "SYNTHETIC", "200-step chain, in-memory durable log",
                           {"the engine could not be opened: " + opened.error().describe()});
            failures.push_back("end_to_end_staged_recovery could not open the engine");
        } else {
            for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity, Domain::Safety}) {
                auto accepted = engine.accept_authority(domain, world.authority(domain));
                if (!accepted.has_value()) {
                    std::fprintf(stderr, "authority: %s\n", accepted.error().describe().c_str());
                    std::exit(1);
                }
            }

            PlanDefinition definition;
            definition.name = "benchmark-chain";
            definition.scope = "synthetic/benchmark";
            definition.strategy = PlanStrategy::Staged;
            IdAllocator allocator{5000};
            std::vector<StepId> steps;
            for (std::uint64_t i = 0; i < kSteps; ++i) {
                auto raw = allocator.allocate(IdKind::Step);
                if (!raw.has_value()) {
                    std::fprintf(stderr, "allocate: the step identity counter is exhausted\n");
                    std::exit(1);
                }
                steps.push_back(StepId{*raw});
            }
            for (std::size_t i = 0; i < steps.size(); ++i) {
                StepSpec spec;
                spec.id = steps[i];
                spec.name = "step-" + std::to_string(i);
                spec.evidence_domains = {Domain::Power};
                if (i != 0) {
                    spec.depends_on.push_back(Dependency{steps[i - 1], EdgeOutcome::AwaitVerified});
                }
                RequestBinding binding;
                binding.step = steps[i];
                binding.binding = 1;
                binding.domain = Domain::Power;
                binding.operation = "set-feed-state";
                CanonicalWriter effect;
                effect.raw("{\"domain\":\"power\",\"key\":");
                effect.quoted("step-" + std::to_string(i));
                effect.raw(",\"value\":\"applied\"}");
                CanonicalWriter parameters;
                parameters.raw("{\"effect\":");
                parameters.quoted(effect.str());
                parameters.raw("}");
                binding.parameters = parameters.take();
                spec.requests.push_back(binding);
                definition.steps.push_back(spec);
            }
            for (const Domain domain : {Domain::Power, Domain::Cooling, Domain::Capacity, Domain::Safety}) {
                const auto reference = engine.current_authority(domain);
                if (reference.has_value()) {
                    definition.authority_requirements[domain] = *reference;
                }
            }

            const auto start = std::chrono::steady_clock::now();
            auto planId = engine.propose_plan(definition);
            if (!planId.has_value()) {
                std::fprintf(stderr, "propose: %s\n", planId.error().describe().c_str());
                std::exit(1);
            }
            auto published = engine.publish_plan(planId.value());
            if (!published.has_value()) {
                std::fprintf(stderr, "publish: %s\n", published.error().describe().c_str());
                std::exit(1);
            }
            auto execution = engine.start_execution(planId.value());
            if (!execution.has_value()) {
                std::fprintf(stderr, "start: %s\n", execution.error().describe().c_str());
                std::exit(1);
            }
            std::uint64_t ticks = 0;
            while (ticks < tickBudget) {
                auto tick = engine.tick();
                if (!tick.has_value()) {
                    std::fprintf(stderr, "tick: %s\n", tick.error().describe().c_str());
                    std::exit(1);
                }
                ++ticks;
                const auto view = engine.execution(execution.value());
                if (view.has_value() && is_terminal(view->lifecycle)) {
                    break;
                }
            }
            const auto view = engine.execution(execution.value());
            const bool complete = view.has_value() && view->lifecycle == Lifecycle::Complete;
            const std::string scale = std::to_string(kSteps) + "-step chain, in-memory durable log, tick budget " +
                                      std::to_string(tickBudget);
            if (!complete) {
                std::vector<std::string> detail;
                detail.push_back("ran " + std::to_string(ticks) + " ticks of a budget of " +
                                 std::to_string(tickBudget) + " without reaching Complete");
                const std::vector<std::string> described =
                    view.has_value() ? describe_execution(engine, execution.value())
                                     : std::vector<std::string>{"the execution is not visible in the engine"};
                detail.insert(detail.end(), described.begin(), described.end());
                report_failure("end_to_end_staged_recovery", "SYNTHETIC", scale.c_str(), detail);
                failures.push_back("end_to_end_staged_recovery did not complete after " + std::to_string(ticks) +
                                   " ticks");
            } else {
                report("end_to_end_staged_recovery", kSteps, millis_since(start), "SYNTHETIC", scale.c_str());
            }
        }
    }

    if (!failures.empty()) {
        std::fprintf(stdout, "benchmark: %llu measured scenario(s) failed\n",
                     static_cast<unsigned long long>(failures.size()));
        for (const std::string& failure : failures) {
            std::fprintf(stdout, "benchmark: failed: %s\n", failure.c_str());
        }
        return 1;
    }
    std::fprintf(stdout, "benchmark: every measured scenario completed\n");
    return 0;
}
