#include "moq/interop/scenarios/lite06_datagram.h"

#include <stdexcept>
#include <string>
#include <utility>

#include "moq/interop/scenarios/lite06_announce.h"
#include "moq/interop/scenarios/lite06_subscribe.h"
#include "moq/interop/session/lite_session.h"

namespace moq::interop::scenarios {
namespace {

using lite06::allowance_step;
using lite06::proved_stimulus;

constexpr std::uint64_t kSubscribeId = 0;

bool fixture_present(const LiteTranscript& transcript) {
    return !l06_path_segments(transcript.broadcast_path).empty() && !l06_path_segments(transcript.track_name).empty();
}

}  // namespace

LiteProbeDefinition l06_datagram_size_probe(std::chrono::milliseconds deadline, std::string_view broadcast_path,
                                            std::string_view track_name, std::chrono::milliseconds window,
                                            std::chrono::milliseconds answer_allowance) {
    if (l06_path_segments(broadcast_path).empty())
        throw std::invalid_argument("a moq-lite-06 datagram probe needs the track fixture's broadcast path");
    if (l06_path_segments(track_name).empty())
        throw std::invalid_argument("a moq-lite-06 datagram probe needs the track fixture's track name");
    if (answer_allowance.count() <= 0)
        throw std::invalid_argument("a moq-lite-06 datagram probe needs a positive answer allowance");
    if (deadline <= answer_allowance + window)
        throw std::invalid_argument("a moq-lite-06 datagram probe needs a deadline beyond its windows");
    auto definition = lite06::allowance_probe(kL06DatagramSize, deadline, window);
    definition.requires_track = true;
    definition.broadcast_path = std::string(broadcast_path);
    definition.track_name = std::string(track_name);
    l06_add_announce_exchange(definition, answer_allowance);
    definition.steps.push_back(lite_open_bidi(
        l06_subscribe_bytes(l06_subscribe(kSubscribeId, broadcast_path, track_name)), false,
        std::string(kL06DatagramSubscribeLabel)));
    definition.steps.push_back(allowance_step(window));
    return definition;
}

namespace {

// The gate both functions share: the right scenario, a judgeable transcript, the fixture, and both stimuli delivered
// exactly as built.
bool datagram_stimuli_proved(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06DatagramSize || !judgeable_with_stimulus(transcript) ||
        !fixture_present(transcript))
        return false;
    if (!proved_stimulus(transcript, kL06SubAnnounceLabel, l06_announce_all_bytes())) return false;
    return proved_stimulus(transcript, kL06DatagramSubscribeLabel,
                           l06_subscribe_bytes(l06_subscribe(kSubscribeId, transcript.broadcast_path,
                                                             transcript.track_name))) != nullptr;
}

}  // namespace

bool l06_datagram_size_inapplicable(const LiteTranscript& transcript) {
    if (!datagram_stimuli_proved(transcript)) return false;
    return std::none_of(transcript.datagrams.begin(), transcript.datagrams.end(),
                        [](const session::LiteDatagram& d) { return d.body || !d.issue.empty(); });
}

std::optional<bool> evaluate_l06_datagram_size_limit(const LiteTranscript& transcript) {
    if (transcript.scenario_id != kL06DatagramSize || !judgeable_with_stimulus(transcript) ||
        !fixture_present(transcript))
        return std::nullopt;
    if (!proved_stimulus(transcript, kL06SubAnnounceLabel, l06_announce_all_bytes())) return std::nullopt;
    if (!proved_stimulus(transcript, kL06DatagramSubscribeLabel,
                         l06_subscribe_bytes(l06_subscribe(kSubscribeId, transcript.broadcast_path,
                                                           transcript.track_name))))
        return std::nullopt;
    bool within_limit = false;
    for (const auto& datagram : transcript.datagrams) {
        if (datagram.issue == session::kIssueDatagramOverLimit) return false;
        if (datagram.body) within_limit = true;
    }
    if (!within_limit) return std::nullopt;  // no datagram to judge (a permission the publisher did not use)
    return true;
}

}  // namespace moq::interop::scenarios
