#include "test.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "recovery/authority.hpp"
#include "recovery/plan.hpp"

using namespace recovery;

// ---------------------------------------------------------------------------
// Authority ledger
// ---------------------------------------------------------------------------
// The ledger holds references the coordinator was handed and chose to accept.
// These tests assert the binding rules the header documents: authority never
// moves backwards, a generation move never inherits content, a retired
// reference stays visible but is not current, and comparison never mutates.

namespace {

[[nodiscard]] AuthorityRef make_reference(Domain domain, std::uint64_t generation, std::string content) {
    return AuthorityRef{std::string{to_string(domain)}, "instance-1", generation, Digest::of(content)};
}

[[nodiscard]] PlanId mint_plan_id() {
    IdAllocator allocator{1};
    auto id = allocator.allocate(IdKind::Plan);
    RC_REQUIRE_MSG(id.has_value(), "the deterministic identity allocator refused to mint a plan identity");
    return PlanId{*id};
}

[[nodiscard]] Plan plan_requiring(const std::map<Domain, AuthorityRef>& requirements) {
    Plan plan{mint_plan_id(), "authority-binding", "test-scope", PlanStrategy::Staged, 1, TimePoint{}};
    for (const auto& [domain, reference] : requirements) {
        plan.require_authority(domain, reference);
    }
    return plan;
}

}  // namespace

RC_TEST(authority_ledger_refuses_a_generation_regression) {
    AuthorityLedger ledger;
    const AuthorityRef newer = make_reference(Domain::Power, 2, "content-2");
    const AuthorityRef older = make_reference(Domain::Power, 1, "content-1");

    RC_REQUIRE_OK(ledger.accept(Domain::Power, newer, TimePoint{10}));

    const Error error =
        RC_REQUIRE_ERR(ledger.accept(Domain::Power, older, TimePoint{20}), ErrorClass::StaleAuthority);
    RC_REQUIRE_EQ(error.code(), std::string{"authority.generation_regression"});

    const auto current = ledger.current(Domain::Power);
    RC_REQUIRE_MSG(current.has_value(), "the accepted generation must survive a refused regression");
    RC_REQUIRE_MSG(*current == newer, "a refused regression must not replace the accepted reference");
}

RC_TEST(authority_ledger_refuses_a_digest_change_within_one_generation) {
    AuthorityLedger ledger;
    const AuthorityRef accepted = make_reference(Domain::Cooling, 4, "content-a");
    const AuthorityRef conflicting = make_reference(Domain::Cooling, 4, "content-b");

    RC_REQUIRE_OK(ledger.accept(Domain::Cooling, accepted, TimePoint{10}));
    RC_REQUIRE_MSG(accepted.digest() != conflicting.digest(),
                   "the test needs two different digests at the same generation");

    const Error error =
        RC_REQUIRE_ERR(ledger.accept(Domain::Cooling, conflicting, TimePoint{20}), ErrorClass::IdentityConflict);
    RC_REQUIRE_EQ(error.code(), std::string{"authority.digest_conflict"});

    const auto current = ledger.current(Domain::Cooling);
    RC_REQUIRE_MSG(current.has_value() && *current == accepted,
                   "a same-generation conflict must leave the accepted content untouched");
    RC_REQUIRE_EQ(ledger.accepted_count(), std::size_t{1});
}

RC_TEST(authority_ledger_accepts_a_strictly_newer_generation) {
    AuthorityLedger ledger;
    const AuthorityRef first = make_reference(Domain::Power, 7, "content-7");
    const AuthorityRef second = make_reference(Domain::Power, 8, "content-8");

    RC_REQUIRE_OK(ledger.accept(Domain::Power, first, TimePoint{10}));
    RC_REQUIRE_OK(ledger.accept(Domain::Power, second, TimePoint{20}));

    const auto current = ledger.current(Domain::Power);
    RC_REQUIRE_MSG(current.has_value(), "a strictly newer generation becomes the current reference");
    RC_REQUIRE_MSG(*current == second, "the current reference must be the newest accepted generation");
    RC_REQUIRE_MSG(ledger.is_current(second, Domain::Power), "the newest reference is current");
    RC_REQUIRE_MSG(!ledger.is_current(first, Domain::Power), "the superseded reference is no longer current");
    RC_REQUIRE_EQ(ledger.accepted_count(), std::size_t{2});

    // Re-accepting exactly the content that is already current is idempotent and
    // must not inflate the acceptance count.
    RC_REQUIRE_OK(ledger.accept(Domain::Power, second, TimePoint{30}));
    RC_REQUIRE_EQ(ledger.accepted_count(), std::size_t{2});
}

RC_TEST(authority_ledger_keeps_a_retired_reference_visible_but_not_current) {
    AuthorityLedger ledger;
    const AuthorityRef reference = make_reference(Domain::Capacity, 3, "content-3");
    RC_REQUIRE_OK(ledger.accept(Domain::Capacity, reference, TimePoint{10}));

    const AuthorityRef retired = RC_REQUIRE_OK(ledger.retire(Domain::Capacity, TimePoint{20}, "operator"));
    RC_REQUIRE_MSG(retired == reference, "retiring returns the reference that was retired");

    RC_REQUIRE_MSG(!ledger.current(Domain::Capacity).has_value(),
                   "a retired reference is not the current reference");
    RC_REQUIRE_MSG(!ledger.is_current(reference, Domain::Capacity), "a retired reference is not current");

    const auto entry = ledger.entries().find(Domain::Capacity);
    RC_REQUIRE_MSG(entry != ledger.entries().end(), "a retired entry remains visible in the ledger");
    RC_REQUIRE_MSG(entry->second.retired, "the entry records that it was retired");
    RC_REQUIRE_MSG(entry->second.reference == reference,
                   "the retired entry still names the reference it used to be bound to");
    RC_REQUIRE_EQ(ledger.retired_count(), std::size_t{1});

    // Retiring twice is idempotent: the second call is not an error and does not
    // count a second retirement.
    const AuthorityRef again = RC_REQUIRE_OK(ledger.retire(Domain::Capacity, TimePoint{30}, "operator"));
    RC_REQUIRE_MSG(again == reference, "retiring an already retired domain reports the same reference");
    RC_REQUIRE_EQ(ledger.retired_count(), std::size_t{1});
}

RC_TEST(authority_ledger_refuses_to_retire_authority_that_was_never_accepted) {
    AuthorityLedger ledger;
    const Error error = RC_REQUIRE_ERR(ledger.retire(Domain::Network, TimePoint{10}, "operator"),
                                       ErrorClass::NotFound);
    RC_REQUIRE_EQ(error.code(), std::string{"authority.not_accepted"});
    RC_REQUIRE_MSG(!ledger.current(Domain::Network).has_value(),
                   "retiring an unknown domain must not create an entry");
    RC_REQUIRE_EQ(ledger.entries().size(), std::size_t{0});

    // The same call succeeds once the domain really has an accepted reference.
    const AuthorityRef reference = make_reference(Domain::Network, 1, "content-1");
    RC_REQUIRE_OK(ledger.accept(Domain::Network, reference, TimePoint{20}));
    RC_REQUIRE_OK(ledger.retire(Domain::Network, TimePoint{30}, "operator"));
}

RC_TEST(authority_ledger_reports_an_unknown_domain_as_not_current) {
    AuthorityLedger ledger;
    RC_REQUIRE_MSG(!ledger.is_current(make_reference(Domain::Safety, 1, "content"), Domain::Safety),
                   "a reference the ledger never accepted is not current");
    RC_REQUIRE_MSG(!ledger.current(Domain::Safety).has_value(),
                   "an unknown domain has no current reference");
    RC_REQUIRE_EQ(ledger.accepted_count(), std::size_t{0});
}

RC_TEST(compare_authority_reports_every_changed_domain) {
    const AuthorityRef power = make_reference(Domain::Power, 2, "power-content");
    const AuthorityRef cooling = make_reference(Domain::Cooling, 2, "cooling-content");
    const AuthorityRef network = make_reference(Domain::Network, 5, "network-content");

    Plan plan = plan_requiring({{Domain::Power, power}, {Domain::Cooling, cooling}, {Domain::Network, network}});

    AuthorityLedger ledger;
    // Power moved to a new generation with new content, Cooling was never
    // accepted, Network matches exactly.
    RC_REQUIRE_OK(ledger.accept(Domain::Power, make_reference(Domain::Power, 3, "power-content-v2"), TimePoint{10}));
    RC_REQUIRE_OK(ledger.accept(Domain::Network, network, TimePoint{10}));

    const AuthorityDelta delta = compare_authority(plan, ledger);
    RC_REQUIRE_MSG(!delta.current, "a plan whose bindings changed is not current");
    RC_REQUIRE_MSG(delta.changed.size() == std::size_t{2},
                   "exactly the two changed domains must be reported, not the unchanged one");
    RC_REQUIRE_MSG(delta.changed[0] == Domain::Power && delta.changed[1] == Domain::Cooling,
                   "changed domains are reported in ascending domain order");
    RC_REQUIRE_MSG(delta.reasons.size() == delta.changed.size(),
                   "every changed domain carries exactly one reason");
    RC_REQUIRE_EQ(delta.reasons[0], std::string{"authority content digest changed"});
    RC_REQUIRE_EQ(delta.reasons[1], std::string{"no accepted authority for the domain"});
    RC_REQUIRE_MSG(delta.summary().find("power") != std::string::npos &&
                       delta.summary().find("cooling") != std::string::npos,
                   "the summary names every changed domain");
}

RC_TEST(compare_authority_accepts_a_fully_current_plan) {
    const AuthorityRef power = make_reference(Domain::Power, 2, "power-content");
    const AuthorityRef cooling = make_reference(Domain::Cooling, 2, "cooling-content");
    Plan plan = plan_requiring({{Domain::Power, power}, {Domain::Cooling, cooling}});

    AuthorityLedger ledger;
    RC_REQUIRE_OK(ledger.accept(Domain::Power, power, TimePoint{10}));
    RC_REQUIRE_OK(ledger.accept(Domain::Cooling, cooling, TimePoint{10}));

    const AuthorityDelta delta = compare_authority(plan, ledger);
    RC_REQUIRE_MSG(delta.current, "a plan bound to the accepted references is current");
    RC_REQUIRE_MSG(delta.changed.empty(), "a current plan reports no changed domains");
    RC_REQUIRE_EQ(delta.summary(), std::string{"current"});

    // Comparison is pure: it must not have accepted, retired, or changed
    // anything in the ledger.
    RC_REQUIRE_EQ(ledger.accepted_count(), std::size_t{2});
    RC_REQUIRE_EQ(ledger.retired_count(), std::size_t{0});
}

RC_TEST(compare_authority_reports_a_retired_domain_as_changed) {
    const AuthorityRef power = make_reference(Domain::Power, 2, "power-content");
    Plan plan = plan_requiring({{Domain::Power, power}});

    AuthorityLedger ledger;
    RC_REQUIRE_OK(ledger.accept(Domain::Power, power, TimePoint{10}));
    RC_REQUIRE_OK(ledger.retire(Domain::Power, TimePoint{20}, "operator"));

    const AuthorityDelta delta = compare_authority(plan, ledger);
    RC_REQUIRE_MSG(!delta.current, "a retired binding is not current, even when the reference matches");
    RC_REQUIRE_MSG(delta.changed.size() == std::size_t{1} && delta.changed[0] == Domain::Power,
                   "the retired domain must be reported as changed");
    RC_REQUIRE_EQ(delta.reasons[0], std::string{"no accepted authority for the domain"});
}
