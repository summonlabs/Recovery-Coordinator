#include "recovery/epoch.hpp"

namespace recovery {

AuthorityRef::AuthorityRef(std::string authority, std::string instance, std::uint64_t generation, Digest digest)
    : authority_(std::move(authority)), instance_(std::move(instance)), generation_(generation), digest_(digest) {}

std::string AuthorityRef::canonical() const {
    CanonicalWriter writer;
    writer.raw("{");
    writer.raw("\"authority\":");
    writer.quoted(authority_);
    writer.raw(",\"digest\":");
    writer.digest(digest_);
    writer.raw(",\"generation\":");
    writer.unsigned_integer(generation_);
    writer.raw(",\"instance\":");
    writer.quoted(instance_);
    writer.raw("}");
    return writer.take();
}

Digest AuthorityRef::structural_digest() const {
    CanonicalWriter writer;
    append_framed(writer, "recovery.authority_ref.v1");
    append_framed(writer, authority_);
    append_framed(writer, instance_);
    append_framed_u64(writer, generation_);
    append_framed(writer, digest_.hex());
    return writer.digest_of();
}

std::optional<AuthorityRef> AuthorityRef::from_canonical(std::string_view text) {
    // The canonical form is deliberately parseable without a general JSON
    // dependency: the field order is fixed and each field has a strict shape.
    static constexpr std::string_view kPrefix = "{\"authority\":";
    if (text.size() < kPrefix.size() || text.substr(0, kPrefix.size()) != kPrefix) {
        return std::nullopt;
    }
    std::size_t pos = kPrefix.size();
    if (pos >= text.size() || text[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    std::string authority;
    while (pos < text.size() && text[pos] != '"') {
        if (text[pos] == '\\') {
            return std::nullopt;
        }
        authority.push_back(text[pos]);
        ++pos;
    }
    if (pos + 1 >= text.size() || text[pos] != '"') {
        return std::nullopt;
    }
    ++pos;
    static constexpr std::string_view kDigestKey = ",\"digest\":\"";
    if (text.substr(pos, kDigestKey.size()) != kDigestKey) {
        return std::nullopt;
    }
    pos += kDigestKey.size();
    if (pos + Digest::kBytes * 2 + 1 > text.size() || text[pos + Digest::kBytes * 2] != '"') {
        return std::nullopt;
    }
    const auto digest = Digest::parse(text.substr(pos, Digest::kBytes * 2));
    if (!digest.has_value()) {
        return std::nullopt;
    }
    pos += Digest::kBytes * 2 + 1;
    static constexpr std::string_view kGenerationKey = ",\"generation\":";
    if (text.substr(pos, kGenerationKey.size()) != kGenerationKey) {
        return std::nullopt;
    }
    pos += kGenerationKey.size();
    const std::size_t genStart = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        ++pos;
    }
    if (pos == genStart || (pos - genStart > 1 && text[genStart] == '0')) {
        return std::nullopt;
    }
    std::uint64_t generation = 0;
    for (std::size_t i = genStart; i < pos; ++i) {
        const auto digit = static_cast<std::uint64_t>(text[i] - '0');
        if (generation > (0xffffffffffffffffull - digit) / 10ull) {
            return std::nullopt;
        }
        generation = generation * 10ull + digit;
    }
    static constexpr std::string_view kInstanceKey = ",\"instance\":\"";
    if (text.substr(pos, kInstanceKey.size()) != kInstanceKey) {
        return std::nullopt;
    }
    pos += kInstanceKey.size();
    std::string instance;
    while (pos < text.size() && text[pos] != '"') {
        if (text[pos] == '\\') {
            return std::nullopt;
        }
        instance.push_back(text[pos]);
        ++pos;
    }
    if (pos + 2 != text.size() || text[pos] != '"' || text[pos + 1] != '}') {
        return std::nullopt;
    }
    return AuthorityRef{std::move(authority), std::move(instance), generation, *digest};
}

bool operator<(const AuthorityRef& lhs, const AuthorityRef& rhs) noexcept {
    if (lhs.authority_ != rhs.authority_) {
        return lhs.authority_ < rhs.authority_;
    }
    if (lhs.instance_ != rhs.instance_) {
        return lhs.instance_ < rhs.instance_;
    }
    if (lhs.generation_ != rhs.generation_) {
        return lhs.generation_ < rhs.generation_;
    }
    return lhs.digest_ < rhs.digest_;
}

std::string_view to_string(Domain value) {
    switch (value) {
        case Domain::Power:
            return "power";
        case Domain::Cooling:
            return "cooling";
        case Domain::Capacity:
            return "capacity";
        case Domain::Workload:
            return "workload";
        case Domain::Network:
            return "network";
        case Domain::FacilityPolicy:
            return "facility_policy";
        case Domain::Safety:
            return "safety";
    }
    return "unknown";
}

std::optional<Domain> domain_from_string(std::string_view text) {
    if (text == "power") {
        return Domain::Power;
    }
    if (text == "cooling") {
        return Domain::Cooling;
    }
    if (text == "capacity") {
        return Domain::Capacity;
    }
    if (text == "workload") {
        return Domain::Workload;
    }
    if (text == "network") {
        return Domain::Network;
    }
    if (text == "facility_policy") {
        return Domain::FacilityPolicy;
    }
    if (text == "safety") {
        return Domain::Safety;
    }
    return std::nullopt;
}

std::string AuthorityId::canonical() const {
    CanonicalWriter writer;
    append_framed(writer, "recovery.authority_id.v1");
    append_framed(writer, to_string(domain_));
    append_framed(writer, instance_);
    return writer.take();
}

}  // namespace recovery
