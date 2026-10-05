#include "moq/interop/requirements/carry_forward.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <regex>
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

}  // namespace moq::interop::requirements
