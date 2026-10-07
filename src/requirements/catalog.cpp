#include "moq/interop/requirements/catalog.h"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#include <tuple>

namespace moq::interop::requirements {
namespace {

using Json = nlohmann::json;

constexpr std::uintmax_t kMaxCatalogBytes = 16 * 1024 * 1024;

void exact_fields(const Json& value, std::initializer_list<std::string_view> expected,
                  std::string_view context, std::string_view optional = {}) {
    const bool has_optional = !optional.empty() && value.is_object() &&
                              value.contains(std::string(optional));
    if (!value.is_object() || value.size() != expected.size() + (has_optional ? 1 : 0)) {
        throw std::runtime_error(std::string(context) + " has missing or unknown fields");
    }
    for (const auto field : expected) {
        if (!value.contains(std::string(field))) {
            throw std::runtime_error(std::string(context) + " has missing field " +
                                     std::string(field));
        }
    }
}

std::string required_string(const Json& value, const char* field) {
    const auto& entry = value.at(field);
    if (!entry.is_string() || entry.get_ref<const std::string&>().empty()) {
        throw std::runtime_error(std::string(field) + " must be a nonempty string");
    }
    return entry.get<std::string>();
}

std::uint64_t unsigned_integer(const Json& value, const char* field, bool allow_zero = false) {
    const auto& entry = value.at(field);
    if (!entry.is_number_unsigned()) {
        throw std::runtime_error(std::string(field) + " must be an unsigned integer");
    }
    const auto number = entry.get<std::uint64_t>();
    if (!allow_zero && number == 0) {
        throw std::runtime_error(std::string(field) + " must be positive");
    }
    return number;
}

unsigned ordinal(const Json& value, const char* field) {
    const auto number = unsigned_integer(value, field);
    if (number > std::numeric_limits<unsigned>::max()) {
        throw std::runtime_error(std::string(field) + " exceeds unsigned range");
    }
    return static_cast<unsigned>(number);
}

std::vector<std::string> identifiers(const Json& value, const char* field) {
    const auto& entries = value.at(field);
    if (!entries.is_array()) {
        throw std::runtime_error(std::string(field) + " must be an array");
    }
    std::vector<std::string> result;
    std::set<std::string> seen;
    for (const auto& entry : entries) {
        if (!entry.is_string() || entry.get_ref<const std::string&>().empty()) {
            throw std::runtime_error(std::string(field) + " contains an empty identifier");
        }
        auto id = entry.get<std::string>();
        if (!seen.insert(id).second) {
            throw std::runtime_error(std::string(field) + " contains a duplicate identifier");
        }
        result.push_back(std::move(id));
    }
    return result;
}

Strength parse_strength(const std::string& value) {
    if (value == "Must") return Strength::Must;
    if (value == "MustNot") return Strength::MustNot;
    if (value == "Should") return Strength::Should;
    if (value == "ShouldNot") return Strength::ShouldNot;
    if (value == "May") return Strength::May;
    throw std::runtime_error("Unknown strength: " + value);
}

Applicability parse_applicability(const std::string& value) {
    if (value == "Applicable") return Applicability::Applicable;
    if (value == "NotApplicable") return Applicability::NotApplicable;
    if (value == "Informative") return Applicability::Informative;
    throw std::runtime_error("Unknown applicability: " + value);
}

Testability parse_testability(const std::string& value) {
    if (value == "Testable") return Testability::Testable;
    if (value == "NotTestable") return Testability::NotTestable;
    if (value == "NotApplicable") return Testability::NotApplicable;
    throw std::runtime_error("Unknown testability: " + value);
}

using Anchor = std::pair<std::size_t, unsigned>;

Requirement parse_requirement(const Json& value, std::size_t line_count,
                              const std::map<Anchor, std::size_t>& occurrence_end_lines,
                              bool catalog_complete) {
    exact_fields(value, {"id", "strength", "source", "actor", "summary", "applicability",
                         "testability", "scenarios", "evaluators", "rationale"}, "requirement",
                 "reviewed");
    bool reviewed = true;
    if (value.contains("reviewed")) {
        if (!value.at("reviewed").is_boolean()) {
            throw std::runtime_error("reviewed must be boolean");
        }
        reviewed = value.at("reviewed").get<bool>();
        if (!reviewed && catalog_complete) {
            throw std::runtime_error("A complete catalog cannot contain an unreviewed requirement");
        }
    }
    const auto& citation = value.at("source");
    exact_fields(citation, {"section", "first_line", "last_line", "occurrence", "clause"},
                 "source reference");

    const auto first = unsigned_integer(citation, "first_line");
    const auto last = unsigned_integer(citation, "last_line");
    if (first > last || last > line_count) {
        throw std::runtime_error("Citation line range is invalid or outside the draft");
    }
    Requirement row{required_string(value, "id"),
                    parse_strength(required_string(value, "strength")),
                    {required_string(citation, "section"), static_cast<std::size_t>(first),
                     static_cast<std::size_t>(last), ordinal(citation, "occurrence"),
                     ordinal(citation, "clause")},
                    required_string(value, "actor"), required_string(value, "summary"),
                    parse_applicability(required_string(value, "applicability")),
                    parse_testability(required_string(value, "testability")),
                    identifiers(value, "scenarios"), identifiers(value, "evaluators"),
                    required_string(value, "rationale"), reviewed};

    const auto end_line = occurrence_end_lines.find(
        Anchor{row.source.first_line, row.source.occurrence});
    if (end_line != occurrence_end_lines.end() && row.source.last_line < end_line->second) {
        throw std::runtime_error("Citation for " + row.id +
                                 " ends before the normative phrase ends at line " +
                                 std::to_string(end_line->second));
    }

    if (row.applicability == Applicability::Applicable) {
        if (row.testability == Testability::NotApplicable) {
            throw std::runtime_error("Applicable requirement cannot have NotApplicable testability");
        }
        if (row.testability == Testability::Testable &&
            (row.scenarios.empty() || row.evaluators.empty())) {
            throw std::runtime_error("Applicable testable requirement needs scenario and evaluator IDs");
        }
    } else if (row.testability != Testability::NotApplicable || !row.scenarios.empty() ||
               !row.evaluators.empty()) {
        throw std::runtime_error("Non-applicable or informative requirement has incompatible testing fields");
    }
    return row;
}

}  // namespace

RequirementCatalog RequirementCatalog::load(const DraftSource& source,
                                             const std::filesystem::path& path,
                                             CatalogLoadMode mode) {
    const auto size = std::filesystem::file_size(path);
    if (size > kMaxCatalogBytes) {
        throw std::runtime_error("Catalog exceeds size limit: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("Cannot open catalog: " + path.string());
    }
    Json document;
    try {
        document = Json::parse(input);
    } catch (const Json::exception& error) {
        throw std::runtime_error("Invalid catalog JSON: " + std::string(error.what()));
    }
    if (!input.good() && !input.eof()) {
        throw std::runtime_error("Cannot read catalog: " + path.string());
    }
    exact_fields(document, {"draft", "source_sha256", "complete", "requirements"}, "catalog");

    const auto draft_number = ordinal(document, "draft");
    const auto digest = required_string(document, "source_sha256");
    if (draft_number != source.number || digest != source.sha256) {
        throw std::runtime_error("Catalog draft or SHA-256 does not match source");
    }
    if (!document.at("complete").is_boolean()) {
        throw std::runtime_error("complete must be boolean");
    }
    const bool complete = document.at("complete").get<bool>();
    if (!complete && mode != CatalogLoadMode::AllowIncomplete) {
        throw std::runtime_error("Incomplete catalog cannot be loaded for production");
    }
    if (!document.at("requirements").is_array()) {
        throw std::runtime_error("requirements must be an array");
    }

    RequirementCatalog catalog{draft_number, digest, complete, {}};
    std::map<Anchor, std::size_t> occurrence_end_lines;
    for (const auto& occurrence : scan_normative_occurrences(source)) {
        occurrence_end_lines.emplace(
            Anchor{occurrence.first_line, occurrence.occurrence_on_line}, occurrence.last_line);
    }
    std::set<std::string> ids;
    std::set<std::tuple<std::size_t, unsigned, unsigned>> anchors;
    for (const auto& value : document.at("requirements")) {
        auto row = parse_requirement(value, source.line_offsets.size(), occurrence_end_lines, complete);
        if (!ids.insert(row.id).second) {
            throw std::runtime_error("Duplicate requirement ID: " + row.id);
        }
        if (!anchors.emplace(row.source.first_line, row.source.occurrence,
                             row.source.clause).second) {
            throw std::runtime_error("Duplicate semantic clause anchor: " + row.id);
        }
        catalog.requirements.push_back(std::move(row));
    }
    return catalog;
}

}  // namespace moq::interop::requirements
