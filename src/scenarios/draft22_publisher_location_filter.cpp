#include "moq/interop/scenarios/draft22_publisher_location_filter.h"

#include "draft22_probe_support.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/location_filter_param.h"
#include "moq/interop/scenarios/parameter_walk.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft22/location_filter.h"

#include <stdexcept>
#include <utility>
#include <variant>

namespace moq::interop::scenarios {
namespace {

using namespace d22support;
namespace d22 = wire::draft22;
using FilterType = d22::LocationFilterType;

// Section 9 message and parameter types.
constexpr std::uint64_t kRequestUpdate = 0x02;
constexpr std::uint64_t kSubscribe = 0x03;
constexpr std::uint64_t kSubscribeOk = 0x04;
constexpr std::uint64_t kPublish = 0x1d;
constexpr std::uint64_t kPublishStateNotify = 0x22;
constexpr std::uint64_t kForward = 0x10;
constexpr std::uint64_t kLocationFilter = 0x21;
// A count no 65535-byte message can reach with real parameters.
constexpr std::uint64_t kMaximumParameters = 64;

// The filter the probe's SUBSCRIBE requests: Absolute Start {7, 0} (Section 9.20.9 Type 0x02).
constexpr d22::LocationFilter kRequested{FilterType::Absolute, 7, 0, std::nullopt, std::nullopt};

// Section 9.6: SUBSCRIBE (Request ID 1) with FORWARD=1 and the requested filter, types ascending.
Bytes subscribe(const Fixture& fixture) {
    Bytes body;
    integer(body, request_id(0));
    track(body, fixture);
    integer(body, 2);
    integer(body, kForward);
    integer(body, 1);
    integer(body, kLocationFilter - kForward);
    const auto value = filter_param_value({kRequested.start_group, kRequested.start_object});
    body.insert(body.end(), value.begin(), value.end());
    return frame(kSubscribe, body);
}

// ------------------------------------------------------------------ observation

enum class State { Pending, Inconclusive, Pass, Fail };

struct Seen {
    std::size_t judged{0};      // LOCATION_FILTERs that decoded
    bool undecodable{false};    // a LOCATION_FILTER that did not decode
    bool changed{false};        // the probe's subscription reported another absolute filter
    bool unreadable{false};     // a judged message whose parameters could not all be read
    bool incomparable{false};   // a relative filter reported on the probe's subscription
};

bool same_filter(const d22::LocationFilter& left, const d22::LocationFilter& right) {
    return left.type == right.type && left.start_group == right.start_group &&
           left.start_object == right.start_object && left.end_group_delta == right.end_group_delta &&
           left.end_object == right.end_object;
}

// Reads a parameter block (count, then parameters) from `body` and judges its LOCATION_FILTER. With
// `requested`, the block is a PUBLISH_STATE_NOTIFY on the probe's subscription and must report that filter.
void judge_parameters(wire::Cursor& body, Seen& seen, const d22::LocationFilter* requested) {
    const auto count = number(body);
    if (!count || *count > kMaximumParameters) { seen.unreadable = true; return; }
    std::vector<d22::LocationFilter> filters;
    const auto walk = walk_message_parameters(body, *count, [&](const WalkedParameter& parameter) {
        if (parameter.type != kLocationFilter) return;
        wire::Cursor value(parameter.value);
        const auto decoded = d22::decode_location_filter(value);
        if (const auto* filter = std::get_if<d22::LocationFilter>(&decoded)) filters.push_back(*filter);
    });
    if (walk.status == ParameterWalkStatus::Malformed && walk.failed_type == kLocationFilter)
        seen.undecodable = true;
    else if (walk.status != ParameterWalkStatus::Complete)
        seen.unreadable = true;
    for (const auto& filter : filters) {
        ++seen.judged;
        if (!requested) continue;
        if (filter.type == FilterType::RelativeGroup || filter.type == FilterType::NextObject)
            seen.incomparable = true;
        else if (!same_filter(filter, *requested))
            seen.changed = true;
    }
}

// Section 9.8: PUBLISH body up to its parameters: Request ID, Track Namespace, Track Name, Track Alias.
bool skip_publish_header(wire::Cursor& body) {
    if (!number(body) || !read_namespace(body)) return false;
    if (!std::holds_alternative<std::span<const std::byte>>(wire::read_length_prefixed_bytes(body, 4096)))
        return false;
    return number(body).has_value();
}

// PUBLISH_STATE_NOTIFY on the probe's subscription, once established by SUBSCRIBE_OK.
void judge_subscription(const RawProbeTranscript& t, const Collected& collected, Seen& seen) {
    const auto response = response_of(t, collected, 0);
    if (!response.stream) return;
    if (response.messages.malformed) { seen.unreadable = true; return; }
    if (response.messages.complete.empty() || response.messages.complete.front().type != kSubscribeOk) return;
    for (std::size_t index = 1; index < response.messages.complete.size(); ++index) {
        const auto& message = response.messages.complete[index];
        if (message.type != kPublishStateNotify) continue;
        wire::Cursor body(message.body);
        judge_parameters(body, seen, &kRequested);
    }
}

// Request streams the publisher opened with PUBLISH: the PUBLISH, then its REQUEST_UPDATEs and
// PUBLISH_STATE_NOTIFYs.
void judge_publications(const Collected& collected, Seen& seen) {
    for (const auto& [id, stream] : collected.streams) {
        if ((id & 3u) != 0u) continue;
        const auto messages = parse_messages(stream.bytes);
        if (messages.complete.empty() || messages.complete.front().type != kPublish) continue;
        if (messages.malformed) seen.unreadable = true;
        for (std::size_t index = 0; index < messages.complete.size(); ++index) {
            const auto& message = messages.complete[index];
            wire::Cursor body(message.body);
            if (index == 0) {
                if (!skip_publish_header(body)) { seen.unreadable = true; continue; }
            } else if (message.type == kRequestUpdate) {
                if (!number(body)) { seen.unreadable = true; continue; }
            } else if (message.type != kPublishStateNotify) {
                continue;
            }
            judge_parameters(body, seen, nullptr);
        }
    }
}

State observe(const RawProbeTranscript& t, bool window_ended) {
    if (t.writes.empty() || !t.stimulus_delivered) return State::Pending;
    // Request streams only: the subscription's media must not exhaust the evidence bound.
    const auto collected = collect(t.events, {kMaximumBytes, true});
    if (!collected.bounded) return State::Inconclusive;
    Seen seen;
    judge_subscription(t, collected, seen);
    judge_publications(collected, seen);
    if (seen.undecodable || seen.changed) return State::Fail;
    if (!window_ended) return State::Pending;
    if (seen.unreadable || seen.incomparable || seen.judged == 0) return State::Inconclusive;
    return State::Pass;
}

RawProbeDefinition build(std::chrono::milliseconds deadline, const Fixture& fixture) {
    require_draft22_wire("draft 22 publisher location filter");
    if (deadline.count() <= 0 || !fetch_first_object_fixture_valid(fixture.ns, fixture.name))
        throw std::invalid_argument("invalid draft 22 publisher location filter fixture or deadline");
    RawProbeDefinition definition;
    definition.id = std::string(kDraft22PublisherLocationFilter);
    definition.setup_bytes = setup_message();
    definition.deadline = deadline;
    definition.peer_setup_ready = setup_ready;
    definition.writes.push_back({RawProbeChannel::NewBidi, subscribe(fixture), false});
    definition.courtesy.publish = RawProbePublishResponse::Accept;
    definition.courtesy.update = RawProbeUpdateResponse::Accept;
    // The whole window is needed unless a failure is already shown.
    definition.response_ready = [](const RawProbeTranscript& t) { return observe(t, false) == State::Fail; };
    return definition;
}

}  // namespace

RawProbeDefinition draft22_publisher_location_filter_probe(std::chrono::milliseconds deadline,
                                                           std::vector<std::vector<std::byte>> track_namespace,
                                                           std::vector<std::byte> track_name) {
    return build(deadline, {std::move(track_namespace), std::move(track_name)});
}

std::optional<bool> evaluate_draft22_publisher_location_filter(const RawProbeTranscript& t) {
    if (t.scenario_id != kDraft22PublisherLocationFilter) return std::nullopt;
    // The stimulus is rebuilt on the draft 22 wire; on any other wire nothing is judged.
    if (current_wire_draft() != 22 || t.writes.empty() || t.harness_failed) return std::nullopt;
    const auto fixture = recover_fixture(t.writes.front().write.bytes, kSubscribe, request_id(0));
    if (!fixture) return std::nullopt;
    const auto proven = prove(t, build(kRebuildDeadline, *fixture), true);
    if (!proven) return std::nullopt;
    const auto state = observe(proven->prefix, proven->ended);
    if (state == State::Pass) return true;
    if (state == State::Fail) return false;
    return std::nullopt;
}

}  // namespace moq::interop::scenarios
