#include "test.hpp"

#include "recovery/process.hpp"

using namespace recovery;

RC_TEST(process_runs_a_child_and_reads_its_status) {
    process::SpawnOptions options;
    options.executable = process::current_executable();
    options.arguments = {"--filter", "no-such-test-name"};
    const auto result = process::run(options);
    RC_REQUIRE(result.ok());
    RC_REQUIRE_EQ(result.exitCode, 0);
}

RC_TEST(test_suite_passes_when_run_from_a_child_process) {
    // The test binary is its own child here: this proves that the process
    // helper starts a real process and observes its real exit status, without
    // depending on any other executable being present.
    process::SpawnOptions options;
    options.executable = process::current_executable();
    options.arguments = {"--filter", "process_runs_a_child_and_reads_its_status"};
    const auto result = process::run(options);
    RC_REQUIRE(result.ok());
    RC_REQUIRE_EQ(result.exitCode, 0);
}
