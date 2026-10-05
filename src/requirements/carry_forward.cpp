#include "moq/interop/requirements/carry_forward.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <utility>

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

}  // namespace moq::interop::requirements
