#include "moq/interop/requirements/carry_forward.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <sstream>
#include <utility>

#include <nlohmann/json.hpp>

namespace moq::interop::requirements {
namespace {

struct Line {
    std::size_t number;
    std::string text;
};

struct Heading {
    std::size_t line;
    std::string number;
    std::string title;
};

// Drops the page footer and header lines and the blank runs around them, so a sentence that
// crosses a page break is contiguous. Original line numbers are preserved on kept lines.
std::vector<Line> clean_lines(const DraftSource& source) {
    static const std::regex footer(R"(\[Page [0-9]+\]\s*$)");
    static const std::regex header(R"(^\f?Internet-Draft\s+moq-transport)");
    std::vector<Line> raw;
    for (std::size_t n = 1; n <= source.line_offsets.size(); ++n) {
        std::string text(source.lines(n, n));
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) {
            text.pop_back();
        }
        raw.push_back({n, std::move(text)});
    }
    std::vector<Line> kept;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        const auto& text = raw[i].text;
        if (std::regex_search(text, footer) || std::regex_search(text, header)) {
            while (!kept.empty() && kept.back().text.empty()) {
                kept.pop_back();
            }
            while (i + 1 < raw.size() && raw[i + 1].text.empty()) {
                ++i;
            }
            continue;
        }
        if (text == "\f") {
            continue;
        }
        kept.push_back(raw[i]);
    }
    return kept;
}

std::vector<std::size_t> sentence_starts(const std::string& text) {
    static const std::vector<std::string> abbreviations = {"e.g.", "i.e.", "etc.", "vs.", "cf."};
    std::vector<std::size_t> starts{0};
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        if ((text[i] != '.' && text[i] != '?' && text[i] != '!') || text[i + 1] != ' ') {
            continue;
        }
        bool abbreviation = false;
        for (const auto& item : abbreviations) {
            if (i + 1 >= item.size() && text.compare(i + 1 - item.size(), item.size(), item) == 0) {
                abbreviation = true;
            }
        }
        std::size_t next = i + 2;
        while (next < text.size() && text[next] == ' ') {
            ++next;
        }
        if (!abbreviation && next < text.size() &&
            (std::isupper(static_cast<unsigned char>(text[next])) || text[next] == '*' ||
             text[next] == '(' || text[next] == '"')) {
            starts.push_back(next);
        }
    }
    return starts;
}

struct Paragraph {
    std::string text;
    std::vector<std::size_t> line_of_char;
    std::size_t first_line{0};
    std::size_t last_line{0};
};

double similarity(const std::string& left, const std::string& right) {
    const auto words = [](const std::string& text) {
        std::set<std::string> result;
        std::istringstream input(text);
        for (std::string word; input >> word;) {
            const auto first = word.find_first_not_of(".,;:()\"");
            const auto last = word.find_last_not_of(".,;:()\"");
            if (first != std::string::npos) {
                result.insert(word.substr(first, last - first + 1));
            }
        }
        return result;
    };
    const auto a = words(left);
    const auto b = words(right);
    if (a.empty() || b.empty()) {
        return 0.0;
    }
    std::size_t shared = 0;
    for (const auto& word : a) {
        shared += b.count(word);
    }
    return static_cast<double>(shared) / static_cast<double>(a.size() + b.size() - shared);
}

using Json = nlohmann::ordered_json;

const char* strength_token(Strength strength) {
    switch (strength) {
        case Strength::Must: return "MUST";
        case Strength::MustNot: return "MUST-NOT";
        case Strength::Should: return "SHOULD";
        case Strength::ShouldNot: return "SHOULD-NOT";
        case Strength::May: return "MAY";
    }
    return "MAY";
}

const char* strength_name(Strength strength) {
    switch (strength) {
        case Strength::Must: return "Must";
        case Strength::MustNot: return "MustNot";
        case Strength::Should: return "Should";
        case Strength::ShouldNot: return "ShouldNot";
        case Strength::May: return "May";
    }
    return "May";
}

const char* applicability_name(Applicability value) {
    switch (value) {
        case Applicability::Applicable: return "Applicable";
        case Applicability::NotApplicable: return "NotApplicable";
        case Applicability::Informative: return "Informative";
    }
    return "Applicable";
}

const char* testability_name(Testability value) {
    switch (value) {
        case Testability::Testable: return "Testable";
        case Testability::NotTestable: return "NotTestable";
        case Testability::NotApplicable: return "NotApplicable";
    }
    return "NotTestable";
}

std::string planned_id(const std::string& id) {
    constexpr std::string_view old_prefix = "d21-";
    return "d22-" + (id.starts_with(old_prefix) ? id.substr(old_prefix.size()) : id);
}

std::vector<std::string> planned_ids(const std::vector<std::string>& ids) {
    std::vector<std::string> result;
    for (const auto& id : ids) {
        result.push_back(planned_id(id));
    }
    return result;
}

bool mentions_location_filter(const OccurrenceContext& context) {
    std::string text = context.sentence + " " + context.section_title;
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text.find("location filter") != std::string::npos ||
           text.find("location_filter") != std::string::npos;
}

Json row_json(const std::string& id, Strength strength, const OccurrenceContext& target,
              unsigned clause, const std::string& actor, const std::string& summary,
              Applicability applicability, Testability testability,
              const std::vector<std::string>& scenarios, const std::vector<std::string>& evaluators,
              const std::string& rationale) {
    return Json{{"id", id},
                {"strength", strength_name(strength)},
                {"source", {{"section", target.section},
                            {"first_line", target.first_line},
                            {"last_line", std::max(target.last_line, target.sentence_last_line)},
                            {"occurrence", target.occurrence_on_line},
                            {"clause", clause}}},
                {"actor", actor},
                {"summary", summary},
                {"applicability", applicability_name(applicability)},
                {"testability", testability_name(testability)},
                {"scenarios", scenarios},
                {"evaluators", evaluators},
                {"rationale", rationale}};
}

Json catalog_json(const DraftSource& source, Json rows) {
    return Json{{"draft", source.number},
                {"source_sha256", source.sha256},
                {"complete", false},
                {"requirements", std::move(rows)}};
}

void write_json(const std::filesystem::path& path, const Json& value) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("Cannot write " + path.string());
    }
    output << value.dump(2) << "\n";
}

std::string partition_name(unsigned draft, std::size_t first, std::size_t last) {
    std::ostringstream name;
    name << "draft" << draft << "-lines-" << std::setfill('0') << std::setw(4) << first << '-'
         << std::setw(4) << last << ".json";
    return name.str();
}

}  // namespace

std::string normalize_text(std::string_view raw) {
    std::string result;
    bool pending_space = false;
    for (const char character : raw) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            pending_space = true;
        } else {
            if (pending_space && !result.empty()) {
                result.push_back(' ');
            }
            result.push_back(character);
            pending_space = false;
        }
    }
    return result;
}

std::vector<OccurrenceContext> extract_contexts(const DraftSource& source) {
    static const std::regex heading_re(R"(^((?:[0-9]+|[A-Z])(?:\.[0-9]+)*)\.\s{2,}(\S.*)$)");
    static const std::regex bullet_re(R"(^\s*\*\s)");
    static const std::regex keyword_re(
        R"(\b(MUST NOT|SHOULD NOT|SHALL NOT|NOT RECOMMENDED|MUST|SHOULD|SHALL|REQUIRED|RECOMMENDED|MAY|OPTIONAL)\b)");

    const auto lines = clean_lines(source);
    std::vector<Heading> headings;
    std::vector<Paragraph> paragraphs;
    Paragraph current;
    const auto flush = [&] {
        if (!current.text.empty()) {
            paragraphs.push_back(std::move(current));
        }
        current = Paragraph{};
    };
    for (const auto& line : lines) {
        std::smatch heading;
        if (std::regex_match(line.text, heading, heading_re)) {
            flush();
            headings.push_back({line.number, heading[1].str(), normalize_text(heading[2].str())});
            continue;
        }
        const auto trimmed = normalize_text(line.text);
        if (trimmed.empty()) {
            flush();
            continue;
        }
        if (std::regex_search(line.text, bullet_re)) {
            flush();
        }
        if (current.text.empty()) {
            current.first_line = line.number;
        } else {
            current.text.push_back(' ');
            current.line_of_char.push_back(line.number);
        }
        current.text += trimmed;
        current.line_of_char.insert(current.line_of_char.end(), trimmed.size(), line.number);
        current.last_line = line.number;
    }
    flush();

    using Anchor = std::pair<std::size_t, unsigned>;
    std::map<Anchor, OccurrenceContext> found;
    for (const auto& paragraph : paragraphs) {
        const auto starts = sentence_starts(paragraph.text);
        std::map<std::size_t, unsigned> per_line;
        std::map<std::size_t, unsigned> per_sentence;
        for (auto it = std::sregex_iterator(paragraph.text.begin(), paragraph.text.end(), keyword_re);
             it != std::sregex_iterator(); ++it) {
            const auto position = static_cast<std::size_t>(it->position());
            const auto line = paragraph.line_of_char[position];
            const auto sentence_index = static_cast<std::size_t>(
                std::upper_bound(starts.begin(), starts.end(), position) - starts.begin() - 1);
            const auto begin = starts[sentence_index];
            const auto end = sentence_index + 1 < starts.size() ? starts[sentence_index + 1]
                                                                : paragraph.text.size();
            OccurrenceContext context;
            context.first_line = line;
            context.occurrence_on_line = ++per_line[line];
            context.sentence = normalize_text(paragraph.text.substr(begin, end - begin));
            context.ordinal_in_sentence = ++per_sentence[sentence_index];
            auto last = end;
            while (last > begin + 1 && paragraph.text[last - 1] == ' ') {
                --last;
            }
            context.sentence_last_line = paragraph.line_of_char[last - 1];
            found.emplace(Anchor{line, context.occurrence_on_line}, std::move(context));
        }
    }

    std::vector<OccurrenceContext> result;
    for (const auto& occurrence : scan_normative_occurrences(source)) {
        OccurrenceContext context;
        const auto it = found.find({occurrence.first_line, occurrence.occurrence_on_line});
        if (it != found.end()) {
            context = it->second;
        }
        context.first_line = occurrence.first_line;
        context.last_line = occurrence.last_line;
        context.occurrence_on_line = occurrence.occurrence_on_line;
        context.strength = occurrence.normalized_strength;
        context.sentence_last_line = std::max(context.sentence_last_line, occurrence.last_line);
        const auto heading = std::upper_bound(
            headings.begin(), headings.end(), occurrence.first_line,
            [](std::size_t line, const Heading& item) { return line < item.line; });
        if (heading != headings.begin()) {
            context.section = std::prev(heading)->number;
            context.section_title = std::prev(heading)->title;
        }
        result.push_back(std::move(context));
    }
    return result;
}

const char* to_string(DeltaClass change) {
    switch (change) {
        case DeltaClass::Identical: return "identical";
        case DeltaClass::Moved: return "moved";
        case DeltaClass::Reworded: return "reworded";
        case DeltaClass::New: return "new";
        case DeltaClass::Removed: return "removed";
    }
    return "new";
}

CarryResult carry_forward(const DraftSource& old_source, const RequirementCatalog& old_catalog,
                          const DraftSource& new_source) {
    using Anchor = std::pair<std::size_t, unsigned>;
    const auto old_contexts = extract_contexts(old_source);
    const auto new_contexts = extract_contexts(new_source);

    std::map<Anchor, std::vector<const Requirement*>> rows_by_anchor;
    for (const auto& row : old_catalog.requirements) {
        rows_by_anchor[{row.source.first_line, row.source.occurrence}].push_back(&row);
    }
    for (auto& [anchor, rows] : rows_by_anchor) {
        std::sort(rows.begin(), rows.end(), [](const Requirement* a, const Requirement* b) {
            return a->source.clause < b->source.clause;
        });
    }
    const auto rows_for = [&](const OccurrenceContext& context) {
        const auto it = rows_by_anchor.find({context.first_line, context.occurrence_on_line});
        return it == rows_by_anchor.end() ? std::vector<const Requirement*>{} : it->second;
    };

    std::vector<bool> used(old_contexts.size(), false);
    std::map<std::pair<std::string, unsigned>, std::vector<std::size_t>> index;
    for (std::size_t i = 0; i < old_contexts.size(); ++i) {
        if (!old_contexts[i].sentence.empty()) {
            index[{old_contexts[i].sentence, old_contexts[i].ordinal_in_sentence}].push_back(i);
        }
    }

    CarryResult result;
    result.matches.resize(new_contexts.size());
    std::vector<bool> matched(new_contexts.size(), false);
    for (std::size_t j = 0; j < new_contexts.size(); ++j) {
        const auto& target = new_contexts[j];
        result.matches[j].target = target;
        if (target.sentence.empty()) {
            continue;
        }
        const auto found = index.find({target.sentence, target.ordinal_in_sentence});
        if (found == index.end()) {
            continue;
        }
        std::optional<std::size_t> chosen;
        for (const auto candidate : found->second) {
            if (used[candidate]) {
                continue;
            }
            if (old_contexts[candidate].section_title == target.section_title) {
                chosen = candidate;
                break;
            }
            if (!chosen) {
                chosen = candidate;
            }
        }
        if (!chosen) {
            continue;
        }
        used[*chosen] = true;
        matched[j] = true;
        result.matches[j].change = old_contexts[*chosen].section_title == target.section_title
                                       ? DeltaClass::Identical
                                       : DeltaClass::Moved;
        result.matches[j].sources = rows_for(old_contexts[*chosen]);
        result.matches[j].similarity = 1.0;
    }

    for (std::size_t j = 0; j < new_contexts.size(); ++j) {
        if (matched[j] || new_contexts[j].sentence.empty()) {
            continue;
        }
        double best = 0.0;
        std::optional<std::size_t> chosen;
        for (std::size_t i = 0; i < old_contexts.size(); ++i) {
            if (used[i] || old_contexts[i].strength != new_contexts[j].strength ||
                old_contexts[i].sentence.empty()) {
                continue;
            }
            const auto score = similarity(old_contexts[i].sentence, new_contexts[j].sentence);
            if (score > best) {
                best = score;
                chosen = i;
            }
        }
        if (chosen && best >= 0.6) {
            used[*chosen] = true;
            result.matches[j].change = DeltaClass::Reworded;
            result.matches[j].sources = rows_for(old_contexts[*chosen]);
            result.matches[j].similarity = best;
        }
    }

    for (std::size_t i = 0; i < old_contexts.size(); ++i) {
        if (!used[i]) {
            for (const auto* row : rows_for(old_contexts[i])) {
                result.removed.push_back(row);
            }
        }
    }
    return result;
}

std::map<std::string, std::string> extract_wire_blocks(const DraftSource& source) {
    static const std::regex open_re(R"(^\s{3}([A-Za-z0-9_][A-Za-z0-9_ ()/-]*?) \{\s*$)");
    static const std::regex close_re(R"(^\s{3}\}\s*$)");
    std::map<std::string, std::string> blocks;
    std::map<std::string, unsigned> seen;
    const auto lines = clean_lines(source);
    for (std::size_t i = 0; i < lines.size(); ++i) {
        std::smatch open;
        if (!std::regex_match(lines[i].text, open, open_re)) {
            continue;
        }
        std::string body;
        std::size_t j = i + 1;
        for (; j < lines.size() && !std::regex_match(lines[j].text, close_re); ++j) {
            const auto piece = normalize_text(lines[j].text);
            if (!piece.empty()) {
                body += (body.empty() ? "" : " ") + piece;
            }
        }
        if (j == lines.size()) {
            continue;
        }
        auto name = normalize_text(open[1].str());
        if (const auto count = ++seen[name]; count > 1) {
            name += "#" + std::to_string(count);
        }
        blocks.emplace(std::move(name), std::move(body));
        i = j;
    }
    return blocks;
}

WireDelta diff_wire_blocks(const DraftSource& old_source, const DraftSource& new_source) {
    const auto old_blocks = extract_wire_blocks(old_source);
    const auto new_blocks = extract_wire_blocks(new_source);
    WireDelta delta;
    for (const auto& [name, body] : new_blocks) {
        const auto it = old_blocks.find(name);
        if (it == old_blocks.end()) {
            delta.added.push_back(name);
        } else if (it->second != body) {
            delta.changed.push_back(name);
        }
    }
    for (const auto& [name, body] : old_blocks) {
        if (!new_blocks.contains(name)) {
            delta.removed.push_back(name);
        }
    }
    return delta;
}

void write_catalog_outputs(const CarryResult& result, const RequirementCatalog& old_catalog,
                           const DraftSource& new_source, const WireDelta& wire_delta,
                           const EmitOptions& options) {
    const auto merged_path =
        options.requirements_dir / ("draft" + std::to_string(new_source.number) + ".json");
    const auto delta_path = options.requirements_dir /
        ("draft" + std::to_string(old_catalog.draft) + "-to-" + std::to_string(new_source.number) +
         "-delta.json");
    if (!options.force) {
        const auto refuse = [](const std::filesystem::path& path) {
            throw std::runtime_error(path.string() +
                                     " exists; pass --force to overwrite reviewed output");
        };
        if (std::filesystem::exists(merged_path)) {
            refuse(merged_path);
        }
        if (std::filesystem::exists(delta_path)) {
            refuse(delta_path);
        }
        const auto parts_dir = options.requirements_dir / "parts";
        const auto part_prefix = "draft" + std::to_string(new_source.number) + "-lines-";
        if (std::filesystem::is_directory(parts_dir)) {
            for (const auto& item : std::filesystem::directory_iterator(parts_dir)) {
                const auto name = item.path().filename().string();
                if (name.starts_with(part_prefix) && name.ends_with(".json")) {
                    refuse(item.path());
                }
            }
        }
    }

    Json rows = Json::array();
    Json entries = Json::array();
    unsigned counter = 0;
    for (const auto& match : result.matches) {
        const auto& target = match.target;
        std::vector<std::string> tags;
        if (mentions_location_filter(target)) {
            tags.push_back("location_filter");
        }
        const bool carried = match.change != DeltaClass::New && !match.sources.empty();
        const auto make_id = [&](Strength strength) {
            std::string section = target.section;
            std::replace(section.begin(), section.end(), '.', '-');
            std::ostringstream id;
            id << "D" << new_source.number << '-' << section << '-' << strength_token(strength) << '-'
               << std::setfill('0') << std::setw(3) << ++counter;
            return id.str();
        };
        const bool reviewed = tags.empty() &&
            (match.change == DeltaClass::Identical || match.change == DeltaClass::Moved);
        std::string note;
        if (match.change == DeltaClass::Moved) {
            note = "section moved";
        } else if (match.change == DeltaClass::Reworded) {
            std::ostringstream text;
            text << "similarity=" << std::fixed << std::setprecision(2) << match.similarity;
            note = text.str();
        }
        if (carried) {
            for (const auto* source_row : match.sources) {
                const auto id = make_id(target.strength);
                rows.push_back(row_json(id, target.strength, target, source_row->source.clause,
                                        source_row->actor, source_row->summary,
                                        source_row->applicability, source_row->testability,
                                        planned_ids(source_row->scenarios),
                                        planned_ids(source_row->evaluators), source_row->rationale));
                entries.push_back(Json{{"draft22_id", id}, {"draft21_id", source_row->id},
                                       {"change", to_string(match.change)}, {"note", note},
                                       {"reviewed", reviewed}, {"tags", tags}});
            }
        } else {
            const auto id = make_id(target.strength);
            const auto summary = target.sentence.empty()
                ? "Unreviewed draft 22 requirement at line " + std::to_string(target.first_line)
                : target.sentence.substr(0, 160);
            rows.push_back(row_json(id, target.strength, target, 1, "endpoint", summary,
                                    Applicability::Applicable, Testability::NotTestable, {}, {},
                                    "Unreviewed new draft 22 obligation; classification pending review."));
            entries.push_back(Json{{"draft22_id", id}, {"draft21_id", ""}, {"change", "new"},
                                   {"note", note}, {"reviewed", false}, {"tags", tags}});
        }
    }
    for (const auto* removed : result.removed) {
        entries.push_back(Json{{"draft22_id", ""}, {"draft21_id", removed->id},
                               {"change", "removed"}, {"note", ""}, {"reviewed", false},
                               {"tags", Json::array()}});
    }

    std::filesystem::create_directories(options.requirements_dir / "parts");
    const auto prefix = "draft" + std::to_string(new_source.number) + "-lines-";
    for (const auto& item : std::filesystem::directory_iterator(options.requirements_dir / "parts")) {
        if (item.path().filename().string().starts_with(prefix)) {
            std::filesystem::remove(item.path());
        }
    }
    const std::size_t line_count = new_source.line_offsets.size();
    std::size_t start = 1;
    std::size_t row_index = 0;
    while (start <= line_count) {
        std::size_t cut = std::min(line_count, start + options.partition_lines - 1);
        while (cut < line_count) {
            bool crosses = false;
            for (const auto& row : rows) {
                const auto first = row.at("source").at("first_line").get<std::size_t>();
                const auto last = row.at("source").at("last_line").get<std::size_t>();
                if (first <= cut && last > cut) {
                    crosses = true;
                    break;
                }
            }
            if (!crosses) {
                break;
            }
            ++cut;
        }
        Json part_rows = Json::array();
        for (; row_index < rows.size() &&
               rows[row_index].at("source").at("first_line").get<std::size_t>() <= cut;
             ++row_index) {
            part_rows.push_back(rows[row_index]);
        }
        write_json(options.requirements_dir / "parts" / partition_name(new_source.number, start, cut),
                   catalog_json(new_source, std::move(part_rows)));
        start = cut + 1;
    }

    write_json(merged_path, catalog_json(new_source, rows));
    write_json(delta_path,
               Json{{"from_draft", old_catalog.draft},
                    {"to_draft", new_source.number},
                    {"from_sha256", old_catalog.source_sha256},
                    {"to_sha256", new_source.sha256},
                    {"wire_delta", {{"added", wire_delta.added},
                                    {"removed", wire_delta.removed},
                                    {"changed", wire_delta.changed},
                                    {"conclusion", ""}}},
                    {"entries", std::move(entries)}});
}

void merge_partitions(const DraftSource& new_source, const std::filesystem::path& requirements_dir) {
    const auto prefix = "draft" + std::to_string(new_source.number) + "-lines-";
    std::vector<std::filesystem::path> paths;
    for (const auto& item : std::filesystem::directory_iterator(requirements_dir / "parts")) {
        if (item.is_regular_file() && item.path().filename().string().starts_with(prefix)) {
            paths.push_back(item.path());
        }
    }
    std::sort(paths.begin(), paths.end());
    Json rows = Json::array();
    for (const auto& path : paths) {
        std::ifstream input(path);
        const auto part = Json::parse(input);
        for (const auto& row : part.at("requirements")) {
            rows.push_back(row);
        }
    }
    write_json(requirements_dir / ("draft" + std::to_string(new_source.number) + ".json"),
               catalog_json(new_source, std::move(rows)));
}

}  // namespace moq::interop::requirements
