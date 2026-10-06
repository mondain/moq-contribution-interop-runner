#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace moq::interop::app {

// The evidence event an unscored draft 22 probe (kUnscoredProbeTraits22) leaves in a run: its verdict is evidence
// only, never a requirement outcome. Events store a detail string and no structured payload, so the verdict and
// its reason travel in a fixed detail format that the run manager writes and the HTTP layer reads back as
// structured fields (`verdict`, `reason` in the events JSON; `unscored_probes` in the results JSON):
//
//   evaluator=<id> verdict=<pass|fail|not_run> scored=false (no catalog row names this probe) reason=<text>
//
// The run manager's context stamp then appends " ordinal=<n>". The prefix up to "scored=false" is the format
// earlier runs stored (without a reason), and it still parses (with an empty reason).
inline constexpr std::string_view kUnscoredProbeVerdictEvent = "unscored_probe_verdict";

struct UnscoredProbeFields22 {
    std::string evaluator;
    std::string verdict;  // pass, fail or not_run
    std::string reason;   // empty for an event stored before reasons were recorded
};

inline std::string unscored_probe_detail_22(std::string_view evaluator, std::string_view verdict,
                                            std::string_view reason) {
    return "evaluator=" + std::string(evaluator) + " verdict=" + std::string(verdict) +
           " scored=false (no catalog row names this probe) reason=" + std::string(reason);
}

// The fields of an unscored_probe_verdict detail; nothing when the detail is not in the format above.
inline std::optional<UnscoredProbeFields22> parse_unscored_probe_detail_22(std::string_view detail) {
    constexpr std::string_view evaluator_key = "evaluator=";
    constexpr std::string_view verdict_key = " verdict=";
    constexpr std::string_view scored_key = " scored=false";
    constexpr std::string_view reason_key = " reason=";
    constexpr std::string_view ordinal_key = " ordinal=";
    if (!detail.starts_with(evaluator_key)) return std::nullopt;
    const auto verdict_at = detail.find(verdict_key);
    if (verdict_at == std::string_view::npos) return std::nullopt;
    const auto verdict_begin = verdict_at + verdict_key.size();
    const auto verdict_end = detail.find(' ', verdict_begin);
    if (verdict_end == std::string_view::npos || detail.substr(verdict_end, scored_key.size()) != scored_key)
        return std::nullopt;
    UnscoredProbeFields22 fields;
    fields.evaluator = detail.substr(evaluator_key.size(), verdict_at - evaluator_key.size());
    fields.verdict = detail.substr(verdict_begin, verdict_end - verdict_begin);
    if (fields.evaluator.empty() ||
        (fields.verdict != "pass" && fields.verdict != "fail" && fields.verdict != "not_run"))
        return std::nullopt;
    if (const auto reason_at = detail.find(reason_key, verdict_end); reason_at != std::string_view::npos) {
        auto reason = detail.substr(reason_at + reason_key.size());
        if (const auto ordinal_at = reason.rfind(ordinal_key); ordinal_at != std::string_view::npos)
            reason = reason.substr(0, ordinal_at);
        fields.reason = reason;
    }
    return fields;
}

}  // namespace moq::interop::app
