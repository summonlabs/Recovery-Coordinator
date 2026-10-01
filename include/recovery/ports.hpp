#ifndef RECOVERY_PORTS_HPP
#define RECOVERY_PORTS_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "recovery/digest.hpp"
#include "recovery/epoch.hpp"
#include "recovery/error.hpp"
#include "recovery/id.hpp"
#include "recovery/journal.hpp"
#include "recovery/observation.hpp"
#include "recovery/plan.hpp"
#include "recovery/time.hpp"

namespace recovery {

// ---------------------------------------------------------------------------
// Ports
// ---------------------------------------------------------------------------
// The coordinator owns no device. It asks adjacent authorities to act, and it
// asks them what the world looks like. Both directions cross exactly one
// narrow interface, and both are bounded by the identity the coordinator is
// allowed to reveal about itself.

// What the coordinator is allowed to disclose while producing one effect.
// A request carries no plan identifiers: an adjacent authority may recognise
// an idempotency key, but it is not told which recovery plan produced it.
class RequestContext {
public:
    RequestContext(std::string requestId, Epoch epoch, std::string idempotency, std::string correlation)
        : requestId_(std::move(requestId)),
          epoch_(epoch),
          idempotency_(std::move(idempotency)),
          correlation_(std::move(correlation)) {}

    [[nodiscard]] const std::string& request_id() const noexcept { return requestId_; }
    [[nodiscard]] Epoch epoch() const noexcept { return epoch_; }
    [[nodiscard]] const std::string& idempotency_key() const noexcept { return idempotency_; }
    [[nodiscard]] const std::string& correlation() const noexcept { return correlation_; }

private:
    std::string requestId_;
    Epoch epoch_;
    std::string idempotency_;
    std::string correlation_;
};

enum class RequestClass : std::uint8_t {
    // The operation changes facility state. The coordinator records a durable
    // attempt before it is dispatched and verifies the effect afterwards.
    Consequential = 1,
    // The operation only reads. A read cannot be a point of no return and is
    // never compensated.
    ReadOnly = 2,
};

[[nodiscard]] std::string_view to_string(RequestClass value);

class Request {
public:
    Request() = default;

    Request(std::string domain, std::string operation, std::string parameters, Digest binding,
            std::string idempotency, RequestContext context, bool pointOfNoReturn, bool compensating, AttemptKind kind,
            std::string correlation);

    [[nodiscard]] const std::string& domain() const noexcept { return domain_; }
    [[nodiscard]] const std::string& operation() const noexcept { return operation_; }
    [[nodiscard]] const std::string& parameters() const noexcept { return parameters_; }
    [[nodiscard]] const Digest& binding_digest() const noexcept { return bindingDigest_; }
    [[nodiscard]] const std::string& idempotency_key() const noexcept { return idempotency_; }
    [[nodiscard]] const RequestContext& context() const noexcept { return context_; }
    [[nodiscard]] bool point_of_no_return() const noexcept { return pointOfNoReturn_; }
    [[nodiscard]] bool compensating() const noexcept { return compensating_; }
    [[nodiscard]] AttemptKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& correlation() const noexcept { return correlation_; }

    [[nodiscard]] std::string canonical() const;
    [[nodiscard]] Digest digest() const;

private:
    std::string domain_;
    std::string operation_;
    std::string parameters_;
    Digest bindingDigest_{};
    std::string idempotency_;
    RequestContext context_{std::string{}, Epoch{}, std::string{}, std::string{}};
    bool pointOfNoReturn_{false};
    bool compensating_{false};
    AttemptKind kind_{AttemptKind::Forward};
    std::string correlation_;
};

enum class ResponseStatus : std::uint8_t {
    // The adjacent authority reports the operation produced its intended
    // effect. Still only a claim until an observation confirms it.
    Completed = 1,
    // The operation is under way.
    InProgress = 2,
    // The adjacent authority declined to perform the request.
    Refused = 3,
    // The adjacent authority reports the operation is impossible or failed.
    Failed = 4,
    // The adjacent authority has no record of the key.
    Unknown = 5,
    // The adjacent authority could not produce a definite outcome in time.
    Indeterminate = 6,
};

[[nodiscard]] std::string_view to_string(ResponseStatus value);
[[nodiscard]] std::optional<ResponseStatus> response_status_from_string(std::string_view text);

// An observation the coordinator is willing to accept, produced by an adjacent
// authority. The coordinator assigns the evidence identity; the authority only
// describes what it saw.
class AdapterObservation {
public:
    AdapterObservation() = default;

    AdapterObservation(Domain domain, std::uint64_t generation, std::string origin, std::string key, Reading reading,
                       TimePoint observedAt, AuthorityRef authority, bool stale);

    [[nodiscard]] Domain domain() const noexcept { return domain_; }
    [[nodiscard]] std::uint64_t generation() const noexcept { return generation_; }
    [[nodiscard]] const std::string& origin() const noexcept { return origin_; }
    [[nodiscard]] const std::string& key() const noexcept { return key_; }
    [[nodiscard]] const Reading& reading() const noexcept { return reading_; }
    [[nodiscard]] TimePoint observed_at() const noexcept { return observedAt_; }
    [[nodiscard]] const AuthorityRef& authority() const noexcept { return authority_; }
    [[nodiscard]] bool stale() const noexcept { return stale_; }

    [[nodiscard]] std::string canonical() const;

private:
    Domain domain_{Domain::Power};
    std::uint64_t generation_{0};
    std::string origin_;
    std::string key_;
    Reading reading_{};
    TimePoint observedAt_{};
    AuthorityRef authority_{};
    bool stale_{false};
};

class AdapterResponse {
public:
    AdapterResponse() = default;

    [[nodiscard]] static AdapterResponse completed(std::string detail);
    [[nodiscard]] static AdapterResponse in_progress(std::string detail);
    [[nodiscard]] static AdapterResponse refused(std::string detail);
    [[nodiscard]] static AdapterResponse failed(std::string detail);
    [[nodiscard]] static AdapterResponse unknown(std::string detail);
    [[nodiscard]] static AdapterResponse indeterminate(std::string detail);

    [[nodiscard]] ResponseStatus status() const noexcept { return status_; }
    [[nodiscard]] const std::string& detail() const noexcept { return detail_; }
    [[nodiscard]] const std::vector<AdapterObservation>& observations() const noexcept { return observations_; }
    [[nodiscard]] const std::vector<Digest>& effect_digests() const noexcept { return effectDigests_; }
    [[nodiscard]] const std::vector<std::string>& expectation_keys() const noexcept { return expectationKeys_; }
    [[nodiscard]] const std::vector<EffectAssessment>& assessments() const noexcept { return assessments_; }

    void add_observation(AdapterObservation observation);
    void add_effect_digest(Digest digest);
    void add_expectation_key(std::string key);
    void add_assessment(EffectAssessment assessment);

    [[nodiscard]] std::string canonical() const;

private:
    ResponseStatus status_{ResponseStatus::Indeterminate};
    std::string detail_;
    std::vector<AdapterObservation> observations_;
    std::vector<Digest> effectDigests_;
    std::vector<std::string> expectationKeys_;
    std::vector<EffectAssessment> assessments_;
};

// The one interface through which the coordinator touches the outside world.
//
// Contract:
//   * dispatch() must be idempotent per idempotency key: presenting the same
//     key twice must not produce the effect twice.
//   * inspect() must answer Unknown when the authority has no record of the
//     key. The coordinator relies on Unknown to decide whether re-dispatching
//     is provably safe, so a wrong Unknown is a correctness defect.
//   * Neither call may re-enter the coordinator. This is enforced: the engine
//     holds its own state lock across both calls and refuses re-entrant calls
//     with an internal error.
class AdjacentAuthorityPort {
public:
    virtual ~AdjacentAuthorityPort() = default;

    [[nodiscard]] virtual std::string authority_name() const = 0;

    [[nodiscard]] virtual Result<AdapterResponse> dispatch(const Request& request) = 0;
    [[nodiscard]] virtual Result<AdapterResponse> inspect(const Request& request) = 0;

    // Fresh evidence for one domain. Returns a plain error when the authority
    // cannot currently speak for that domain.
    [[nodiscard]] virtual Result<AdapterResponse> observe(const Request& request, Domain domain) = 0;

    // An authority reference as currently held by that authority. Used to
    // re-establish freshness after a restart.
    [[nodiscard]] virtual Result<AuthorityRef> current_authority(Domain domain) const = 0;
};

// ---------------------------------------------------------------------------
// Durable log
// ---------------------------------------------------------------------------
// The engine routes every state change through one of these before it is
// visible in memory. An implementation that returns an error causes the
// coordinator to stop applying work rather than continue on unrecorded state.
class DurableLog {
public:
    virtual ~DurableLog() = default;

    [[nodiscard]] virtual Result<std::uint64_t> append_record(RecordKind kind, std::string_view payload) = 0;
    [[nodiscard]] virtual std::uint64_t last_sequence() const = 0;
    [[nodiscard]] virtual bool poisoned() const = 0;

    // The complete committed record stream. Recovery rebuilds authoritative
    // state by replaying exactly these records, so an implementation that
    // cannot produce them must fail loudly rather than return an empty stream.
    [[nodiscard]] virtual Result<std::vector<JournalRecord>> read_records() const {
        return errors::unsupported("log.read", "this durable log cannot be replayed");
    }

    // Records the epoch this incarnation claimed, so that a successor can learn
    // the highest epoch this medium has ever seen without reinterpreting every
    // payload. A medium that cannot carry this out of band returns Unsupported
    // and the coordinator carries on: the BeginEpoch record is always the
    // authoritative statement, and this is a convenience over it, never a
    // substitute for it.
    [[nodiscard]] virtual Result<Epoch> record_claimed_epoch(Epoch epoch, std::string_view incarnation) {
        (void)epoch;
        (void)incarnation;
        return errors::unsupported("log.claimed_epoch", "this durable log does not record a claimed epoch");
    }
};

// Journal-backed durable log. One journal, one writer.
class JournalLog final : public DurableLog {
public:
    explicit JournalLog(Journal& journal) : journal_(&journal) {}

    [[nodiscard]] Result<std::uint64_t> append_record(RecordKind kind, std::string_view payload) override;
    [[nodiscard]] std::uint64_t last_sequence() const override;
    [[nodiscard]] bool poisoned() const override { return poisoned_; }
    [[nodiscard]] Result<std::vector<JournalRecord>> read_records() const override;
    [[nodiscard]] Result<Epoch> record_claimed_epoch(Epoch epoch, std::string_view incarnation) override;

private:
    Journal* journal_{nullptr};
    bool poisoned_{false};
};

// In-memory durable log. It is deterministic and append-only, which makes it
// suitable for tests and for dry runs that must not touch durable state. It is
// never a substitute for the journal when a durability claim is being made, and
// the coordinator reports it as such.
class MemoryLog final : public DurableLog {
public:
    [[nodiscard]] Result<std::uint64_t> append_record(RecordKind kind, std::string_view payload) override;
    [[nodiscard]] std::uint64_t last_sequence() const override { return lastSequence_; }
    [[nodiscard]] bool poisoned() const override { return poisoned_; }
    [[nodiscard]] Result<std::vector<JournalRecord>> read_records() const override;
    [[nodiscard]] Result<Epoch> record_claimed_epoch(Epoch epoch, std::string_view incarnation) override;

    [[nodiscard]] const std::vector<JournalRecord>& records() const noexcept { return records_; }
    [[nodiscard]] Epoch claimed_epoch() const noexcept { return claimedEpoch_; }
    [[nodiscard]] const std::string& claimed_incarnation() const noexcept { return claimedIncarnation_; }
    [[nodiscard]] Result<bool> verify_chain() const;

    // Test support: simulate a durable-media failure.
    void poison() { poisoned_ = true; }

private:
    std::vector<JournalRecord> records_{};
    std::uint64_t lastSequence_{0};
    Digest lastChain_{};
    bool poisoned_{false};
    Epoch claimedEpoch_{};
    std::string claimedIncarnation_{};
};

// A clock the engine is allowed to read. The engine never calls the system
// clock on its own, so a deterministic clock makes the whole coordinator
// reproducible.
class Clock {
public:
    virtual ~Clock() = default;
    [[nodiscard]] virtual TimePoint now() const = 0;
};

// Fixed clock: returns the same instant until it is advanced. Intended for
// deterministic tests and for replay.
class FixedClock final : public Clock {
public:
    explicit FixedClock(TimePoint start) : now_(start) {}
    [[nodiscard]] TimePoint now() const override { return now_; }
    void advance(Duration delta) {
        const auto next = now_.add(delta);
        if (next.has_value()) {
            now_ = *next;
        }
    }
    void set(TimePoint value) { now_ = value; }

private:
    TimePoint now_{};
};

}  // namespace recovery

#endif  // RECOVERY_PORTS_HPP