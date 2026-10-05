#pragma once

#include "moq/interop/requirements/catalog.h"

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Carry-forward of a reviewed requirement catalog to a new draft.
//
// Purpose: match every normative occurrence of the new draft to the draft 21
// catalog rows, classify it (identical, moved, reworded, new, removed), and
// emit catalog partitions (requirements/parts/draft22-lines-*.json), the
// merged catalog (requirements/draft22.json) and a delta file
// (requirements/draft21-to-22-delta.json).
//
// Reproducibility: `generate` produces only the UNREVIEWED baseline (copied
// rows, placeholder rows for new occurrences, unreviewed delta entries). The
// reviewed draft 22 catalog lives in the hand-edited partition files and the
// delta notes. After review the only safe command is `merge`, which rebuilds
// the merged catalog from the partitions; `generate --force` over a reviewed
// catalog discards every hand edit.
//
// Known matcher limitations:
//  - clean_lines joins paragraphs across a page boundary, so a sentence next
//    to a page break can absorb the following paragraph; this misclassified
//    draft 22 lines ~3477 (D22-8-4-MAY-263) and ~3802 (D22-9-MUST-294) as
//    new, with their draft 21 rows as removed.
//  - Repeated twin sentences (SUBSCRIBE_NAMESPACE vs SUBSCRIBE_TRACKS) can be
//    cross-paired by the greedy exact pass; the result needs hand re-pairing.
//
// Lifetime: CarryResult holds pointers into old_catalog, so the caller must
// keep old_catalog alive while the result is used.

namespace moq::interop::requirements {

std::string normalize_text(std::string_view raw);

struct OccurrenceContext {
    std::size_t first_line{0};
    std::size_t last_line{0};
    unsigned occurrence_on_line{0};
    Strength strength{Strength::Must};
    std::string section;
    std::string section_title;
    std::string sentence;
    unsigned ordinal_in_sentence{0};
    std::size_t sentence_last_line{0};
};

std::vector<OccurrenceContext> extract_contexts(const DraftSource& source);

enum class DeltaClass { Identical, Moved, Reworded, New, Removed };

const char* to_string(DeltaClass change);

struct CarryMatch {
    DeltaClass change{DeltaClass::New};
    OccurrenceContext target;
    std::vector<const Requirement*> sources;
    double similarity{0.0};
};

struct CarryResult {
    std::vector<CarryMatch> matches;
    std::vector<const Requirement*> removed;
};

CarryResult carry_forward(const DraftSource& old_source, const RequirementCatalog& old_catalog,
                          const DraftSource& new_source);

struct WireDelta {
    std::vector<std::string> added;
    std::vector<std::string> removed;
    std::vector<std::string> changed;
};

std::map<std::string, std::string> extract_wire_blocks(const DraftSource& source);
WireDelta diff_wire_blocks(const DraftSource& old_source, const DraftSource& new_source);

struct EmitOptions {
    std::filesystem::path requirements_dir;
    std::size_t partition_lines{1200};
    bool force{false};
};

void write_catalog_outputs(const CarryResult& result, const RequirementCatalog& old_catalog,
                           const DraftSource& new_source, const WireDelta& wire_delta,
                           const EmitOptions& options);

void merge_partitions(const DraftSource& new_source, const std::filesystem::path& requirements_dir);

}  // namespace moq::interop::requirements
