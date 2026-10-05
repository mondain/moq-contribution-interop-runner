#pragma once

#include "moq/interop/requirements/catalog.h"

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

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

}  // namespace moq::interop::requirements
