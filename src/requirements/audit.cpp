#include "moq/interop/requirements/catalog.h"

#include <map>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace moq::interop::requirements {
namespace {

using Anchor = std::pair<std::size_t, unsigned>;

Strength normalized_strength(const std::string& phrase) {
    if (phrase == "MUST NOT" || phrase == "SHALL NOT") return Strength::MustNot;
    if (phrase == "SHOULD NOT" || phrase == "NOT RECOMMENDED") return Strength::ShouldNot;
    if (phrase == "MUST" || phrase == "SHALL" || phrase == "REQUIRED") return Strength::Must;
    if (phrase == "SHOULD" || phrase == "RECOMMENDED") return Strength::Should;
    return Strength::May;
}

std::string collapse_whitespace(std::string_view phrase) {
    std::string result;
    bool pending_space = false;
    for (const char character : phrase) {
        if (character == ' ' || character == '\t' || character == '\n' || character == '\r') {
            pending_space = true;
        } else {
            if (pending_space && !result.empty()) result.push_back(' ');
            result.push_back(character);
            pending_space = false;
        }
    }
    return result;
}

}  // namespace

bool AuditReport::ok() const {
    return missing.empty() && multiply_classified.empty() && errors.empty();
}

std::vector<NormativeOccurrence> scan_normative_occurrences(const DraftSource& source) {
    static const std::regex pattern(
        R"(\b(MUST\s+NOT|SHALL\s+NOT|SHOULD\s+NOT|NOT\s+RECOMMENDED|MUST|SHALL|SHOULD|RECOMMENDED|REQUIRED|OPTIONAL|MAY)\b)");
    std::vector<NormativeOccurrence> occurrences;
    std::size_t line = 1;
    std::size_t scanned_to = 0;
    unsigned ordinal_on_line = 0;
    std::size_t previous_line = 0;
    for (std::sregex_iterator match(source.text.begin(), source.text.end(), pattern), end;
         match != end; ++match) {
        const auto offset = static_cast<std::size_t>(match->position());
        while (scanned_to < offset) {
            if (source.text[scanned_to] == '\n') ++line;
            ++scanned_to;
        }
        if (line != previous_line) {
            ordinal_on_line = 0;
            previous_line = line;
        }
        ++ordinal_on_line;
        const auto phrase = collapse_whitespace(match->str());
        const auto after = offset + static_cast<std::size_t>(match->length());
        const bool quoted = offset > 0 && after < source.text.size() &&
                            source.text[offset - 1] == '"' && source.text[after] == '"';
        occurrences.push_back({phrase, normalized_strength(phrase), line, ordinal_on_line, quoted});
    }
    return occurrences;
}

AuditReport audit_normative_occurrences(const DraftSource& source,
                                         const RequirementCatalog& catalog) {
    AuditReport report;
    if (catalog.draft != source.number || catalog.source_sha256 != source.sha256) {
        report.errors.push_back("Catalog draft or SHA-256 does not match source");
    }
    if (!catalog.complete) {
        report.errors.push_back("Incomplete catalog cannot pass the full-corpus audit");
    }
    const auto occurrences = scan_normative_occurrences(source);
    std::map<Anchor, NormativeOccurrence> known;
    for (const auto& occurrence : occurrences) {
        known.emplace(Anchor{occurrence.first_line, occurrence.occurrence_on_line}, occurrence);
    }
    std::map<Anchor, std::vector<const Requirement*>> records;
    for (const auto& row : catalog.requirements) {
        const Anchor anchor{row.source.first_line, row.source.occurrence};
        if (!known.contains(anchor)) {
            report.errors.push_back("Requirement " + row.id + " has no normative occurrence at line " +
                                    std::to_string(anchor.first) + " occurrence " +
                                    std::to_string(anchor.second));
        } else {
            records[anchor].push_back(&row);
        }
    }
    for (const auto& [anchor, occurrence] : known) {
        const auto found = records.find(anchor);
        if (found == records.end()) {
            report.missing.push_back(occurrence);
            continue;
        }
        std::set<unsigned> clauses;
        bool conflict = false;
        for (const auto* row : found->second) {
            if (row->strength != occurrence.normalized_strength || row->source.clause == 0 ||
                !clauses.insert(row->source.clause).second) {
                conflict = true;
            }
        }
        if (conflict) {
            report.multiply_classified.push_back(occurrence);
        }
    }
    return report;
}

}  // namespace moq::interop::requirements
