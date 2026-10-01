#ifndef RECOVERY_CODEC_HPP
#define RECOVERY_CODEC_HPP

// Durable codecs. Every journal payload is produced and consumed here, and
// nowhere else. The codecs are total and strict in both directions:
//
//   * encoding a value always produces the same bytes for the same value;
//   * decoding refuses anything that does not round-trip byte for byte.
//
// They exist as a separate translation unit so that the durable format has one
// definition and can be reviewed without reading the decision engine.

#include <cstdint>
#include <string>
#include <vector>

#include "recovery/engine.hpp"
#include "recovery/error.hpp"
#include "recovery/json.hpp"
#include "recovery/plan.hpp"

namespace recovery {

struct ExecutionStateCodec;  // forward declaration for friend access

// The attempt codec is the only place outside the engine that may read or
// reconstruct the private fields of an Attempt.
class AttemptCodec {
public:
    [[nodiscard]] static json::Value to_json(const Attempt& attempt);
    [[nodiscard]] static Result<Attempt> from_committed_json(const json::Value& value);
    static void apply_state(Attempt& attempt, const json::Value& value);
};

namespace codec {

[[nodiscard]] json::Value encode(ExecutionId id);
[[nodiscard]] Result<ExecutionId> decode_execution_id(const json::Value& value);
[[nodiscard]] json::Value encode(PlanId id);
[[nodiscard]] Result<PlanId> decode_plan_id(const json::Value& value);
[[nodiscard]] json::Value encode(StepId id);
[[nodiscard]] Result<StepId> decode_step_id(const json::Value& value);
[[nodiscard]] json::Value encode(AttemptId id);
[[nodiscard]] Result<AttemptId> decode_attempt_id(const json::Value& value);
[[nodiscard]] json::Value encode(EvidenceId id);
[[nodiscard]] Result<EvidenceId> decode_evidence_id(const json::Value& value);
[[nodiscard]] json::Value encode(const Digest& digest);
[[nodiscard]] Result<Digest> decode_digest(const json::Value& value);
[[nodiscard]] json::Value encode(TimePoint instant);
[[nodiscard]] Result<TimePoint> decode_time(const json::Value& value);
[[nodiscard]] json::Value encode(Domain domain);
[[nodiscard]] Result<Domain> decode_domain(const json::Value& value);
[[nodiscard]] json::Value encode(const AuthorityRef& reference);
[[nodiscard]] Result<AuthorityRef> decode_authority(const json::Value& value);
[[nodiscard]] json::Value encode(const RequestBinding& binding);
[[nodiscard]] Result<RequestBinding> decode_binding(const json::Value& value);

// Strict member accessors. A missing or wrongly typed member is a corruption
// error, never a default.
[[nodiscard]] Result<std::string> want_string(const json::Value& object, const char* key);
[[nodiscard]] Result<bool> want_bool(const json::Value& object, const char* key);
[[nodiscard]] Result<std::uint64_t> want_u64(const json::Value& object, const char* key);
[[nodiscard]] Result<const json::Value*> want_member(const json::Value& object, const char* key);

// Parses a durable payload and refuses anything that is not already canonical.
[[nodiscard]] Result<json::Value> parse_canonical(std::string_view payload);

// Plan canonical text <-> structured form, used for the published-plan record
// and for the plan content digest.
[[nodiscard]] json::Value plan_to_json(const Plan& plan);
[[nodiscard]] Result<Plan> plan_from_json(const json::Value& value, Digest& contentOut);
[[nodiscard]] Digest plan_content_digest_of(const json::Value& planJson);
[[nodiscard]] json::Value step_spec_to_json(const StepSpec& spec);
[[nodiscard]] Result<StepSpec> step_spec_from_json(const json::Value& value);
[[nodiscard]] json::Value gate_to_json(const ReadinessGate& gate);
[[nodiscard]] Result<ReadinessGate> gate_from_json(const json::Value& value);
[[nodiscard]] json::Value dependency_to_json(const Dependency& dependency);
[[nodiscard]] Result<Dependency> dependency_from_json(const json::Value& value);

[[nodiscard]] json::Value observation_to_json(const Observation& observation);
[[nodiscard]] Result<Observation> observation_from_json(const json::Value& value, EvidenceId assigned);
[[nodiscard]] json::Value reading_to_json(const Reading& reading);
[[nodiscard]] Result<Reading> reading_from_json(const json::Value& value);
[[nodiscard]] json::Value attempt_view_to_json(const AttemptView& view);

}  // namespace codec
}  // namespace recovery

#endif  // RECOVERY_CODEC_HPP
