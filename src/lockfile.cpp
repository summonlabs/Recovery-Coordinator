#include "recovery/lockfile.hpp"

#include "recovery/fileio.hpp"

#include "fileio_internal.hpp"

namespace recovery {

struct LockFile::Impl {
    void* handle{nullptr};
};

LockFile::LockFile() = default;

LockFile::~LockFile() { release(); }

LockFile::LockFile(LockFile&& other) noexcept : impl_(std::move(other.impl_)), path_(std::move(other.path_)) {}

LockFile& LockFile::operator=(LockFile&& other) noexcept {
    if (this != &other) {
        release();
        impl_ = std::move(other.impl_);
        path_ = std::move(other.path_);
    }
    return *this;
}

Result<bool> LockFile::try_acquire(const std::string& path) {
    if (held()) {
        return true;
    }
    if (path.empty()) {
        return errors::invalid_argument("lock.path", "lock path is empty");
    }
    auto directory = fileio::ensure_parent_directory(path);
    if (!directory.has_value()) {
        return directory.error();
    }
    auto handle = fileio::open_lock_handle(path);
    if (!handle.has_value()) {
        return handle.error();
    }
    auto locked = fileio::try_lock_exclusive(handle.value());
    if (!locked.has_value()) {
        fileio::close_handle(handle.value());
        return locked.error();
    }
    if (locked.value() == fileio::AttemptResult::Contended) {
        fileio::close_handle(handle.value());
        return errors::contended("lock.contended", "another process holds the single-writer lock");
    }

    impl_ = std::make_unique<Impl>();
    impl_->handle = handle.value();
    path_ = path;
    return true;
}

Result<bool> LockFile::acquire(const std::string& path, Duration budget, TimePoint now) {
    (void)now;
    const NanoCount limit = fileio::monotonic_nanos() + budget.nanos();
    bool reportedContention = false;
    while (true) {
        auto attempt = try_acquire(path);
        if (attempt.has_value()) {
            return attempt;
        }
        if (attempt.error().cls() != ErrorClass::Contended) {
            return attempt;
        }
        reportedContention = true;
        if (budget.is_zero()) {
            return errors::contended("lock.contended", "another process holds the single-writer lock");
        }
        if (fileio::monotonic_nanos() >= limit) {
            return errors::contended("lock.timeout",
                                     "the single-writer lock was not released within the configured budget");
        }
        fileio::sleep_millis(2);
    }
    (void)reportedContention;
}

bool LockFile::held() const noexcept { return impl_ != nullptr && impl_->handle != nullptr; }

void LockFile::release() {
    if (impl_ != nullptr && impl_->handle != nullptr) {
        fileio::unlock_and_close(impl_->handle);
        impl_->handle = nullptr;
    }
    impl_.reset();
    path_.clear();
}

}  // namespace recovery
