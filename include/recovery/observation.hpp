#ifndef RECOVERY_OBSERVATION_HPP
#define RECOVERY_OBSERVATION_HPP

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "recovery/canonical.hpp"
#include "recovery/digest.hpp"
#include "recovery/epoch.hpp"
#include "recovery/error.hpp"
#include "recovery/id.hpp"
#include "recovery/time.hpp"

namespace recovery {

// ---------------------------------------------------------------------------
// Readings
// ---------------------------------------------------------------------------
// A reading is a value the coordinator was shown about a domain, together with
// the origin that produced it. Values are strongly typed and totally ordered so
// that "the same reading" has exactly one representation: a comparison never
// depends on formatting, spelling, or a locale.
enum class ReadingKind : std::uint8_t {
    Flag = 1,
    Count = 2,
    BasisPoints = 3,
    Text = 4,
};

[[nodiscard]] std::string_view to_string(ReadingKind value);

class Reading {
public:
    Reading() = default;

    [[nodiscard]] static Reading flag(bool value);
    [[nodiscard]] static Reading count(std::uint64_t value);
    // Ratio in hundredths of a percent: 10000 == 100.00%.
    [[nodiscard]] static Reading basis_points(std::uint32_t value);
    [[nodiscard]] static Reading text(std::string value);

    [[nodiscard]] ReadingKind kind() const noexcept { return kind_; }
    [[nodiscard]] bool flag_value() const noexcept { return flag_; }
    [[nodiscard]] std::uint64_t count_value() const noexcept { return count_; }
    [[nodiscard]] std::uint32_t basis_points_value() const noexcept { return basisPoints_; }
    [[nodiscard]] const std::string& text_value() const noexcept { return text_; }

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] static std::optional<Reading> from_canonical(std::string_view text);

    friend bool operator==(const Reading& lhs, const Reading& rhs) noexcept;
    friend bool operator!=(const Reading& lhs, const Reading& rhs) noexcept { return !(lhs == rhs); }
    friend bool operator<(const Reading& lhs, const Reading& rhs) noexcept;

private:
    ReadingKind kind_{ReadingKind::Flag};
    bool flag_{false};
    std::uint64_t count_{0};
    std::uint32_t basisPoints_{0};
    std::string text_;
};

// Where an observation came from. Provenance is part of the reading's identity
// and is never inferred from the value itself.
enum class ObservationSource : std::uint8_t {
    // Reported by an adjacent authority adapter.
    Adapter = 1,
    // Presented by an operator or a control plane outside the adapter set.
    Operator = 2,
    // Produced by the coordinator itself (for example, a compensation result).
    Coordinator = 3,
    // Presented by the durable journal during replay. Never accepted as fresh
    // evidence; only used to rebuild state.
    Replay = 4,
};

[[nodiscard]] std::string_view to_string(ObservationSource value);
[[nodiscard]] std::optional<ObservationSource> observation_source_from_string(std::string_view text);

class Observation {
public:
    Observation() = default;

    Observation(EvidenceId id, Domain domain, std::uint64_t generation, ObservationSource source, std::string origin,
                std::string key, Reading reading, TimePoint observedAt, AuthorityRef authority, bool stale);

    [[nodiscard]] const EvidenceId& id() const noexcept { return id_; }
    [[nodiscard]] Domain domain() const noexcept { return domain_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] ObservationSource source() const noexcept { return source_; }
    [[nodiscard]] const std::string& origin() const noexcept { return origin_; }
    [[nodiscard]] const std::string& key() const noexcept { return key_; }
    [[nodiscard]] const Reading& reading() const noexcept { return reading_; }
    [[nodiscard]] TimePoint observed_at() const noexcept { return observedAt_; }
    [[nodiscard]] const AuthorityRef& authority() const noexcept { return authority_; }
    [[nodiscard]] bool stale() const noexcept { return stale_; }

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] Digest digest() const;

    // Total order used by every canonical rendering and every deterministic
    // fold over observations: domain, then key, then generation, then origin,
    // then identity. Never insertion order, never container order.
    friend bool operator<(const Observation& lhs, const Observation& rhs) noexcept;
    friend bool operator==(const Observation& lhs, const Observation& rhs) noexcept;

private:
    EvidenceId id_{};
    Domain domain_{Domain::Power};
    std::uint64_t generation_{0};
    ObservationSource source_{ObservationSource::Adapter};
    std::string origin_;
    std::string key_;
    Reading reading_{};
    TimePoint observedAt_{};
    AuthorityRef authority_{};
    bool stale_{false};
};

// The coordinator's current, explicitly accepted view of one domain.
class EvidenceView {
public:
    EvidenceView() = default;

    [[nodiscard]] bool established() const noexcept { return established_; }
    [[nodiscard]] const AuthorityRef& authority() const noexcept { return authority_; }
    [[nodiscard]] TimePoint observed_at() const noexcept { return observedAt_; }
    [[nodiscard]] const std::map<std::string, Reading>& readings() const noexcept { return readings_; }

    [[nodiscard]] std::optional<Reading> find(std::string_view key) const;

    // A view is current when it was established, has not been fenced, and is
    // not older than the caller's freshness bound at the caller's "now".
    [[nodiscard]] bool is_current(TimePoint now, Duration freshness) const;

    [[nodiscard]] std::string canonical() const;

private:
    friend class ObservationStore;

    bool established_{false};
    AuthorityRef authority_{};
    TimePoint observedAt_{};
    std::map<std::string, Reading> readings_{};
};

struct ObservationAcceptance {
    EvidenceId id{};
    bool accepted{false};
    // True when the exact same evidence (id and digest) was already accepted.
    bool replayed{false};
    std::string reason;
    Digest digest{};
};

// Bounded, in-memory evidence store. It is authoritative only for the current
// coordinator incarnation: durability of evidence is owned by the journal,
// which re-presents accepted observations on replay.
class ObservationStore {
public:
    explicit ObservationStore(std::size_t max_per_domain = 4096) : maxPerDomain_(max_per_domain) {}

    // Applies one observation. An observation whose id was already accepted
    // with the same digest is an idempotent replay and does not move the view.
    // An observation with the same id and a different digest is refused.
    [[nodiscard]] ObservationAcceptance accept(const Observation& observation);

    [[nodiscard]] const EvidenceView& view(Domain domain) const;
    [[nodiscard]] std::size_t observation_count(Domain domain) const;
    [[nodiscard]] std::size_t total_observations() const noexcept { return total_; }
    [[nodiscard]] std::size_t observation_count() const noexcept { return total_; }
    // Every observation this store holds for one domain, in the total order
    // defined by Observation. The internal per-domain state is deliberately
    // not exposed.
    [[nodiscard]] std::vector<Observation> observations_for(Domain domain) const;
    [[nodiscard]] std::vector<Domain> domains() const;

    // Fences every view: no view is current afterwards until fresh evidence is
    // presented. Used on restart and on an explicit authority reset.
    void fence_all();

    void clear();

private:
    struct DomainState {
        EvidenceView view{};
        std::vector<Observation> observations{};
        std::map<EvidenceId, Digest> seen{};
        bool fenced{false};
    };

    void rebuild_view(Domain domain);

    std::size_t maxPerDomain_{4096};
    std::size_t total_{0};
    std::map<Domain, DomainState> observations_{};
};

}  // namespace recovery

#endif  // RECOVERY_OBSERVATION_HPP