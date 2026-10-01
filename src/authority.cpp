#include "recovery/authority.hpp"

#include "recovery/plan.hpp"

namespace recovery {

Result<AuthorityRef> AuthorityLedger::accept(Domain domain, const AuthorityRef& reference, TimePoint now) {
    if (reference.is_zero()) {
        return errors::invalid_argument("authority.zero_reference", "authority reference is empty");
    }
    auto it = entries_.find(domain);
    if (it != entries_.end()) {
        const Entry& existing = it->second;
        if (!existing.retired && existing.reference == reference) {
            // Idempotent re-acceptance of exactly the same content.
            return reference;
        }
        if (!existing.retired && reference.generation() < existing.reference.generation()) {
            return errors::stale_authority("authority.generation_regression",
                                           "authority generation would move backwards");
        }
        if (!existing.retired && reference.generation() == existing.reference.generation() &&
            reference.digest() != existing.reference.digest()) {
            return errors::identity_conflict("authority.digest_conflict",
                                             "same authority generation presented with different content");
        }
        if (!existing.retired && !existing.reference.same_structure(reference)) {
            return errors::identity_conflict("authority.structure_change",
                                             "authority identity changed without a retirement");
        }
        if (!existing.retired && existing.reference.generation() > reference.generation()) {
            return errors::stale_authority("authority.generation_regression",
                                           "authority generation would move backwards");
        }
    }

    Entry entry;
    entry.reference = reference;
    entry.acceptedAt = now;
    entry.retired = false;
    entry.domain = domain;
    entries_[domain] = std::move(entry);
    ++acceptedCount_;
    return reference;
}

Result<AuthorityRef> AuthorityLedger::retire(Domain domain, TimePoint now, std::string_view reason) {
    (void)reason;
    auto it = entries_.find(domain);
    if (it == entries_.end()) {
        return errors::not_found("authority.not_accepted", "cannot retire authority that was never accepted");
    }
    if (it->second.retired) {
        return it->second.reference;
    }
    it->second.retired = true;
    it->second.retiredAt = now;
    ++retiredCount_;
    return it->second.reference;
}

bool AuthorityLedger::is_current(const AuthorityRef& reference, Domain domain) const {
    const auto it = entries_.find(domain);
    if (it == entries_.end()) {
        return false;
    }
    if (it->second.retired) {
        return false;
    }
    return it->second.reference == reference;
}

std::optional<AuthorityRef> AuthorityLedger::current(Domain domain) const {
    const auto it = entries_.find(domain);
    if (it == entries_.end() || it->second.retired) {
        return std::nullopt;
    }
    return it->second.reference;
}

std::string AuthorityLedger::canonical() const {
    CanonicalWriter writer;
    writer.raw("[");
    bool first = true;
    for (const auto& [domain, entry] : entries_) {
        if (!first) {
            writer.raw(",");
        }
        first = false;
        writer.raw("{\"accepted_at\":");
        writer.quoted(to_rfc3339(entry.acceptedAt));
        writer.raw(",\"domain\":");
        writer.quoted(to_string(domain));
        writer.raw(",\"reference\":");
        writer.raw(entry.reference.canonical());
        writer.raw(",\"retired\":");
        writer.boolean(entry.retired);
        writer.raw("}");
    }
    writer.raw("]");
    return writer.take();
}

void AuthorityLedger::clear() {
    entries_.clear();
    acceptedCount_ = 0;
    retiredCount_ = 0;
}

std::string AuthorityDelta::summary() const {
    if (current) {
        return "current";
    }
    std::string out = "changed(";
    for (std::size_t i = 0; i < changed.size(); ++i) {
        if (i != 0) {
            out.push_back(',');
        }
        out.append(to_string(changed[i]));
    }
    out.push_back(')');
    if (!reasons.empty()) {
        out.push_back(':');
        out.append(reasons.front());
    }
    return out;
}

AuthorityDelta compare_authority(const Plan& plan, const AuthorityLedger& ledger) {
    AuthorityDelta delta;
    delta.current = true;
    for (const auto& [domain, requirement] : plan.authority_requirements()) {
        const auto current = ledger.current(domain);
        if (!current.has_value()) {
            delta.current = false;
            delta.changed.push_back(domain);
            delta.reasons.push_back("no accepted authority for the domain");
            continue;
        }
        if (*current != requirement) {
            delta.current = false;
            delta.changed.push_back(domain);
            if (current->digest() != requirement.digest()) {
                delta.reasons.push_back("authority content digest changed");
            } else if (current->generation() != requirement.generation()) {
                delta.reasons.push_back("authority generation changed");
            } else {
                delta.reasons.push_back("authority identity changed");
            }
        }
    }
    return delta;
}

}  // namespace recovery
