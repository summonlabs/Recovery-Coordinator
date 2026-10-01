#ifndef RECOVERY_LOCKFILE_HPP
#define RECOVERY_LOCKFILE_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "recovery/error.hpp"
#include "recovery/time.hpp"

namespace recovery {

// Kernel-owned single-writer exclusion.
//
// The lock is an exclusive lock on a file that the kernel releases when the
// holding process dies for any reason, including an abrupt kill. That property
// is the whole point: a cooperative lock (a pid file, a marker record, a
// timestamp) can survive a dead holder and silently hand mutation authority to
// a successor. This one cannot.
//
// The lock grants mutual exclusion only. It carries no authority: a successor
// that acquires the lock is a new incarnation and must claim a new epoch
// explicitly before it may dispatch anything.
class LockFile {
public:
    // The destructor and the move operations are defined in the translation
    // unit that also defines Impl, so the incomplete-type unique_ptr is never
    // instantiated by a consumer of the header.
    LockFile();
    ~LockFile();

    LockFile(const LockFile&) = delete;
    LockFile& operator=(const LockFile&) = delete;
    LockFile(LockFile&&) noexcept;
    LockFile& operator=(LockFile&&) noexcept;

    // Attempts to take the lock without blocking. Returns a Contended error
    // when another live process holds it.
    [[nodiscard]] Result<bool> try_acquire(const std::string& path);

    // Takes the lock, waiting up to the given budget. A zero budget means
    // "try once". Returns Contended when the budget is exhausted.
    [[nodiscard]] Result<bool> acquire(const std::string& path, Duration budget, TimePoint now);

    [[nodiscard]] bool held() const noexcept;
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

    // Releases the lock. Called automatically on destruction.
    void release();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::string path_;
};

}  // namespace recovery

#endif  // RECOVERY_LOCKFILE_HPP
