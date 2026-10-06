// docs/scenario-reference.md must describe the draft 22 scenarios the runner executes: its "Draft 22
// scenarios" section lists every executable draft 22 id exactly once (kind, draft 21 implementation), both
// unscored probes, no other draft 22 id, and states counts that match the registry and lineage tables.
#include "moq/interop/app/lineage.h"
#include "moq/interop/app/own_scenarios_22.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/draft22_lineage_data.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using moq::interop::app::executable_scenarios;
using moq::interop::app::implementation_scenario_id;
using moq::interop::app::kUnscoredProbeTraits22;
using moq::interop::app::shared_scenario_ids_22;
using moq::interop::requirements::lineage_data::kOwnScenarios22;

constexpr std::string_view kSectionHeading = "## Draft 22 scenarios";
constexpr std::string_view kExecutableHeading = "### Executable draft 22 scenarios";
constexpr std::string_view kUnscoredHeading = "### Unscored draft 22 probes";

std::vector<std::string> doc_lines() {
    const std::filesystem::path path =
        std::filesystem::path{MOQ_INTEROP_PROJECT_SOURCE_DIR} / "docs" / "scenario-reference.md";
    std::ifstream in{path};
    EXPECT_TRUE(in.good()) << path;
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) lines.push_back(line);
    return lines;
}

// The lines after `heading` up to the next heading of the same or a higher level.
std::vector<std::string> section(const std::vector<std::string>& lines, std::string_view heading) {
    const auto level = heading.find(' ');
    const std::string stop(level, '#');
    std::vector<std::string> out;
    bool inside = false;
    for (const auto& line : lines) {
        if (line == heading) {
            inside = true;
            continue;
        }
        if (inside && line.starts_with('#')) {
            const auto hashes = line.find_first_not_of('#');
            if (hashes != std::string::npos && hashes <= level && line[hashes] == ' ') break;
        }
        if (inside) out.push_back(line);
    }
    return out;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const auto& line : lines) text += line + "\n";
    return text;
}

std::string trim(std::string text) {
    const auto first = text.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    const auto last = text.find_last_not_of(' ');
    return text.substr(first, last - first + 1);
}

std::string unquote(std::string cell) {
    cell = trim(std::move(cell));
    if (cell.size() >= 2 && cell.front() == '`' && cell.back() == '`') return cell.substr(1, cell.size() - 2);
    return cell;
}

// Table rows whose first cell is a backquoted draft 22 id, split into cells.
std::vector<std::vector<std::string>> id_rows(const std::vector<std::string>& lines) {
    std::vector<std::vector<std::string>> rows;
    for (const auto& line : lines) {
        if (!line.starts_with("| `d22-")) continue;
        std::vector<std::string> cells;
        std::stringstream in{line.substr(1)};
        for (std::string cell; std::getline(in, cell, '|');) cells.push_back(trim(cell));
        if (!cells.empty() && cells.back().empty()) cells.pop_back();
        rows.push_back(std::move(cells));
    }
    return rows;
}

bool is_own(std::string_view id) {
    return std::find(kOwnScenarios22.begin(), kOwnScenarios22.end(), id) != kOwnScenarios22.end();
}

bool is_unscored(std::string_view id) {
    return std::any_of(kUnscoredProbeTraits22.begin(), kUnscoredProbeTraits22.end(),
                       [&](const auto& probe) { return probe.id == id; });
}

std::size_t executable_shared_count() {
    const auto ids = executable_scenarios(22);
    return static_cast<std::size_t>(std::count_if(ids.begin(), ids.end(), [](std::string_view id) {
        return implementation_scenario_id(id).has_value();
    }));
}

TEST(ScenarioReferenceDoc, HasDraft22Section) {
    const auto lines = doc_lines();
    EXPECT_FALSE(section(lines, kSectionHeading).empty()) << "missing heading: " << kSectionHeading;
    EXPECT_FALSE(section(lines, kExecutableHeading).empty()) << "missing heading: " << kExecutableHeading;
    EXPECT_FALSE(section(lines, kUnscoredHeading).empty()) << "missing heading: " << kUnscoredHeading;
}

TEST(ScenarioReferenceDoc, ListsEveryExecutableDraft22IdOnce) {
    const auto rows = id_rows(section(doc_lines(), kExecutableHeading));
    std::multiset<std::string> listed;
    for (const auto& row : rows) listed.insert(unquote(row.front()));
    std::set<std::string> expected;
    for (const auto id : executable_scenarios(22)) expected.emplace(id);
    for (const auto& id : expected) EXPECT_EQ(listed.count(id), 1U) << id;
    for (const auto& id : listed) EXPECT_TRUE(expected.contains(id)) << "not executable: " << id;
    EXPECT_EQ(listed.size(), expected.size());
}

TEST(ScenarioReferenceDoc, KindAndImplementationMatchLineage) {
    const auto rows = id_rows(section(doc_lines(), kExecutableHeading));
    ASSERT_FALSE(rows.empty());
    for (const auto& row : rows) {
        ASSERT_GE(row.size(), 3U) << row.front();
        const auto id = unquote(row[0]);
        const auto implementation = implementation_scenario_id(id);
        if (implementation) {
            EXPECT_EQ(row[1], "shared") << id;
            EXPECT_EQ(unquote(row[2]), std::string{*implementation}) << id;
        } else {
            EXPECT_TRUE(is_own(id)) << id;
            EXPECT_EQ(row[1], "own") << id;
            EXPECT_EQ(row[2], "none") << id;
        }
    }
}

TEST(ScenarioReferenceDoc, ListsBothUnscoredProbes) {
    const auto rows = id_rows(section(doc_lines(), kUnscoredHeading));
    std::set<std::string> listed;
    for (const auto& row : rows) listed.insert(unquote(row.front()));
    std::set<std::string> expected;
    for (const auto& probe : kUnscoredProbeTraits22) expected.emplace(probe.id);
    EXPECT_EQ(listed, expected);
    EXPECT_EQ(rows.size(), expected.size());
}

TEST(ScenarioReferenceDoc, NamesNoOtherDraft22Id) {
    const auto text = joined(section(doc_lines(), kSectionHeading));
    std::set<std::string> executable;
    for (const auto id : executable_scenarios(22)) executable.emplace(id);
    const std::regex token{"`(d22-[a-z0-9-]+)`"};
    std::size_t seen = 0;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), token); it != std::sregex_iterator(); ++it) {
        const auto id = (*it)[1].str();
        ++seen;
        EXPECT_TRUE(executable.contains(id) || is_unscored(id)) << "neither executable nor unscored: " << id;
    }
    EXPECT_GT(seen, 0U);
}

TEST(ScenarioReferenceDoc, StatedCountsMatchRegistry) {
    const auto lines = doc_lines();
    const auto text = joined(section(lines, kSectionHeading));
    const auto shared = executable_shared_count();
    const auto own = executable_scenarios(22).size() - shared;
    EXPECT_EQ(own, kOwnScenarios22.size());
    std::ostringstream counts;
    counts << "Counts: " << executable_scenarios(22).size() << " executable draft 22 scenarios (" << shared
           << " shared with draft 21, of the " << shared_scenario_ids_22().size()
           << " shared scenario IDs, and " << own << " of draft 22's own) and " << kUnscoredProbeTraits22.size()
           << " unscored probes.";
    EXPECT_NE(text.find(counts.str()), std::string::npos) << "expected the line: " << counts.str();

    // The count line near the top of the page names each draft's executable total.
    std::ostringstream totals;
    totals << executable_scenarios(18).size() << " for draft 18, " << executable_scenarios(21).size()
           << " for draft 21 and " << executable_scenarios(22).size() << " for draft 22";
    EXPECT_NE(joined(lines).find(totals.str()), std::string::npos) << "expected: " << totals.str();
}

}  // namespace
