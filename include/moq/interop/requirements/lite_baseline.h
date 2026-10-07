#pragma once

#include "moq/interop/requirements/catalog.h"

#include <filesystem>
#include <vector>

// Unreviewed baseline catalog for moq-lite-06 (draft number 106): one row per normative
// occurrence of the draft text, every row Applicable + NotTestable with reviewed:false, so the
// staged audit passes while the classification is still pending. Review replaces the rows by
// hand; after that only the guarded regeneration below is safe.

namespace moq::interop::requirements {

struct LiteBaselineOptions {
    std::filesystem::path requirements_dir;
    bool force{false};
};

// Pure: the baseline rows for `source` (source.number == 106), in occurrence order.
std::vector<Requirement> lite_baseline_rows(const DraftSource& source);

// Writes <requirements_dir>/moq-lite-06.json (complete:false). Throws std::runtime_error when the
// file exists and any of its rows is reviewed (or the file is unreadable), unless options.force;
// the existing file is left untouched in that case.
void write_lite_baseline(const DraftSource& source, const LiteBaselineOptions& options);

}  // namespace moq::interop::requirements
