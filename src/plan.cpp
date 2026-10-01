#include "recovery/plan.hpp"

#include <algorithm>
#include <set>

namespace recovery {

std::string_view to_string(PlanStrategy value) {
    switch (value) {
        case PlanStrategy::Assessment:
            return "assessment";
        case PlanStrategy::Immediate:
            return "immediate";
        case PlanStrategy::Staged:
            return "staged";
        case PlanStrategy::StagedWithRetry:
            return "staged_with_retry";
    }
    return "unknown";
}

std::optional<PlanStrategy> plan_strategy_from_string(std::string_view text) {
    if (text == "assessment") {
        return PlanStrategy::Assessment;
    }
    if (text == "immediate") {
        return PlanStrategy::Immediate;
    }
    if (text == "staged") {
        return PlanStrategy::Staged;
    }
    if (text == "staged_with_retry") {
        return PlanStrategy::StagedWithRetry;
    }
    return std::nullopt;
}

std::string_view to_string(PlanStatus value) {
    switch (value) {
        case PlanStatus::Draft:
            return "draft";
        case PlanStatus::Published:
            return "published";
        case PlanStatus::Superseded:
            return "superseded";
    }
    return "unknown";
}

std::string_view to_string(Lifecycle value) {
    switch (value) {
        case Lifecycle::Idle:
            return "idle";
        case Lifecycle::Running:
            return "running";
        case Lifecycle::Blocked:
            return "blocked";
        case Lifecycle::Stale:
            return "stale";
        case Lifecycle::Complete:
            return "complete";
        case Lifecycle::Failed:
            return "failed";
        case Lifecycle::Cancelled:
            return "cancelled";
        case Lifecycle::Abandoned:
            return "abandoned";
        case Lifecycle::Compensating:
            return "compensating";
        case Lifecycle::Compensated:
            return "compensated";
    }
    return "unknown";
}

bool is_terminal(Lifecycle value) {
    switch (value) {
        case Lifecycle::Complete:
        case Lifecycle::Failed:
        case Lifecycle::Cancelled:
        case Lifecycle::Abandoned:
        case Lifecycle::Compensated:
            return true;
        default:
            return false;
    }
}

std::string_view to_string(StepState value) {
    switch (value) {
        case StepState::Pending:
            return "pending";
        case StepState::Ready:
            return "ready";
        case StepState::Dispatched:
            return "dispatched";
        case StepState::Acknowledged:
            return "acknowledged";
        case StepState::Executing:
            return "executing";
        case StepState::Verified:
            return "verified";
        case StepState::Failed:
            return "failed";
        case StepState::Refused:
            return "refused";
        case StepState::Skipped:
            return "skipped";
        case StepState::Compensating:
            return "compensating";
        case StepState::Compensated:
            return "compensated";
        case StepState::Uncompensable:
            return "uncompensable";
    }
    return "unknown";
}

std::string_view to_string(EdgeOutcome value) {
    switch (value) {
        case EdgeOutcome::AwaitVerified:
            return "await_verified";
        case EdgeOutcome::AwaitResolved:
            return "await_resolved";
        case EdgeOutcome::OnFailure:
            return "on_failure";
    }
    return "unknown";
}

std::optional<EdgeOutcome> edge_outcome_from_string(std::string_view text) {
    if (text == "await_verified") {
        return EdgeOutcome::AwaitVerified;
    }
    if (text == "await_resolved") {
        return EdgeOutcome::AwaitResolved;
    }
    if (text == "on_failure") {
        return EdgeOutcome::OnFailure;
    }
    return std::nullopt;
}

std::string_view to_string(AttemptStatus value) {
    switch (value) {
        case AttemptStatus::Pending:
            return "pending";
        case AttemptStatus::Dispatched:
            return "dispatched";
        case AttemptStatus::EffectPresent:
            return "effect_present";
        case AttemptStatus::Failed:
            return "failed";
        case AttemptStatus::Refused:
            return "refused";
        case AttemptStatus::Unverifiable:
            return "unverifiable";
        case AttemptStatus::Superseded:
            return "superseded";
    }
    return "unknown";
}

std::optional<AttemptStatus> attempt_status_from_string(std::string_view text) {
    if (text == "pending") {
        return AttemptStatus::Pending;
    }
    if (text == "dispatched") {
        return AttemptStatus::Dispatched;
    }
    if (text == "effect_present") {
        return AttemptStatus::EffectPresent;
    }
    if (text == "failed") {
        return AttemptStatus::Failed;
    }
    if (text == "refused") {
        return AttemptStatus::Refused;
    }
    if (text == "unverifiable") {
        return AttemptStatus::Unverifiable;
    }
    if (text == "superseded") {
        return AttemptStatus::Superseded;
    }
    return std::nullopt;
}

bool is_resolved(AttemptStatus value) {
    switch (value) {
        case AttemptStatus::EffectPresent:
        case AttemptStatus::Failed:
        case AttemptStatus::Refused:
        case AttemptStatus::Superseded:
            return true;
        case AttemptStatus::Pending:
        case AttemptStatus::Dispatched:
        case AttemptStatus::Unverifiable:
            return false;
    }
    return false;
}

std::string_view to_string(AttemptKind value) {
    switch (value) {
        case AttemptKind::Forward:
            return "forward";
        case AttemptKind::Compensation:
            return "compensation";
    }
    return "unknown";
}

std::optional<AttemptKind> attempt_kind_from_string(std::string_view text) {
    if (text == "forward") {
        return AttemptKind::Forward;
    }
    if (text == "compensation") {
        return AttemptKind::Compensation;
    }
    return std::nullopt;
}

std::string_view to_string(AttestationOrigin value) {
    switch (value) {
        case AttestationOrigin::DispatchResponse:
            return "dispatch_response";
        case AttestationOrigin::Inspection:
            return "inspection";
        case AttestationOrigin::Presented:
            return "presented";
        case AttestationOrigin::Replay:
            return "replay";
    }
    return "unknown";
}

std::optional<AttestationOrigin> attestation_origin_from_string(std::string_view text) {
    if (text == "dispatch_response") {
        return AttestationOrigin::DispatchResponse;
    }
    if (text == "inspection") {
        return AttestationOrigin::Inspection;
    }
    if (text == "presented") {
        return AttestationOrigin::Presented;
    }
    if (text == "replay") {
        return AttestationOrigin::Replay;
    }
    return std::nullopt;
}

std::string_view to_string(EffectStatus value) {
    switch (value) {
        case EffectStatus::NotAssessed:
            return "not_assessed";
        case EffectStatus::Verified:
            return "verified";
        case EffectStatus::Contradicted:
            return "contradicted";
        case EffectStatus::Unverifiable:
            return "unverifiable";
    }
    return "unknown";
}

std::optional<EffectStatus> effect_status_from_string(std::string_view text) {
    if (text == "not_assessed") {
        return EffectStatus::NotAssessed;
    }
    if (text == "verified") {
        return EffectStatus::Verified;
    }
    if (text == "contradicted") {
        return EffectStatus::Contradicted;
    }
    if (text == "unverifiable") {
        return EffectStatus::Unverifiable;
    }
    return std::nullopt;
}

bool operator==(const ReadinessGate& lhs, const ReadinessGate& rhs) noexcept {
    if (lhs.domain != rhs.domain) {
        return false;
    }
    if (lhs.required_keys != rhs.required_keys) {
        return false;
    }
    if (lhs.max_age != rhs.max_age) {
        return false;
    }
    if (lhs.require_domain_authority != rhs.require_domain_authority) {
        return false;
    }
    if (lhs.expected.size() != rhs.expected.size()) {
        return false;
    }
    auto lhsIt = lhs.expected.begin();
    auto rhsIt = rhs.expected.begin();
    for (; lhsIt != lhs.expected.end(); ++lhsIt, ++rhsIt) {
        if (lhsIt->first != rhsIt->first || !(lhsIt->second == rhsIt->second)) {
            return false;
        }
    }
    return true;
}

bool operator==(const Acknowledgement& lhs, const Acknowledgement& rhs) noexcept {
    return lhs.present == rhs.present && lhs.origin == rhs.origin && lhs.actor == rhs.actor &&
           lhs.detail == rhs.detail && lhs.at == rhs.at;
}

bool operator==(const ObservationAttestation& lhs, const ObservationAttestation& rhs) noexcept {
    return lhs.present == rhs.present && lhs.origin == rhs.origin && lhs.evidence == rhs.evidence && lhs.at == rhs.at;
}

bool operator==(const EffectAssessment& lhs, const EffectAssessment& rhs) noexcept {
    return lhs.status == rhs.status && lhs.expectation == rhs.expectation && lhs.observed == rhs.observed &&
           lhs.authority == rhs.authority && lhs.evidence == rhs.evidence && lhs.at == rhs.at &&
           lhs.detail == rhs.detail;
}

std::string RequestBinding::canonical() const {
    CanonicalWriter writer;
    writer.raw("{\"binding\":");
    writer.unsigned_integer(binding);
    writer.raw(",\"domain\":");
    writer.quoted(to_string(domain));
    writer.raw(",\"operation\":");
    writer.quoted(operation);
    writer.raw(",\"parameters\":");
    writer.quoted(parameters);
    writer.raw(",\"point_of_no_return\":");
    writer.boolean(point_of_no_return);
    writer.raw(",\"step\":");
    writer.quoted(step.to_text());
    writer.raw("}");
    return writer.take();
}

Digest RequestBinding::digest() const {
    CanonicalWriter writer;
    append_framed(writer, "recovery.request_binding.v1");
    append_framed(writer, canonical());
    return writer.digest_of();
}

bool operator==(const RequestBinding& lhs, const RequestBinding& rhs) noexcept {
    return lhs.step == rhs.step && lhs.binding == rhs.binding && lhs.domain == rhs.domain &&
           lhs.operation == rhs.operation && lhs.parameters == rhs.parameters &&
           lhs.point_of_no_return == rhs.point_of_no_return;
}

bool operator<(const RequestBinding& lhs, const RequestBinding& rhs) noexcept {
    if (lhs.step != rhs.step) {
        return lhs.step < rhs.step;
    }
    if (lhs.binding != rhs.binding) {
        return lhs.binding < rhs.binding;
    }
    if (lhs.domain != rhs.domain) {
        return lhs.domain < rhs.domain;
    }
    if (lhs.operation != rhs.operation) {
        return lhs.operation < rhs.operation;
    }
    return lhs.parameters < rhs.parameters;
}

std::string idempotency_key(const ExecutionId& execution, const StepId& step, std::uint32_t attempt_number,
                            const RequestBinding& binding, AttemptKind kind) {
    CanonicalWriter writer;
    append_framed(writer, "recovery.idempotency_key.v1");
    append_framed(writer, execution.to_text());
    append_framed(writer, step.to_text());
    append_framed_u64(writer, attempt_number);
    append_framed(writer, to_string(kind));
    append_framed(writer, binding.digest().hex());
    const Digest digest = writer.digest_of();
    // The key is the full digest: adjacent authorities are expected to store it
    // verbatim. Truncating it would trade a real collision risk for cosmetics.
    std::string key = "rc1-";
    key.append(execution.to_text());
    key.push_back('-');
    key.append(std::to_string(attempt_number));
    key.push_back('-');
    key.append(digest.hex());
    return key;
}

Attempt::Attempt(AttemptId id, ExecutionId execution, StepId step, std::uint32_t attempt_number,
                 AttemptKind kind, Epoch epoch, std::uint64_t plan_revision, RequestBinding binding,
                 Digest binding_digest, std::string idempotency, TimePoint committed_at)
    : id_(id),
      execution_(execution),
      step_(step),
      attemptNumber_(attempt_number),
      kind_(kind),
      epoch_(epoch),
      planRevision_(plan_revision),
      binding_(std::move(binding)),
      bindingDigest_(binding_digest),
      idempotency_(std::move(idempotency)),
      committedAt_(committed_at) {}

Plan::Plan(PlanId id, std::string name, std::string scope, PlanStrategy strategy, std::uint64_t revision,
           TimePoint created_at)
    : id_(id),
      name_(std::move(name)),
      scope_(std::move(scope)),
      strategy_(strategy),
      revision_(revision),
      createdAt_(created_at) {}

void Plan::add_step(StepSpec spec) { steps_.push_back(std::move(spec)); }

void Plan::require_authority(Domain domain, AuthorityRef reference) {
    authorityRequirements_[domain] = std::move(reference);
}

void Plan::require_freshness(Domain domain, Duration freshness) {
    freshnessRequirements_[domain] = freshness;
}

const StepSpec* Plan::find_step(const StepId& id) const {
    for (const StepSpec& step : steps_) {
        if (step.id == id) {
            return &step;
        }
    }
    return nullptr;
}

std::string Plan::canonical() const {
    CanonicalWriter writer;
    writer.raw("{\"authority\":[");
    bool first = true;
    for (const auto& [domain, reference] : authorityRequirements_) {
        if (!first) {
            writer.raw(",");
        }
        first = false;
        writer.raw("{\"domain\":");
        writer.quoted(to_string(domain));
        writer.raw(",\"reference\":");
        writer.raw(reference.canonical());
        writer.raw("}");
    }
    writer.raw("],\"created_at\":");
    writer.quoted(to_rfc3339(createdAt_));
    writer.raw(",\"freshness\":[");
    first = true;
    for (const auto& [domain, freshness] : freshnessRequirements_) {
        if (!first) {
            writer.raw(",");
        }
        first = false;
        writer.raw("{\"domain\":");
        writer.quoted(to_string(domain));
        writer.raw(",\"max_age_nanos\":");
        writer.unsigned_integer(freshness.nanos());
        writer.raw("}");
    }
    writer.raw("],\"id\":");
    writer.quoted(id_.to_text());
    writer.raw(",\"name\":");
    writer.quoted(name_);
    writer.raw(",\"revision\":");
    writer.unsigned_integer(revision_);
    writer.raw(",\"scope\":");
    writer.quoted(scope_);
    writer.raw(",\"status\":");
    writer.quoted(to_string(status_));
    writer.raw(",\"steps\":[");
    for (std::size_t i = 0; i < steps_.size(); ++i) {
        if (i != 0) {
            writer.raw(",");
        }
        const StepSpec& step = steps_[i];
        writer.raw("{\"compensable\":");
        writer.boolean(step.compensable);
        writer.raw(",\"compensation_required\":");
        writer.boolean(step.compensation_required);
        writer.raw(",\"compensation_requests\":[");
        for (std::size_t j = 0; j < step.compensation_requests.size(); ++j) {
            if (j != 0) {
                writer.raw(",");
            }
            writer.raw(step.compensation_requests[j].canonical());
        }
        writer.raw("],\"deadline\":");
        writer.quoted(to_rfc3339(step.deadline));
        writer.raw(",\"evidence_domains\":[");
        for (std::size_t j = 0; j < step.evidence_domains.size(); ++j) {
            if (j != 0) {
                writer.raw(",");
            }
            writer.quoted(to_string(step.evidence_domains[j]));
        }
        writer.raw("],\"gates\":[");
        for (std::size_t j = 0; j < step.gates.size(); ++j) {
            if (j != 0) {
                writer.raw(",");
            }
            const ReadinessGate& gate = step.gates[j];
            writer.raw("{\"domain\":");
            writer.quoted(to_string(gate.domain));
            writer.raw(",\"expected\":{");
            bool firstExpected = true;
            for (const auto& [key, reading] : gate.expected) {
                if (!firstExpected) {
                    writer.raw(",");
                }
                firstExpected = false;
                writer.quoted(key);
                writer.raw(":");
                writer.raw(reading.canonical());
            }
            writer.raw("},\"max_age_nanos\":");
            writer.unsigned_integer(gate.max_age.nanos());
            writer.raw(",\"require_domain_authority\":");
            writer.boolean(gate.require_domain_authority);
            writer.raw(",\"required_keys\":[");
            for (std::size_t k = 0; k < gate.required_keys.size(); ++k) {
                if (k != 0) {
                    writer.raw(",");
                }
                writer.quoted(gate.required_keys[k]);
            }
            writer.raw("]}");
        }
        writer.raw("],\"id\":");
        writer.quoted(step.id.to_text());
        writer.raw(",\"name\":");
        writer.quoted(step.name);
        writer.raw(",\"requests\":[");
        for (std::size_t j = 0; j < step.requests.size(); ++j) {
            if (j != 0) {
                writer.raw(",");
            }
            writer.raw(step.requests[j].canonical());
        }
        writer.raw("],\"requires\":[");
        for (std::size_t j = 0; j < step.depends_on.size(); ++j) {
            if (j != 0) {
                writer.raw(",");
            }
            writer.raw("{\"outcome\":");
            writer.quoted(to_string(step.depends_on[j].outcome));
            writer.raw(",\"predecessor\":");
            writer.quoted(step.depends_on[j].predecessor.to_text());
            writer.raw("}");
        }
        writer.raw("]}");
    }
    writer.raw("],\"strategy\":");
    writer.quoted(to_string(strategy_));
    writer.raw("}");
    return writer.take();
}

Digest Plan::content_digest() const {
    CanonicalWriter writer;
    append_framed(writer, "recovery.plan_content.v1");
    append_framed(writer, canonical());
    return writer.digest_of();
}

std::optional<std::size_t> step_position(const Plan& plan, const StepId& step) {
    const std::vector<StepSpec>& steps = plan.steps();
    for (std::size_t i = 0; i < steps.size(); ++i) {
        if (steps[i].id == step) {
            return i;
        }
    }
    return std::nullopt;
}

std::vector<StepId> dependents_of(const Plan& plan, const StepId& step) {
    std::vector<StepId> out;
    for (const StepSpec& candidate : plan.steps()) {
        for (const Dependency& dependency : candidate.depends_on) {
            if (dependency.predecessor == step) {
                out.push_back(candidate.id);
                break;
            }
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

Result<std::vector<StepId>> topological_order(const Plan& plan) {
    const std::vector<StepSpec>& steps = plan.steps();
    std::map<StepId, std::size_t> indegree;
    std::map<StepId, std::vector<StepId>> successors;
    for (const StepSpec& step : steps) {
        indegree.emplace(step.id, 0);
    }
    for (const StepSpec& step : steps) {
        std::set<StepId> unique;
        for (const Dependency& dependency : step.depends_on) {
            if (indegree.find(dependency.predecessor) == indegree.end()) {
                return errors::precondition_failed("plan.unknown_predecessor",
                                                    "step references a predecessor that is not in the plan");
            }
            if (!unique.insert(dependency.predecessor).second) {
                continue;
            }
            successors[dependency.predecessor].push_back(step.id);
            ++indegree[step.id];
        }
    }
    for (auto& [id, list] : successors) {
        (void)id;
        std::sort(list.begin(), list.end());
    }

    // A deterministic ready set: always emit the smallest ready identity. This
    // is a pure function of the plan content.
    std::set<StepId> ready;
    for (const auto& [id, degree] : indegree) {
        if (degree == 0) {
            ready.insert(id);
        }
    }

    std::vector<StepId> order;
    order.reserve(steps.size());
    while (!ready.empty()) {
        const StepId current = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(current);
        for (const StepId& successor : successors[current]) {
            auto it = indegree.find(successor);
            if (it->second > 0) {
                --it->second;
            }
            if (it->second == 0) {
                ready.insert(successor);
            }
        }
    }

    if (order.size() != steps.size()) {
        return errors::identity_conflict("plan.dependency_cycle", "the dependency graph contains a cycle");
    }
    return order;
}

namespace {

[[nodiscard]] Error step_error(std::string_view rule, const StepId& step, std::string detail) {
    std::string message = "step ";
    message.append(step.to_text());
    message.append(": ");
    message.append(detail);
    return errors::invalid_argument(std::string{rule}, std::move(message));
}

}  // namespace

Result<PlanId> validate_plan_shape(Plan& plan, const PlanValidationOptions& options) {
    if (plan.id().is_zero()) {
        return errors::invalid_argument("plan.identity", "plan identity is zero");
    }
    if (plan.name().empty()) {
        return errors::invalid_argument("plan.name", "plan name is empty");
    }
    if (plan.scope().empty()) {
        return errors::invalid_argument("plan.scope", "plan scope is empty");
    }
    if (!is_valid_utf8(plan.name()) || !is_valid_utf8(plan.scope())) {
        return errors::invalid_argument("plan.encoding", "plan name or scope is not valid UTF-8");
    }
    const std::vector<StepSpec>& steps = plan.steps();
    if (steps.empty()) {
        return errors::invalid_argument("plan.steps", "plan contains no steps");
    }
    if (steps.size() > options.max_steps) {
        return errors::limit_exceeded("plan.step_count", "plan exceeds the maximum step count");
    }

    std::set<StepId> identities;
    for (const StepSpec& step : steps) {
        if (step.id.is_zero()) {
            return errors::invalid_argument("plan.step_identity", "step identity is zero");
        }
        if (!identities.insert(step.id).second) {
            return errors::identity_conflict("plan.duplicate_step", "two steps share one identity");
        }
        if (step.name.empty()) {
            return step_error("plan.step_name", step.id, "step name is empty");
        }
        if (!is_valid_utf8(step.name)) {
            return step_error("plan.step_encoding", step.id, "step name is not valid UTF-8");
        }
        if (step.depends_on.size() > options.max_dependencies_per_step) {
            return step_error("plan.dependency_count", step.id, "step exceeds the dependency limit");
        }
        if (step.requests.size() > options.max_requests_per_step) {
            return step_error("plan.request_count", step.id, "step exceeds the request limit");
        }
        if (step.compensation_requests.size() > options.max_compensation_requests_per_step) {
            return step_error("plan.compensation_count", step.id, "step exceeds the compensation request limit");
        }
        if (step.evidence_domains.size() > options.max_evidence_domains_per_step) {
            return step_error("plan.evidence_domain_count", step.id, "step exceeds the evidence domain limit");
        }
        if (step.compensable && step.compensation_requests.empty()) {
            return step_error("plan.compensation_missing", step.id,
                              "step is marked compensable but declares no compensation request");
        }
        if (!step.compensable && !step.compensation_requests.empty()) {
            return step_error("plan.compensation_unused", step.id,
                              "step declares compensation requests but is not marked compensable");
        }

        std::set<std::uint32_t> bindingNumbers;
        for (const RequestBinding& binding : step.requests) {
            if (binding.step != step.id) {
                return step_error("plan.binding_step", step.id, "request binding names a different step");
            }
            if (!bindingNumbers.insert(binding.binding).second) {
                return step_error("plan.binding_number", step.id, "two request bindings share one binding number");
            }
            if (binding.operation.empty()) {
                return step_error("plan.binding_operation", step.id, "request binding has an empty operation");
            }
            if (!is_valid_utf8(binding.operation)) {
                return step_error("plan.binding_encoding", step.id, "request binding operation is not valid UTF-8");
            }
            if (binding.parameters.size() > options.max_parameters_bytes) {
                return step_error("plan.binding_parameters", step.id, "request binding parameters exceed the limit");
            }
            if (!binding.parameters.empty() && binding.parameters.front() != '{') {
                return step_error("plan.binding_parameters", step.id,
                                  "request binding parameters must be canonical JSON object text");
            }
        }
        for (const RequestBinding& binding : step.compensation_requests) {
            if (binding.step != step.id) {
                return step_error("plan.compensation_step", step.id, "compensation binding names a different step");
            }
            if (binding.operation.empty()) {
                return step_error("plan.compensation_operation", step.id, "compensation binding has an empty operation");
            }
            if (binding.point_of_no_return) {
                return step_error("plan.compensation_pnr", step.id,
                                  "a compensation request cannot itself be a point of no return");
            }
            if (binding.parameters.size() > options.max_parameters_bytes) {
                return step_error("plan.compensation_parameters", step.id,
                                  "compensation binding parameters exceed the limit");
            }
        }
        for (const ReadinessGate& gate : step.gates) {
            if (gate.required_keys.empty() && gate.expected.empty()) {
                return step_error("plan.gate_empty", step.id, "readiness gate requires nothing");
            }
            for (const std::string& key : gate.required_keys) {
                if (key.empty()) {
                    return step_error("plan.gate_key", step.id, "readiness gate has an empty required key");
                }
                if (!is_valid_utf8(key)) {
                    return step_error("plan.gate_encoding", step.id, "readiness gate key is not valid UTF-8");
                }
            }
            for (const auto& [key, reading] : gate.expected) {
                (void)reading;
                if (key.empty() || !is_valid_utf8(key)) {
                    return step_error("plan.gate_expected_key", step.id, "readiness gate expectation key is invalid");
                }
            }
        }
        std::set<Domain> domains;
        for (const Domain domain : step.evidence_domains) {
            if (!domains.insert(domain).second) {
                return step_error("plan.evidence_domain_duplicate", step.id,
                                  "step lists one evidence domain twice");
            }
        }
    }

    for (const StepSpec& step : steps) {
        std::set<StepId> predecessors;
        for (const Dependency& dependency : step.depends_on) {
            if (dependency.predecessor == step.id) {
                return step_error("plan.self_dependency", step.id, "step depends on itself");
            }
            if (identities.find(dependency.predecessor) == identities.end()) {
                return step_error("plan.unknown_predecessor", step.id, "step depends on an unknown predecessor");
            }
            if (!predecessors.insert(dependency.predecessor).second) {
                return step_error("plan.duplicate_dependency", step.id,
                                  "step declares two edges to the same predecessor");
            }
        }
    }

    auto order = topological_order(plan);
    if (!order.has_value()) {
        return order.error();
    }
    return plan.id();
}

Result<Digest> validate_plan_authority(const Plan& plan, const ObservationStore& evidence, TimePoint now) {
    // A plan may only be bound to authority the coordinator can currently see,
    // and the binding must match what the caller claims it saw. This is the one
    // place where a plan acquires authority references; after this point the
    // references are immutable for the life of the plan.
    for (const auto& [domain, requirement] : plan.authority_requirements()) {
        const EvidenceView& view = evidence.view(domain);
        if (!view.established()) {
            return errors::stale_authority("plan.authority_unestablished",
                                           "no evidence has been accepted for a required domain");
        }
        if (view.authority() != requirement) {
            return errors::stale_authority("plan.authority_mismatch",
                                           "accepted evidence does not match the plan authority requirement");
        }
        const auto freshness = plan.freshness_requirements().find(domain);
        const Duration bound = freshness == plan.freshness_requirements().end() ? Duration{} : freshness->second;
        if (!view.is_current(now, bound)) {
            return errors::stale_authority("plan.authority_stale",
                                           "accepted evidence for a required domain is outside its freshness bound");
        }
    }
    return plan.content_digest();
}

}  // namespace recovery