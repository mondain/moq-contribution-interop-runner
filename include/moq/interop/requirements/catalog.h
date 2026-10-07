#pragma once

#include "moq/interop/requirements/draft_source.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace moq::interop::requirements {

enum class Strength { Must, MustNot, Should, ShouldNot, May };
enum class Applicability { Applicable, NotApplicable, Informative };
enum class Testability { Testable, NotTestable, NotApplicable };
enum class CatalogLoadMode { RequireComplete, AllowIncomplete };

struct SourceRef {
    std::string section;
    std::size_t first_line;
    std::size_t last_line;
    unsigned occurrence;
    unsigned clause;
};

struct Requirement {
    std::string id;
    Strength strength;
    SourceRef source;
    std::string actor;
    std::string summary;
    Applicability applicability;
    Testability testability;
    std::vector<std::string> scenarios;
    std::vector<std::string> evaluators;
    std::string rationale;
    // False only for a staged baseline row whose classification is still pending. Absent in the
    // JSON means true; a complete catalog cannot hold an unreviewed row.
    bool reviewed{true};
};

struct RequirementCatalog {
    unsigned draft;
    std::string source_sha256;
    bool complete;
    std::vector<Requirement> requirements;

    static RequirementCatalog load(const DraftSource& source, const std::filesystem::path& path,
                                   CatalogLoadMode mode = CatalogLoadMode::RequireComplete);
};

struct NormativeOccurrence {
    std::string phrase;
    Strength normalized_strength;
    std::size_t first_line;
    unsigned occurrence_on_line;
    bool quoted_bcp14_vocabulary;
    std::size_t last_line;
};

struct AuditReport {
    std::vector<NormativeOccurrence> missing;
    std::vector<NormativeOccurrence> multiply_classified;
    std::vector<std::string> errors;
    bool ok() const;
};

std::vector<NormativeOccurrence> scan_normative_occurrences(const DraftSource& source);
AuditReport audit_normative_occurrences(const DraftSource& source,
                                         const RequirementCatalog& catalog);
// Same checks as audit_normative_occurrences except that an incomplete catalog is not an error,
// for catalogs staged for review (rows may still be unreviewed).
AuditReport audit_normative_occurrences_staged(const DraftSource& source,
                                               const RequirementCatalog& catalog);

}  // namespace moq::interop::requirements
