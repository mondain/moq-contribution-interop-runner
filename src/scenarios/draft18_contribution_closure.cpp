#include "draft18_contribution_internal.h"

namespace moq::interop::scenarios::contribution {
namespace {

constexpr std::uint64_t kSubscribeId = 1;
constexpr std::uint64_t kUpdateId = 3;
constexpr std::uint64_t kCancelled = 0x1;  // section 3.3.3 CANCELLED

bool open_and_incomplete(const SubgroupStream& stream) {
    return !stream.malformed && !stream.fin && !stream.reset && !stream.objects.empty() &&
           !delivered_final_object(stream);
}

// The subscription's first subgroup stream that had delivered Objects, was still
// open, and had not delivered its final Object within the events of `view`.
std::optional<SubgroupStream> open_incomplete_subgroup(const RawProbeTranscript& view) {
    const auto alias = subscription_alias(view);
    if (!alias) return std::nullopt;
    for (auto& stream : subscription_streams(view, *alias))
        if (open_and_incomplete(stream)) return std::move(stream);
    return std::nullopt;
}

bool open_subgroup_ready(const RawProbeGateInput& input) {
    return open_incomplete_subgroup(view_of(input)).has_value();
}

// Section 11.4.3: closing a subgroup before all its Objects were delivered
// requires a reset. After a runner-made trigger on a subgroup that was open and
// incomplete, a reset shows the rule is followed; a FIN does not say whether
// every Object had been delivered, so it proves nothing either way.
std::optional<bool> open_subgroup_reset_after(const RawProbeTranscript& transcript, std::size_t trigger) {
    if (!bounded(transcript) || trigger >= transcript.writes.size() ||
        !transcript.writes[trigger].delivery_event_count) return std::nullopt;
    const auto marker = std::min(*transcript.writes[trigger].delivery_event_count, transcript.events.size());
    RawProbeTranscript before;
    before.events.assign(transcript.events.begin(), transcript.events.begin() + static_cast<std::ptrdiff_t>(marker));
    before.writes = transcript.writes;
    const auto target = open_incomplete_subgroup(before);
    if (!target) return std::nullopt;
    for (std::size_t i = marker; i < transcript.events.size(); ++i) {
        if (terminal(transcript.events[i])) break;
        if (const auto* reset = std::get_if<transport::PeerResetEvent>(&transcript.events[i]);
            reset && reset->stream_id == target->id) return true;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&transcript.events[i]);
            data && data->stream_id == target->id && data->fin) return std::nullopt;
    }
    return std::nullopt;
}
bool target_closed_after(const RawProbeTranscript& transcript, std::size_t trigger) {
    if (trigger >= transcript.writes.size() || !transcript.writes[trigger].delivery_event_count) return false;
    const auto marker = std::min(*transcript.writes[trigger].delivery_event_count, transcript.events.size());
    RawProbeTranscript before;
    before.events.assign(transcript.events.begin(), transcript.events.begin() + static_cast<std::ptrdiff_t>(marker));
    before.writes = transcript.writes;
    const auto target = open_incomplete_subgroup(before);
    if (!target) return false;
    for (std::size_t i = marker; i < transcript.events.size(); ++i) {
        if (const auto* reset = std::get_if<transport::PeerResetEvent>(&transcript.events[i]);
            reset && reset->stream_id == target->id) return true;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&transcript.events[i]);
            data && data->stream_id == target->id && data->fin) return true;
    }
    return false;
}

RawProbeDefinition triggered_definition(const char* id, std::vector<RawProbeWrite> writes,
                                        std::size_t trigger, std::chrono::milliseconds deadline) {
    return {id, setup_message({}), std::move(writes), true, setup_ready, deadline,
            [trigger](const RawProbeTranscript& transcript) { return target_closed_after(transcript, trigger); },
            {}};
}

// Section 11.4.3 (continued): a stream closed with FIN whose subgroup carries
// on afterwards on another stream was closed before delivering all its Objects.
std::optional<bool> early_termination_resets(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto alias = subscription_alias(transcript);
    if (!alias) return std::nullopt;
    const auto streams = subscription_streams(transcript, *alias);
    bool reset_continued = false;
    for (const auto& first : streams) {
        if (first.malformed || first.objects.empty() || !first.closed_event) continue;
        for (const auto& later : streams) {
            if (&first == &later || later.malformed || later.objects.empty()) continue;
            if (later.first_event < *first.closed_event) continue;
            if (later.header.group_id != first.header.group_id) continue;
            if (later.objects.front().subgroup_id != first.objects.front().subgroup_id ||
                !first.objects.front().subgroup_id) continue;
            if (later.objects.front().object_id <= first.objects.back().object_id) continue;
            if (first.fin && !first.reset) return false;
            if (first.reset) reset_continued = true;
        }
    }
    return reset_continued ? std::optional<bool>{true} : std::nullopt;
}
bool early_termination_seen(const RawProbeTranscript& transcript) {
    const auto alias = subscription_alias(transcript);
    if (!alias) return false;
    const auto streams = subscription_streams(transcript, *alias);
    for (const auto& first : streams) {
        if (first.malformed || first.objects.empty() || !first.closed_event) continue;
        for (const auto& later : streams)
            if (&first != &later && !later.malformed && !later.objects.empty() &&
                later.first_event >= *first.closed_event && later.header.group_id == first.header.group_id &&
                first.objects.front().subgroup_id && later.objects.front().subgroup_id == first.objects.front().subgroup_id &&
                later.objects.front().object_id > first.objects.back().object_id)
                return true;
    }
    return false;
}

}  // namespace

std::vector<Draft18ContributionProbe> closure_probes(std::chrono::milliseconds deadline, const Fixture& fixture) {
    std::vector<Draft18ContributionProbe> result;
    const char* evaluator = "incomplete-subgroup-closure-uses-reset";
    const d18::Parameters forwarding{forward_parameter(1)};
    const auto subscribe = [&] {
        return RawProbeWrite{RawProbeChannel::NewBidi, subscribe_request(fixture, kSubscribeId, forwarding), false};
    };
    const auto observe_reset = [](std::size_t trigger) -> Observe {
        return [trigger](const RawProbeTranscript& transcript, bool) {
            return open_subgroup_reset_after(transcript, trigger);
        };
    };
    {
        // Cancel the SUBSCRIBE: FIN our side of the request stream, then STOP_SENDING the response.
        RawProbeWrite finish{RawProbeChannel::NewBidi, {}, true, 0};
        finish.evidence_ready = open_subgroup_ready;
        RawProbeWrite stop{RawProbeChannel::NewBidi, {}, false, 1};
        stop.operation = RawProbeOperation::StopSending;
        stop.application_error = kCancelled;
        result.push_back(make_probe("D18-11-4-3-MUST-002", evaluator,
            triggered_definition("cancel-subscription-before-next-subgroup-object-is-produced",
                                 {subscribe(), std::move(finish), std::move(stop)}, 1, deadline),
            observe_reset(1), true));
    }
    {
        // Move the Start Location past the open subgroup's Group.
        RawProbeWrite advance{RawProbeChannel::NewBidi, {}, false, 0};
        advance.prepare_bytes = [](const RawProbeGateInput& input) -> std::optional<Bytes> {
            const auto target = open_incomplete_subgroup(view_of(input));
            if (!target) return std::nullopt;
            return request_update(kUpdateId, {{0x21, d18::SubscriptionFilter{
                d18::SubscriptionFilterType::AbsoluteStart, d18::Location{target->header.group_id + 1, 0},
                std::nullopt}}});
        };
        result.push_back(make_probe("D18-11-4-3-MUST-002", evaluator,
            triggered_definition("advance-start-location-while-subgroup-remains-incomplete",
                                 {subscribe(), std::move(advance)}, 1, deadline),
            observe_reset(1), true));
    }
    {
        RawProbeWrite pause{RawProbeChannel::NewBidi, request_update(kUpdateId, {forward_parameter(0)}), false, 0};
        pause.evidence_ready = open_subgroup_ready;
        result.push_back(make_probe("D18-11-4-3-MUST-002", evaluator,
            triggered_definition("pause-forwarding-with-an-unsent-subgroup-object",
                                 {subscribe(), std::move(pause)}, 1, deadline),
            observe_reset(1), true));
    }
    result.push_back(make_probe("D18-11-4-3-MUST-005", evaluator,
        RawProbeDefinition{"publisher-terminates-subgroup-before-final-object-production", setup_message({}),
            {subscribe()}, true, setup_ready, deadline, early_termination_seen, {}},
        early_termination_resets, true));
    return result;
}

}  // namespace moq::interop::scenarios::contribution
