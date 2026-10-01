#ifndef RECOVERY_ID_HPP
#define RECOVERY_ID_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>

namespace recovery {

// ---------------------------------------------------------------------------
// Identity kinds
// ---------------------------------------------------------------------------
// Every identity in this library is a 128 bit value minted from a monotone
// engine counter, never from a clock, a host name, a random device, or a
// pointer. Deterministic identity is what makes replay, idempotent redispatch,
// and golden canonical encodings possible.
enum class IdKind : std::uint8_t {
    Plan = 1,
    Step = 2,
    Attempt = 3,
    Request = 4,
    Evidence = 5,
    Event = 6,
    Execution = 7,
};

[[nodiscard]] std::string_view to_string(IdKind value);

class Id {
public:
    static constexpr std::size_t kBytes = 16;

    Id() = default;
    explicit Id(const std::array<std::uint8_t, kBytes>& bytes) : bytes_(bytes) {}

    [[nodiscard]] const std::array<std::uint8_t, kBytes>& bytes() const noexcept { return bytes_; }
    [[nodiscard]] bool is_zero() const noexcept;
    [[nodiscard]] std::string hex() const;

    // Prefix used by all text renderings, e.g. "plan".
    [[nodiscard]] static std::string_view prefix(IdKind kind);
    [[nodiscard]] static std::optional<IdKind> kind_from_prefix(std::string_view prefix);

    [[nodiscard]] static std::optional<Id> parse(IdKind kind, std::string_view text);
    [[nodiscard]] static std::optional<Id> parse_hex(std::string_view text);

    [[nodiscard]] std::string to_text(IdKind kind) const;

    friend bool operator==(const Id& lhs, const Id& rhs) noexcept { return lhs.bytes_ == rhs.bytes_; }
    friend bool operator!=(const Id& lhs, const Id& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const Id& lhs, const Id& rhs) noexcept { return lhs.bytes_ < rhs.bytes_; }

private:
    std::array<std::uint8_t, kBytes> bytes_{};
};

// Strongly typed wrappers. The compiler refuses to mix a PlanId with a StepId.
template <IdKind Kind, typename Tag>
class TypedId {
public:
    TypedId() = default;
    explicit TypedId(Id value) : value_(value) {}

    [[nodiscard]] const Id& id() const noexcept { return value_; }
    [[nodiscard]] std::string hex() const { return value_.hex(); }
    [[nodiscard]] std::string to_text() const { return value_.to_text(Kind); }
    [[nodiscard]] bool is_zero() const noexcept { return value_.is_zero(); }
    [[nodiscard]] static constexpr IdKind kind() noexcept { return Kind; }

    [[nodiscard]] static std::optional<TypedId> parse(std::string_view text) {
        const auto parsed = Id::parse(Kind, text);
        if (!parsed.has_value()) {
            return std::nullopt;
        }
        return TypedId{*parsed};
    }

    friend bool operator==(const TypedId& lhs, const TypedId& rhs) noexcept { return lhs.value_ == rhs.value_; }
    friend bool operator!=(const TypedId& lhs, const TypedId& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const TypedId& lhs, const TypedId& rhs) noexcept { return lhs.value_ < rhs.value_; }

private:
    Id value_{};
};

struct PlanTag;
struct StepTag;
struct AttemptTag;
struct RequestTag;
struct EvidenceTag;
struct EventTag;
struct ExecutionTag;

using PlanId = TypedId<IdKind::Plan, PlanTag>;
using StepId = TypedId<IdKind::Step, StepTag>;
using AttemptId = TypedId<IdKind::Attempt, AttemptTag>;
using RequestId = TypedId<IdKind::Request, RequestTag>;
using EvidenceId = TypedId<IdKind::Evidence, EvidenceTag>;
using EventId = TypedId<IdKind::Event, EventTag>;
using ExecutionId = TypedId<IdKind::Execution, ExecutionTag>;

// Monotone, deterministic identity allocator. One allocator belongs to one
// coordinator incarnation; the counter itself is part of the durable journal
// so that a restarted coordinator never reuses an identity.
class IdAllocator {
public:
    IdAllocator() = default;
    explicit IdAllocator(std::uint64_t next);

    [[nodiscard]] std::uint64_t next_counter() const noexcept { return next_; }

    // Returns nullopt when the counter would overflow. The caller must refuse
    // the operation rather than wrap.
    [[nodiscard]] std::optional<Id> allocate(IdKind kind);

    void observe(std::uint64_t counter);

private:
    std::uint64_t next_{1};
};

// Deterministic 64 bit value derived from an identity. Used to seed per-plan
// deterministic behaviour (for example synthetic adapter scripts) without ever
// consulting a random device.
[[nodiscard]] std::uint64_t derive_seed(const Id& value, std::string_view purpose);

}  // namespace recovery

#endif  // RECOVERY_ID_HPP