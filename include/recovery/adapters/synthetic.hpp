#ifndef RECOVERY_ADAPTERS_SYNTHETIC_HPP
#define RECOVERY_ADAPTERS_SYNTHETIC_HPP

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "recovery/error.hpp"
#include "recovery/observation.hpp"
#include "recovery/plan.hpp"
#include "recovery/ports.hpp"
#include "recovery/time.hpp"

namespace recovery {
namespace synthetic {

// ---------------------------------------------------------------------------
// A synthetic adjacent authority
// ---------------------------------------------------------------------------
// PROVENANCE: SYNTHETIC.
//
// Nothing in this header talks to real facility hardware, a BMS, a DCIM, a
// PDU, a UPS, a generator, a chiller, a CDU, a rack controller, an accelerator,
// or an RDMA fabric. It models the *behaviour* the coordinator must cope with:
// an authority that can accept a request, refuse it, fail it, answer
// ambiguously, die mid-call, or report evidence later.
//
// Two properties make it useful as a proof instrument:
//
//   * exactly-once application per idempotency key, enforced through a durable
//     ledger file that is written before the effect is reported;
//   * an explicit effect journal, so a test can prove that a request the
//     coordinator believed it had reissued was in fact applied only once.
class World {
public:
    World() = default;

    static Result<World> open(const std::string& path);

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] std::uint64_t apply_count(std::string_view key) const;
    [[nodiscard]] std::size_t accepted_key_count() const noexcept { return accepted_.size(); }
    [[nodiscard]] std::size_t applied_operation_count() const noexcept { return appliedOps_.size(); }
    [[nodiscard]] const std::vector<std::string>& effect_journal() const noexcept { return effectJournal_; }

    // Generation of the domain *state*: it advances every time an effect is
    // applied. Observations stamp it, which is what lets the evidence view tell
    // a newer reading from an older one.
    [[nodiscard]] std::uint64_t generation(Domain domain) const;
    // Generation of the domain *authority*: it advances only when the authority
    // itself changes, never because the facility changed. It is part of the
    // AuthorityRef the coordinator accepts.
    [[nodiscard]] std::uint64_t authority_generation(Domain domain) const;
    // The authority reference for the domain. Applying an effect never moves it:
    // the authority stays the authority, whatever its state now is. Only
    // replace_authority and bump_generation change it, which is what makes a
    // change of authority an explicit act rather than a side effect of recovery.
    [[nodiscard]] AuthorityRef authority(Domain domain) const;
    [[nodiscard]] std::string authority_name(Domain domain) const;
    [[nodiscard]] std::map<std::string, Reading> readings(Domain domain) const;

    // Sets one reading. The domain state changed, so the state generation
    // advances; the authority reference is untouched.
    void set_reading(Domain domain, std::string key, Reading reading);
    void set_authority_name(Domain domain, std::string name);
    // Changes the authority identity without touching readings, which is how a
    // test produces a change of authority rather than a change of content.
    void replace_authority(Domain domain, std::string instance, std::uint64_t generation);
    // Advances the authority generation without touching the readings or the
    // state generation, so that the authority digest and the authority
    // generation move together and the new binding is explicit.
    void bump_generation(Domain domain);

    [[nodiscard]] Result<bool> save() const;

private:
    friend class Adapter;

    struct DomainState {
        std::string instance{"authority-1"};
        // State generation: advances with every applied effect.
        std::uint64_t generation{1};
        // Authority generation: advances only when the authority changes.
        std::uint64_t authorityGeneration{1};
        std::map<std::string, Reading> readings{};
    };

    // Digest of the authority descriptor (domain, instance, authority
    // generation) and never of the facility readings: the readings are what the
    // authority said, not who the authority is.
    [[nodiscard]] Digest authority_digest(Domain domain) const;

    std::string path_{};
    std::map<Domain, DomainState> domains_{};
    std::map<std::string, std::uint64_t> accepted_{};
    std::map<std::string, std::string> appliedOps_{};
    std::vector<std::string> effectJournal_{};
};

// The adapter that presents a World to the coordinator. It deliberately
// interprets nothing: it applies what the request's parameters describe and
// reports what the world then contains.
class Adapter final : public AdjacentAuthorityPort {
public:
    struct Options {
        // Number of dispatch calls after which the adapter starts reporting the
        // same outcome again without applying anything, modelling an authority
        // whose first response was lost.
        std::uint64_t lose_response_after{0};
        // When true, an operation whose effect already exists is reported as
        // applied again (this is what an idempotency ledger is supposed to make
        // harmless).
        bool reapply{false};
        // Absolute path of a directory the adapter may use for its own state.
        std::string statePath{};
    };

    Adapter(World& world, Options options);

    [[nodiscard]] std::string authority_name() const override { return "synthetic-adjacent-authority"; }

    [[nodiscard]] Result<AdapterResponse> dispatch(const Request& request) override;
    [[nodiscard]] Result<AdapterResponse> inspect(const Request& request) override;
    [[nodiscard]] Result<AdapterResponse> observe(const Request& request, Domain domain) override;
    [[nodiscard]] Result<AuthorityRef> current_authority(Domain domain) const override;

    [[nodiscard]] std::uint64_t dispatch_calls() const noexcept { return dispatchCalls_; }
    [[nodiscard]] std::uint64_t inspect_calls() const noexcept { return inspectCalls_; }

private:
    World* world_{nullptr};
    Options options_{};
    std::uint64_t dispatchCalls_{0};
    std::uint64_t inspectCalls_{0};
};

}  // namespace synthetic
}  // namespace recovery

#endif  // RECOVERY_ADAPTERS_SYNTHETIC_HPP
