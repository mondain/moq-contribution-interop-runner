#pragma once

// Settling helpers shared by the draft-18 raw probes. They read RawProbeClock,
// so a window is a period of time rather than a count of controller polls.

#include "moq/interop/scenarios/raw_probe.h"

#include <algorithm>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>

namespace moq::interop::scenarios::probe_timing {

// Becomes true once `seen` has held for `window`. The first sighting is
// forgotten whenever `seen` is false, and also when the event log is shorter
// than at the previous call: that means the definition serves a new session,
// which may not have evaluated `seen` before it first holds.
inline std::function<bool(const RawProbeTranscript&)> settled_after(
    std::function<bool(const RawProbeTranscript&)> seen, std::chrono::milliseconds window) {
    struct State {
        std::optional<RawProbeClock::time_point> first;
        std::size_t events{0};
    };
    auto state = std::make_shared<State>();
    return [seen = std::move(seen), window, state](const RawProbeTranscript& transcript) {
        if (transcript.events.size() < state->events) state->first.reset();
        state->events = transcript.events.size();
        if (!seen(transcript)) {
            state->first.reset();
            return false;
        }
        const auto now = RawProbeClock::now();
        if (!state->first) state->first = now;
        return now - *state->first >= window;
    };
}

inline std::chrono::milliseconds quiet_window(std::chrono::milliseconds deadline) {
    return std::clamp(deadline / 4, std::chrono::milliseconds{1}, std::chrono::milliseconds{50});
}

}  // namespace moq::interop::scenarios::probe_timing
