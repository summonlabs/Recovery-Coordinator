#include "recovery/fileio.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>
#include <vector>

#include "fileio_internal.hpp"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace recovery {
namespace fileio {
namespace {

// Reflected Castagnoli polynomial.
constexpr std::uint32_t kCrc32cPolynomial = 0x82f63b78u;

[[nodiscard]] const std::array<std::uint32_t, 256>& crc_table() {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> result{};
        // Each entry is built by shifting the index itself, so the table is a
        // pure function of the polynomial and needs no lookup. The iterator form
        // keeps the write provably inside the array without an index expression
        // a static analyser has to bound.
        auto entry = result.begin();
        for (std::uint32_t index = 0; index < 256u; ++index, ++entry) {
            std::uint32_t value = index;
            for (int bit = 0; bit < 8; ++bit) {
                value = (value & 1u) != 0u ? (value >> 1) ^ kCrc32cPolynomial : (value >> 1);
            }
            *entry = value;
        }
        return result;
    }();
    return table;
}

[[nodiscard]] std::string system_message(const std::error_code& code) {
    return code.message();
}

// One conversion point between UTF-8 coordinator text and the host path type.
// On Windows the coordinator's UTF-8 bytes are widened by the standard
// filesystem library; on POSIX the bytes are already the path bytes.
[[nodiscard]] std::filesystem::path to_path(const std::string& text) {
#if defined(_WIN32)
    return std::filesystem::path{std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size())};
#else
    return std::filesystem::path{text};
#endif
}

}  // namespace

std::uint32_t crc32c(const void* data, std::size_t size) noexcept {
    return crc32c_extend(0xffffffffu, data, size) ^ 0xffffffffu;
}

std::uint32_t crc32c_extend(std::uint32_t seed, const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::uint32_t value = seed;
    const auto& table = crc_table();
    for (std::size_t i = 0; i < size; ++i) {
        value = table[(value ^ bytes[i]) & 0xffu] ^ (value >> 8);
    }
    return value;
}

bool exists(const std::string& path) {
    std::error_code code;
    return std::filesystem::exists(to_path(path), code) && !code;
}

std::optional<std::uint64_t> file_size(const std::string& path) {
    std::error_code code;
    const auto size = std::filesystem::file_size(to_path(path), code);
    if (code) {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(size);
}

Result<std::vector<std::uint8_t>> read_file(const std::string& path, std::uint64_t max_bytes) {
    if (path.empty()) {
        return errors::invalid_argument("fileio.path", "path is empty");
    }
    const auto size = file_size(path);
    if (!size.has_value()) {
        return errors::persistence("fileio.missing", "file does not exist or its size is unknown");
    }
    if (*size > max_bytes) {
        return errors::limit_exceeded("fileio.too_large", "file exceeds the configured read limit");
    }
    std::ifstream stream(to_path(path), std::ios::binary);
    if (!stream) {
        return errors::persistence("fileio.open", "file could not be opened for reading");
    }
    std::vector<std::uint8_t> buffer(static_cast<std::size_t>(*size));
    if (*size != 0) {
        stream.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(*size));
        if (stream.gcount() != static_cast<std::streamsize>(*size)) {
            return errors::persistence("fileio.short_read", "file was shorter than its reported size");
        }
    }
    if (!stream) {
        return errors::persistence("fileio.read", "reading the file failed");
    }
    return buffer;
}

namespace {

[[nodiscard]] Result<bool> flush_path(const std::string& path) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(to_path(path).c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return errors::persistence("fileio.open_flush", "file could not be reopened for a device flush");
    }
    const BOOL ok = FlushFileBuffers(handle);
    CloseHandle(handle);
    if (ok == 0) {
        return errors::persistence("fileio.flush", "device flush failed");
    }
    return true;
#else
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return errors::persistence("fileio.open_flush", "file could not be reopened for a device flush");
    }
    const int ok = ::fsync(descriptor);
    ::close(descriptor);
    if (ok != 0) {
        return errors::persistence("fileio.flush", "device flush failed");
    }
    return true;
#endif
}

}  // namespace

Result<bool> publish_atomically(const std::string& path, const std::vector<std::uint8_t>& content) {
    if (path.empty()) {
        return errors::invalid_argument("fileio.path", "path is empty");
    }
    auto directory = ensure_parent_directory(path);
    if (!directory.has_value()) {
        return directory.error();
    }
    const std::string staging = path + ".staging";
    {
        std::ofstream stream(to_path(staging),
                             std::ios::binary | std::ios::trunc);
        if (!stream) {
            return errors::persistence("fileio.staging_open", "staging file could not be created");
        }
        if (!content.empty()) {
            stream.write(reinterpret_cast<const char*>(content.data()), static_cast<std::streamsize>(content.size()));
        }
        stream.flush();
        if (!stream) {
            return errors::persistence("fileio.staging_write", "staging write failed");
        }
    }
    auto flushed = flush_path(staging);
    if (!flushed.has_value()) {
        std::error_code ignored;
        std::filesystem::remove(to_path(staging), ignored);
        return flushed.error();
    }

    // Read the staged bytes back and compare before anything replaces the
    // destination. A publish that cannot be verified is not a publish.
    auto readback = read_file(staging, content.size() + 1);
    if (!readback.has_value()) {
        std::error_code ignored;
        std::filesystem::remove(to_path(staging), ignored);
        return readback.error();
    }
    if (readback.value() != content) {
        std::error_code ignored;
        std::filesystem::remove(to_path(staging), ignored);
        return errors::persistence("fileio.readback_mismatch", "staged bytes did not match the intended bytes");
    }

    std::error_code code;
    std::filesystem::rename(to_path(staging),
                            to_path(path), code);
    if (code) {
        // A rename onto an existing file is not portable; fall back to a
        // remove-then-rename, accepting that a crash in this window leaves the
        // destination missing rather than partial. A missing journal is
        // detected on open; a partial one is refused.
        std::error_code removeCode;
        std::filesystem::remove(to_path(path), removeCode);
        std::error_code retryCode;
        std::filesystem::rename(to_path(staging),
                                to_path(path), retryCode);
        if (retryCode) {
            return errors::persistence("fileio.publish", "staged file could not be published: " + retryCode.message());
        }
    }
    auto published = flush_path(path);
    if (!published.has_value()) {
        return published.error();
    }
    return true;
}

Result<bool> remove_file(const std::string& path) {
    std::error_code code;
    const bool removed = std::filesystem::remove(to_path(path), code);
    if (code) {
        return errors::persistence("fileio.remove", "file could not be removed: " + code.message());
    }
    (void)removed;
    return true;
}

Result<bool> truncate_file(const std::string& path, std::uint64_t length) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(to_path(path).c_str(), GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return errors::persistence("fileio.truncate_open", "file could not be opened for truncation");
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(length);
    BOOL ok = SetFilePointerEx(handle, position, nullptr, FILE_BEGIN);
    if (ok != 0) {
        ok = SetEndOfFile(handle);
    }
    if (ok != 0) {
        ok = FlushFileBuffers(handle);
    }
    CloseHandle(handle);
    if (ok == 0) {
        return errors::persistence("fileio.truncate", "file could not be truncated");
    }
    return true;
#else
    if (::truncate(path.c_str(), static_cast<off_t>(length)) != 0) {
        return errors::persistence("fileio.truncate", "file could not be truncated");
    }
    return flush_path(path);
#endif
}

Result<bool> ensure_parent_directory(const std::string& path) {
    std::error_code code;
    const std::filesystem::path target{to_path(path)};
    const std::filesystem::path parent = target.parent_path();
    if (parent.empty()) {
        return true;
    }
    if (std::filesystem::exists(parent, code)) {
        if (code) {
            return errors::persistence("fileio.stat", "parent directory could not be inspected: " + code.message());
        }
        return true;
    }
    std::filesystem::create_directories(parent, code);
    if (code) {
        return errors::persistence("fileio.mkdir", "parent directory could not be created: " + code.message());
    }
    return true;
}

std::string absolute_path(const std::string& path) {
    std::error_code code;
    const auto abs = std::filesystem::absolute(to_path(path), code);
    if (code) {
        return path;
    }
    return abs.lexically_normal().string();
}

std::uint64_t monotonic_nanos() noexcept {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

void sleep_millis(std::uint64_t millis) {
    std::this_thread::sleep_for(std::chrono::milliseconds(millis));
}

Result<void*> open_lock_handle(const std::string& path) {
#if defined(_WIN32)
    const HANDLE handle = CreateFileW(to_path(path).c_str(),
                                      GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return errors::persistence("lock.open", "lock file could not be opened");
    }
    return static_cast<void*>(handle);
#else
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return errors::persistence("lock.open", "lock file could not be opened");
    }
    return reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor) + 1);
#endif
}

Result<AttemptResult> try_lock_exclusive(void* handle) {
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    const BOOL ok = LockFileEx(static_cast<HANDLE>(handle), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                               &overlapped);
    if (ok != 0) {
        return AttemptResult::Acquired;
    }
    const DWORD lastError = GetLastError();
    if (lastError == ERROR_LOCK_VIOLATION || lastError == ERROR_IO_PENDING) {
        return AttemptResult::Contended;
    }
    return errors::persistence("lock.acquire", "the single-writer lock could not be taken");
#else
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1);
    struct flock operation {};
    operation.l_type = F_WRLCK;
    operation.l_whence = SEEK_SET;
    operation.l_start = 0;
    operation.l_len = 0;
    if (::fcntl(descriptor, F_SETLK, &operation) == 0) {
        return AttemptResult::Acquired;
    }
    return AttemptResult::Contended;
#endif
}

void unlock_and_close(void* handle) {
#if defined(_WIN32)
    OVERLAPPED overlapped{};
    UnlockFileEx(static_cast<HANDLE>(handle), 0, 1, 0, &overlapped);
    CloseHandle(static_cast<HANDLE>(handle));
#else
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1);
    struct flock operation {};
    operation.l_type = F_UNLCK;
    operation.l_whence = SEEK_SET;
    ::fcntl(descriptor, F_SETLK, &operation);
    ::close(descriptor);
#endif
}

void close_handle(void* handle) {
    if (handle == nullptr) {
        return;
    }
#if defined(_WIN32)
    CloseHandle(static_cast<HANDLE>(handle));
#else
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1);
    ::close(descriptor);
#endif
}

Result<bool> flush_descriptor(int descriptor) {
#if defined(_WIN32)
    const intptr_t raw = _get_osfhandle(descriptor);
    if (raw == -1) {
        return errors::persistence("fileio.handle", "no operating system handle for the descriptor");
    }
    if (FlushFileBuffers(reinterpret_cast<HANDLE>(raw)) == 0) {
        return errors::persistence("fileio.flush", "device flush failed");
    }
    return true;
#else
    if (::fsync(descriptor) != 0) {
        return errors::persistence("fileio.flush", "device flush failed");
    }
    return true;
#endif
}

Result<bool> flush_handle(void* handle) {
#if defined(_WIN32)
    if (handle == nullptr) {
        return errors::invalid_argument("fileio.handle", "handle is null");
    }
    if (FlushFileBuffers(static_cast<HANDLE>(handle)) == 0) {
        return errors::persistence("fileio.flush", "device flush failed");
    }
    return true;
#else
    if (handle == nullptr) {
        return errors::invalid_argument("fileio.handle", "handle is null");
    }
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle) - 1);
    return flush_descriptor(descriptor);
#endif
}


// ---------------------------------------------------------------------------
// Append handle
// ---------------------------------------------------------------------------

AppendHandle::~AppendHandle() { close(); }

AppendHandle::AppendHandle(AppendHandle&& other) noexcept : handle_(other.handle_), size_(other.size_) {
    other.handle_ = nullptr;
    other.size_ = 0;
}

AppendHandle& AppendHandle::operator=(AppendHandle&& other) noexcept {
    if (this != &other) {
        close();
        handle_ = other.handle_;
        size_ = other.size_;
        other.handle_ = nullptr;
        other.size_ = 0;
    }
    return *this;
}

Result<bool> AppendHandle::open(const std::string& path, bool readWrite) {
    close();
    if (path.empty()) {
        return errors::invalid_argument("fileio.path", "path is empty");
    }
    auto directory = ensure_parent_directory(path);
    if (!directory.has_value()) {
        return directory.error();
    }
#if defined(_WIN32)
    const DWORD access = readWrite ? (GENERIC_READ | GENERIC_WRITE) : GENERIC_WRITE;
    (void)readWrite;
    const HANDLE handle = CreateFileW(to_path(path).c_str(), access, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return errors::persistence("fileio.open_append", "journal file could not be opened for appending");
    }
    LARGE_INTEGER size{};
    if (GetFileSizeEx(handle, &size) == 0) {
        CloseHandle(handle);
        return errors::persistence("fileio.size", "journal file size could not be read");
    }
    LARGE_INTEGER end{};
    end.QuadPart = size.QuadPart;
    if (SetFilePointerEx(handle, end, nullptr, FILE_BEGIN) == 0) {
        CloseHandle(handle);
        return errors::persistence("fileio.seek", "journal file could not be positioned at its end");
    }
    (void)readWrite;
    handle_ = handle;
    size_ = static_cast<std::uint64_t>(size.QuadPart);
    return true;
#else
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return errors::persistence("fileio.open_append", "journal file could not be opened for appending");
    }
    handle_ = reinterpret_cast<void*>(static_cast<std::intptr_t>(descriptor) + 1);
    const auto size = file_size(path);
    size_ = size.has_value() ? *size : 0;
    return true;
#endif
}

Result<bool> AppendHandle::append_and_flush(const std::vector<std::uint8_t>& bytes) {
    if (handle_ == nullptr) {
        return errors::precondition_failed("fileio.closed", "the append handle is not open");
    }
    if (bytes.empty()) {
        return true;
    }
#if defined(_WIN32)
    DWORD written = 0;
    if (WriteFile(static_cast<HANDLE>(handle_), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) == 0) {
        return errors::persistence("fileio.write", "journal append failed");
    }
    if (written != bytes.size()) {
        return errors::persistence("fileio.short_write", "journal append wrote fewer bytes than requested");
    }
    if (FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
        return errors::persistence("fileio.flush", "journal device flush failed");
    }
#else
    const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_) - 1);
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const ssize_t written = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written <= 0) {
            return errors::persistence("fileio.write", "journal append failed");
        }
        offset += static_cast<std::size_t>(written);
    }
    if (::fsync(descriptor) != 0) {
        return errors::persistence("fileio.flush", "journal device flush failed");
    }
#endif
    size_ += bytes.size();
    return true;
}

void AppendHandle::close() {
#if defined(_WIN32)
    if (handle_ != nullptr) {
        CloseHandle(static_cast<HANDLE>(handle_));
        handle_ = nullptr;
    }
#else
    if (handle_ != nullptr) {
        const int descriptor = static_cast<int>(reinterpret_cast<std::intptr_t>(handle_) - 1);
        ::close(descriptor);
        handle_ = nullptr;
    }
#endif
    size_ = 0;
}

}  // namespace fileio
}  // namespace recovery