#ifndef RECOVERY_EPOCH_HPP
#define RECOVERY_EPOCH_HPP

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include "recovery/canonical.hpp"
#include "recovery/digest.hpp"
#include "recovery/error.hpp"
#include "recovery/id.hpp"

namespace recovery {

// ---------------------------------------------------------------------------
// Authority and generation binding
// ---------------------------------------------------------------------------
// The coordinator owns no facility truth. Everything it decides is a statement
// of the form "relative to the authority I was shown". An AuthorityRef is that
// binding: a structural identity for the authority, the monotonically
// increasing generation of its content, and the digest of exactly the bytes
// the coordinator consumed.
//
// Binding rules enforced by this library:
//
//   * A generation alone is never authority. The digest must match too.
//   * A higher generation does not inherit the digest of a lower one. When the
//     generation moves, the digest must be re-established explicitly.
//   * Recovered, cached, or replayed authority is not current authority until
//     it is re-presented and accepted again.
class AuthorityRef {
public:
    AuthorityRef() = default;

    AuthorityRef(std::string authority, std::string instance, std::uint64_t generation, Digest digest);

    [[nodiscard]] const std::string& authority() const noexcept { return authority_; }
    [[nodiscard]] const std::string& instance() const noexcept { return instance_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const Digest& digest() const noexcept { return digest_; }
    [[nodiscard]] bool is_zero() const noexcept { return authority_.empty(); }

    [[nodiscard]] bool same_structure(const AuthorityRef& other) const noexcept {
        return authority_ == other.authority_ && instance_ == other.instance_;
    }

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] Digest structural_digest() const;

    [[nodiscard]] static std::optional<AuthorityRef> from_canonical(std::string_view text);

    friend bool operator==(const AuthorityRef& lhs, const AuthorityRef& rhs) noexcept {
        return lhs.authority_ == rhs.authority_ && lhs.instance_ == rhs.instance_ &&
               lhs.generation_ == rhs.generation_ && lhs.digest_ == rhs.digest_;
    }
    friend bool operator!=(const AuthorityRef& lhs, const AuthorityRef& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const AuthorityRef& lhs, const AuthorityRef& rhs) noexcept;

private:
    std::string authority_;
    std::string instance_;
    std::uint64_t generation_{0};
    Digest digest_{};
};

enum class Domain : std::uint8_t {
    Power = 1,
    Cooling = 2,
    Capacity = 3,
    Workload = 4,
    Network = 5,
    FacilityPolicy = 6,
    Safety = 7,
};

[[nodiscard]] std::string_view to_string(Domain value);
[[nodiscard]] std::optional<Domain> domain_from_string(std::string_view text);

// Reference to one adjacent authority: which domain it governs and which
// deployment instance of it the coordinator is talking to.
class AuthorityId {
public:
    AuthorityId() = default;
    AuthorityId(Domain domain, std::string instance) : domain_(domain), instance_(std::move(instance)) {}

    [[nodiscard]] Domain domain() const noexcept { return domain_; }
    [[nodiscard]] const std::string& instance() const noexcept { return instance_; }

    [[nodiscard]] std::string canonical() const;

    friend bool operator==(const AuthorityId& lhs, const AuthorityId& rhs) noexcept {
        return lhs.domain_ == rhs.domain_ && lhs.instance_ == rhs.instance_;
    }
    friend bool operator!=(const AuthorityId& lhs, const AuthorityId& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const AuthorityId& lhs, const AuthorityId& rhs) noexcept {
        if (lhs.domain_ != rhs.domain_) {
            return lhs.domain_ < rhs.domain_;
        }
        return lhs.instance_ < rhs.instance_;
    }

private:
    Domain domain_{Domain::Power};
    std::string instance_;
};

// ---------------------------------------------------------------------------
// Control epoch / incarnation
// ---------------------------------------------------------------------------
// Mutation authority is not inherited. A coordinator process that opens a
// journal claims a new epoch explicitly, and every consequential external
// request carries the epoch it was issued under. A journal records the epochs
// it has seen so that a successor can prove it did not silently adopt a
// predecessor's authority.
class Epoch {
public:
    Epoch() = default;
    explicit Epoch(std::uint64_t value) : value_(value) {}

    [[nodiscard]] std::uint64_t value() const noexcept { return value_; }
    [[nodiscard]] bool is_zero() const noexcept { return value_ == 0; }
    [[nodiscard]] std::string to_text() const { return "epoch-" + std::to_string(value_); }

    friend bool operator==(const Epoch& lhs, const Epoch& rhs) noexcept { return lhs.value_ == rhs.value_; }
    friend bool operator!=(const Epoch& lhs, const Epoch& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const Epoch& lhs, const Epoch& rhs) noexcept { return lhs.value_ < rhs.value_; }

private:
    std::uint64_t value_{0};
};

}  // namespace recovery

#endif  // RECOVERY_EPOCH_HPP
