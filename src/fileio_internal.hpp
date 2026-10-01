#ifndef RECOVERY_FILEIO_INTERNAL_HPP
#define RECOVERY_FILEIO_INTERNAL_HPP

// Platform plumbing for the durability layer. Not installed, not part of the
// public surface. Everything here is deliberately synchronous: the journal
// makes a durability claim, and a durability claim not backed by a completed
// device flush would not be one the coordinator is allowed to make.

#include <cstdint>
#include <string>
#include <vector>

#include "recovery/error.hpp"

namespace recovery {
namespace fileio {

// Result of a non-blocking attempt to take an exclusive lock.
enum class AttemptResult {
    Acquired,
    Contended,
};

[[nodiscard]] Result<void*> open_lock_handle(const std::string& path);
[[nodiscard]] Result<AttemptResult> try_lock_exclusive(void* handle);
void unlock_and_close(void* handle);
void close_handle(void* handle);

// Append-only file handle used by the journal. Each append writes the whole
// byte range and then flushes it to the device before returning.
class AppendHandle {
public:
    AppendHandle() = default;
    ~AppendHandle();

    AppendHandle(const AppendHandle&) = delete;
    AppendHandle& operator=(const AppendHandle&) = delete;
    AppendHandle(AppendHandle&& other) noexcept;
    AppendHandle& operator=(AppendHandle&& other) noexcept;

    [[nodiscard]] Result<bool> open(const std::string& path, bool readWrite);
    [[nodiscard]] Result<bool> append_and_flush(const std::vector<std::uint8_t>& bytes);

    // Constructs a record whose checksum field is a fixed point: the checksum is
    // computed with the field zeroed and then patched into place, so a verifier
    // that repeats the computation over the bytes it read obtains the same
    // value. A plain write-then-hash would make the checksum cover a field that
    // changes after the hash is taken.
    [[nodiscard]] Result<bool> append_with_checksum_patch(std::vector<std::uint8_t>& bytes, std::size_t checksumOffset,
                                                          std::size_t checksumRange);
    [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
    [[nodiscard]] bool is_open() const noexcept { return handle_ != nullptr; }
    void close();

private:
    void* handle_{nullptr};
    std::uint64_t size_{0};
};

// Monotone nanoseconds from a source that never jumps backwards. Used only for
// bounded waits; it never enters durable state.
[[nodiscard]] std::uint64_t monotonic_nanos() noexcept;

void sleep_millis(std::uint64_t millis);

// Flush a descriptor or a handle to the device.
[[nodiscard]] Result<bool> flush_descriptor(int descriptor);
[[nodiscard]] Result<bool> flush_handle(void* handle);

}  // namespace fileio
}  // namespace recovery

#endif  // RECOVERY_FILEIO_INTERNAL_HPP