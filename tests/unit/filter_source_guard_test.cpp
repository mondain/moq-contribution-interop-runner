// Static guard: no scenario writes LOCATION_FILTER (0x21) bytes outside the wire-aware builder.
//
// Where draft 21 and draft 22 bytes coincide (a field count equal to a draft 22 Type, single-byte
// fields) no behavioral test can tell a bypass from the builder (tests/integration/
// draft22_filter_guard_test.cpp covers the sites where they differ). This test reads src/scenarios
// instead: every `0x21` in code (comments are ignored) must be one of the known uses below, counted per
// file. A new write must go through scenarios::filter_param_value / filter_param
// (include/moq/interop/scenarios/location_filter_param.h), which emits the active wire draft's form.
#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {

const std::filesystem::path kScenarios = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR) / "src/scenarios";

// Every `0x21` token allowed in scenario code, per file (path relative to src/scenarios): how many, how
// many of them write a filter (each must be backed by a filter_param_value call in the same file), and
// what they are.
struct Allowed {
    int count;
    int writes;
    const char* uses;
};
const std::map<std::string, Allowed> kAllowed{
    // The builder and the shared reader themselves.
    {"location_filter_param.cpp", {0, 0, "the builder"}},
    {"parameter_walk.cpp", {1, 0, "kLocationFilter: the wire-aware reader"}},
    {"draft21_contribution_support.cpp", {1, 1, "filter_param(): {0x21, filter_param_value(fields)}"}},
    // A type delta immediately followed by filter_param_value's bytes.
    {"draft21_gap_a.cpp", {1, 1, "location_filter(): delta 0x21 - previous, then filter_param_value"}},
    {"fetch_probe.cpp", {1, 1, "FETCH: delta 0x21, then filter_param_value"}},
    {"fetch_response.cpp", {1, 1, "FETCH: delta 0x21, then filter_param_value"}},
    {"fetch_first_object.cpp", {1, 1, "FETCH: delta 0x21, then filter_param_value"}},
    {"fetch_group_order.cpp", {1, 1, "FETCH: delta 0x21, then filter_param_value"}},
    {"immutable_repeat.cpp", {1, 1, "FETCH: delta 0x21, then filter_param_value"}},
    // draft21_close: the overflow probe's type byte before filter_param_value, and a reader of a
    // received PUBLISH (decode_publish_for_wire presents it in draft 21 form).
    {"draft21_close.cpp", {2, 1, "std::byte{0x21} + filter_param_value; parameter.type == 0x21 reader"}},
    {"draft21_response.cpp", {1, 0, "parameter.type == 0x21 reader of a decode_publish_for_wire PUBLISH"}},
};

// Files outside the guard: draft 18's SubscriptionFilter is a different parameter in a different draft.
bool excluded(const std::string& name) { return name.rfind("draft18", 0) == 0; }

// The source with comments blanked out (string and character literals kept), so `0x21` in prose is
// not counted.
std::string code_only(const std::string& source) {
    std::string out;
    out.reserve(source.size());
    for (std::size_t index = 0; index < source.size(); ++index) {
        const char c = source[index];
        const char next = index + 1 < source.size() ? source[index + 1] : '\0';
        if (c == '/' && next == '/') {
            while (index < source.size() && source[index] != '\n') ++index;
            out.push_back('\n');
        } else if (c == '/' && next == '*') {
            index += 2;
            while (index + 1 < source.size() && !(source[index] == '*' && source[index + 1] == '/'))
                out.push_back(source[index++] == '\n' ? '\n' : ' ');
            ++index;
        } else if (c == '"' || c == '\'') {
            out.push_back(c);
            for (++index; index < source.size() && source[index] != c; ++index) {
                out.push_back(source[index]);
                if (source[index] == '\\' && index + 1 < source.size()) out.push_back(source[++index]);
            }
            if (index < source.size()) out.push_back(c);
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string read(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::ostringstream text;
    text << input.rdbuf();
    return text.str();
}

// Line numbers of each `0x21` token (not 0x210, 0x21f, ...).
std::vector<int> filter_tokens(const std::string& code) {
    static const std::regex token(R"(\b0[xX]0*21\b)");
    std::vector<int> lines;
    for (auto it = std::sregex_iterator(code.begin(), code.end(), token); it != std::sregex_iterator(); ++it)
        lines.push_back(1 + static_cast<int>(std::count(code.begin(), code.begin() + it->position(), '\n')));
    return lines;
}

// Calls of filter_param_value (not nested_filter_param_value, which builds a FILL_PARAMETERS value).
int builder_calls(const std::string& code) {
    static const std::regex call(R"(\bfilter_param_value\s*\()");
    return static_cast<int>(std::distance(std::sregex_iterator(code.begin(), code.end(), call), std::sregex_iterator()));
}

TEST(FilterSourceGuard, CommentStrippingKeepsCodeAndDropsProse) {
    const auto code = code_only("a(0x21); // 0x21\n/* 0x21\n */ \"//\" b(0x21);\n");
    EXPECT_EQ(filter_tokens(code), (std::vector<int>{1, 3}));
    EXPECT_TRUE(filter_tokens("0x210 0x21f x0x21").empty());
    EXPECT_EQ(builder_calls(code_only("filter_param_value({}); nested_filter_param_value({}); "
                                      "// filter_param_value(\nfilter_param_value ({7, 9})")),
              2);
}

TEST(FilterSourceGuard, NoLocationFilterIsWrittenOutsideTheBuilder) {
    ASSERT_TRUE(std::filesystem::is_directory(kScenarios)) << kScenarios;
    std::map<std::string, int> seen;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(kScenarios)) {
        if (!entry.is_regular_file()) continue;
        const auto name = entry.path().lexically_relative(kScenarios).generic_string();
        const auto extension = entry.path().extension();
        if ((extension != ".cpp" && extension != ".h") || excluded(entry.path().filename().string())) continue;
        const auto code = code_only(read(entry.path()));
        // The draft 21 length-prefixed helper, never valid for a filter under wire draft 22.
        EXPECT_FALSE(std::regex_search(code, std::regex(R"(param_lp\s*\(\s*0[xX]0*21\b)")))
            << "src/scenarios/" << name << " writes LOCATION_FILTER with param_lp(0x21, ...), which keeps the "
               "draft 21 Length under wire draft 22; use filter_param(fields) or filter_param_value(fields)";
        const auto lines = filter_tokens(code);
        seen[name] = static_cast<int>(lines.size());
        const auto allowed = kAllowed.find(name);
        const int expected = allowed == kAllowed.end() ? 0 : allowed->second.count;
        EXPECT_EQ(static_cast<int>(lines.size()), expected)
            << "src/scenarios/" << name << " has " << lines.size() << " uses of 0x21 (lines "
            << ::testing::PrintToString(lines) << "), " << expected << " expected"
            << (allowed == kAllowed.end() ? "" : std::string(" (") + allowed->second.uses + ")")
            << ". A LOCATION_FILTER must be written through scenarios::filter_param_value (or filter_param), "
               "which emits draft 22's Type + fields under wire draft 22; raw 0x21 bytes keep the draft 21 "
               "Length. If this is a reader or a type delta followed by filter_param_value, update kAllowed in "
               "tests/unit/filter_source_guard_test.cpp.";
        // Each allowlisted write is a type (or delta) that a filter_param_value call must complete.
        if (allowed != kAllowed.end())
            EXPECT_GE(builder_calls(code), allowed->second.writes)
                << "src/scenarios/" << name << " has " << allowed->second.writes << " allowlisted 0x21 writes ("
                << allowed->second.uses << ") but only " << builder_calls(code)
                << " filter_param_value calls: a 0x21 write is not followed by the wire-aware builder";
    }
    // The allowlist must not outlive the code it describes.
    for (const auto& [name, allowed] : kAllowed)
        EXPECT_TRUE(seen.contains(name)) << "src/scenarios/" << name << " is allowlisted but does not exist";
}

}  // namespace
