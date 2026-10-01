#ifndef RECOVERY_PROCESS_HPP
#define RECOVERY_PROCESS_HPP

// Real operating system process control, used by the crash, restart and
// single-writer proofs. Nothing here is simulated: the child processes are
// genuine processes that this test binary starts, and the lock under test is a
// kernel lock that a dead process releases on its own.

#include <cstdint>
#include <string>
#include <vector>

#include "recovery/error.hpp"

namespace recovery {
namespace process {

struct Result_ {
    int exitCode{0};
    bool started{false};
    std::string error{};
    [[nodiscard]] bool ok() const noexcept { return started && error.empty(); }
};

struct SpawnOptions {
    std::string executable{};
    std::vector<std::string> arguments{};
    std::string workingDirectory{};
    std::string stdoutFile{};
    std::string stderrFile{};
};

// Starts a child process and waits for it to exit. Output is redirected to
// files when a path is given; pipes are deliberately not used so that an output
// pipe can never be the reason a test hangs.
[[nodiscard]] Result_ run(const SpawnOptions& options);

// Starts a child process without waiting for it.
class Child {
public:
    Child() = default;
    ~Child();

    Child(const Child&) = delete;
    Child& operator=(const Child&) = delete;
    Child(Child&& other) noexcept;
    Child& operator=(Child&& other) noexcept;

    [[nodiscard]] Result_ start(const SpawnOptions& options);
    // Waits for the child and returns its exit code. Calling this twice is an
    // error, not a silent success.
    [[nodiscard]] Result_ wait();
    // Terminates the child immediately and reaps it. Used only to clean up a
    // process a test deliberately left running.
    void terminate();
    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] std::uint32_t id() const noexcept { return id_; }

private:
    void* process_{nullptr};
    void* thread_{nullptr};
    std::uint32_t id_{0};
    bool reaped_{false};
};

// Absolute path of the running executable. Child processes are started from it
// so that a test never depends on the working directory.
[[nodiscard]] std::string current_executable();

[[nodiscard]] bool wait_for_file(const std::string& path, std::uint64_t timeoutMillis);
[[nodiscard]] bool wait_for_file_absent(const std::string& path, std::uint64_t timeoutMillis);

// Sleeps for the given number of milliseconds.
void sleep_millis(std::uint64_t millis);

}  // namespace process
}  // namespace recovery

#endif  // RECOVERY_PROCESS_HPP
