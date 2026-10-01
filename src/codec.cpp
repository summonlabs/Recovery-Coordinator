#include "codec.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdio>

#include "recovery/canonical.hpp"
#include "recovery/sha256.hpp"

namespace recovery {
namespace {

namespace key {
constexpr const char* kActor = "actor";
constexpr const char* kAt = "at";
constexpr const char* kAttempt = "attempt";
constexpr const char* kAttempts = "attempts";
constexpr const char* kAuthority = "authority";
constexpr const char* kBinding = "binding";
constexpr const char* kBindings = "bindings";
constexpr const char* kCompensable = "compensable";
constexpr const char* kCompensated = "compensated";
constexpr const char* kCompensation = "compensation";
constexpr const char* kCompensationAttempts = "compensation_attempts";
constexpr const char* kCompensationRequired = "compensation_required";
constexpr const char* kCreatedAt = "created_at";
constexpr const char* kDeadline = "deadline";
constexpr const char* kDependencies = "dependencies";
constexpr const char* kDetail = "detail";
constexpr const char* kDigest = "digest";
constexpr const char* kDomain = "domain";
constexpr const char* kDomains = "domains";
constexpr const char* kEvidenceDomains = "evidence_domains";
constexpr const char* kExpected = "expected";
constexpr const char* kFreshness = "freshness";
constexpr const char* kGates = "gates";
constexpr const char* kGeneration = "generation";
constexpr const char* kId = "id";
constexpr const char* kInstance = "instance";
constexpr const char* kKey = "key";
constexpr const char* kKind = "kind";
constexpr const char* kMaxAge = "max_age_nanos";
constexpr const char* kName = "name";
constexpr const char* kOperation = "operation";
constexpr const char* kOutcome = "outcome";
constexpr const char* kParameters = "parameters";
constexpr const char* kPointOfNoReturn = "point_of_no_return";
constexpr const char* kPredecessor = "predecessor";
constexpr const char* kReading = "reading";
constexpr const char* kRequireDomainAuthority = "require_domain_authority";
constexpr const char* kRequiredKeys = "required_keys";
constexpr const char* kRequirements = "requirements";
constexpr const char* kRevision = "revision";
constexpr const char* kScope = "scope";
constexpr const char* kSource = "source";
constexpr const char* kStale = "stale";
constexpr const char* kStatus = "status";
constexpr const char* kStep = "step";
constexpr const char* kSteps = "steps";
constexpr const char* kStrategy = "strategy";
constexpr const char* kValue = "value";
}  // namespace key

[[nodiscard]] json::Array string_array(const std::vector<std::string>& values) {
    json::Array array;
    array.reserve(values.size());
    for (const std::string& value : values) {
        array.push_back(json::Value{value});
    }
    return array;
}

}  // namespace

namespace codec {

Result<json::Value> parse_canonical(std::string_view payload) {
    json::Decoder decoder(payload);
    auto value = decoder.decode();
    if (!value.has_value()) {
        return value.error();
    }
    if (value.value().kind() != json::Value::Kind::Object) {
        return errors::corrupt_state("record.not_object", "a durable record payload is not a JSON object");
    }
    if (json::encode(value.value()) != payload) {
        return errors::corrupt_state("record.not_canonical", "a durable record payload is not canonically encoded");
    }
    return value;
}

Result<const json::Value*> want_member(const json::Value& object, const char* key) {
    if (object.kind() != json::Value::Kind::Object) {
        return errors::corrupt_state("record.not_object", "a durable record payload is not a JSON object");
    }
    const auto it = object.object().find(key);
    if (it == object.object().end()) {
        return errors::corrupt_state("record.missing_field", std::string{"record is missing field "} + key);
    }
    return &it->second;
}

Result<std::string> want_string(const json::Value& object, const char* key) {
    auto value = want_member(object, key);
    if (!value.has_value()) {
        return value.error();
    }
    if (value.value()->kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.field_type", std::string{"field "} + key + " is not a string");
    }
    return value.value()->string();
}

Result<bool> want_bool(const json::Value& object, const char* key) {
    auto value = want_member(object, key);
    if (!value.has_value()) {
        return value.error();
    }
    if (value.value()->kind() != json::Value::Kind::Boolean) {
        return errors::corrupt_state("record.field_type", std::string{"field "} + key + " is not a boolean");
    }
    return value.value()->boolean();
}

Result<std::uint64_t> want_u64(const json::Value& object, const char* key) {
    auto value = want_member(object, key);
    if (!value.has_value()) {
        return value.error();
    }
    if (value.value()->kind() == json::Value::Kind::Unsigned) {
        return value.value()->unsigned_value();
    }
    if (value.value()->kind() == json::Value::Kind::Signed && value.value()->signed_value() >= 0) {
        return static_cast<std::uint64_t>(value.value()->signed_value());
    }
    return errors::corrupt_state("record.field_type", std::string{"field "} + key + " is not an unsigned integer");
}

json::Value encode(ExecutionId id) { return json::Value{id.to_text()}; }

Result<ExecutionId> decode_execution_id(const json::Value& value) {
    if (value.kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.id", "an execution identity is not a string");
    }
    auto parsed = ExecutionId::parse(value.string());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.id", "an execution identity is not canonically encoded");
    }
    return *parsed;
}

json::Value encode(PlanId id) { return json::Value{id.to_text()}; }

Result<PlanId> decode_plan_id(const json::Value& value) {
    if (value.kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.id", "a plan identity is not a string");
    }
    auto parsed = PlanId::parse(value.string());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.id", "a plan identity is not canonically encoded");
    }
    return *parsed;
}

json::Value encode(StepId id) { return json::Value{id.to_text()}; }

Result<StepId> decode_step_id(const json::Value& value) {
    if (value.kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.id", "a step identity is not a string");
    }
    auto parsed = StepId::parse(value.string());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.id", "a step identity is not canonically encoded");
    }
    return *parsed;
}

json::Value encode(AttemptId id) { return json::Value{id.to_text()}; }

Result<AttemptId> decode_attempt_id(const json::Value& value) {
    if (value.kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.id", "an attempt identity is not a string");
    }
    auto parsed = AttemptId::parse(value.string());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.id", "an attempt identity is not canonically encoded");
    }
    return *parsed;
}

json::Value encode(EvidenceId id) { return json::Value{id.to_text()}; }

Result<EvidenceId> decode_evidence_id(const json::Value& value) {
    if (value.kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.id", "an evidence identity is not a string");
    }
    auto parsed = EvidenceId::parse(value.string());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.id", "an evidence identity is not canonically encoded");
    }
    return *parsed;
}

json::Value encode(const Digest& digest) { return json::Value{digest.hex()}; }

Result<Digest> decode_digest(const json::Value& value) {
    if (value.kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.digest", "a digest is not a string");
    }
    auto parsed = Digest::parse(value.string());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.digest", "a digest is not canonically encoded");
    }
    return *parsed;
}

json::Value encode(TimePoint instant) { return json::Value{to_rfc3339(instant)}; }

Result<TimePoint> decode_time(const json::Value& value) {
    if (value.kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.time", "an instant is not a string");
    }
    auto parsed = parse_rfc3339(value.string());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.time", "an instant is not canonically encoded");
    }
    return *parsed;
}

json::Value encode(Domain domain) { return json::Value{std::string{to_string(domain)}}; }

Result<Domain> decode_domain(const json::Value& value) {
    if (value.kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.domain", "a domain is not a string");
    }
    auto parsed = domain_from_string(value.string());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.domain", "a domain name is not recognised");
    }
    return *parsed;
}

json::Value encode(const AuthorityRef& reference) {
    json::Object object;
    object[key::kAuthority] = json::Value{reference.authority()};
    object[key::kDigest] = json::Value{reference.digest().hex()};
    object[key::kGeneration] = json::Value{static_cast<unsigned long long>(reference.generation())};
    object[key::kInstance] = json::Value{reference.instance()};
    return json::Value{std::move(object)};
}

Result<AuthorityRef> decode_authority(const json::Value& value) {
    auto authority = want_string(value, key::kAuthority);
    if (!authority.has_value()) {
        return authority.error();
    }
    auto instance = want_string(value, key::kInstance);
    if (!instance.has_value()) {
        return instance.error();
    }
    auto generation = want_u64(value, key::kGeneration);
    if (!generation.has_value()) {
        return generation.error();
    }
    auto digest = want_member(value, key::kDigest);
    if (!digest.has_value()) {
        return digest.error();
    }
    auto parsed = decode_digest(*digest.value());
    if (!parsed.has_value()) {
        return parsed.error();
    }
    AuthorityRef reference{authority.value(), instance.value(), generation.value(), *parsed};
    if (reference.canonical() != json::encode(codec::encode(reference))) {
        return errors::corrupt_state("record.authority", "an authority reference is not canonically encoded");
    }
    return reference;
}

json::Value encode(const RequestBinding& binding) {
    json::Object object;
    object[key::kBinding] = json::Value{static_cast<unsigned long long>(binding.binding)};
    object[key::kDomain] = codec::encode(binding.domain);
    object[key::kOperation] = json::Value{binding.operation};
    object[key::kParameters] = json::Value{binding.parameters};
    object[key::kPointOfNoReturn] = json::Value{binding.point_of_no_return};
    object[key::kStep] = codec::encode(binding.step);
    return json::Value{std::move(object)};
}

Result<RequestBinding> decode_binding(const json::Value& value) {
    RequestBinding binding;
    auto step = want_member(value, key::kStep);
    if (!step.has_value()) {
        return step.error();
    }
    auto stepId = decode_step_id(*step.value());
    if (!stepId.has_value()) {
        return stepId.error();
    }
    binding.step = *stepId;
    auto number = want_u64(value, key::kBinding);
    if (!number.has_value()) {
        return number.error();
    }
    if (number.value() > 0xffffffffull) {
        return errors::corrupt_state("record.binding_number", "a request binding number is out of range");
    }
    binding.binding = static_cast<std::uint32_t>(number.value());
    auto domain = want_member(value, key::kDomain);
    if (!domain.has_value()) {
        return domain.error();
    }
    auto parsedDomain = decode_domain(*domain.value());
    if (!parsedDomain.has_value()) {
        return parsedDomain.error();
    }
    binding.domain = *parsedDomain;
    auto operation = want_string(value, key::kOperation);
    if (!operation.has_value()) {
        return operation.error();
    }
    binding.operation = operation.value();
    auto parameters = want_string(value, key::kParameters);
    if (!parameters.has_value()) {
        return parameters.error();
    }
    binding.parameters = parameters.value();
    auto pnr = want_bool(value, key::kPointOfNoReturn);
    if (!pnr.has_value()) {
        return pnr.error();
    }
    binding.point_of_no_return = pnr.value();
    return binding;
}

json::Value dependency_to_json(const Dependency& dependency) {
    json::Object object;
    object[key::kOutcome] = json::Value{std::string{to_string(dependency.outcome)}};
    object[key::kPredecessor] = codec::encode(dependency.predecessor);
    return json::Value{std::move(object)};
}

Result<Dependency> dependency_from_json(const json::Value& value) {
    Dependency dependency;
    auto outcome = want_string(value, key::kOutcome);
    if (!outcome.has_value()) {
        return outcome.error();
    }
    auto parsed = edge_outcome_from_string(outcome.value());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.edge_outcome", "a dependency outcome is not recognised");
    }
    dependency.outcome = *parsed;
    auto predecessor = want_member(value, key::kPredecessor);
    if (!predecessor.has_value()) {
        return predecessor.error();
    }
    auto step = decode_step_id(*predecessor.value());
    if (!step.has_value()) {
        return step.error();
    }
    dependency.predecessor = *step;
    if (json::encode(dependency_to_json(dependency)) != json::encode(value)) {
        return errors::corrupt_state("record.dependency", "a dependency is not canonically encoded");
    }
    return dependency;
}

json::Value reading_to_json(const Reading& reading) {
    // A reading is stored exactly as its canonical rendering, as a JSON string.
    // The rendering already carries the kind, so a reader never has to guess
    // which of the typed values a stored number represents.
    return json::Value{reading.canonical()};
}

Result<Reading> reading_from_json(const json::Value& value) {
    if (value.kind() != json::Value::Kind::String) {
        return errors::corrupt_state("record.reading", "a reading is not stored as its canonical rendering");
    }
    auto parsed = Reading::from_canonical(value.string());
    if (!parsed.has_value()) {
        return errors::corrupt_state("record.reading", "a reading rendering is not recognised");
    }
    if (parsed->canonical() != value.string()) {
        return errors::corrupt_state("record.reading", "a reading rendering is not canonically encoded");
    }
    return *parsed;
}

json::Value gate_to_json(const ReadinessGate& gate) {
    json::Object expected;
    for (const auto& [name, reading] : gate.expected) {
        expected[name] = reading_to_json(reading);
    }
    json::Object object;
    object[key::kDomain] = codec::encode(gate.domain);
    object[key::kExpected] = json::Value{std::move(expected)};
    object[key::kMaxAge] = json::Value{static_cast<unsigned long long>(gate.max_age.nanos())};
    object[key::kRequireDomainAuthority] = json::Value{gate.require_domain_authority};
    object[key::kRequiredKeys] = json::Value{string_array(gate.required_keys)};
    return json::Value{std::move(object)};
}

Result<ReadinessGate> gate_from_json(const json::Value& value) {
    ReadinessGate gate;
    auto domain = want_member(value, key::kDomain);
    if (!domain.has_value()) {
        return domain.error();
    }
    auto parsedDomain = decode_domain(*domain.value());
    if (!parsedDomain.has_value()) {
        return parsedDomain.error();
    }
    gate.domain = *parsedDomain;

    auto expected = want_member(value, key::kExpected);
    if (!expected.has_value()) {
        return expected.error();
    }
    if (expected.value()->kind() != json::Value::Kind::Object) {
        return errors::corrupt_state("record.gate", "gate expectations are not an object");
    }
    for (const auto& [name, readingValue] : expected.value()->object()) {
        auto reading = reading_from_json(readingValue);
        if (!reading.has_value()) {
            return reading.error();
        }
        gate.expected.emplace(name, *reading);
    }

    auto maxAge = want_u64(value, key::kMaxAge);
    if (!maxAge.has_value()) {
        return maxAge.error();
    }
    gate.max_age = Duration{maxAge.value()};

    auto requireAuthority = want_bool(value, key::kRequireDomainAuthority);
    if (!requireAuthority.has_value()) {
        return requireAuthority.error();
    }
    gate.require_domain_authority = requireAuthority.value();

    auto requiredKeys = want_member(value, key::kRequiredKeys);
    if (!requiredKeys.has_value()) {
        return requiredKeys.error();
    }
    if (requiredKeys.value()->kind() != json::Value::Kind::Array) {
        return errors::corrupt_state("record.gate", "gate required keys are not an array");
    }
    for (const json::Value& entry : requiredKeys.value()->array()) {
        if (entry.kind() != json::Value::Kind::String) {
            return errors::corrupt_state("record.gate", "a gate required key is not a string");
        }
        gate.required_keys.push_back(entry.string());
    }
    if (json::encode(gate_to_json(gate)) != json::encode(value)) {
        return errors::corrupt_state("record.gate", "a readiness gate is not canonically encoded");
    }
    return gate;
}

json::Value step_spec_to_json(const StepSpec& spec) {
    json::Array dependencies;
    dependencies.reserve(spec.depends_on.size());
    for (const Dependency& dependency : spec.depends_on) {
        dependencies.push_back(dependency_to_json(dependency));
    }
    json::Array requests;
    requests.reserve(spec.requests.size());
    for (const RequestBinding& binding : spec.requests) {
        requests.push_back(codec::encode(binding));
    }
    json::Array compensations;
    compensations.reserve(spec.compensation_requests.size());
    for (const RequestBinding& binding : spec.compensation_requests) {
        compensations.push_back(codec::encode(binding));
    }
    json::Array gates;
    gates.reserve(spec.gates.size());
    for (const ReadinessGate& gate : spec.gates) {
        gates.push_back(gate_to_json(gate));
    }
    json::Array domains;
    domains.reserve(spec.evidence_domains.size());
    for (const Domain domain : spec.evidence_domains) {
        domains.push_back(codec::encode(domain));
    }
    json::Object object;
    object[key::kCompensable] = json::Value{spec.compensable};
    object[key::kCompensationRequired] = json::Value{spec.compensation_required};
    object[key::kDeadline] = codec::encode(spec.deadline);
    object[key::kDependencies] = json::Value{std::move(dependencies)};
    object[key::kEvidenceDomains] = json::Value{std::move(domains)};
    object[key::kGates] = json::Value{std::move(gates)};
    object[key::kId] = codec::encode(spec.id);
    object[key::kName] = json::Value{spec.name};
    object[key::kOperation] = json::Value{std::string{"step"}};
    object["compensation_requests"] = json::Value{std::move(compensations)};
    object["requests"] = json::Value{std::move(requests)};
    return json::Value{std::move(object)};
}

Result<StepSpec> step_spec_from_json(const json::Value& value) {
    StepSpec spec;
    auto id = want_member(value, key::kId);
    if (!id.has_value()) {
        return id.error();
    }
    auto stepId = decode_step_id(*id.value());
    if (!stepId.has_value()) {
        return stepId.error();
    }
    spec.id = *stepId;
    auto name = want_string(value, key::kName);
    if (!name.has_value()) {
        return name.error();
    }
    spec.name = name.value();

    auto dependencies = want_member(value, key::kDependencies);
    if (!dependencies.has_value()) {
        return dependencies.error();
    }
    if (dependencies.value()->kind() != json::Value::Kind::Array) {
        return errors::corrupt_state("record.step", "step dependencies are not an array");
    }
    for (const json::Value& entry : dependencies.value()->array()) {
        auto dependency = dependency_from_json(entry);
        if (!dependency.has_value()) {
            return dependency.error();
        }
        spec.depends_on.push_back(*dependency);
    }

    auto requests = want_member(value, "requests");
    if (!requests.has_value()) {
        return requests.error();
    }
    if (requests.value()->kind() != json::Value::Kind::Array) {
        return errors::corrupt_state("record.step", "step requests are not an array");
    }
    for (const json::Value& entry : requests.value()->array()) {
        auto binding = decode_binding(entry);
        if (!binding.has_value()) {
            return binding.error();
        }
        spec.requests.push_back(*binding);
    }

    auto compensations = want_member(value, "compensation_requests");
    if (!compensations.has_value()) {
        return compensations.error();
    }
    if (compensations.value()->kind() != json::Value::Kind::Array) {
        return errors::corrupt_state("record.step", "step compensation requests are not an array");
    }
    for (const json::Value& entry : compensations.value()->array()) {
        auto binding = decode_binding(entry);
        if (!binding.has_value()) {
            return binding.error();
        }
        spec.compensation_requests.push_back(*binding);
    }

    auto gates = want_member(value, key::kGates);
    if (!gates.has_value()) {
        return gates.error();
    }
    if (gates.value()->kind() != json::Value::Kind::Array) {
        return errors::corrupt_state("record.step", "step gates are not an array");
    }
    for (const json::Value& entry : gates.value()->array()) {
        auto gate = gate_from_json(entry);
        if (!gate.has_value()) {
            return gate.error();
        }
        spec.gates.push_back(*gate);
    }

    auto domains = want_member(value, key::kEvidenceDomains);
    if (!domains.has_value()) {
        return domains.error();
    }
    if (domains.value()->kind() != json::Value::Kind::Array) {
        return errors::corrupt_state("record.step", "step evidence domains are not an array");
    }
    for (const json::Value& entry : domains.value()->array()) {
        auto domain = decode_domain(entry);
        if (!domain.has_value()) {
            return domain.error();
        }
        spec.evidence_domains.push_back(*domain);
    }

    auto deadline = want_member(value, key::kDeadline);
    if (!deadline.has_value()) {
        return deadline.error();
    }
    auto parsedDeadline = decode_time(*deadline.value());
    if (!parsedDeadline.has_value()) {
        return parsedDeadline.error();
    }
    spec.deadline = *parsedDeadline;

    auto compensable = want_bool(value, key::kCompensable);
    if (!compensable.has_value()) {
        return compensable.error();
    }
    spec.compensable = compensable.value();

    auto compensationRequired = want_bool(value, key::kCompensationRequired);
    if (!compensationRequired.has_value()) {
        return compensationRequired.error();
    }
    spec.compensation_required = compensationRequired.value();

    if (json::encode(step_spec_to_json(spec)) != json::encode(value)) {
        return errors::corrupt_state("record.step", "a step specification is not canonically encoded");
    }
    return spec;
}

json::Value plan_to_json(const Plan& plan) {
    json::Array authority;
    for (const auto& [domain, reference] : plan.authority_requirements()) {
        json::Object entry;
        entry[key::kDomain] = codec::encode(domain);
        entry["reference"] = codec::encode(reference);
        authority.push_back(json::Value{std::move(entry)});
    }
    json::Array freshness;
    for (const auto& [domain, bound] : plan.freshness_requirements()) {
        json::Object entry;
        entry[key::kDomain] = codec::encode(domain);
        entry[key::kMaxAge] = json::Value{static_cast<unsigned long long>(bound.nanos())};
        freshness.push_back(json::Value{std::move(entry)});
    }
    json::Array steps;
    steps.reserve(plan.steps().size());
    for (const StepSpec& spec : plan.steps()) {
        steps.push_back(step_spec_to_json(spec));
    }
    json::Object object;
    object[key::kCreatedAt] = codec::encode(plan.created_at());
    object[key::kId] = codec::encode(plan.id());
    object[key::kName] = json::Value{plan.name()};
    object[key::kRequirements] = json::Value{std::move(authority)};
    object[key::kFreshness] = json::Value{std::move(freshness)};
    object[key::kRevision] = json::Value{static_cast<unsigned long long>(plan.revision())};
    object[key::kScope] = json::Value{plan.scope()};
    object[key::kSteps] = json::Value{std::move(steps)};
    object[key::kStrategy] = json::Value{std::string{to_string(plan.strategy())}};
    return json::Value{std::move(object)};
}

Result<Plan> plan_from_json(const json::Value& value, Digest& contentOut) {
    auto id = want_member(value, key::kId);
    if (!id.has_value()) {
        return id.error();
    }
    auto planId = decode_plan_id(*id.value());
    if (!planId.has_value()) {
        return planId.error();
    }
    auto name = want_string(value, key::kName);
    if (!name.has_value()) {
        return name.error();
    }
    auto scope = want_string(value, key::kScope);
    if (!scope.has_value()) {
        return scope.error();
    }
    auto strategy = want_string(value, key::kStrategy);
    if (!strategy.has_value()) {
        return strategy.error();
    }
    auto parsedStrategy = plan_strategy_from_string(strategy.value());
    if (!parsedStrategy.has_value()) {
        return errors::corrupt_state("record.plan_strategy", "a plan strategy is not recognised");
    }
    auto revision = want_u64(value, key::kRevision);
    if (!revision.has_value()) {
        return revision.error();
    }
    auto createdAt = want_member(value, key::kCreatedAt);
    if (!createdAt.has_value()) {
        return createdAt.error();
    }
    auto parsedCreated = decode_time(*createdAt.value());
    if (!parsedCreated.has_value()) {
        return parsedCreated.error();
    }

    Plan plan{*planId, name.value(), scope.value(), *parsedStrategy, revision.value(), *parsedCreated};

    auto steps = want_member(value, key::kSteps);
    if (!steps.has_value()) {
        return steps.error();
    }
    if (steps.value()->kind() != json::Value::Kind::Array) {
        return errors::corrupt_state("record.plan", "plan steps are not an array");
    }
    for (const json::Value& entry : steps.value()->array()) {
        auto spec = step_spec_from_json(entry);
        if (!spec.has_value()) {
            return spec.error();
        }
        plan.add_step(*spec);
    }

    auto requirements = want_member(value, key::kRequirements);
    if (!requirements.has_value()) {
        return requirements.error();
    }
    if (requirements.value()->kind() != json::Value::Kind::Array) {
        return errors::corrupt_state("record.plan", "plan authority requirements are not an array");
    }
    for (const json::Value& entry : requirements.value()->array()) {
        auto domain = want_member(entry, key::kDomain);
        if (!domain.has_value()) {
            return domain.error();
        }
        auto parsedDomain = decode_domain(*domain.value());
        if (!parsedDomain.has_value()) {
            return parsedDomain.error();
        }
        auto reference = want_member(entry, "reference");
        if (!reference.has_value()) {
            return reference.error();
        }
        auto parsedReference = decode_authority(*reference.value());
        if (!parsedReference.has_value()) {
            return parsedReference.error();
        }
        plan.require_authority(*parsedDomain, *parsedReference);
    }

    auto freshness = want_member(value, key::kFreshness);
    if (!freshness.has_value()) {
        return freshness.error();
    }
    if (freshness.value()->kind() != json::Value::Kind::Array) {
        return errors::corrupt_state("record.plan", "plan freshness requirements are not an array");
    }
    for (const json::Value& entry : freshness.value()->array()) {
        auto domain = want_member(entry, key::kDomain);
        if (!domain.has_value()) {
            return domain.error();
        }
        auto parsedDomain = decode_domain(*domain.value());
        if (!parsedDomain.has_value()) {
            return parsedDomain.error();
        }
        auto maxAge = want_u64(entry, key::kMaxAge);
        if (!maxAge.has_value()) {
            return maxAge.error();
        }
        plan.require_freshness(*parsedDomain, Duration{maxAge.value()});
    }

    if (json::encode(plan_to_json(plan)) != json::encode(value)) {
        return errors::corrupt_state("record.plan", "a plan is not canonically encoded");
    }
    contentOut = plan_content_digest_of(value);
    return plan;
}

Digest plan_content_digest_of(const json::Value& planJson) {
    CanonicalWriter writer;
    append_framed(writer, "recovery.plan_content.v1");
    append_framed(writer, json::encode(planJson));
    return writer.digest_of();
}

json::Value observation_to_json(const Observation& observation) {
    json::Object object;
    object[key::kAuthority] = codec::encode(observation.authority());
    object[key::kDomain] = codec::encode(observation.domain());
    object[key::kGeneration] = json::Value{static_cast<unsigned long long>(observation.generation())};
    object[key::kId] = codec::encode(observation.id());
    object[key::kKey] = json::Value{observation.key()};
    object["observed_at"] = codec::encode(observation.observed_at());
    object["origin"] = json::Value{observation.origin()};
    object[key::kReading] = reading_to_json(observation.reading());
    object[key::kSource] = json::Value{std::string{to_string(observation.source())}};
    object[key::kStale] = json::Value{observation.stale()};
    return json::Value{std::move(object)};
}

Result<Observation> observation_from_json(const json::Value& value, EvidenceId assigned) {
    auto domain = want_member(value, key::kDomain);
    if (!domain.has_value()) {
        return domain.error();
    }
    auto parsedDomain = decode_domain(*domain.value());
    if (!parsedDomain.has_value()) {
        return parsedDomain.error();
    }
    auto generation = want_u64(value, key::kGeneration);
    if (!generation.has_value()) {
        return generation.error();
    }
    auto origin = want_string(value, "origin");
    if (!origin.has_value()) {
        return origin.error();
    }
    auto key = want_string(value, key::kKey);
    if (!key.has_value()) {
        return key.error();
    }
    auto reading = want_member(value, key::kReading);
    if (!reading.has_value()) {
        return reading.error();
    }
    auto parsedReading = reading_from_json(*reading.value());
    if (!parsedReading.has_value()) {
        return parsedReading.error();
    }
    auto observedAt = want_member(value, "observed_at");
    if (!observedAt.has_value()) {
        return observedAt.error();
    }
    auto parsedObserved = decode_time(*observedAt.value());
    if (!parsedObserved.has_value()) {
        return parsedObserved.error();
    }
    auto authority = want_member(value, key::kAuthority);
    if (!authority.has_value()) {
        return authority.error();
    }
    auto parsedAuthority = decode_authority(*authority.value());
    if (!parsedAuthority.has_value()) {
        return parsedAuthority.error();
    }
    auto stale = want_bool(value, key::kStale);
    if (!stale.has_value()) {
        return stale.error();
    }
    auto source = want_string(value, key::kSource);
    if (!source.has_value()) {
        return source.error();
    }
    auto parsedSource = observation_source_from_string(source.value());
    if (!parsedSource.has_value()) {
        return errors::corrupt_state("record.observation_source", "an observation source is not recognised");
    }
    auto id = want_member(value, key::kId);
    if (!id.has_value()) {
        return id.error();
    }
    auto parsedId = decode_evidence_id(*id.value());
    if (!parsedId.has_value()) {
        return parsedId.error();
    }
    if (assigned.is_zero()) {
        assigned = *parsedId;
    }
    Observation observation{assigned,          *parsedDomain,   generation.value(), *parsedSource,
                            origin.value(),    key.value(),     *parsedReading,    *parsedObserved,
                            *parsedAuthority,  stale.value()};
    // The evidence identity is content derived, so a decoded observation is
    // re-derived rather than trusted. A record whose identity does not match its
    // own content is a corruption, not an observation.
    CanonicalWriter identityWriter;
    append_framed(identityWriter, "recovery.evidence_identity.v1");
    append_framed(identityWriter, origin.value());
    append_framed(identityWriter, to_string(*parsedDomain));
    append_framed_u64(identityWriter, generation.value());
    append_framed(identityWriter, key.value());
    append_framed(identityWriter, parsedReading->canonical());
    append_framed(identityWriter, to_rfc3339(*parsedObserved));
    append_framed(identityWriter, parsedAuthority->canonical());
    const Digest derived = identityWriter.digest_of();
    std::array<std::uint8_t, Id::kBytes> derivedBytes{};
    std::copy(derived.bytes().begin(), derived.bytes().begin() + static_cast<std::ptrdiff_t>(Id::kBytes),
              derivedBytes.begin());
    if (derivedBytes != parsedId->id().bytes()) {
        return errors::corrupt_state("record.observation_identity",
                                     "the recorded evidence identity does not match its content");
    }
    return observation;
}

json::Value attempt_view_to_json(const AttemptView& view) {
    json::Object object;
    object[key::kAttempt] = codec::encode(view.id);
    object["ambiguous"] = json::Value{view.ambiguous};
    object["attempt_number"] = json::Value{static_cast<unsigned long long>(view.attempt_number)};
    object["acknowledged"] = json::Value{view.acknowledged};
    object[key::kBinding] = codec::encode(view.binding);
    object["detail"] = json::Value{view.detail};
    object["dispatch_count"] = json::Value{static_cast<unsigned long long>(view.dispatch_count)};
    object["effect"] = json::Value{std::string{to_string(view.effect)}};
    object[key::kKind] = json::Value{std::string{to_string(view.kind)}};
    object["observed"] = json::Value{view.observed};
    object[key::kStatus] = json::Value{std::string{to_string(view.status)}};
    object[key::kStep] = codec::encode(view.step);
    object["idempotency_key"] = json::Value{view.idempotency};
    object["epoch"] = json::Value{static_cast<unsigned long long>(view.epoch.value())};
    return json::Value{std::move(object)};
}

}  // namespace codec

// ---------------------------------------------------------------------------
// Attempt codec
// ---------------------------------------------------------------------------
// The durable attempt record is the unit of "the coordinator intended this
// external effect". It carries the exact request binding, so a restarted
// coordinator can reconstruct what it asked for without recomputing anything,
// and it carries the epoch it was issued under, so a successor can tell that
// the request was not its own.

json::Value AttemptCodec::to_json(const Attempt& attempt) {
    json::Object ack;
    ack["at"] = json::Value{to_rfc3339(attempt.acknowledgement().at)};
    ack["actor"] = json::Value{attempt.acknowledgement().actor};
    ack["detail"] = json::Value{attempt.acknowledgement().detail};
    ack["origin"] = json::Value{std::string{to_string(attempt.acknowledgement().origin)}};
    ack["present"] = json::Value{attempt.acknowledgement().present};

    json::Object observation;
    observation["at"] = json::Value{to_rfc3339(attempt.observation().at)};
    observation["evidence"] = json::Value{attempt.observation().evidence.to_text()};
    observation["origin"] = json::Value{std::string{to_string(attempt.observation().origin)}};
    observation["present"] = json::Value{attempt.observation().present};

    json::Object assessment;
    assessment["at"] = json::Value{to_rfc3339(attempt.assessment().at)};
    assessment["authority"] = json::Value{attempt.assessment().authority.canonical()};
    assessment["detail"] = json::Value{attempt.assessment().detail};
    assessment["evidence"] = json::Value{attempt.assessment().evidence.to_text()};
    assessment["expectation"] = json::Value{attempt.assessment().expectation.hex()};
    assessment["observed"] = json::Value{attempt.assessment().observed.hex()};
    assessment["status"] = json::Value{std::string{to_string(attempt.assessment().status)}};

    json::Object object;
    object["acknowledgement"] = json::Value{std::move(ack)};
    object["ambiguous"] = json::Value{attempt.ambiguous()};
    object["assessment"] = json::Value{std::move(assessment)};
    object["attempt"] = codec::encode(attempt.id());
    object["attempt_number"] = json::Value{static_cast<unsigned long long>(attempt.attempt_number())};
    object["binding"] = codec::encode(attempt.binding());
    object["binding_digest"] = json::Value{attempt.binding_digest().hex()};
    object["committed_at"] = codec::encode(attempt.committed_at());
    object["detail"] = json::Value{attempt.detail()};
    object["dispatch_count"] = json::Value{static_cast<unsigned long long>(attempt.dispatch_count())};
    object["dispatch_intent_at"] =
        attempt.dispatch_intent_at().has_value()
            ? codec::encode(*attempt.dispatch_intent_at())
            : json::Value{std::string{}};
    object["epoch"] = json::Value{static_cast<unsigned long long>(attempt.epoch().value())};
    object["execution"] = json::Value{attempt.execution().to_text()};
    object["idempotency_key"] = json::Value{attempt.idempotency()};
    object["kind"] = json::Value{std::string{to_string(attempt.kind())}};
    object["observation"] = json::Value{std::move(observation)};
    object["revision"] = json::Value{static_cast<unsigned long long>(attempt.plan_revision())};
    object["status"] = json::Value{std::string{to_string(attempt.status())}};
    object["step"] = codec::encode(attempt.step());
    return json::Value{std::move(object)};
}

Result<Attempt> AttemptCodec::from_committed_json(const json::Value& value) {
    auto id = codec::decode_attempt_id(*codec::want_member(value, "attempt").value());
    if (!id.has_value()) {
        return id.error();
    }
    auto execution = codec::decode_execution_id(*codec::want_member(value, "execution").value());
    if (!execution.has_value()) {
        return execution.error();
    }
    auto step = codec::decode_step_id(*codec::want_member(value, "step").value());
    if (!step.has_value()) {
        return step.error();
    }
    auto number = codec::want_u64(value, "attempt_number");
    if (!number.has_value()) {
        return number.error();
    }
    if (number.value() > 0xffffffffull) {
        return errors::corrupt_state("record.attempt_number", "an attempt number is out of range");
    }
    auto kind = codec::want_string(value, "kind");
    if (!kind.has_value()) {
        return kind.error();
    }
    auto parsedKind = attempt_kind_from_string(kind.value());
    if (!parsedKind.has_value()) {
        return errors::corrupt_state("record.attempt_kind", "an attempt kind is not recognised");
    }
    auto epoch = codec::want_u64(value, "epoch");
    if (!epoch.has_value()) {
        return epoch.error();
    }
    auto revision = codec::want_u64(value, "revision");
    if (!revision.has_value()) {
        return revision.error();
    }
    auto binding = codec::decode_binding(*codec::want_member(value, "binding").value());
    if (!binding.has_value()) {
        return binding.error();
    }
    auto bindingDigest = codec::decode_digest(*codec::want_member(value, "binding_digest").value());
    if (!bindingDigest.has_value()) {
        return bindingDigest.error();
    }
    if (binding->digest() != *bindingDigest) {
        return errors::corrupt_state("record.binding_digest", "the recorded binding digest does not match the binding");
    }
    auto idempotency = codec::want_string(value, "idempotency_key");
    if (!idempotency.has_value()) {
        return idempotency.error();
    }
    auto committedAt = codec::decode_time(*codec::want_member(value, "committed_at").value());
    if (!committedAt.has_value()) {
        return committedAt.error();
    }
    if (binding->step != *step) {
        return errors::corrupt_state("record.binding_step", "the attempt binding names a different step");
    }

    Attempt attempt{*id,
                    *execution,
                    *step,
                    static_cast<std::uint32_t>(number.value()),
                    *parsedKind,
                    Epoch{epoch.value()},
                    revision.value(),
                    *binding,
                    *bindingDigest,
                    idempotency.value(),
                    *committedAt};
    apply_state(attempt, value);
    return attempt;
}

void AttemptCodec::apply_state(Attempt& attempt, const json::Value& value) {
    auto status = codec::want_string(value, "status");
    if (status.has_value()) {
        const auto parsed = attempt_status_from_string(status.value());
        if (parsed.has_value()) {
            attempt.status_ = *parsed;
        }
    }
    auto detail = codec::want_string(value, "detail");
    if (detail.has_value()) {
        attempt.detail_ = detail.value();
    }
    auto ambiguous = codec::want_bool(value, "ambiguous");
    if (ambiguous.has_value()) {
        attempt.ambiguous_ = ambiguous.value();
    }
    auto dispatchCount = codec::want_u64(value, "dispatch_count");
    if (dispatchCount.has_value() && dispatchCount.value() <= 0xffffffffull) {
        attempt.dispatchCount_ = static_cast<std::uint32_t>(dispatchCount.value());
    }
    auto dispatchIntent = codec::want_string(value, "dispatch_intent_at");
    if (dispatchIntent.has_value() && !dispatchIntent.value().empty()) {
        const auto parsed = parse_rfc3339(dispatchIntent.value());
        if (parsed.has_value()) {
            attempt.dispatchIntentAt_ = *parsed;
        }
    }

    auto ack = codec::want_member(value, "acknowledgement");
    if (ack.has_value() && ack.value()->kind() == json::Value::Kind::Object) {
        auto present = codec::want_bool(*ack.value(), "present");
        if (present.has_value()) {
            attempt.acknowledgement_.present = present.value();
        }
        auto origin = codec::want_string(*ack.value(), "origin");
        if (origin.has_value()) {
            const auto parsed = attestation_origin_from_string(origin.value());
            if (parsed.has_value()) {
                attempt.acknowledgement_.origin = *parsed;
            }
        }
        auto actor = codec::want_string(*ack.value(), "actor");
        if (actor.has_value()) {
            attempt.acknowledgement_.actor = actor.value();
        }
        auto ackDetail = codec::want_string(*ack.value(), "detail");
        if (ackDetail.has_value()) {
            attempt.acknowledgement_.detail = ackDetail.value();
        }
        auto at = codec::want_string(*ack.value(), "at");
        if (at.has_value()) {
            const auto parsed = parse_rfc3339(at.value());
            if (parsed.has_value()) {
                attempt.acknowledgement_.at = *parsed;
            }
        }
    }

    auto observation = codec::want_member(value, "observation");
    if (observation.has_value() && observation.value()->kind() == json::Value::Kind::Object) {
        auto present = codec::want_bool(*observation.value(), "present");
        if (present.has_value()) {
            attempt.observation_.present = present.value();
        }
        auto evidence = codec::want_string(*observation.value(), "evidence");
        if (evidence.has_value() && !evidence.value().empty()) {
            const auto parsed = EvidenceId::parse(evidence.value());
            if (parsed.has_value()) {
                attempt.observation_.evidence = *parsed;
            }
        }
        auto origin = codec::want_string(*observation.value(), "origin");
        if (origin.has_value()) {
            const auto parsed = attestation_origin_from_string(origin.value());
            if (parsed.has_value()) {
                attempt.observation_.origin = *parsed;
            }
        }
        auto at = codec::want_string(*observation.value(), "at");
        if (at.has_value()) {
            const auto parsed = parse_rfc3339(at.value());
            if (parsed.has_value()) {
                attempt.observation_.at = *parsed;
            }
        }
    }

    auto assessment = codec::want_member(value, "assessment");
    if (assessment.has_value() && assessment.value()->kind() == json::Value::Kind::Object) {
        auto statusText = codec::want_string(*assessment.value(), "status");
        if (statusText.has_value()) {
            const auto parsed = effect_status_from_string(statusText.value());
            if (parsed.has_value()) {
                attempt.assessment_.status = *parsed;
            }
        }
        auto expectation = codec::want_string(*assessment.value(), "expectation");
        if (expectation.has_value()) {
            const auto parsed = Digest::parse(expectation.value());
            if (parsed.has_value()) {
                attempt.assessment_.expectation = *parsed;
            }
        }
        auto observed = codec::want_string(*assessment.value(), "observed");
        if (observed.has_value()) {
            const auto parsed = Digest::parse(observed.value());
            if (parsed.has_value()) {
                attempt.assessment_.observed = *parsed;
            }
        }
        auto authority = codec::want_string(*assessment.value(), "authority");
        if (authority.has_value()) {
            const auto parsed = AuthorityRef::from_canonical(authority.value());
            if (parsed.has_value()) {
                attempt.assessment_.authority = *parsed;
            }
        }
        auto evidence = codec::want_string(*assessment.value(), "evidence");
        if (evidence.has_value() && !evidence.value().empty()) {
            const auto parsed = EvidenceId::parse(evidence.value());
            if (parsed.has_value()) {
                attempt.assessment_.evidence = *parsed;
            }
        }
        auto at = codec::want_string(*assessment.value(), "at");
        if (at.has_value()) {
            const auto parsed = parse_rfc3339(at.value());
            if (parsed.has_value()) {
                attempt.assessment_.at = *parsed;
            }
        }
        auto assessmentDetail = codec::want_string(*assessment.value(), "detail");
        if (assessmentDetail.has_value()) {
            attempt.assessment_.detail = assessmentDetail.value();
        }
    }
}

}  // namespace recovery
