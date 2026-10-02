#include "draft18_contribution_internal.h"

namespace moq::interop::scenarios::contribution {
namespace {

constexpr std::uint64_t kRequestId = 1;

const d18::GoawayMessage* control_goaway(const PeerControl& control) {
    for (const auto& message : control.messages)
        if (const auto* goaway = std::get_if<d18::GoawayMessage>(&message)) return goaway;
    return nullptr;
}

// The publisher's control GOAWAY visible in a gate's event prefix.
std::optional<std::uint64_t> control_goaway_cutoff(const RawProbeGateInput& input) {
    RawProbeTranscript view;
    view.events.assign(input.events.begin(), input.events.end());
    const auto control = peer_control(view);
    if (!control) return std::nullopt;
    const auto* goaway = control_goaway(*control);
    if (!goaway || !goaway->request_id) return std::nullopt;
    return goaway->request_id;
}

// Every GOAWAY the publisher sent: on its control stream, or on a request
// stream it opened (section 10.4).
std::vector<d18::GoawayMessage> publisher_goaways(const RawProbeTranscript& transcript) {
    std::vector<d18::GoawayMessage> result;
    if (const auto control = peer_control(transcript))
        for (const auto& message : control->messages)
            if (const auto* goaway = std::get_if<d18::GoawayMessage>(&message)) result.push_back(*goaway);
    std::set<transport::StreamId> streams;
    for (const auto& event : transcript.events) {
        if (terminal(event)) break;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event))
            if ((data->stream_id & 3u) == 0u) streams.insert(data->stream_id);
    }
    for (const auto stream : streams) {
        const auto reply = stream_reply(transcript, stream);
        for (const auto& message : reply.messages)
            if (const auto* goaway = std::get_if<d18::GoawayMessage>(&message)) result.push_back(*goaway);
    }
    return result;
}

// Section 10.4: a client sends a zero-length New Session URI in any GOAWAY.
std::optional<bool> client_goaway_has_empty_uri(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto goaways = publisher_goaways(transcript);
    if (goaways.empty()) return std::nullopt;
    return std::all_of(goaways.begin(), goaways.end(),
                       [](const auto& goaway) { return goaway.new_session_uri.empty(); });
}

// Section 10.4: a request arriving after the sender's control GOAWAY, or at or
// above its cutoff, is rejected with REQUEST_ERROR GOING_AWAY.
std::optional<bool> request_rejected_as_going_away(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto control = peer_control(transcript);
    if (!control || !control_goaway(*control)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    if (reply.messages.empty()) return std::nullopt;
    const auto* error = request_error(reply);
    return error && error->error_code == kErrorGoingAway;
}

}  // namespace

std::vector<Draft18ContributionProbe> goaway_probes(std::chrono::milliseconds deadline) {
    std::vector<Draft18ContributionProbe> result;
    result.push_back(make_probe("D18-10-4-MUST-001", "client-goaway-new-session-uri-empty",
        RawProbeDefinition{"observe-publisher-client-goaway", setup_message({}), {}, true, setup_ready, deadline,
            [](const RawProbeTranscript& transcript) { return !publisher_goaways(transcript).empty(); }, {}},
        client_goaway_has_empty_uri));

    const auto going_away = "request-error-going-away";
    const auto first_reply = [](const RawProbeTranscript& transcript) {
        return !write_reply(transcript, 0).messages.empty();
    };
    {
        RawProbeWrite request{RawProbeChannel::NewBidi, survival_request(kRequestId), false};
        request.evidence_ready = [](const RawProbeGateInput& input) {
            return control_goaway_cutoff(input).has_value();
        };
        result.push_back(make_probe("D18-10-4-MUST-009", going_away,
            RawProbeDefinition{"send-new-request-after-publisher-control-goaway", setup_message({}),
                {std::move(request)}, true, setup_ready, deadline, first_reply, {}},
            request_rejected_as_going_away));
    }
    {
        // The first Request ID the GOAWAY says might not have been processed.
        RawProbeWrite request{RawProbeChannel::NewBidi, {}, false};
        request.prepare_bytes = [](const RawProbeGateInput& input) -> std::optional<Bytes> {
            const auto cutoff = control_goaway_cutoff(input);
            // The runner is a server, so only an odd cutoff is a Request ID it can send.
            if (!cutoff || (*cutoff & 1u) == 0u) return std::nullopt;
            return survival_request(*cutoff);
        };
        result.push_back(make_probe("D18-10-4-MUST-008", going_away,
            RawProbeDefinition{"publisher-control-goaway-with-pending-request-at-cutoff", setup_message({}),
                {std::move(request)}, true, setup_ready, deadline, first_reply, {}},
            request_rejected_as_going_away));
    }
    return result;
}

}  // namespace moq::interop::scenarios::contribution
