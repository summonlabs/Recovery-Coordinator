#include "recovery/adapters/synthetic.hpp"

#include <algorithm>

#include "recovery/canonical.hpp"
#include "recovery/fileio.hpp"
#include "recovery/json.hpp"

namespace recovery {
namespace synthetic {
namespace {

constexpr const char* kParametersDomain = "domain";
constexpr const char* kParametersEffect = "effect";
constexpr const char* kParametersGeneration = "generation";
constexpr const char* kParametersInstance = "instance";
constexpr const char* kParametersKey = "key";
constexpr const char* kParametersReport = "report";
constexpr const char* kParametersText = "text";
constexpr const char* kParametersValue = "value";

[[nodiscard]] Result<json::Value> parse_object(std::string_view text, std::string_view what) {
    if (text.empty()) {
        return json::Value{json::Object{}};
    }
    json::Decoder decoder(text);
    auto value = decoder.decode();
    if (!value.has_value()) {
        return errors::corrupt_state(std::string{"synthetic."} + std::string{what},
                                     "the parameters are not canonical JSON: " + value.error().describe());
    }
    if (value.value().kind() != json::Value::Kind::Object) {
        return errors::corrupt_state(std::string{"synthetic."} + std::string{what},
                                     "the parameters are not a JSON object");
    }
    return value;
}

[[nodiscard]] std::optional<std::string> string_member(const json::Value& object, const char* key) {
    const auto it = object.object().find(key);
    if (it == object.object().end() || it->second.kind() != json::Value::Kind::String) {
        return std::nullopt;
    }
    return it->second.string();
}

}  // namespace

std::uint64_t World::generation(Domain domain) const {
    const auto it = domains_.find(domain);
    if (it == domains_.end()) {
        return 1;
    }
    return it->second.generation;
}

std::uint64_t World::authority_generation(Domain domain) const {
    const auto it = domains_.find(domain);
    if (it == domains_.end()) {
        return 1;
    }
    return it->second.authorityGeneration;
}

Digest World::authority_digest(Domain domain) const {
    const auto it = domains_.find(domain);
    CanonicalWriter writer;
    append_framed(writer, "recovery.synthetic.authority.v1");
    append_framed(writer, to_string(domain));
    if (it == domains_.end()) {
        append_framed(writer, "authority-1");
        append_framed_u64(writer, 1);
        return writer.digest_of();
    }
    // Only the authority descriptor is folded in. The readings are state, not
    // identity: an authority that applied the effect it was asked for is the
    // same authority afterwards, so the reference the coordinator accepted
    // stays current while the recovery plan runs.
    append_framed(writer, it->second.instance);
    append_framed_u64(writer, it->second.authorityGeneration);
    return writer.digest_of();
}

AuthorityRef World::authority(Domain domain) const {
    const auto it = domains_.find(domain);
    const std::string instance = it == domains_.end() ? std::string{"authority-1"} : it->second.instance;
    const std::uint64_t generation = it == domains_.end() ? 1 : it->second.authorityGeneration;
    return AuthorityRef{std::string{to_string(domain)}, instance, generation, authority_digest(domain)};
}

std::string World::authority_name(Domain domain) const {
    const auto it = domains_.find(domain);
    return it == domains_.end() ? std::string{"authority-1"} : it->second.instance;
}

std::map<std::string, Reading> World::readings(Domain domain) const {
    const auto it = domains_.find(domain);
    if (it == domains_.end()) {
        return {};
    }
    return it->second.readings;
}

void World::set_reading(Domain domain, std::string key, Reading reading) {
    DomainState& state = domains_[domain];
    state.readings[std::move(key)] = std::move(reading);
    // The state changed, so its generation advances. The authority reference
    // deliberately does not move: the same authority now reports newer state.
    if (state.generation != 0xffffffffffffffffull) {
        state.generation += 1;
    }
}

void World::set_authority_name(Domain domain, std::string name) {
    domains_[domain].instance = std::move(name);
}

void World::replace_authority(Domain domain, std::string instance, std::uint64_t generation) {
    DomainState& state = domains_[domain];
    state.instance = std::move(instance);
    state.authorityGeneration = generation;
}

void World::bump_generation(Domain domain) {
    DomainState& state = domains_[domain];
    if (state.authorityGeneration == 0xffffffffffffffffull) {
        return;
    }
    state.authorityGeneration += 1;
}

std::uint64_t World::apply_count(std::string_view key) const {
    const auto it = accepted_.find(std::string{key});
    if (it == accepted_.end()) {
        return 0;
    }
    return it->second;
}

Result<bool> World::save() const {
    if (path_.empty()) {
        return true;
    }
    json::Object root;
    json::Array domains;
    for (const auto& [domain, state] : domains_) {
        json::Object readings;
        for (const auto& [key, reading] : state.readings) {
            json::Object entry;
            entry["kind"] = json::Value{std::string{to_string(reading.kind())}};
            switch (reading.kind()) {
                case ReadingKind::Flag:
                    entry["value"] = json::Value{reading.flag_value()};
                    break;
                case ReadingKind::Count:
                    entry["value"] = json::Value{static_cast<unsigned long long>(reading.count_value())};
                    break;
                case ReadingKind::BasisPoints:
                    entry["value"] = json::Value{static_cast<unsigned long long>(reading.basis_points_value())};
                    break;
                case ReadingKind::Text:
                    entry["value"] = json::Value{reading.text_value()};
                    break;
            }
            readings[key] = json::Value{std::move(entry)};
        }
        json::Object entry;
        entry["authority_generation"] = json::Value{static_cast<unsigned long long>(state.authorityGeneration)};
        entry["domain"] = json::Value{std::string{to_string(domain)}};
        entry["generation"] = json::Value{static_cast<unsigned long long>(state.generation)};
        entry["instance"] = json::Value{state.instance};
        entry["readings"] = json::Value{std::move(readings)};
        domains.push_back(json::Value{std::move(entry)});
    }
    root["domains"] = json::Value{std::move(domains)};

    json::Object accepted;
    for (const auto& [key, count] : accepted_) {
        accepted[key] = json::Value{static_cast<unsigned long long>(count)};
    }
    root["accepted"] = json::Value{std::move(accepted)};

    json::Object operations;
    for (const auto& [key, operation] : appliedOps_) {
        operations[key] = json::Value{operation};
    }
    root["operations"] = json::Value{std::move(operations)};

    json::Array journal;
    for (const std::string& entry : effectJournal_) {
        journal.push_back(json::Value{entry});
    }
    root["journal"] = json::Value{std::move(journal)};

    const std::string text = json::encode(json::Value{std::move(root)});
    std::vector<std::uint8_t> bytes(text.begin(), text.end());
    return fileio::publish_atomically(path_, bytes);
}

Result<World> World::open(const std::string& path) {
    World world;
    world.path_ = path;
    if (path.empty() || !fileio::exists(path)) {
        return world;
    }
    auto bytes = fileio::read_file(path, 64ull * 1024ull * 1024ull);
    if (!bytes.has_value()) {
        return bytes.error();
    }
    const std::string text(reinterpret_cast<const char*>(bytes.value().data()), bytes.value().size());
    json::Decoder decoder(text);
    auto root = decoder.decode();
    if (!root.has_value()) {
        return errors::corrupt_state("synthetic.world", "the authority state file is not valid JSON");
    }
    const json::Object& object = root.value().object();

    const auto domainsIt = object.find("domains");
    if (domainsIt != object.end() && domainsIt->second.kind() == json::Value::Kind::Array) {
        for (const json::Value& entry : domainsIt->second.array()) {
            const auto domainText = string_member(entry, "domain");
            const auto instance = string_member(entry, "instance");
            if (!domainText.has_value() || !instance.has_value()) {
                return errors::corrupt_state("synthetic.world", "a domain entry is missing its identity");
            }
            const auto domain = domain_from_string(*domainText);
            if (!domain.has_value()) {
                return errors::corrupt_state("synthetic.world", "a domain entry names an unknown domain");
            }
            const auto generationIt = entry.object().find("generation");
            if (generationIt == entry.object().end() ||
                generationIt->second.kind() != json::Value::Kind::Unsigned) {
                return errors::corrupt_state("synthetic.world", "a domain entry has no generation");
            }
            DomainState state;
            state.instance = *instance;
            state.generation = generationIt->second.unsigned_value();
            const auto authorityGenerationIt = entry.object().find("authority_generation");
            if (authorityGenerationIt != entry.object().end() &&
                authorityGenerationIt->second.kind() == json::Value::Kind::Unsigned) {
                state.authorityGeneration = authorityGenerationIt->second.unsigned_value();
            }
            const auto readingsIt = entry.object().find("readings");
            if (readingsIt != entry.object().end() && readingsIt->second.kind() == json::Value::Kind::Object) {
                for (const auto& [key, readingValue] : readingsIt->second.object()) {
                    const auto kindText = string_member(readingValue, "kind");
                    const auto valueIt = readingValue.object().find("value");
                    if (!kindText.has_value() || valueIt == readingValue.object().end()) {
                        return errors::corrupt_state("synthetic.world", "a reading entry is malformed");
                    }
                    Reading reading;
                    if (*kindText == "flag" && valueIt->second.kind() == json::Value::Kind::Boolean) {
                        reading = Reading::flag(valueIt->second.boolean());
                    } else if (*kindText == "count" && valueIt->second.kind() == json::Value::Kind::Unsigned) {
                        reading = Reading::count(valueIt->second.unsigned_value());
                    } else if (*kindText == "basis_points" && valueIt->second.kind() == json::Value::Kind::Unsigned &&
                               valueIt->second.unsigned_value() <= 0xffffffffull) {
                        reading = Reading::basis_points(static_cast<std::uint32_t>(valueIt->second.unsigned_value()));
                    } else if (*kindText == "text" && valueIt->second.kind() == json::Value::Kind::String) {
                        reading = Reading::text(valueIt->second.string());
                    } else {
                        return errors::corrupt_state("synthetic.world", "a reading entry has an unknown kind");
                    }
                    state.readings[key] = std::move(reading);
                }
            }
            world.domains_[*domain] = std::move(state);
        }
    }

    const auto acceptedIt = object.find("accepted");
    if (acceptedIt != object.end() && acceptedIt->second.kind() == json::Value::Kind::Object) {
        for (const auto& [key, count] : acceptedIt->second.object()) {
            if (count.kind() != json::Value::Kind::Unsigned) {
                return errors::corrupt_state("synthetic.world", "an accepted key count is malformed");
            }
            world.accepted_[key] = count.unsigned_value();
        }
    }
    const auto operationsIt = object.find("operations");
    if (operationsIt != object.end() && operationsIt->second.kind() == json::Value::Kind::Object) {
        for (const auto& [key, operation] : operationsIt->second.object()) {
            if (operation.kind() != json::Value::Kind::String) {
                return errors::corrupt_state("synthetic.world", "an applied operation is malformed");
            }
            world.appliedOps_[key] = operation.string();
        }
    }
    const auto journalIt = object.find("journal");
    if (journalIt != object.end() && journalIt->second.kind() == json::Value::Kind::Array) {
        for (const json::Value& entry : journalIt->second.array()) {
            if (entry.kind() != json::Value::Kind::String) {
                return errors::corrupt_state("synthetic.world", "an effect journal entry is malformed");
            }
            world.effectJournal_.push_back(entry.string());
        }
    }
    return world;
}

// One applied effect: the domain and reading the request named (the
// expectation) together with the reading the world holds afterwards (the
// observation). Keeping both is what lets the authority state, in its own
// words, whether the effect satisfied the request.
struct AppliedEffect {
    Domain domain{Domain::Power};
    std::string key{};
    Reading expected{};
    Reading observed{};
};

// Stable identity of one (domain, key, reading) statement. Expectation and
// observation are the same shape on purpose: equality of the two digests is
// exactly "the world now holds the reading the request asked for".
[[nodiscard]] Digest effect_statement_digest(Domain domain, std::string_view key, const Reading& reading) {
    CanonicalWriter writer;
    append_framed(writer, "recovery.synthetic.effect.v1");
    append_framed(writer, to_string(domain));
    append_framed(writer, key);
    append_framed(writer, reading.canonical());
    return writer.digest_of();
}

// The authority's own assessment of the effect it just produced. This is a
// claim, not an observation: the coordinator only promotes it after it has
// accepted the observation that confirms it and checked that the authority
// reference is still the one it accepted.
[[nodiscard]] EffectAssessment assess_effect(const World& world, const AppliedEffect& effect) {
    EffectAssessment assessment;
    assessment.status = effect.observed == effect.expected ? EffectStatus::Verified : EffectStatus::Contradicted;
    assessment.expectation = effect_statement_digest(effect.domain, effect.key, effect.expected);
    assessment.observed = effect_statement_digest(effect.domain, effect.key, effect.observed);
    assessment.authority = world.authority(effect.domain);
    assessment.at = TimePoint{};
    assessment.detail = assessment.status == EffectStatus::Verified
                            ? "the authority confirms the reading the request named"
                            : "the world does not hold the reading the request named";
    return assessment;
}

// Reports what the authority now holds for one applied effect: the observation
// the coordinator accepts as evidence, and the authority's own assessment of
// whether the request's expectation was satisfied. Both must be present for the
// coordinator to verify the effect; neither alone is enough.
void report_effect(AdapterResponse& response, const World& world, const AppliedEffect& effect) {
    AdapterObservation observation{effect.domain,
                                   world.generation(effect.domain),
                                   std::string{"synthetic/"} + std::string{to_string(effect.domain)},
                                   effect.key,
                                   effect.observed,
                                   TimePoint{},
                                   world.authority(effect.domain),
                                   false};
    response.add_observation(std::move(observation));
    response.add_assessment(assess_effect(world, effect));
}

// Applies a scripted effect to the world and returns the resulting reading, if
// the effect named one. The world is saved before the effect is reported, so a
// crash between the save and the report models an authority that changed the
// facility and then died.
//
// Applying an effect changes the domain's *state*: the state generation
// advances. It does not change the domain's *authority*, which only moves when
// the effect explicitly replaces the instance or the authority generation. The
// coordinator accepted one authority reference for the life of the plan; the
// recovery itself must not invalidate that acceptance.
[[nodiscard]] Result<std::optional<AppliedEffect>> apply_world_effect(World& world, std::string_view effectText) {
    auto fields = parse_object(effectText, "effect");
    if (!fields.has_value()) {
        return fields.error();
    }
    const json::Value& object = fields.value();
    const auto domainText = string_member(object, kParametersDomain);
    if (!domainText.has_value()) {
        return errors::invalid_argument("synthetic.effect_domain", "the effect names no domain");
    }
    const auto domain = domain_from_string(*domainText);
    if (!domain.has_value()) {
        return errors::invalid_argument("synthetic.effect_domain", "the effect names an unknown domain");
    }
    const auto keyText = string_member(object, kParametersKey);
    if (!keyText.has_value()) {
        return errors::invalid_argument("synthetic.effect_key", "the effect names no reading");
    }
    const auto valueIt = object.object().find(kParametersValue);
    if (valueIt == object.object().end()) {
        return errors::invalid_argument("synthetic.effect_value", "the effect has no value");
    }
    Reading reading;
    switch (valueIt->second.kind()) {
        case json::Value::Kind::Boolean:
            reading = Reading::flag(valueIt->second.boolean());
            break;
        case json::Value::Kind::Unsigned:
            reading = Reading::count(valueIt->second.unsigned_value());
            break;
        case json::Value::Kind::String:
            reading = Reading::text(valueIt->second.string());
            break;
        default:
            return errors::invalid_argument("synthetic.effect_value", "the effect value is not representable");
    }
    // An effect may name a new authority instance or authority generation. That
    // is a deliberate change of authority, and it is the only thing in this
    // function that moves the AuthorityRef.
    const auto instanceText = string_member(object, kParametersInstance);
    const auto generationIt = object.object().find(kParametersGeneration);
    if (instanceText.has_value()) {
        world.set_authority_name(*domain, *instanceText);
    }
    if (generationIt != object.object().end() && generationIt->second.kind() == json::Value::Kind::Unsigned) {
        world.replace_authority(*domain, world.authority_name(*domain), generationIt->second.unsigned_value());
    }
    world.set_reading(*domain, *keyText, reading);

    AppliedEffect applied;
    applied.domain = *domain;
    applied.key = *keyText;
    applied.expected = reading;
    // What the world actually holds now, read back rather than assumed, so the
    // assessment the authority emits is a statement about the world.
    const auto readings = world.readings(*domain);
    const auto actual = readings.find(*keyText);
    applied.observed = actual == readings.end() ? Reading{} : actual->second;
    return std::make_optional(applied);
}
Adapter::Adapter(World& world, Options options) : world_(&world), options_(std::move(options)) {}

Result<AuthorityRef> Adapter::current_authority(Domain domain) const {
    if (world_ == nullptr) {
        return errors::internal("synthetic.no_world", "the adapter has no world");
    }
    return world_->authority(domain);
}

Result<AdapterResponse> Adapter::observe(const Request& request, Domain domain) {
    (void)request;
    if (world_ == nullptr) {
        return errors::internal("synthetic.no_world", "the adapter has no world");
    }
    AdapterResponse response = AdapterResponse::completed("observation");
    const auto readings = world_->readings(domain);
    std::uint64_t index = 0;
    for (const auto& [key, reading] : readings) {
        AdapterObservation observation{domain, world_->generation(domain),
                                       std::string{"synthetic/"} + std::string{to_string(domain)}, key, reading,
                                       TimePoint{}, world_->authority(domain), false};
        (void)index;
        response.add_observation(std::move(observation));
    }
    return response;
}

Result<AdapterResponse> Adapter::dispatch(const Request& request) {
    ++dispatchCalls_;
    if (world_ == nullptr) {
        return errors::internal("synthetic.no_world", "the adapter has no world");
    }

    auto parameters = parse_object(request.parameters(), "dispatch");
    if (!parameters.has_value()) {
        return parameters.error();
    }
    const json::Value& object = parameters.value();

    // A scripted report status lets a test drive every adapter answer the
    // coordinator must cope with, without inventing a second code path.
    const auto report = string_member(object, kParametersReport);
    if (report.has_value() && *report != "completed") {
        if (*report == "refused") {
            return AdapterResponse::refused("scripted refusal");
        }
        if (*report == "failed") {
            return AdapterResponse::failed("scripted failure");
        }
        if (*report == "unknown") {
            return AdapterResponse::unknown("scripted unknown");
        }
        if (*report == "in_progress") {
            return AdapterResponse::in_progress("scripted in progress");
        }
        if (*report == "indeterminate") {
            return AdapterResponse::indeterminate("scripted indeterminate outcome");
        }
        if (*report == "transport_error") {
            return errors::operation_failed("synthetic.transport", "the scripted transport failed");
        }
        if (*report == "completed_without_effect") {
            AdapterResponse response = AdapterResponse::completed("scripted completion without an effect");
            response.add_expectation_key("effect");
            return response;
        }
        return errors::invalid_argument("synthetic.report", "the scripted report status is not recognised");
    }

    const auto effect = string_member(object, kParametersEffect);
    if (!effect.has_value()) {
        return errors::invalid_argument("synthetic.effect", "the request has no effect description");
    }

    // Exactly once per key. The ledger is written before the response, so a
    // repeated key never produces a second effect even when the first response
    // was lost.
    const bool known = world_->accepted_.find(request.idempotency_key()) != world_->accepted_.end();
    std::optional<AppliedEffect> appliedEffect;
    if (known && !options_.reapply) {
        const auto existing = world_->appliedOps_.find(request.idempotency_key());
        if (existing != world_->appliedOps_.end()) {
            auto replayed = apply_world_effect(*world_, existing->second);
            if (replayed.has_value()) {
                appliedEffect = replayed.value();
            }
        }
    } else {
        auto applied = apply_world_effect(*world_, *effect);
        if (!applied.has_value()) {
            return applied.error();
        }
        appliedEffect = applied.value();
        world_->appliedOps_[request.idempotency_key()] = *effect;
        world_->effectJournal_.push_back(request.idempotency_key());
        world_->accepted_[request.idempotency_key()] = world_->apply_count(request.idempotency_key()) + 1;
        auto saved = world_->save();
        if (!saved.has_value()) {
            return saved.error();
        }
    }

    if (!known && options_.lose_response_after != 0 && dispatchCalls_ >= options_.lose_response_after) {
        // The effect happened and the answer was lost. This is exactly the
        // ambiguity the coordinator must reconcile rather than reissue.
        return AdapterResponse::indeterminate("the response was lost after the effect was applied");
    }

    AdapterResponse response = AdapterResponse::completed(known ? "already applied" : "applied");
    if (appliedEffect.has_value()) {
        report_effect(response, *world_, *appliedEffect);
    }
    response.add_effect_digest(request.binding_digest());
    return response;
}

Result<AdapterResponse> Adapter::inspect(const Request& request) {
    ++inspectCalls_;
    if (world_ == nullptr) {
        return errors::internal("synthetic.no_world", "the adapter has no world");
    }
    auto parameters = parse_object(request.parameters(), "inspect");
    if (!parameters.has_value()) {
        return parameters.error();
    }
    const auto report = string_member(parameters.value(), kParametersReport);
    if (report.has_value() && *report == "transport_error") {
        return errors::operation_failed("synthetic.transport", "the scripted transport failed");
    }

    const auto known = world_->appliedOps_.find(request.idempotency_key());
    if (known == world_->appliedOps_.end()) {
        // A proven absence is what permits a re-dispatch. The coordinator
        // relies on this answer, so the adapter must never guess it.
        return AdapterResponse::unknown("the authority has no record of the key");
    }

    AdapterResponse response = AdapterResponse::completed("already applied");
    auto applied = apply_world_effect(*world_, known->second);
    if (applied.has_value() && applied.value().has_value()) {
        // Reconciliation answers with the same pair a dispatch answers with: the
        // current observation, and the authority's assessment of it. This is
        // what lets a successor incarnation resolve an attempt that was in
        // flight when its predecessor died.
        report_effect(response, *world_, applied.value().value());
    }
    return response;
}

}  // namespace synthetic
}  // namespace recovery
