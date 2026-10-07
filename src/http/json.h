#pragma once

#include "moq/interop/app/draft_traits.h"

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace moq::interop::http::detail {

// The external JSON form of a draft: an integer for the MoQ Transport drafts, the text name for moq-lite.
// The number 106 is an internal storage form and never leaves the process.
inline nlohmann::json draft_json(app::DraftVersion draft) {
    if (app::is_moqt(draft)) return app::draft_number(draft);
    return std::string(app::draft_text(draft));
}

// The same for a catalog's stored draft number; an unknown number is shown as itself.
inline nlohmann::json catalog_draft_json(unsigned number) {
    const auto draft = app::parse_draft(number);
    return draft ? draft_json(*draft) : nlohmann::json(number);
}

// The draft as it reads in a message or a page: the number for MoQ Transport, the name for moq-lite.
inline std::string draft_display(app::DraftVersion draft) {
    return app::is_moqt(draft) ? std::to_string(app::draft_number(draft)) : std::string(app::draft_text(draft));
}

// A completeness-JSON draft (an integer, or the moq-lite name) as table cell text; anything else is empty.
inline std::string draft_cell(const nlohmann::json& value) {
    if (value.is_string()) return value.get<std::string>();
    if (value.is_number_integer()) return std::to_string(value.get<long long>());
    return {};
}

// Accepts the integers 18, 21 and 22 and the string "moq-lite-06"; everything else (floats, negatives,
// other strings, other types, and the internal integer 106) is refused.
inline std::optional<app::DraftVersion> parse_draft_json(const nlohmann::json& value) {
    if (value.is_number_integer()) {
        const auto number = value.get<long long>();
        if (number < 0 || number > 0xffff) return std::nullopt;
        const auto draft = app::parse_draft(static_cast<unsigned>(number));
        if (!draft || !app::is_moqt(*draft)) return std::nullopt;
        return draft;
    }
    if (value.is_string()) return app::parse_draft_text(value.get<std::string>());
    return std::nullopt;
}

}  // namespace moq::interop::http::detail
