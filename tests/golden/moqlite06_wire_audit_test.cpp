// Audit of the moq-lite-06 codecs against docs/draft-lcurley-moq-lite-06.txt.
//
// The draft loader and digest for moq-lite are L1c's job, so this test reads the text file directly. A
// checkout without the (untracked) draft text skips every test with a message instead of failing.
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/fetch.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/goaway.h"
#include "moq/interop/wire/moqlite06/probe.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/subscribe.h"
#include "moq/interop/wire/moqlite06/track.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace moq::interop::wire::moqlite06 {
namespace {

const std::filesystem::path kDraftPath =
    std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR) / "docs/draft-lcurley-moq-lite-06.txt";

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t\r\f");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t\r\f");
    return text.substr(begin, end - begin + 1);
}

std::optional<std::vector<std::string>> draft_lines() {
    std::ifstream input(kDraftPath);
    if (!input) return std::nullopt;
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) lines.push_back(line);
    return lines;
}

#define REQUIRE_DRAFT(lines_name)                                                              \
    const auto lines_name##_opt = draft_lines();                                               \
    if (!lines_name##_opt) {                                                                   \
        GTEST_SKIP() << "docs/draft-lcurley-moq-lite-06.txt is absent (untracked draft text); " \
                        "skipping the moq-lite-06 draft audit";                                \
    }                                                                                          \
    const auto& lines_name = *lines_name##_opt

bool is_page_furniture(const std::string& line) {
    const auto text = trim(line);
    return text.empty() || text.rfind("Curley ", 0) == 0 || text.rfind("Internet-Draft ", 0) == 0;
}

// One field line of a message figure, normalized: no trailing comma, single spaces.
std::string normalize_field(const std::string& line) {
    std::string text = trim(line);
    if (!text.empty() && text.back() == ',') text.pop_back();
    return std::regex_replace(trim(text), std::regex(R"(\s+)"), " ");
}

// Every `NAME Message {` figure inside section 7, in order of appearance, with its field lines.
std::vector<std::pair<std::string, std::vector<std::string>>> section7_figures(
    const std::vector<std::string>& lines) {
    static const std::regex section_start(R"(^7\.\s+Encoding\s*$)");
    static const std::regex next_section(R"(^8\.\s+\S.*$)");
    static const std::regex figure_open(R"(^   ([A-Z][A-Z_]*) Message \{\s*$)");
    std::vector<std::pair<std::string, std::vector<std::string>>> figures;
    bool inside = false;
    std::optional<std::size_t> open;
    for (const auto& line : lines) {
        if (!inside) {
            inside = std::regex_match(line, section_start);
            continue;
        }
        if (std::regex_match(line, next_section)) break;
        std::smatch match;
        if (!open && std::regex_match(line, match, figure_open)) {
            figures.push_back({match[1].str(), {}});
            open = figures.size() - 1;
            continue;
        }
        if (!open) continue;
        if (is_page_furniture(line)) continue;
        if (trim(line) == "}") {
            open.reset();
            continue;
        }
        figures[*open].second.push_back(normalize_field(line));
    }
    return figures;
}

// The ASCII table whose caption line is `caption` (for example "Table 4"): its data rows as trimmed cells,
// continuation rows (empty first cell) dropped. Page headers and footers inside a table are skipped.
std::vector<std::vector<std::string>> table_before(const std::vector<std::string>& lines,
                                                  const std::string& caption) {
    std::size_t caption_line = lines.size();
    for (std::size_t i = 0; i < lines.size(); ++i) {
        if (trim(lines[i]) == caption) {
            caption_line = i;
            break;
        }
    }
    std::vector<std::vector<std::string>> rows;
    if (caption_line == lines.size()) return rows;
    for (std::size_t i = caption_line; i-- > 0;) {
        const auto text = trim(lines[i]);
        if (is_page_furniture(lines[i])) continue;
        if (text.front() == '+') continue;
        if (text.front() != '|') break;
        std::vector<std::string> cells;
        std::size_t start = 1;
        for (auto bar = text.find('|', start); bar != std::string::npos; bar = text.find('|', start)) {
            cells.push_back(trim(text.substr(start, bar - start)));
            start = bar + 1;
        }
        // Skips continuation rows (empty first cell) and the header row (its first cell is not a 0x code).
        if (cells.empty() || cells[0].rfind("0x", 0) != 0) continue;
        rows.insert(rows.begin(), cells);
    }
    return rows;
}

std::uint64_t parse_code(const std::string& cell) { return std::stoull(cell, nullptr, 16); }

// --- Message mapping -------------------------------------------------------------------------------------

struct Mapping {
    bool implemented;
    std::string detail;                    // the codec functions
    std::vector<std::string> fields;       // the figure, mirroring the codec's field order
    std::optional<std::uint64_t> type;     // the `Type (i) = 0xN` value, for the Type-prefixed messages
};

const std::map<std::string, Mapping>& mappings() {
    static const std::map<std::string, Mapping> table = {
        {"SETUP",
         {true, "encode_setup/decode_setup/build_setup/read_capabilities",
          {"Message Length (i)", "Parameter Count (i)", "Setup Parameter (..) ..."}, std::nullopt}},
        {"ANNOUNCE_REQUEST",
         {true, "encode_announce_request/decode_announce_request",
          {"Message Length (i)", "Broadcast Path Prefix (s)"}, std::nullopt}},
        {"ANNOUNCE_OK",
         {true, "encode_announce_ok/decode_announce_ok",
          {"Message Length (i)", "Hop ID (i)", "Active Count (i)"}, std::nullopt}},
        {"ANNOUNCE_START",
         {true, "encode_announce_message/decode_announce_message (AnnounceStart)",
          {"Type (i) = 0x0", "Message Length (i)", "Route Prefix Suffix (s)", "Hop Count (i)",
           "Hop ID (i) ...", "Warm Route Cost (i)", "Cold Route Cost (i)"},
          kAnnounceTypeStart}},
        {"ANNOUNCE_END",
         {true, "encode_announce_message/decode_announce_message (AnnounceEnd)",
          {"Type (i) = 0x1", "Message Length (i)", "Announce ID (i)"}, kAnnounceTypeEnd}},
        {"ANNOUNCE_UPDATE",
         {true, "encode_announce_message/decode_announce_message (AnnounceUpdate)",
          {"Type (i) = 0x2", "Message Length (i)", "Announce ID (i)", "Hop Count (i)", "Hop ID (i) ...",
           "Warm Route Cost (i)", "Cold Route Cost (i)"},
          kAnnounceTypeUpdate}},
        {"SUBSCRIBE",
         {true, "encode_subscribe/decode_subscribe",
          {"Message Length (i)", "Subscribe ID (i)", "Broadcast Path (s)", "Track Name (s)",
           "Subscriber Priority (8)", "Subscriber Max Age (i)", "Group Start (i)", "Group End (i)",
           "Frame Start (i)", "Frame End (i)"},
          std::nullopt}},
        {"SUBSCRIBE_UPDATE",
         {true, "encode_subscribe_update/decode_subscribe_update",
          {"Message Length (i)", "Subscriber Priority (8)", "Subscriber Max Age (i)", "Group Start (i)",
           "Group End (i)", "Frame Start (i)", "Frame End (i)"},
          std::nullopt}},
        {"SUBSCRIBE_OK",
         {true, "encode_subscribe_response/decode_subscribe_response (SubscribeOk)",
          {"Type (i) = 0x0", "Message Length (i)", "Group (i)"}, kSubscribeTypeOk}},
        {"SUBSCRIBE_END",
         {true, "encode_subscribe_response/decode_subscribe_response (SubscribeEnd)",
          {"Type (i) = 0x1", "Message Length (i)", "Group (i)"}, kSubscribeTypeEnd}},
        {"SUBSCRIBE_DROP",
         {true, "encode_subscribe_response/decode_subscribe_response (SubscribeDrop)",
          {"Type (i) = 0x2", "Message Length (i)", "Group Start (i)", "Group End (i)", "Error Code (i)"},
          kSubscribeTypeDrop}},
        {"GROUP",
         {true, "encode_group_header/decode_group_header",
          {"Message Length (i)", "Subscribe ID (i)", "Group Sequence (i)", "Frame Start (i)"},
          std::nullopt}},
        {"FRAME",
         {true, "encode_frame/decode_frame",
          {"Timestamp Delta (i)", "Message Length (i)", "Payload (b)"}, std::nullopt}},
        {"TRACK",
         {true, "encode_track_request/decode_track_request",
          {"Message Length (i)", "Broadcast Path (s)", "Track Name (s)"}, std::nullopt}},
        {"TRACK_INFO",
         {true, "encode_track_info/decode_track_info",
          {"Message Length (i)", "Publisher Priority (8)", "Publisher Max Age (i)", "Timescale (i)"}, std::nullopt}},
        {"FETCH",
         {true, "encode_fetch_request/decode_fetch_request",
          {"Message Length (i)", "Broadcast Path (s)", "Track Name (s)", "Subscriber Priority (8)",
           "Group Sequence (i)", "Frame Start (i)", "Frame End (i)"},
          std::nullopt}},
        {"PROBE",
         {true, "encode_probe/decode_probe", {"Message Length (i)", "Bitrate (i)", "RTT (i)"}, std::nullopt}},
        {"GOAWAY",
         {true, "encode_goaway/decode_goaway", {"Message Length (i)", "New Session URI (s)"}, std::nullopt}},
    };
    return table;
}

TEST(MoqLite06WireAudit, Section7MessageSetMatchesTheMapping) {
    REQUIRE_DRAFT(lines);
    const auto figures = section7_figures(lines);
    std::set<std::string> drafted;
    for (const auto& figure : figures) {
        EXPECT_TRUE(drafted.insert(figure.first).second) << "duplicate figure " << figure.first;
    }
    std::set<std::string> mapped;
    for (const auto& entry : mappings()) mapped.insert(entry.first);
    EXPECT_EQ(mapped.size(), 18u);
    for (const auto& name : drafted) {
        EXPECT_TRUE(mapped.contains(name))
            << name << " is in draft section 7 but has no mapping in this audit";
    }
    for (const auto& name : mapped) {
        EXPECT_TRUE(drafted.contains(name)) << name << " is mapped but has no figure in draft section 7";
    }
}

// L2a closes the audit: every message of draft section 7 has a codec, so nothing is out of scope any more.
TEST(MoqLite06WireAudit, EveryMessageIsImplementedAndNamesItsCodec) {
    for (const auto& [name, mapping] : mappings()) {
        EXPECT_TRUE(mapping.implemented) << name << " must have a codec";
        EXPECT_FALSE(mapping.detail.empty()) << name << " must name its codec functions";
    }
}

TEST(MoqLite06WireAudit, ImplementedFiguresMatchTheCodecFieldOrder) {
    REQUIRE_DRAFT(lines);
    std::map<std::string, std::vector<std::string>> figures;
    for (auto& figure : section7_figures(lines)) figures[figure.first] = std::move(figure.second);
    std::size_t compared = 0;
    for (const auto& [name, mapping] : mappings()) {
        if (!mapping.implemented) continue;
        SCOPED_TRACE(name);
        const auto it = figures.find(name);
        ASSERT_NE(it, figures.end());
        EXPECT_EQ(it->second, mapping.fields) << "the draft figure no longer matches the codec field order";
        ++compared;
    }
    EXPECT_EQ(compared, 18u);
}

TEST(MoqLite06WireAudit, TypePrefixedMessagesPinTheSixTypeValues) {
    REQUIRE_DRAFT(lines);
    std::map<std::string, std::vector<std::string>> figures;
    for (auto& figure : section7_figures(lines)) figures[figure.first] = std::move(figure.second);
    static const std::regex type_field(R"(^Type \(i\) = 0x([0-9A-Fa-f]+)$)");
    std::map<std::string, std::uint64_t> in_draft;
    for (const auto& [name, fields] : figures) {
        if (fields.empty()) continue;
        std::smatch match;
        if (std::regex_match(fields.front(), match, type_field)) in_draft[name] = parse_code(match[1].str());
        // Type is only ever the first field (it precedes Message Length).
        for (std::size_t i = 1; i < fields.size(); ++i) {
            EXPECT_NE(fields[i].rfind("Type (i)", 0), 0u) << name << " has a Type field that is not first";
        }
    }
    const std::map<std::string, std::uint64_t> expected = {
        {"ANNOUNCE_START", kAnnounceTypeStart}, {"ANNOUNCE_END", kAnnounceTypeEnd},
        {"ANNOUNCE_UPDATE", kAnnounceTypeUpdate}, {"SUBSCRIBE_OK", kSubscribeTypeOk},
        {"SUBSCRIBE_END", kSubscribeTypeEnd}, {"SUBSCRIBE_DROP", kSubscribeTypeDrop},
    };
    EXPECT_EQ(in_draft, expected);
    EXPECT_EQ(kAnnounceTypeStart, 0x0u);
    EXPECT_EQ(kAnnounceTypeEnd, 0x1u);
    EXPECT_EQ(kAnnounceTypeUpdate, 0x2u);
    EXPECT_EQ(kSubscribeTypeOk, 0x0u);
    EXPECT_EQ(kSubscribeTypeEnd, 0x1u);
    EXPECT_EQ(kSubscribeTypeDrop, 0x2u);
    // The mapping table agrees with the codec constants too.
    for (const auto& [name, mapping] : mappings()) {
        if (mapping.type) EXPECT_EQ(in_draft[name], *mapping.type) << name;
    }
}

TEST(MoqLite06WireAudit, StreamTypeTablesMatchTheEnums) {
    REQUIRE_DRAFT(lines);
    // Table 4 (bidirectional): ID, Stream, Creator.
    const std::vector<std::vector<std::string>> bidi_expected = {
        {"0x1", "Announce", "Subscriber"}, {"0x2", "Subscribe", "Subscriber"},
        {"0x3", "Fetch", "Subscriber"},    {"0x4", "Probe", "Subscriber"},
        {"0x5", "Goaway", "Either"},       {"0x6", "Track", "Subscriber"},
    };
    EXPECT_EQ(table_before(lines, "Table 4"), bidi_expected);
    const std::map<std::string, BidiStreamType> bidi_enum = {
        {"Announce", BidiStreamType::Announce}, {"Subscribe", BidiStreamType::Subscribe},
        {"Fetch", BidiStreamType::Fetch},       {"Probe", BidiStreamType::Probe},
        {"Goaway", BidiStreamType::Goaway},     {"Track", BidiStreamType::Track},
    };
    for (const auto& row : table_before(lines, "Table 4")) {
        const auto value = parse_code(row[0]);
        ASSERT_TRUE(bidi_enum.contains(row[1])) << row[1];
        EXPECT_EQ(static_cast<std::uint64_t>(bidi_enum.at(row[1])), value) << row[1];
        EXPECT_EQ(as_bidi_stream_type(value), bidi_enum.at(row[1])) << row[1];
    }
    EXPECT_EQ(bidi_enum.size(), 6u);

    // Table 5 (unidirectional).
    const std::vector<std::vector<std::string>> uni_expected = {
        {"0x0", "Group", "Publisher"},
        {"0x1", "Setup", "Either"},
    };
    EXPECT_EQ(table_before(lines, "Table 5"), uni_expected);
    EXPECT_EQ(static_cast<std::uint64_t>(UniStreamType::Group), 0x0u);
    EXPECT_EQ(static_cast<std::uint64_t>(UniStreamType::Setup), 0x1u);
    EXPECT_EQ(as_uni_stream_type(0x0), UniStreamType::Group);
    EXPECT_EQ(as_uni_stream_type(0x1), UniStreamType::Setup);
    EXPECT_FALSE(as_uni_stream_type(0x2).has_value());
}

TEST(MoqLite06WireAudit, SetupParameterTableMatchesTheConstants) {
    REQUIRE_DRAFT(lines);
    const std::vector<std::vector<std::string>> expected = {
        {"0x1", "Probe", "Level (i)"}, {"0x2", "Path", "Path (s)"},   {"0x3", "Role", "Role (i)"},
        {"0x4", "Cost", "Cost (i)"},   {"0x5", "Hop", "Hop ID (i)"},
    };
    EXPECT_EQ(table_before(lines, "Table 6"), expected);
    const std::map<std::string, std::uint64_t> constants = {
        {"Probe", kParamProbe}, {"Path", kParamPath}, {"Role", kParamRole},
        {"Cost", kParamCost},   {"Hop", kParamHop},
    };
    for (const auto& row : table_before(lines, "Table 6")) {
        ASSERT_TRUE(constants.contains(row[1])) << row[1];
        EXPECT_EQ(constants.at(row[1]), parse_code(row[0])) << row[1];
    }
    EXPECT_EQ(constants.size(), 5u);
}

// --- Error-code tables, pinned as data for L1d (no codec uses them yet) ---------------------------------------

using CodeList = std::vector<std::pair<std::uint64_t, std::string>>;

CodeList code_rows(const std::vector<std::string>& lines, const std::string& caption) {
    CodeList codes;
    for (const auto& row : table_before(lines, caption)) {
        if (row.size() >= 2) codes.push_back({parse_code(row[0]), row[1]});
    }
    return codes;
}

// Draft 4.4.1, Table 2.
const CodeList kSessionErrorCodes = {
    {0x0, "NO_ERROR"},
    {0x1, "INTERNAL_ERROR"},
    {0x2, "UNAUTHORIZED"},
    {0x3, "PROTOCOL_VIOLATION"},
    {0x6, "KEY_VALUE_FORMATTING_ERROR"},
    {0x10, "GOAWAY_TIMEOUT"},
    {0x11, "CONTROL_MESSAGE_TIMEOUT"},
    {0x15, "VERSION_NEGOTIATION_FAILED"},
};

// Draft 4.4.2, Table 3.
const CodeList kStreamErrorCodes = {
    {0x0, "INTERNAL_ERROR"},   {0x1, "CANCELLED"},        {0x2, "DELIVERY_TIMEOUT"},
    {0x3, "SESSION_CLOSED"},   {0x4, "GOING_AWAY"},       {0x5, "TOO_FAR_BEHIND"},
    {0x12, "MALFORMED_TRACK"}, {0x30, "NO_CAPACITY"},     {0x31, "CONTROL_TIMEOUT"},
    {0x32, "GROUP_TOO_LARGE"}, {0x33, "NOT_FOUND"},       {0x34, "OLD"},
    {0x35, "EVICTED"},         {0x36, "UNROUTABLE"},      {0x37, "WRONG_SIZE"},
    {0x38, "FRAME_TOO_LARGE"}, {0x39, "TIMESTAMP_MISMATCH"},
};

TEST(MoqLite06WireAudit, SessionErrorCodesArePinned) {
    REQUIRE_DRAFT(lines);
    EXPECT_EQ(code_rows(lines, "Table 2"), kSessionErrorCodes);
}

TEST(MoqLite06WireAudit, StreamErrorCodesArePinned) {
    REQUIRE_DRAFT(lines);
    EXPECT_EQ(code_rows(lines, "Table 3"), kStreamErrorCodes);
}

TEST(MoqLite06WireAudit, ErrorCodeSpacesAreSeparateAndKeepTheirRanges) {
    // Draft 4.4: nothing moq-lite specific below 32, 32-47 reserved, 48-63 moq-lite's own, 64+ applications.
    for (const auto& [code, name] : kStreamErrorCodes) {
        const bool shared = code < 32;
        const bool own = code >= 48 && code <= 63;
        EXPECT_TRUE(shared || own) << name << " is in a reserved or application range";
    }
    // The same number means different things in each space (0x1 is INTERNAL_ERROR vs CANCELLED).
    EXPECT_NE(kSessionErrorCodes[1].second, kStreamErrorCodes[1].second);
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
