#include "moq/interop/requirements/lineage.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace moq::interop::requirements {
namespace {

std::set<std::string> planned(const std::vector<std::string>& names) {
    std::set<std::string> result;
    for (const auto& name : names) {
        result.insert(name.starts_with("d21-") ? "d22-" + name.substr(4) : name);
    }
    return result;
}

bool same_obligation(const DeltaEntry& entry) {
    if (entry.d21.empty() || !entry.tags.empty()) return false;
    if (entry.change == "identical" || entry.change == "moved") return true;
    if (entry.change == "reworded") {
        return entry.note.find("editorial rewording") != std::string::npos ||
               entry.note.starts_with("re-paired by hand");
    }
    return false;
}

bool testable(const Requirement& row) {
    return row.applicability == Applicability::Applicable && row.testability == Testability::Testable;
}

std::string quoted(const std::string& text) { return "\"" + text + "\""; }

}  // namespace

std::vector<DeltaEntry> load_delta_entries(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot open " + path.string());
    const auto document = nlohmann::json::parse(input);
    std::vector<DeltaEntry> result;
    for (const auto& item : document.at("entries")) {
        result.push_back({item.at("draft22_id").get<std::string>(), item.at("draft21_id").get<std::string>(),
                          item.at("change").get<std::string>(), item.at("note").get<std::string>(),
                          item.at("tags").get<std::vector<std::string>>()});
    }
    return result;
}

Lineage build_lineage(const LineageInput& input) {
    std::map<std::string, const Requirement*> by_d21;
    for (const auto& row : input.draft21.requirements) by_d21[row.id] = &row;
    std::map<std::string, const DeltaEntry*> by_d22;
    for (const auto& entry : input.delta) {
        if (!entry.d22.empty()) by_d22[entry.d22] = &entry;
    }
    std::map<std::string, const Equivalence*> equivalent;
    for (const auto& item : input.equivalences) equivalent[item.d22] = &item;

    Lineage lineage;
    std::map<std::string, bool> row_shared;
    for (const auto& row : input.draft22.requirements) {
        std::vector<std::string> sources;
        bool shared = false;
        bool check_names = true;
        if (const auto it = equivalent.find(row.id); it != equivalent.end()) {
            sources = it->second->d21;
            shared = !sources.empty();
            check_names = false;  // one draft 22 row may merge several draft 21 rows
        } else if (const auto delta = by_d22.find(row.id); delta != by_d22.end() && same_obligation(*delta->second)) {
            sources = {delta->second->d21};
            shared = true;
        }
        if (shared && check_names && sources.size() == 1) {
            const auto counterpart = by_d21.find(sources.front());
            shared = counterpart != by_d21.end() &&
                     planned(counterpart->second->scenarios) == std::set<std::string>(row.scenarios.begin(), row.scenarios.end()) &&
                     planned(counterpart->second->evaluators) == std::set<std::string>(row.evaluators.begin(), row.evaluators.end());
        }
        for (const auto& source : sources) {
            if (shared && !by_d21.contains(source)) shared = false;
        }
        row_shared[row.id] = shared;
        if (shared) {
            for (const auto& source : sources) lineage.shared_rows.push_back({source, row.id});
        } else {
            lineage.own_rows.push_back(row.id);
        }
    }

    // Scenarios and evaluators: shared only if every testable row naming them is shared.
    std::map<std::string, bool> scenario_ok;
    std::map<std::string, bool> evaluator_ok;
    for (const auto& row : input.draft22.requirements) {
        if (!testable(row)) continue;
        for (const auto& name : row.scenarios) {
            auto [it, inserted] = scenario_ok.emplace(name, true);
            it->second = it->second && row_shared[row.id];
        }
        for (const auto& name : row.evaluators) {
            auto [it, inserted] = evaluator_ok.emplace(name, true);
            it->second = it->second && row_shared[row.id];
        }
    }
    const auto classify = [&](const std::map<std::string, bool>& names, std::vector<NamePair>& shared_out,
                              std::vector<std::string>& own_out, bool scenarios) {
        for (const auto& [name, ok] : names) {
            const auto d21 = name.starts_with("d22-") ? "d21-" + name.substr(4) : std::string{};
            const bool forced_own = scenarios && input.filter_building_scenarios.contains(d21);
            if (ok && !forced_own && !d21.empty()) {
                shared_out.push_back({name, d21});
            } else {
                own_out.push_back(name);
            }
        }
    };
    classify(scenario_ok, lineage.shared_scenarios, lineage.own_scenarios, true);
    classify(evaluator_ok, lineage.shared_evaluators, lineage.own_evaluators, false);

    std::sort(lineage.shared_rows.begin(), lineage.shared_rows.end(), [](const auto& a, const auto& b) {
        return std::tie(a.d21, a.d22) < std::tie(b.d21, b.d22);
    });
    std::sort(lineage.own_rows.begin(), lineage.own_rows.end());
    return lineage;  // the name vectors come out sorted because std::map iterates in order
}

std::string render_lineage_header(const Lineage& lineage) {
    std::ostringstream out;
    out << "// GENERATED by `moq-interop-catalog-carry lineage` from requirements/draft21-to-22-delta.json and the\n"
           "// draft 21 and draft 22 catalogs. Do not edit; a test regenerates this text and fails on drift.\n"
           "#pragma once\n\n#include <array>\n#include <string_view>\n\n"
           "namespace moq::interop::requirements::lineage_data {\n\n"
           "struct RowPair {\n    std::string_view d21;\n    std::string_view d22;\n};\n\n"
           "struct NamePair {\n    std::string_view d22;\n    std::string_view d21;\n};\n\n";
    const auto rows = [&](const char* name, const std::vector<RowPair>& values) {
        out << "inline constexpr std::array<RowPair, " << values.size() << "> " << name << "{{\n";
        for (const auto& value : values) out << "    {" << quoted(value.d21) << ", " << quoted(value.d22) << "},\n";
        out << "}};\n\n";
    };
    const auto pairs = [&](const char* name, const std::vector<NamePair>& values) {
        out << "inline constexpr std::array<NamePair, " << values.size() << "> " << name << "{{\n";
        for (const auto& value : values) out << "    {" << quoted(value.d22) << ", " << quoted(value.d21) << "},\n";
        out << "}};\n\n";
    };
    const auto names = [&](const char* name, const std::vector<std::string>& values) {
        out << "inline constexpr std::array<std::string_view, " << values.size() << "> " << name << "{{\n";
        for (const auto& value : values) out << "    " << quoted(value) << ",\n";
        out << "}};\n\n";
    };
    rows("kSharedRows", lineage.shared_rows);
    names("kOwnRows22", lineage.own_rows);
    pairs("kSharedScenarios", lineage.shared_scenarios);
    std::vector<std::string> scenario_ids;
    for (const auto& pair : lineage.shared_scenarios) scenario_ids.push_back(pair.d22);
    names("kSharedScenarioIds22", scenario_ids);
    names("kOwnScenarios22", lineage.own_scenarios);
    pairs("kSharedEvaluators", lineage.shared_evaluators);
    names("kOwnEvaluators22", lineage.own_evaluators);
    out << "}  // namespace moq::interop::requirements::lineage_data\n";
    return out.str();
}

}  // namespace moq::interop::requirements
