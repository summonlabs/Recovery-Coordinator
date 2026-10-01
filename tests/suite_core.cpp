#include "test.hpp"

#include "recovery/canonical.hpp"
#include "recovery/digest.hpp"
#include "recovery/id.hpp"
#include "recovery/json.hpp"
#include "recovery/time.hpp"

using namespace recovery;

RC_TEST(sha256_known_vector) {
    const Digest digest = Digest::of("abc");
    RC_REQUIRE_EQ(digest.hex(), std::string{"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"});
}

RC_TEST(sha256_empty_vector) {
    const Digest digest = Digest::of("");
    RC_REQUIRE_EQ(digest.hex(), std::string{"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"});
}

RC_TEST(digest_parse_is_strict) {
    RC_REQUIRE(!Digest::parse("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015a").has_value());
    RC_REQUIRE(!Digest::parse("BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD").has_value());
    RC_REQUIRE(Digest::parse("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad").has_value());
}

RC_TEST(json_round_trip_is_canonical) {
    json::Object object;
    object["b"] = json::Value{true};
    object["a"] = json::Value{static_cast<unsigned long long>(7)};
    const std::string encoded = json::encode(json::Value{object});
    RC_REQUIRE_EQ(encoded, std::string{"{\"a\":7,\"b\":true}"});
    json::Decoder decoder(encoded);
    auto decoded = decoder.decode();
    RC_REQUIRE(decoded.has_value());
    RC_REQUIRE_EQ(json::encode(decoded.value()), encoded);
}

RC_TEST(json_refuses_trailing_and_duplicate) {
    json::Decoder trailing("{\"a\":1} x");
    RC_REQUIRE(!trailing.decode().has_value());
    json::Decoder duplicate("{\"a\":1,\"a\":2}");
    RC_REQUIRE(!duplicate.decode().has_value());
}

RC_TEST(rfc3339_round_trip) {
    const TimePoint instant{1767225600123456789ull};
    const std::string text = to_rfc3339(instant);
    auto parsed = parse_rfc3339(text);
    RC_REQUIRE(parsed.has_value());
    RC_REQUIRE_EQ(parsed->nanos(), instant.nanos());
    RC_REQUIRE(!parse_rfc3339("2026-01-01T00:00:00+01:00").has_value());
    RC_REQUIRE(!parse_rfc3339("2026-01-01 00:00:00Z").has_value());
}

RC_TEST(identity_round_trip) {
    IdAllocator allocator{7};
    auto first = allocator.allocate(IdKind::Plan);
    RC_REQUIRE(first.has_value());
    const PlanId plan{*first};
    auto parsed = PlanId::parse(plan.to_text());
    RC_REQUIRE(parsed.has_value());
    RC_REQUIRE_EQ(parsed->hex(), plan.hex());
    RC_REQUIRE(!PlanId::parse(plan.to_text().substr(5)).has_value());
    RC_REQUIRE(!ExecutionId::parse(plan.to_text()).has_value());
}

RC_TEST(identity_allocator_refuses_overflow) {
    IdAllocator allocator{0xffffffffffffffffull};
    auto value = allocator.allocate(IdKind::Step);
    RC_REQUIRE(value.has_value());
    RC_REQUIRE(!allocator.allocate(IdKind::Step).has_value());
}
