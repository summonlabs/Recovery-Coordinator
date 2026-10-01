#ifndef RECOVERY_AUTHORITY_HPP
#define RECOVERY_AUTHORITY_HPP

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "recovery/epoch.hpp"
#include "recovery/error.hpp"
#include "recovery/id.hpp"
#include "recovery/observation.hpp"
#include "recovery/time.hpp"

namespace recovery {

class Plan;

// ---------------------------------------------------------------------------
// Authority ledger
// ---------------------------------------------------------------------------
// The ledger answers exactly one question: "relative to the authority the
// coordinator has explicitly accepted, is this reference still current?"
//
// It never answers a question about the world. It holds no facility state, no
// cached reading, and no health judgement. Every entry is a reference the
// coordinator was handed and chose to accept.
class AuthorityLedger {
public:
    struct Entry {
        AuthorityRef reference{};
        TimePoint acceptedAt{};
        TimePoint retiredAt{};
        bool retired{false};
        Domain domain{Domain::Power};
    };

    // Accepts a reference as current for its domain. A reference whose
    // generation is lower than the currently accepted generation is refused:
    // authority never moves backwards. A reference with the same generation but
    // a different digest is refused as a conflict rather than silently
    // replacing the accepted content.
    [[nodiscard]] Result<AuthorityRef> accept(Domain domain, const AuthorityRef& reference, TimePoint now);

    // Marks the accepted reference for a domain as retired. A retired reference
    // is not current, and no attempt bound to it may be dispatched. The
    // reference itself is retained so that the coordinator can still explain
    // what it used to be bound to.
    [[nodiscard]] Result<AuthorityRef> retire(Domain domain, TimePoint now, std::string_view reason);

    [[nodiscard]] bool is_current(const AuthorityRef& reference, Domain domain) const;
    [[nodiscard]] std::optional<AuthorityRef> current(Domain domain) const;
    [[nodiscard]] const std::map<Domain, Entry>& entries() const noexcept { return entries_; }
    [[nodiscard]] std::size_t accepted_count() const noexcept { return acceptedCount_; }
    [[nodiscard]] std::size_t retired_count() const noexcept { return retiredCount_; }

    [[nodiscard]] std::string canonical() const;
    void clear();

private:
    std::map<Domain, Entry> entries_{};
    std::size_t acceptedCount_{0};
    std::size_t retiredCount_{0};
};

// Result of comparing a plan's bindings against the ledger.
struct AuthorityDelta {
    bool current{false};
    std::vector<Domain> changed{};
    std::vector<std::string> reasons{};

    [[nodiscard]] std::string summary() const;
};

// Pure comparison: no state is modified. A plan is current only when every
// domain it requires is accepted and byte-identical to the requirement.
[[nodiscard]] AuthorityDelta compare_authority(const Plan& plan, const AuthorityLedger& ledger);

}  // namespace recovery

#endif  // RECOVERY_AUTHORITY_HPP
