#pragma once

#include "moq/interop/requirements/catalog.h"

#include <cstddef>
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

}  // namespace moq::interop::requirements
