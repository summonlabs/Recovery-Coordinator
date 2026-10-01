#include "recovery/ports.hpp"

#include "recovery/canonical.hpp"
#include "recovery/sha256.hpp"

namespace recovery {

std::string_view to_string(RequestClass value) {
    switch (value) {
        case RequestClass::Consequential:
            return "consequential";
        case RequestClass::ReadOnly:
            return "read_only";
    }
    return "unknown";
}

std::string_view to_string(ResponseStatus value) {
    switch (value) {
        case ResponseStatus::Completed:
            return "completed";
        case ResponseStatus::InProgress:
            return "in_progress";
        case ResponseStatus::Refused:
            return "refused";
        case ResponseStatus::Failed:
            return "failed";
        case ResponseStatus::Unknown:
            return "unknown";
        case ResponseStatus::Indeterminate:
            return "indeterminate";
    }
    return "unknown";
}

std::optional<ResponseStatus> response_status_from_string(std::string_view text) {
    if (text == "completed") {
        return ResponseStatus::Completed;
    }
    if (text == "in_progress") {
        return ResponseStatus::InProgress;
    }
    if (text == "refused") {
        return ResponseStatus::Refused;
    }
    if (text == "failed") {
        return ResponseStatus::Failed;
    }
    if (text == "unknown") {
        return ResponseStatus::Unknown;
    }
    if (text == "indeterminate") {
        return ResponseStatus::Indeterminate;
    }
    return std::nullopt;
}

Request::Request(std::string domain, std::string operation, std::string parameters, Digest binding,
                 std::string idempotency, RequestContext context, bool pointOfNoReturn, bool compensating,
                 AttemptKind kind, std::string correlation)
    : domain_(std::move(domain)),
      operation_(std::move(operation)),
      parameters_(std::move(parameters)),
      bindingDigest_(binding),
      idempotency_(std::move(idempotency)),
      context_(std::move(context)),
      pointOfNoReturn_(pointOfNoReturn),
      compensating_(compensating),
      kind_(kind),
      correlation_(std::move(correlation)) {}

std::string Request::canonical() const {
    CanonicalWriter writer;
    writer.raw("{\"binding_digest\":");
    writer.digest(bindingDigest_);
    writer.raw(",\"compensating\":");
    writer.boolean(compensating_);
    writer.raw(",\"correlation\":");
    writer.quoted(correlation_);
    writer.raw(",\"domain\":");
    writer.quoted(domain_);
    writer.raw(",\"epoch\":");
    writer.unsigned_integer(context_.epoch().value());
    writer.raw(",\"idempotency_key\":");
    writer.quoted(idempotency_);
    writer.raw(",\"kind\":");
    writer.quoted(to_string(kind_));
    writer.raw(",\"operation\":");
    writer.quoted(operation_);
    writer.raw(",\"parameters\":");
    writer.quoted(parameters_);
    writer.raw(",\"point_of_no_return\":");
    writer.boolean(pointOfNoReturn_);
    writer.raw(",\"request_id\":");
    writer.quoted(context_.request_id());
    writer.raw("}");
    return writer.take();
}

Digest Request::digest() const {
    CanonicalWriter writer;
    append_framed(writer, "recovery.request.v1");
    append_framed(writer, canonical());
    return writer.digest_of();
}

AdapterObservation::AdapterObservation(Domain domain, std::uint64_t generation, std::string origin, std::string key,
                                       Reading reading, TimePoint observedAt, AuthorityRef authority, bool stale)
    : domain_(domain),
      generation_(generation),
      origin_(std::move(origin)),
      key_(std::move(key)),
      reading_(std::move(reading)),
      observedAt_(observedAt),
      authority_(std::move(authority)),
      stale_(stale) {}

std::string AdapterObservation::canonical() const {
    CanonicalWriter writer;
    writer.raw("{\"authority\":");
    writer.raw(authority_.canonical());
    writer.raw(",\"domain\":");
    writer.quoted(to_string(domain_));
    writer.raw(",\"generation\":");
    writer.unsigned_integer(generation_);
    writer.raw(",\"key\":");
    writer.quoted(key_);
    writer.raw(",\"observed_at\":");
    writer.quoted(to_rfc3339(observedAt_));
    writer.raw(",\"origin\":");
    writer.quoted(origin_);
    writer.raw(",\"reading\":");
    writer.raw(reading_.canonical());
    writer.raw(",\"stale\":");
    writer.boolean(stale_);
    writer.raw("}");
    return writer.take();
}

AdapterResponse AdapterResponse::completed(std::string detail) {
    AdapterResponse response;
    response.status_ = ResponseStatus::Completed;
    response.detail_ = std::move(detail);
    return response;
}

AdapterResponse AdapterResponse::in_progress(std::string detail) {
    AdapterResponse response;
    response.status_ = ResponseStatus::InProgress;
    response.detail_ = std::move(detail);
    return response;
}

AdapterResponse AdapterResponse::refused(std::string detail) {
    AdapterResponse response;
    response.status_ = ResponseStatus::Refused;
    response.detail_ = std::move(detail);
    return response;
}

AdapterResponse AdapterResponse::failed(std::string detail) {
    AdapterResponse response;
    response.status_ = ResponseStatus::Failed;
    response.detail_ = std::move(detail);
    return response;
}

AdapterResponse AdapterResponse::unknown(std::string detail) {
    AdapterResponse response;
    response.status_ = ResponseStatus::Unknown;
    response.detail_ = std::move(detail);
    return response;
}

AdapterResponse AdapterResponse::indeterminate(std::string detail) {
    AdapterResponse response;
    response.status_ = ResponseStatus::Indeterminate;
    response.detail_ = std::move(detail);
    return response;
}

void AdapterResponse::add_observation(AdapterObservation observation) {
    observations_.push_back(std::move(observation));
}

void AdapterResponse::add_effect_digest(Digest digest) { effectDigests_.push_back(digest); }

void AdapterResponse::add_expectation_key(std::string key) { expectationKeys_.push_back(std::move(key)); }

void AdapterResponse::add_assessment(EffectAssessment assessment) { assessments_.push_back(std::move(assessment)); }

std::string AdapterResponse::canonical() const {
    CanonicalWriter writer;
    writer.raw("{\"assessments\":[");
    for (std::size_t i = 0; i < assessments_.size(); ++i) {
        if (i != 0) {
            writer.raw(",");
        }
        const EffectAssessment& assessment = assessments_[i];
        writer.raw("{\"at\":");
        writer.quoted(to_rfc3339(assessment.at));
        writer.raw(",\"authority\":");
        writer.raw(assessment.authority.canonical());
        writer.raw(",\"detail\":");
        writer.quoted(assessment.detail);
        writer.raw(",\"evidence\":");
        writer.quoted(assessment.evidence.to_text());
        writer.raw(",\"expectation\":");
        writer.digest(assessment.expectation);
        writer.raw(",\"observed\":");
        writer.digest(assessment.observed);
        writer.raw(",\"status\":");
        writer.quoted(to_string(assessment.status));
        writer.raw("}");
    }
    writer.raw("],\"detail\":");
    writer.quoted(detail_);
    writer.raw(",\"effect_digests\":[");
    for (std::size_t i = 0; i < effectDigests_.size(); ++i) {
        if (i != 0) {
            writer.raw(",");
        }
        writer.digest(effectDigests_[i]);
    }
    writer.raw("],\"expectation_keys\":[");
    for (std::size_t i = 0; i < expectationKeys_.size(); ++i) {
        if (i != 0) {
            writer.raw(",");
        }
        writer.quoted(expectationKeys_[i]);
    }
    writer.raw("],\"observations\":[");
    for (std::size_t i = 0; i < observations_.size(); ++i) {
        if (i != 0) {
            writer.raw(",");
        }
        writer.raw(observations_[i].canonical());
    }
    writer.raw("],\"status\":");
    writer.quoted(to_string(status_));
    writer.raw("}");
    return writer.take();
}

Result<std::vector<JournalRecord>> JournalLog::read_records() const {
    if (journal_ == nullptr) {
        return errors::internal("log.no_journal", "the durable log has no journal");
    }
    return journal_->read_all();
}

Result<std::uint64_t> JournalLog::append_record(RecordKind kind, std::string_view payload) {
    if (poisoned_) {
        return errors::persistence("log.poisoned", "the durable log is no longer usable");
    }
    if (journal_ == nullptr) {
        return errors::internal("log.no_journal", "the durable log has no journal");
    }
    auto appended = journal_->append(kind, payload);
    if (!appended.has_value()) {
        poisoned_ = true;
        return appended.error();
    }
    return journal_->last_sequence();
}

std::uint64_t JournalLog::last_sequence() const { return journal_ == nullptr ? 0 : journal_->last_sequence(); }

Result<Epoch> JournalLog::record_claimed_epoch(Epoch epoch, std::string_view incarnation) {
    if (journal_ == nullptr) {
        return errors::internal("log.no_journal", "the durable log has no journal");
    }
    // The journal refuses an epoch that is not strictly greater than every epoch
    // its header has seen, so this is a second, header-level guard on top of the
    // coordinator's own epoch rule. Two independent guards on the same property
    // is the point: the header survives even when replay cannot parse a record.
    auto claimed = journal_->claim_epoch(epoch, incarnation);
    if (!claimed.has_value()) {
        poisoned_ = true;
        return claimed.error();
    }
    return claimed;
}

Result<std::vector<JournalRecord>> MemoryLog::read_records() const { return records_; }

Result<Epoch> MemoryLog::record_claimed_epoch(Epoch epoch, std::string_view incarnation) {
    if (epoch.value() <= claimedEpoch_.value()) {
        return errors::stale_authority("log.claimed_epoch",
                                       "the claimed epoch must be strictly greater than every epoch already claimed");
    }
    claimedEpoch_ = epoch;
    claimedIncarnation_.assign(incarnation);
    return claimedEpoch_;
}

Result<std::uint64_t> MemoryLog::append_record(RecordKind kind, std::string_view payload) {
    if (poisoned_) {
        return errors::persistence("log.poisoned", "the durable log is no longer usable");
    }
    if (lastSequence_ == 0xffffffffffffffffull) {
        poisoned_ = true;
        return errors::overflow("log.sequence", "the log sequence counter is exhausted");
    }
    JournalRecord record;
    record.sequence = lastSequence_ + 1;
    record.kind = kind;
    record.payload.assign(payload);
    Sha256 hasher;
    hasher.update(lastChain_.bytes().data(), Digest::kBytes);
    const std::uint8_t kindBytes[2] = {static_cast<std::uint8_t>(static_cast<std::uint16_t>(kind) & 0xffu),
                                       static_cast<std::uint8_t>((static_cast<std::uint16_t>(kind) >> 8) & 0xffu)};
    hasher.update(kindBytes, 2);
    hasher.update(record.payload.data(), record.payload.size());
    const std::uint8_t sequenceBytes[8] = {
        static_cast<std::uint8_t>(record.sequence & 0xffu),
        static_cast<std::uint8_t>((record.sequence >> 8) & 0xffu),
        static_cast<std::uint8_t>((record.sequence >> 16) & 0xffu),
        static_cast<std::uint8_t>((record.sequence >> 24) & 0xffu),
        static_cast<std::uint8_t>((record.sequence >> 32) & 0xffu),
        static_cast<std::uint8_t>((record.sequence >> 40) & 0xffu),
        static_cast<std::uint8_t>((record.sequence >> 48) & 0xffu),
        static_cast<std::uint8_t>((record.sequence >> 56) & 0xffu)};
    hasher.update(sequenceBytes, 8);
    record.chain = Digest{hasher.finish()};
    lastChain_ = record.chain;
    lastSequence_ = record.sequence;
    records_.push_back(std::move(record));
    return lastSequence_;
}

Result<bool> MemoryLog::verify_chain() const {
    Digest chain{};
    std::uint64_t sequence = 0;
    for (const JournalRecord& record : records_) {
        Sha256 hasher;
        hasher.update(chain.bytes().data(), Digest::kBytes);
        const std::uint8_t kindBytes[2] = {static_cast<std::uint8_t>(static_cast<std::uint16_t>(record.kind) & 0xffu),
                                           static_cast<std::uint8_t>(
                                               (static_cast<std::uint16_t>(record.kind) >> 8) & 0xffu)};
        hasher.update(kindBytes, 2);
        hasher.update(record.payload.data(), record.payload.size());
        const std::uint8_t sequenceBytes[8] = {
            static_cast<std::uint8_t>(record.sequence & 0xffu),
            static_cast<std::uint8_t>((record.sequence >> 8) & 0xffu),
            static_cast<std::uint8_t>((record.sequence >> 16) & 0xffu),
            static_cast<std::uint8_t>((record.sequence >> 24) & 0xffu),
            static_cast<std::uint8_t>((record.sequence >> 32) & 0xffu),
            static_cast<std::uint8_t>((record.sequence >> 40) & 0xffu),
            static_cast<std::uint8_t>((record.sequence >> 48) & 0xffu),
            static_cast<std::uint8_t>((record.sequence >> 56) & 0xffu)};
        hasher.update(sequenceBytes, 8);
        if (Digest{hasher.finish()} != record.chain || record.sequence != sequence + 1) {
            return false;
        }
        chain = record.chain;
        sequence = record.sequence;
    }
    return true;
}

}  // namespace recovery