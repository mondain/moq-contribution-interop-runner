#include "moq/interop/requirements/carry_forward.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/wire/draft21/message_types.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moq::interop::wire {
namespace {

namespace req = moq::interop::requirements;

const std::filesystem::path kRoot = MOQ_INTEROP_PROJECT_SOURCE_DIR;

req::DraftSource load(unsigned draft) {
    return req::load_draft_source(draft, kRoot / "docs",
                                  kRoot / "requirements/draft-digests.json");
}

std::string trim(std::string_view text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string_view::npos) return {};
    const auto end = text.find_last_not_of(" \t");
    return std::string(text.substr(begin, end - begin + 1));
}

struct TableRow {
    std::string code;  // 0x<lowercase hex, no leading zeros>, or the lowercased grease form
    std::string name;  // second cell without a trailing "(Section N)" reference
    std::string flags; // third cell, empty when absent
};

std::string normalize_code(const std::string& cell) {
    static const std::regex simple(R"(^0[xX]([0-9A-Fa-f]+)$)");
    std::smatch match;
    if (std::regex_match(cell, match, simple)) {
        // String based so 0xffffffffffffffffff (more than 64 bits) is handled too.
        std::string digits = match[1].str();
        std::transform(digits.begin(), digits.end(), digits.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        const auto first_nonzero = digits.find_first_not_of('0');
        digits = first_nonzero == std::string::npos ? "0" : digits.substr(first_nonzero);
        return "0x" + digits;
    }
    std::string lowered = cell;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered;
}

// Every ASCII-art table row whose first or second cell is a code point, keyed by the title of the
// heading above it. Section numbers are not used because they shift between drafts.
std::map<std::string, std::vector<TableRow>> table_rows(const req::DraftSource& source) {
    static const std::regex heading(R"(^((?:[0-9]+|[A-Z])(?:\.[0-9]+)*)\.\s{2,}(\S.*)$)");
    static const std::regex code_cell(
        R"(^0[xX][0-9A-Fa-f]+(?: \* N \+ 0[xX][0-9A-Fa-f]+)?$)");
    static const std::regex section_ref(R"(\s*\((?:see )?Section [0-9A-Z.]+\)\s*$)");
    std::map<std::string, std::vector<TableRow>> tables;
    std::string title;
    TableRow* open_row = nullptr;  // the row a wrapped continuation line extends
    std::istringstream input(source.text);
    for (std::string line; std::getline(input, line);) {
        std::smatch heading_match;
        if (std::regex_match(line, heading_match, heading)) {
            title = trim(heading_match[2].str());
            open_row = nullptr;
            continue;
        }
        const auto first = line.find_first_not_of(' ');
        if (first == std::string::npos || line[first] != '|') {
            open_row = nullptr;
            continue;
        }
        std::vector<std::string> cells;
        std::size_t start = first + 1;
        for (auto bar = line.find('|', start); bar != std::string::npos;
             bar = line.find('|', start)) {
            cells.push_back(trim(line.substr(start, bar - start)));
            start = bar + 1;
        }
        if (cells.size() >= 2 && cells[0].empty() && open_row != nullptr) {
            // A cell wrapped onto a second line (draft 21 wraps some rows that draft 22 does not).
            if (!cells[1].empty()) open_row->name += " " + cells[1];
            if (cells.size() > 2 && !cells[2].empty()) {
                open_row->flags += open_row->flags.empty() ? cells[2] : " " + cells[2];
            }
            continue;
        }
        open_row = nullptr;
        if (cells.size() < 2) continue;
        // Most tables lead with the code point; the IANA error code tables (draft 22
        // section 16.11) lead with the name and carry the code in the second cell.
        const bool code_first = std::regex_match(cells[0], code_cell);
        const bool name_first = !code_first && std::regex_match(cells[1], code_cell);
        if (!code_first && !name_first) continue;
        auto& rows = tables[title];
        if (code_first) {
            rows.push_back({normalize_code(cells[0]), cells[1],
                            cells.size() > 2 ? cells[2] : std::string{}});
        } else {
            rows.push_back({normalize_code(cells[1]), cells[0],
                            cells.size() > 2 ? cells[2] : std::string{}});
        }
        open_row = &rows.back();
    }
    for (auto& entry : tables) {
        for (auto& row : entry.second) {
            row.name = trim(std::regex_replace(row.name, section_ref, ""));
        }
    }
    return tables;
}

// Tables whose (code, name) rows differ between drafts 21 and 22 but were reviewed against
// the draft text and found editorial. The value is the justification, with draft line numbers.
// This list starts empty: an entry is added only after reading both texts.
const std::map<std::string, std::string> kReviewedDifferences = {};

TEST(Draft22WireAudit, OnlyTheReviewedWireFiguresChangedBetweenDrafts) {
    // The figure diff is the evidence that every re-exported draft 21 module's wire
    // definition is unchanged in draft 22 (requirements/draft21-to-22-delta.json, wire_delta).
    const auto delta = req::diff_wire_blocks(load(21), load(22));
    EXPECT_TRUE(delta.added.empty());
    EXPECT_TRUE(delta.removed.empty());
    EXPECT_EQ(delta.changed, (std::vector<std::string>{"LOCATION_FILTER Parameter",
                                                       "PUBLISH_NAMESPACE Message"}));
    EXPECT_EQ(req::extract_wire_blocks(load(22)).size(), 41u);
    EXPECT_EQ(req::extract_wire_blocks(load(21)).size(), 41u);
}

TEST(Draft22WireAudit, RegistryTablesMatchBetweenDrafts) {
    const auto old_tables = table_rows(load(21));
    const auto new_tables = table_rows(load(22));
    std::set<std::string> titles;
    for (const auto& entry : old_tables) titles.insert(entry.first);
    for (const auto& entry : new_tables) titles.insert(entry.first);

    using Key = std::pair<std::string, std::string>;
    const auto keys_for = [](const std::map<std::string, std::vector<TableRow>>& tables,
                             const std::string& title) {
        std::set<Key> keys;
        const auto it = tables.find(title);
        if (it != tables.end()) {
            for (const auto& row : it->second) keys.insert({row.code, row.name});
        }
        return keys;
    };

    std::size_t compared = 0;
    for (const auto& title : titles) {
        const auto before = keys_for(old_tables, title);
        const auto after = keys_for(new_tables, title);
        if (before == after) {
            compared += before.empty() ? 0 : 1;
            continue;
        }
        if (kReviewedDifferences.contains(title)) continue;
        std::string detail;
        for (const auto& key : before) {
            if (!after.contains(key)) detail += "\n  only in draft 21: " + key.first + " " + key.second;
        }
        for (const auto& key : after) {
            if (!before.contains(key)) detail += "\n  only in draft 22: " + key.first + " " + key.second;
        }
        ADD_FAILURE() << "table(s) under '" << title
                      << "' differ between drafts 21 and 22:" << detail;
    }
    EXPECT_GE(compared, 8u) << "the table scan compared too few registries to be meaningful";
}

TEST(Draft22WireAudit, MessageTypeTableMatchesTheSharedClassifier) {
    // draft-ietf-moq-transport-22 section 9, Table 5.
    static const std::map<std::string, draft21::MessageKind> kinds = {
        {"SETUP", draft21::MessageKind::Setup},
        {"GOAWAY", draft21::MessageKind::Goaway},
        {"SUBSCRIBE", draft21::MessageKind::Subscribe},
        {"SUBSCRIBE_OK", draft21::MessageKind::SubscribeOk},
        {"PUBLISH_STATE_NOTIFY", draft21::MessageKind::PublishStateNotify},
        {"PUBLISH", draft21::MessageKind::Publish},
        {"PUBLISH_DONE", draft21::MessageKind::PublishDone},
        {"FETCH", draft21::MessageKind::Fetch},
        {"FETCH_OK", draft21::MessageKind::FetchOk},
        {"TRACK_STATUS", draft21::MessageKind::TrackStatus},
        {"PUBLISH_NAMESPACE", draft21::MessageKind::PublishNamespace},
        {"SUBSCRIBE_NAMESPACE", draft21::MessageKind::SubscribeNamespace},
        {"SUBSCRIBE_TRACKS", draft21::MessageKind::SubscribeTracks},
        {"NAMESPACE", draft21::MessageKind::Namespace},
        {"NAMESPACE_DONE", draft21::MessageKind::NamespaceDone},
        {"PUBLISH_SKIPPED", draft21::MessageKind::PublishSkipped},
        {"REQUEST_UPDATE", draft21::MessageKind::RequestUpdate},
        {"REQUEST_OK", draft21::MessageKind::RequestOk},
        {"REQUEST_ERROR", draft21::MessageKind::RequestError},
    };
    static const std::regex flag_cell(R"(^(Control|Request|First)(, (Control|Request|First))*$)");
    const auto tables = table_rows(load(22));
    std::size_t checked = 0;
    std::size_t reserved = 0;
    for (const auto& [title, rows] : tables) {
        for (const auto& row : rows) {
            if (!std::regex_match(row.flags, flag_cell)) continue;
            SCOPED_TRACE(title + ": " + row.code + " " + row.name);
            const auto code = std::stoull(row.code.substr(2), nullptr, 16);
            const auto info = draft21::classify_message_type(code);
            if (row.name.rfind("RESERVED", 0) == 0) {
                // Reserved historical types (here 0x1E, PUBLISH_OK in <= 17) are deliberately
                // absent from the classifier, so they must stay unclassified.
                EXPECT_FALSE(info.has_value());
                ++reserved;
                continue;
            }
            ASSERT_TRUE(info.has_value());
            EXPECT_EQ(info->control, row.flags.find("Control") != std::string::npos);
            EXPECT_EQ(info->request, row.flags.find("Request") != std::string::npos);
            EXPECT_EQ(info->request_starter, row.flags.find("First") != std::string::npos);
            ASSERT_TRUE(kinds.contains(row.name));
            EXPECT_EQ(info->kind, kinds.at(row.name));
            ++checked;
        }
    }
    EXPECT_EQ(checked, 19u);
    EXPECT_EQ(reserved, 1u);
}

}  // namespace
}  // namespace moq::interop::wire
