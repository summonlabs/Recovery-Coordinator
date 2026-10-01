#include "recovery/observation.hpp"

#include <algorithm>

#include "recovery/json.hpp"

namespace recovery {
namespace {

constexpr std::string_view kReadingPrefixFlag = "{\"flag\":";
constexpr std::string_view kReadingPrefixCount = "{\"count\":";
constexpr std::string_view kReadingPrefixBps = "{\"ratio_bp\":";
constexpr std::string_view kReadingPrefixText = "{\"text\":";

[[nodiscard]] std::optional<std::uint64_t> parse_unsigned(std::string_view text, std::size_t& pos) {
    const std::size_t start = pos;
    std::uint64_t value = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        const auto digit = static_cast<std::uint64_t>(text[pos] - '0');
        if (value > (0xffffffffffffffffull - digit) / 10ull) {
            return std::nullopt;
        }
        value = value * 10ull + digit;
        ++pos;
    }
    if (pos == start) {
        return std::nullopt;
    }
    if (pos - start > 1 && text[start] == '0') {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::string_view to_string(ReadingKind value) {
    switch (value) {
        case ReadingKind::Flag:
            return "flag";
        case ReadingKind::Count:
            return "count";
        case ReadingKind::BasisPoints:
            return "basis_points";
        case ReadingKind::Text:
            return "text";
    }
    return "unknown";
}

std::string_view to_string(ObservationSource value) {
    switch (value) {
        case ObservationSource::Adapter:
            return "adapter";
        case ObservationSource::Operator:
            return "operator";
        case ObservationSource::Coordinator:
            return "coordinator";
        case ObservationSource::Replay:
            return "replay";
    }
    return "unknown";
}

std::optional<ObservationSource> observation_source_from_string(std::string_view text) {
    if (text == "adapter") {
        return ObservationSource::Adapter;
    }
    if (text == "operator") {
        return ObservationSource::Operator;
    }
    if (text == "coordinator") {
        return ObservationSource::Coordinator;
    }
    if (text == "replay") {
        return ObservationSource::Replay;
    }
    return std::nullopt;
}

Reading Reading::flag(bool value) {
    Reading reading;
    reading.kind_ = ReadingKind::Flag;
    reading.flag_ = value;
    return reading;
}

Reading Reading::count(std::uint64_t value) {
    Reading reading;
    reading.kind_ = ReadingKind::Count;
    reading.count_ = value;
    return reading;
}

Reading Reading::basis_points(std::uint32_t value) {
    Reading reading;
    reading.kind_ = ReadingKind::BasisPoints;
    reading.basisPoints_ = value;
    return reading;
}

Reading Reading::text(std::string value) {
    Reading reading;
    reading.kind_ = ReadingKind::Text;
    reading.text_ = std::move(value);
    return reading;
}

std::string Reading::canonical() const {
    CanonicalWriter writer;
    switch (kind_) {
        case ReadingKind::Flag:
            writer.raw("{\"flag\":");
            writer.boolean(flag_);
            writer.raw("}");
            break;
        case ReadingKind::Count:
            writer.raw("{\"count\":");
            writer.unsigned_integer(count_);
            writer.raw("}");
            break;
        case ReadingKind::BasisPoints:
            writer.raw("{\"ratio_bp\":");
            writer.unsigned_integer(basisPoints_);
            writer.raw("}");
            break;
        case ReadingKind::Text:
            writer.raw("{\"text\":");
            writer.quoted(text_);
            writer.raw("}");
            break;
    }
    return writer.take();
}

std::optional<Reading> Reading::from_canonical(std::string_view text) {
    if (text.size() < 3) {
        return std::nullopt;
    }
    if (text.substr(0, kReadingPrefixFlag.size()) == kReadingPrefixFlag) {
        const std::string_view rest = text.substr(kReadingPrefixFlag.size());
        if (rest == "true}") {
            return Reading::flag(true);
        }
        if (rest == "false}") {
            return Reading::flag(false);
        }
        return std::nullopt;
    }
    if (text.substr(0, kReadingPrefixCount.size()) == kReadingPrefixCount) {
        std::size_t pos = kReadingPrefixCount.size();
        const auto value = parse_unsigned(text, pos);
        if (!value.has_value() || pos + 1 != text.size() || text[pos] != '}') {
            return std::nullopt;
        }
        return Reading::count(*value);
    }
    if (text.substr(0, kReadingPrefixBps.size()) == kReadingPrefixBps) {
        std::size_t pos = kReadingPrefixBps.size();
        const auto value = parse_unsigned(text, pos);
        if (!value.has_value() || *value > 0xffffffffull || pos + 1 != text.size() || text[pos] != '}') {
            return std::nullopt;
        }
        return Reading::basis_points(static_cast<std::uint32_t>(*value));
    }
    if (text.substr(0, kReadingPrefixText.size()) == kReadingPrefixText) {
        // The text reading is stored as a canonical JSON string token, so the
        // value is decoded by the same strict decoder that produced it rather
        // than by a second, hand written unescaper.
        const std::string_view rest = text.substr(kReadingPrefixText.size());
        if (rest.size() < 3 || rest.back() != '}') {
            return std::nullopt;
        }
        json::Decoder decoder(rest.substr(0, rest.size() - 1));
        auto decoded = decoder.decode();
        if (!decoded.has_value() || decoded.value().kind() != json::Value::Kind::String) {
            return std::nullopt;
        }
        return Reading::text(decoded.value().string());
    }
    return std::nullopt;
}

bool operator==(const Reading& lhs, const Reading& rhs) noexcept {
    if (lhs.kind_ != rhs.kind_) {
        return false;
    }
    switch (lhs.kind_) {
        case ReadingKind::Flag:
            return lhs.flag_ == rhs.flag_;
        case ReadingKind::Count:
            return lhs.count_ == rhs.count_;
        case ReadingKind::BasisPoints:
            return lhs.basisPoints_ == rhs.basisPoints_;
        case ReadingKind::Text:
            return lhs.text_ == rhs.text_;
    }
    return false;
}

bool operator<(const Reading& lhs, const Reading& rhs) noexcept {
    if (lhs.kind_ != rhs.kind_) {
        return lhs.kind_ < rhs.kind_;
    }
    switch (lhs.kind_) {
        case ReadingKind::Flag:
            return static_cast<int>(lhs.flag_) < static_cast<int>(rhs.flag_);
        case ReadingKind::Count:
            return lhs.count_ < rhs.count_;
        case ReadingKind::BasisPoints:
            return lhs.basisPoints_ < rhs.basisPoints_;
        case ReadingKind::Text:
            return lhs.text_ < rhs.text_;
    }
    return false;
}

Observation::Observation(EvidenceId id, Domain domain, std::uint64_t generation, ObservationSource source,
                         std::string origin, std::string key, Reading reading, TimePoint observedAt,
                         AuthorityRef authority, bool stale)
    : id_(id),
      domain_(domain),
      generation_(generation),
      source_(source),
      origin_(std::move(origin)),
      key_(std::move(key)),
      reading_(std::move(reading)),
      observedAt_(observedAt),
      authority_(std::move(authority)),
      stale_(stale) {}

std::string Observation::canonical() const {
    CanonicalWriter writer;
    writer.raw("{\"authority\":");
    writer.raw(authority_.canonical());
    writer.raw(",\"domain\":");
    writer.quoted(to_string(domain_));
    writer.raw(",\"generation\":");
    writer.unsigned_integer(generation_);
    writer.raw(",\"id\":");
    writer.quoted(id_.to_text());
    writer.raw(",\"key\":");
    writer.quoted(key_);
    writer.raw(",\"observed_at\":");
    writer.quoted(to_rfc3339(observedAt_));
    writer.raw(",\"origin\":");
    writer.quoted(origin_);
    writer.raw(",\"reading\":");
    writer.raw(reading_.canonical());
    writer.raw(",\"source\":");
    writer.quoted(to_string(source_));
    writer.raw(",\"stale\":");
    writer.boolean(stale_);
    writer.raw("}");
    return writer.take();
}

Digest Observation::digest() const {
    CanonicalWriter writer;
    append_framed(writer, "recovery.observation.v1");
    append_framed(writer, canonical());
    return writer.digest_of();
}

bool operator<(const Observation& lhs, const Observation& rhs) noexcept {
    if (lhs.domain_ != rhs.domain_) {
        return lhs.domain_ < rhs.domain_;
    }
    if (lhs.key_ != rhs.key_) {
        return lhs.key_ < rhs.key_;
    }
    if (lhs.generation_ != rhs.generation_) {
        return lhs.generation_ < rhs.generation_;
    }
    if (lhs.source_ != rhs.source_) {
        return lhs.source_ < rhs.source_;
    }
    if (lhs.origin_ != rhs.origin_) {
        return lhs.origin_ < rhs.origin_;
    }
    return lhs.id_ < rhs.id_;
}

bool operator==(const Observation& lhs, const Observation& rhs) noexcept {
    return lhs.canonical() == rhs.canonical();
}

std::optional<Reading> EvidenceView::find(std::string_view key) const {
    const auto it = readings_.find(std::string{key});
    if (it == readings_.end()) {
        return std::nullopt;
    }
    return it->second;
}

bool EvidenceView::is_current(TimePoint now, Duration freshness) const {
    if (!established_) {
        return false;
    }
    if (observedAt_.is_after(now)) {
        // Evidence from the future is not evidence.
        return false;
    }
    // A zero bound is the engine's "no bound was named" convention, exactly as
    // a zero observation budget means "do not give up" and a plan that names no
    // freshness requirement accepts any age. A zero bound cannot mean "this
    // view must have been taken at exactly this instant": no observation could
    // ever satisfy it, because the caller's clock has always moved on by the
    // time the view is read.
    if (freshness.is_zero()) {
        return true;
    }
    const auto age = now.since(observedAt_);
    if (!age.has_value()) {
        return false;
    }
    return !(freshness < *age);
}

std::string EvidenceView::canonical() const {
    CanonicalWriter writer;
    writer.raw("{\"authority\":");
    writer.raw(authority_.canonical());
    writer.raw(",\"established\":");
    writer.boolean(established_);
    writer.raw(",\"observed_at\":");
    writer.quoted(to_rfc3339(observedAt_));
    writer.raw(",\"readings\":{");
    bool first = true;
    for (const auto& [key, reading] : readings_) {
        if (!first) {
            writer.raw(",");
        }
        first = false;
        writer.quoted(key);
        writer.raw(":");
        writer.raw(reading.canonical());
    }
    writer.raw("}}");
    return writer.take();
}

ObservationAcceptance ObservationStore::accept(const Observation& observation) {
    ObservationAcceptance result;
    result.id = observation.id();
    result.digest = observation.digest();

    if (observation.id().is_zero()) {
        result.reason = "observation identity is zero";
        return result;
    }
    if (observation.authority().is_zero()) {
        result.reason = "observation carries no authority reference";
        return result;
    }
    if (!observation.stale() && !is_valid_utf8(observation.key())) {
        result.reason = "observation key is not valid UTF-8";
        return result;
    }

    DomainState& state = observations_[observation.domain()];
    const auto seen = state.seen.find(observation.id());
    if (seen != state.seen.end()) {
        if (seen->second == result.digest) {
            result.accepted = true;
            result.replayed = true;
            return result;
        }
        result.reason = "observation identity already accepted with different content";
        return result;
    }

    state.observations.push_back(observation);
    state.seen.emplace(observation.id(), result.digest);
    ++total_;
    result.accepted = true;

    if (!observation.stale() && observation.source() != ObservationSource::Replay) {
        // Fresh evidence for this domain is what ends a fence. fence_all()
        // documents "no view is current afterwards until fresh evidence is
        // presented", so presenting it must re-establish the view for exactly
        // the domain it describes.
        state.fenced = false;
    }

    if (maxPerDomain_ != 0 && state.observations.size() > maxPerDomain_) {
        // Bounded store: drop the oldest non-authoritative copy. The durable
        // journal keeps the full history; this store exists to answer "what is
        // current now".
        state.observations.erase(state.observations.begin());
    }

    rebuild_view(observation.domain());
    return result;
}

void ObservationStore::rebuild_view(Domain domain) {
    auto it = observations_.find(domain);
    if (it == observations_.end()) {
        return;
    }
    const DomainState& state = it->second;

    // Deterministic fold. Candidates are considered in total order
    // (generation, authority generation, observation instant, identity) so the
    // resulting view never depends on insertion order.
    std::vector<const Observation*> candidates;
    candidates.reserve(state.observations.size());
    for (const Observation& candidate : state.observations) {
        if (candidate.stale() || candidate.source() == ObservationSource::Replay) {
            continue;
        }
        candidates.push_back(&candidate);
    }

    EvidenceView view;
    if (!candidates.empty()) {
        std::sort(candidates.begin(), candidates.end(), [](const Observation* lhs, const Observation* rhs) {
            if (lhs->generation() != rhs->generation()) {
                return lhs->generation() < rhs->generation();
            }
            if (lhs->authority().generation() != rhs->authority().generation()) {
                return lhs->authority().generation() < rhs->authority().generation();
            }
            if (lhs->observed_at() != rhs->observed_at()) {
                return lhs->observed_at() < rhs->observed_at();
            }
            return *lhs < *rhs;
        });

        const Observation& newest = *candidates.back();
        view.authority_ = newest.authority();
        view.observedAt_ = newest.observed_at();
        view.established_ = !state.fenced;

        // Only readings from the newest generation, and from the newest
        // authority generation within it, contribute to the current view.
        const std::uint64_t generation = newest.generation();
        const std::uint64_t authorityGeneration = newest.authority().generation();
        for (const Observation* candidate : candidates) {
            if (candidate->generation() != generation) {
                continue;
            }
            if (candidate->authority().generation() != authorityGeneration) {
                continue;
            }
            const auto existing = view.readings_.find(candidate->key());
            if (existing == view.readings_.end() || existing->second != candidate->reading()) {
                view.readings_[candidate->key()] = candidate->reading();
            }
        }
    }
    it->second.view = std::move(view);
}

const EvidenceView& ObservationStore::view(Domain domain) const {
    static const EvidenceView kEmpty{};
    const auto it = observations_.find(domain);
    if (it == observations_.end()) {
        return kEmpty;
    }
    return it->second.view;
}

std::size_t ObservationStore::observation_count(Domain domain) const {
    const auto it = observations_.find(domain);
    if (it == observations_.end()) {
        return 0;
    }
    return it->second.observations.size();
}

std::vector<Observation> ObservationStore::observations_for(Domain domain) const {
    std::vector<Observation> out;
    const auto it = observations_.find(domain);
    if (it == observations_.end()) {
        return out;
    }
    out = it->second.observations;
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<Domain> ObservationStore::domains() const {
    std::vector<Domain> out;
    out.reserve(observations_.size());
    for (const auto& [domain, state] : observations_) {
        (void)state;
        out.push_back(domain);
    }
    return out;
}

void ObservationStore::fence_all() {
    for (auto& [domain, state] : observations_) {
        (void)domain;
        state.fenced = true;
        state.view.established_ = false;
    }
}

void ObservationStore::clear() {
    observations_.clear();
    total_ = 0;
}

}  // namespace recovery
