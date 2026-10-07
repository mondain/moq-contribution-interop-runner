#include "moq/interop/requirements/lite_baseline.h"

#include "moq/interop/requirements/carry_forward.h"

#include <algorithm>
#include <fstream>
#include <stdexcept>
#include <string>

#include <nlohmann/json.hpp>

namespace moq::interop::requirements {
namespace {

constexpr unsigned kLiteDraft = 106;

void refuse_reviewed_overwrite(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) {
        return;
    }
    std::ifstream input(path, std::ios::binary);
    const auto document = nlohmann::json::parse(input, nullptr, false);
    if (document.is_discarded() || !document.is_object() || !document.contains("requirements") ||
        !document.at("requirements").is_array()) {
        throw std::runtime_error(path.string() + " exists but is not a readable catalog; pass --force");
    }
    for (const auto& row : document.at("requirements")) {
        const bool unreviewed =
            row.is_object() && row.contains("reviewed") && row.at("reviewed") == false;
        if (!unreviewed) {
            throw std::runtime_error(path.string() +
                                     " has reviewed rows; pass --force to discard the review");
        }
    }
}

}  // namespace

std::vector<Requirement> lite_baseline_rows(const DraftSource& source) {
    if (source.number != kLiteDraft) {
        throw std::invalid_argument("lite baseline requires draft 106");
    }
    std::vector<Requirement> rows;
    unsigned sequence = 0;
    for (const auto& context : extract_contexts(source)) {
        const auto summary = context.sentence.empty()
            ? "Unreviewed moq-lite-06 requirement at line " + std::to_string(context.first_line)
            : context.sentence.substr(0, 160);
        rows.push_back(Requirement{
            requirement_id("L06", context.section, context.strength, ++sequence),
            context.strength,
            {context.section, context.first_line,
             std::max(context.last_line, context.sentence_last_line), context.occurrence_on_line, 1},
            "endpoint",
            summary,
            Applicability::Applicable,
            Testability::NotTestable,
            {},
            {},
            "Unreviewed: classification pending.",
            false});
    }
    return rows;
}

void write_lite_baseline(const DraftSource& source, const LiteBaselineOptions& options) {
    const auto path = options.requirements_dir / "moq-lite-06.json";
    if (!options.force) {
        refuse_reviewed_overwrite(path);
    }
    write_catalog_file(path, source, lite_baseline_rows(source), false);
}

}  // namespace moq::interop::requirements
