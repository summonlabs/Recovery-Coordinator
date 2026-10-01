#include "recovery/process.hpp"

#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

#include "recovery/fileio.hpp"

#if defined(_WIN32)
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace recovery {
namespace process {
namespace {

[[nodiscard]] std::string quote_argument(const std::string& value) {
    // The Windows command line is parsed by the child's C runtime. An argument
    // is quoted whenever it contains a space or a quote, and embedded quotes are
    // escaped with a backslash.
    if (value.find_first_of(" \t\"") == std::string::npos) {
        return value;
    }
    std::string out = "\"";
    for (const char c : value) {
        if (c == '"') {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

}  // namespace

std::string current_executable() {
#if defined(_WIN32)
    std::vector<char> buffer(MAX_PATH);
    while (true) {
        const DWORD written = GetModuleFileNameA(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (written == 0) {
            return {};
        }
        if (written < buffer.size()) {
            return std::string(buffer.data(), written);
        }
        buffer.resize(buffer.size() * 2);
    }
#else
    std::vector<char> buffer(4096);
    const ssize_t written = ::readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (written <= 0) {
        return {};
    }
    return std::string(buffer.data(), static_cast<std::size_t>(written));
#endif
}

void sleep_millis(std::uint64_t millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

bool wait_for_file(const std::string& path, std::uint64_t timeoutMillis) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMillis);
    while (std::chrono::steady_clock::now() < deadline) {
        if (fileio::exists(path)) {
            return true;
        }
        sleep_millis(2);
    }
    return fileio::exists(path);
}

bool wait_for_file_absent(const std::string& path, std::uint64_t timeoutMillis) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMillis);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!fileio::exists(path)) {
            return true;
        }
        sleep_millis(2);
    }
    return !fileio::exists(path);
}

Result_ run(const SpawnOptions& options) {
    Child child;
    auto started = child.start(options);
    if (!started.ok()) {
        return started;
    }
    return child.wait();
}

Child::~Child() {
    if (process_ != nullptr || thread_ != nullptr) {
        terminate();
    }
}

Child::Child(Child&& other) noexcept
    : process_(other.process_), thread_(other.thread_), id_(other.id_), reaped_(other.reaped_) {
    other.process_ = nullptr;
    other.thread_ = nullptr;
    other.id_ = 0;
    other.reaped_ = false;
}

Child& Child::operator=(Child&& other) noexcept {
    if (this != &other) {
        terminate();
        process_ = other.process_;
        thread_ = other.thread_;
        id_ = other.id_;
        reaped_ = other.reaped_;
        other.process_ = nullptr;
        other.thread_ = nullptr;
        other.id_ = 0;
        other.reaped_ = false;
    }
    return *this;
}

Result_ Child::start(const SpawnOptions& options) {
    Result_ result;
    if (process_ != nullptr) {
        result.error = "the child is already started";
        return result;
    }
    if (options.executable.empty()) {
        result.error = "no executable was given";
        return result;
    }

    if (!options.stdoutFile.empty()) {
        auto directory = fileio::ensure_parent_directory(options.stdoutFile);
        if (!directory.has_value()) {
            result.error = directory.error().describe();
            return result;
        }
    }
    if (!options.stderrFile.empty()) {
        auto directory = fileio::ensure_parent_directory(options.stderrFile);
        if (!directory.has_value()) {
            result.error = directory.error().describe();
            return result;
        }
    }

#if defined(_WIN32)
    std::string commandLine = quote_argument(options.executable);
    for (const std::string& argument : options.arguments) {
        commandLine.push_back(' ');
        commandLine.append(quote_argument(argument));
    }
    std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
    mutableCommand.push_back('\0');

    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);

    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE stdoutHandle = INVALID_HANDLE_VALUE;
    HANDLE stderrHandle = INVALID_HANDLE_VALUE;

    if (!options.stdoutFile.empty()) {
        stdoutHandle = CreateFileA(options.stdoutFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (stdoutHandle == INVALID_HANDLE_VALUE) {
            result.error = "the child's stdout file could not be created";
            return result;
        }
        startup.hStdOutput = stdoutHandle;
        startup.dwFlags |= STARTF_USESTDHANDLES;
    }
    if (!options.stderrFile.empty()) {
        stderrHandle = CreateFileA(options.stderrFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &attributes,
                                   CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (stderrHandle == INVALID_HANDLE_VALUE) {
            if (stdoutHandle != INVALID_HANDLE_VALUE) {
                CloseHandle(stdoutHandle);
            }
            result.error = "the child's stderr file could not be created";
            return result;
        }
        startup.hStdError = stderrHandle;
        startup.dwFlags |= STARTF_USESTDHANDLES;
    }

    PROCESS_INFORMATION information{};
    const BOOL created = CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE, 0, nullptr,
                                        options.workingDirectory.empty() ? nullptr : options.workingDirectory.c_str(),
                                        &startup, &information);
    if (stdoutHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(stdoutHandle);
    }
    if (stderrHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(stderrHandle);
    }
    if (created == 0) {
        result.error = "the child process could not be created";
        return result;
    }
    process_ = information.hProcess;
    thread_ = information.hThread;
    id_ = information.dwProcessId;
    result.started = true;
    return result;
#else
    std::vector<std::string> storage;
    storage.push_back(options.executable);
    for (const std::string& argument : options.arguments) {
        storage.push_back(argument);
    }
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (!options.stdoutFile.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, options.stdoutFile.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    if (!options.stderrFile.empty()) {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, options.stderrFile.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    }
    pid_t pid = 0;
    const int spawned = ::posix_spawn(&pid, options.executable.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawned != 0) {
        result.error = std::string{"the child process could not be created: "} + std::strerror(spawned);
        return result;
    }
    process_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(pid));
    id_ = static_cast<std::uint32_t>(pid);
    result.started = true;
    return result;
#endif
}

Result_ Child::wait() {
    Result_ result;
    if (process_ == nullptr) {
        result.error = "the child was never started";
        return result;
    }
    if (reaped_) {
        result.error = "the child was already reaped";
        return result;
    }
#if defined(_WIN32)
    const DWORD waited = WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
    if (waited != WAIT_OBJECT_0) {
        result.error = "waiting for the child failed";
        return result;
    }
    DWORD code = 0;
    if (GetExitCodeProcess(static_cast<HANDLE>(process_), &code) == 0) {
        result.error = "the child exit code could not be read";
        return result;
    }
    CloseHandle(static_cast<HANDLE>(process_));
    if (thread_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(thread_));
        thread_ = nullptr;
    }
    process_ = nullptr;
    reaped_ = true;
    result.exitCode = static_cast<int>(code);
    result.started = true;
    return result;
#else
    const auto pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            result.error = "waiting for the child failed";
            return result;
        }
    }
    process_ = nullptr;
    reaped_ = true;
    result.started = true;
    if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else {
        result.exitCode = 128 + WTERMSIG(status);
    }
    return result;
#endif
}

void Child::terminate() {
    if (process_ != nullptr && !reaped_) {
#if defined(_WIN32)
        TerminateProcess(static_cast<HANDLE>(process_), 1);
        WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);
        CloseHandle(static_cast<HANDLE>(process_));
        if (thread_ != nullptr) {
            CloseHandle(static_cast<HANDLE>(thread_));
        }
#else
        const auto pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
        ::kill(pid, SIGKILL);
        int status = 0;
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
#endif
    }
    process_ = nullptr;
    thread_ = nullptr;
    reaped_ = true;
    id_ = 0;
}

bool Child::running() const noexcept {
    if (process_ == nullptr) {
        return false;
    }
#if defined(_WIN32)
    return WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_TIMEOUT;
#else
    const auto pid = static_cast<pid_t>(reinterpret_cast<std::intptr_t>(process_));
    int status = 0;
    const pid_t waited = ::waitpid(pid, &status, WNOHANG);
    return waited == 0;
#endif
}

}  // namespace process
}  // namespace recovery
