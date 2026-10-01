#include "test.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "recovery/observation.hpp"

using namespace recovery;

// ---------------------------------------------------------------------------
// Evidence store and evidence views
// ---------------------------------------------------------------------------
// The store answers one question for the coordinator: "what do I currently
// know about this domain, and on whose authority?" Every assertion below is a
// binding rule from the header: newest generation wins, a lower generation is
// history rather than a view, one identity never means two different contents,
// and a fence is only lifted by fresh evidence.

namespace {

constexpr std::uint64_t kSecond = 1000000000ull;

[[nodiscard]] EvidenceId evidence_id(std::uint8_t fill) {
    std::array<std::uint8_t, Id::kBytes> bytes{};
    bytes.fill(fill);
    return EvidenceId{Id{bytes}};
}

[[nodiscard]] AuthorityRef make_authority(Domain domain, std::uint64_t generation, std::string content) {
    return AuthorityRef{std::string{to_string(domain)}, "instance-1", generation, Digest::of(content)};
}

[[nodiscard]] Observation make_observation(std::uint8_t identity, Domain domain, std::uint64_t generation,
                                           std::string key, Reading reading, TimePoint at,
                                           const AuthorityRef& authority,
                                           ObservationSource source = ObservationSource::Adapter,
                                           bool stale = false) {
    return Observation{evidence_id(identity), domain, generation, source, "test/origin", std::move(key),
                       std::move(reading), at, authority, stale};
}

// Accepts an observation and fails the test with the store's own reason when the
// evidence was refused, so a refusal is never silently ignored.
void require_accepted(const ObservationAcceptance& acceptance, const char* expectation) {
    RC_REQUIRE_MSG(acceptance.accepted,
                   std::string{"the evidence store refused an observation that "} + expectation + ": " +
                       acceptance.reason);
}

}  // namespace

RC_TEST(evidence_view_keeps_the_newest_generation) {
    ObservationStore store;
    const AuthorityRef first = make_authority(Domain::Power, 1, "power-generation-1");
    const AuthorityRef second = make_authority(Domain::Power, 2, "power-generation-2");

    const ObservationAcceptance older =
        store.accept(make_observation(1, Domain::Power, 1, "power_ok", Reading::flag(true), TimePoint{kSecond},
                                      first));
    require_accepted(older, "reports generation 1");
    RC_REQUIRE_MSG(!older.replayed, "the first observation of an identity is new evidence, not a replay");

    const ObservationAcceptance newer = store.accept(
        make_observation(2, Domain::Power, 2, "power_ok", Reading::flag(false), TimePoint{2 * kSecond}, second));
    require_accepted(newer, "reports the newer generation 2");

    const EvidenceView& view = store.view(Domain::Power);
    RC_REQUIRE_MSG(view.established(), "an accepted observation must establish the domain view");
    const auto reading = view.find("power_ok");
    RC_REQUIRE_MSG(reading.has_value(), "the newest generation must still carry the reading it reported");
    RC_REQUIRE_MSG(*reading == Reading::flag(false),
                   "the view must show the reading from the newest generation, not the older one");
    RC_REQUIRE_MSG(view.authority() == second, "the view must be bound to the newest authority reference");
    RC_REQUIRE_MSG(view.observed_at() == TimePoint{2 * kSecond},
                   "the view instant must be the instant of the newest observation");

    RC_REQUIRE_EQ(store.observation_count(Domain::Power), std::size_t{2});
    RC_REQUIRE_EQ(store.total_observations(), std::size_t{2});
}

RC_TEST(evidence_view_ignores_readings_from_a_lower_generation) {
    ObservationStore store;
    const AuthorityRef newer = make_authority(Domain::Cooling, 5, "cooling-generation-5");
    const AuthorityRef older = make_authority(Domain::Cooling, 4, "cooling-generation-4");

    require_accepted(store.accept(make_observation(1, Domain::Cooling, 5, "cooling_ok", Reading::flag(true),
                                                   TimePoint{2 * kSecond}, newer)),
                     "reports the newer generation 5");
    require_accepted(store.accept(make_observation(2, Domain::Cooling, 4, "legacy_reading", Reading::flag(true),
                                                   TimePoint{3 * kSecond}, older)),
                     "reports the older generation 4");

    const EvidenceView& view = store.view(Domain::Cooling);
    RC_REQUIRE_MSG(view.established(), "the domain view is established by the accepted observation");
    RC_REQUIRE_MSG(view.find("cooling_ok").has_value(), "the newest generation contributes its reading");
    RC_REQUIRE_MSG(!view.find("legacy_reading").has_value(),
                   "a lower generation must not contribute readings to the current view");
    RC_REQUIRE_MSG(view.authority() == newer, "the view stays bound to the newest generation");

    // The lower generation is still part of the domain history: the store is
    // the coordinator's memory, not only its current belief.
    RC_REQUIRE_EQ(store.observation_count(Domain::Cooling), std::size_t{2});
    const std::vector<Observation> history = store.observations_for(Domain::Cooling);
    RC_REQUIRE_MSG(history.size() == std::size_t{2}, "the store must retain both observations as history");
}

RC_TEST(evidence_store_refuses_one_identity_with_different_content) {
    ObservationStore store;
    const AuthorityRef authority = make_authority(Domain::Power, 1, "power-generation-1");

    const Observation original =
        make_observation(7, Domain::Power, 1, "power_ok", Reading::flag(true), TimePoint{kSecond}, authority);
    const Observation conflicting =
        make_observation(7, Domain::Power, 1, "power_ok", Reading::flag(false), TimePoint{kSecond}, authority);

    const ObservationAcceptance first = store.accept(original);
    require_accepted(first, "is the first presentation of its identity");
    RC_REQUIRE_MSG(!first.replayed, "the first observation is new evidence");
    RC_REQUIRE_MSG(first.digest != conflicting.digest(),
                   "two observations with the same identity and different content must not share a digest");

    const ObservationAcceptance second = store.accept(conflicting);
    RC_REQUIRE_MSG(!second.accepted,
                   "an observation reusing an accepted identity with different content must be refused");
    RC_REQUIRE_MSG(!second.reason.empty(), "a refusal must state why it was refused");
    RC_REQUIRE_MSG(!second.replayed, "a refused observation is not a replay");

    const auto reading = store.view(Domain::Power).find("power_ok");
    RC_REQUIRE_MSG(reading.has_value() && *reading == Reading::flag(true),
                   "a refused observation must not change the accepted reading");
    RC_REQUIRE_EQ(store.observation_count(Domain::Power), std::size_t{1});
}

RC_TEST(evidence_store_treats_a_repeated_observation_as_an_idempotent_replay) {
    ObservationStore store;
    const AuthorityRef authority = make_authority(Domain::Capacity, 3, "capacity-generation-3");
    const Observation observation =
        make_observation(9, Domain::Capacity, 3, "capacity_ok", Reading::count(4), TimePoint{kSecond}, authority);

    const ObservationAcceptance first = store.accept(observation);
    require_accepted(first, "is the first presentation of its identity");
    RC_REQUIRE_MSG(!first.replayed, "the first presentation is new evidence");

    const ObservationAcceptance replay = store.accept(observation);
    RC_REQUIRE_MSG(replay.accepted, "re-presenting identical evidence is accepted, not an error");
    RC_REQUIRE_MSG(replay.replayed, "re-presenting identical evidence must be reported as a replay");
    RC_REQUIRE_EQ(replay.digest.hex(), first.digest.hex());
    RC_REQUIRE_EQ(store.observation_count(Domain::Capacity), std::size_t{1});
    RC_REQUIRE_EQ(store.total_observations(), std::size_t{1});

    const auto reading = store.view(Domain::Capacity).find("capacity_ok");
    RC_REQUIRE_MSG(reading.has_value() && *reading == Reading::count(4),
                   "an idempotent replay must not move the view");
}

RC_TEST(evidence_store_refuses_an_observation_without_authority) {
    ObservationStore store;
    const Observation observation = make_observation(11, Domain::Safety, 1, "safety_ok", Reading::flag(true),
                                                     TimePoint{kSecond}, AuthorityRef{});

    const ObservationAcceptance acceptance = store.accept(observation);
    RC_REQUIRE_MSG(!acceptance.accepted, "an observation that names no authority is not evidence");
    RC_REQUIRE_MSG(!store.view(Domain::Safety).established(),
                   "an observation without authority must not establish a view");
    RC_REQUIRE_EQ(store.total_observations(), std::size_t{0});
}

RC_TEST(evidence_view_is_not_current_beyond_the_freshness_bound) {
    ObservationStore store;
    const AuthorityRef authority = make_authority(Domain::Power, 1, "power-generation-1");
    const TimePoint observed{100 * kSecond};
    require_accepted(store.accept(make_observation(1, Domain::Power, 1, "power_ok", Reading::flag(true), observed,
                                                   authority)),
                     "carries a complete authority reference");

    const EvidenceView& view = store.view(Domain::Power);
    RC_REQUIRE_MSG(view.is_current(observed, Duration{0}),
                   "evidence observed at the instant it is read is current even with a zero bound");
    RC_REQUIRE_MSG(view.is_current(TimePoint{observed.nanos() + kSecond}, Duration{kSecond}),
                   "evidence exactly at the freshness bound is still current");
    RC_REQUIRE_MSG(!view.is_current(TimePoint{observed.nanos() + kSecond + 1}, Duration{kSecond}),
                   "evidence one nanosecond beyond the freshness bound is not current");
    RC_REQUIRE_MSG(!view.is_current(TimePoint{observed.nanos() - 1}, Duration{1000 * kSecond}),
                   "evidence from the future is not current evidence");
}

RC_TEST(fence_all_makes_a_view_not_current_until_fresh_evidence_is_presented) {
    ObservationStore store;
    const AuthorityRef power = make_authority(Domain::Power, 1, "power-generation-1");
    const AuthorityRef cooling = make_authority(Domain::Cooling, 1, "cooling-generation-1");
    require_accepted(store.accept(make_observation(1, Domain::Power, 1, "power_ok", Reading::flag(true), TimePoint{0},
                                                   power)),
                     "carries the power authority");
    require_accepted(store.accept(make_observation(2, Domain::Cooling, 1, "cooling_ok", Reading::flag(true),
                                                   TimePoint{0}, cooling)),
                     "carries the cooling authority");

    RC_REQUIRE_MSG(store.view(Domain::Power).is_current(TimePoint{0}, Duration{kSecond}),
                   "the view is current before the fence");

    store.fence_all();
    RC_REQUIRE_MSG(!store.view(Domain::Power).established(), "a fence must leave no view established");
    RC_REQUIRE_MSG(!store.view(Domain::Power).is_current(TimePoint{0}, Duration{1000 * kSecond}),
                   "after a fence, no amount of freshness bound makes the old view current");
    RC_REQUIRE_MSG(!store.view(Domain::Cooling).established(), "a fence applies to every domain at once");

    // Fresh evidence for one domain ends the fence for that domain only. This is
    // the documented meaning of "no view is current afterwards until fresh
    // evidence is presented": a restarted coordinator re-establishes what it can
    // see, and nothing more.
    const AuthorityRef powerAfterRestart = make_authority(Domain::Power, 7, "power-generation-7");
    require_accepted(store.accept(make_observation(3, Domain::Power, 7, "power_ok", Reading::flag(true), TimePoint{0},
                                                   powerAfterRestart)),
                     "is fresh evidence presented after a fence");

    RC_REQUIRE_MSG(store.view(Domain::Power).established(),
                   "fresh evidence presented after a fence must re-establish that domain's view");
    RC_REQUIRE_MSG(store.view(Domain::Power).is_current(TimePoint{0}, Duration{kSecond}),
                   "the re-established view must be current");
    RC_REQUIRE_MSG(store.view(Domain::Power).authority() == powerAfterRestart,
                   "the re-established view must be bound to the fresh evidence's authority");
    RC_REQUIRE_MSG(!store.view(Domain::Cooling).established(),
                   "evidence for one domain must not lift the fence on another domain");
}

RC_TEST(observations_for_returns_the_documented_total_order) {
    ObservationStore store;
    const AuthorityRef authority = make_authority(Domain::Power, 1, "power-generation-1");

    // Accepted out of order on purpose: the result must be ordered by key and
    // then generation, never by insertion order.
    require_accepted(store.accept(make_observation(3, Domain::Power, 2, "a", Reading::count(2), TimePoint{2 * kSecond},
                                                   make_authority(Domain::Power, 2, "power-generation-2"))),
                     "carries the newest authority");
    require_accepted(store.accept(make_observation(2, Domain::Power, 1, "b", Reading::count(1), TimePoint{kSecond},
                                                   authority)),
                     "carries the first authority");
    require_accepted(store.accept(make_observation(1, Domain::Power, 1, "a", Reading::count(0), TimePoint{kSecond},
                                                   authority)),
                     "carries the first authority");

    const std::vector<Observation> ordered = store.observations_for(Domain::Power);
    RC_REQUIRE_MSG(ordered.size() == std::size_t{3}, "every accepted observation is retained for the domain");
    RC_REQUIRE_MSG(ordered[0].key() == "a" && ordered[0].generation() == 1,
                   "the total order starts with the lowest key at the lowest generation");
    RC_REQUIRE_MSG(ordered[1].key() == "a" && ordered[1].generation() == 2,
                   "within one key the order is by ascending generation");
    RC_REQUIRE_MSG(ordered[2].key() == "b" && ordered[2].generation() == 1,
                   "keys are compared before generations");
}

RC_TEST(evidence_store_separates_domains) {
    ObservationStore store;
    require_accepted(store.accept(make_observation(1, Domain::Power, 1, "ok", Reading::flag(true), TimePoint{0},
                                                   make_authority(Domain::Power, 1, "power-1"))),
                     "carries the power authority");
    require_accepted(store.accept(make_observation(2, Domain::Safety, 1, "ok", Reading::flag(false), TimePoint{0},
                                                   make_authority(Domain::Safety, 1, "safety-1"))),
                     "carries the safety authority");

    const auto power = store.view(Domain::Power).find("ok");
    const auto safety = store.view(Domain::Safety).find("ok");
    RC_REQUIRE_MSG(power.has_value() && *power == Reading::flag(true),
                   "the power view must show the power reading");
    RC_REQUIRE_MSG(safety.has_value() && *safety == Reading::flag(false),
                   "the safety view must show the safety reading, not another domain's reading");
    RC_REQUIRE_EQ(store.domains().size(), std::size_t{2});
}
