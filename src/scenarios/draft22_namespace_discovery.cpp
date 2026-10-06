#include "moq/interop/scenarios/draft22_namespace_discovery.h"

#include "draft22_probe_support.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {

using namespace d22support;

// Section 9 message types and Section 12.5 stream error codes.
constexpr std::uint64_t kSubscribe = 0x03;
constexpr std::uint64_t kSubscribeOk = 0x04;
constexpr std::uint64_t kRequestError = 0x05;
constexpr std::uint64_t kRequestOk = 0x07;
constexpr std::uint64_t kNamespace = 0x08;
constexpr std::uint64_t kSubscribeNamespace = 0x50;
constexpr std::uint64_t kForward = 0x10;
constexpr std::uint64_t kCancelled = 0x1;
// The other prefixes go out this long after the empty one was cancelled, so the publisher has processed
// the cancellation before it sees a prefix that overlaps it.
constexpr std::chrono::milliseconds kAfterCancel{250};

// Writes of the probe, by index.
constexpr std::size_t kSubscribeWrite = 0;
constexpr std::size_t kEmptyWrite = 1;
constexpr std::size_t kCancelWrite = 2;
constexpr std::size_t kMatchingWrite = 3;
constexpr std::size_t kNonmatchingWrite = 4;
constexpr std::size_t kWriteCount = 5;

// Section 9.6: SUBSCRIBE (Request ID 1) with FORWARD=0, so the publisher sends no Objects.
Bytes subscribe(const Fixture& fixture) {
    Bytes body;
    integer(body, request_id(0));
    track(body, fixture);
    integer(body, 1);
    integer(body, kForward);
    integer(body, 0);
    return frame(kSubscribe, body);
}

// Section 9.15: SUBSCRIBE_NAMESPACE with `prefix` and no parameters.
Bytes subscribe_namespace(std::uint64_t id, const Namespace& prefix) {
    Bytes body;
    integer(body, id);
    track_namespace(body, prefix);
    integer(body, 0);
    return frame(kSubscribeNamespace, body);
}

Namespace matching_prefix(const Fixture& fixture) { return {fixture.ns.front()}; }

Namespace nonmatching_prefix(const Fixture& fixture) {
    return {draft22_nonmatching_prefix_field(fixture.ns.front())};
}

// ------------------------------------------------------------------ observation

enum class State { Pending, Inconclusive, Pass, Fail };

// What one SUBSCRIBE_NAMESPACE response says.
struct Discovery {
    enum class Answer { Unanswered, Accepted, Refused } answer{Answer::Unanswered};
    bool finished{false};  // FIN: nothing more follows (Section 6.4.2.2)
    bool reset{false};
    bool unreadable{false};
    bool exact{false};      // a NAMESPACE for exactly the track namespace
    bool covering{false};   // a NAMESPACE for a shorter namespace the track namespace lies in
    bool ignored_prefix{false};  // a suffix showing the prefix was ignored or matched by bytes
    // A NAMESPACE whose suffix could not be read (it breaks the Section 8.7 limits or has bytes after the
    // suffix). It may name the track's namespace, so no failure can rest on its absence.
    bool unparsed_namespace{false};
};

bool is_prefix(const Namespace& prefix, const Namespace& of) {
    return prefix.size() <= of.size() && std::equal(prefix.begin(), prefix.end(), of.begin());
}

Discovery discovery_of(const RawProbeTranscript& t, const Collected& collected, std::size_t index,
                       const Namespace& prefix, const Namespace& ns) {
    Discovery result;
    const auto response = response_of(t, collected, index);
    if (!response.stream) return result;
    result.finished = response.stream->fin;
    result.reset = response.stream->reset;
    if (response.messages.malformed) { result.unreadable = true; return result; }
    if (response.messages.complete.empty()) return result;
    const auto first = response.messages.complete.front().type;
    if (first != kRequestOk) {
        // REQUEST_ERROR, or a first message another row judges.
        result.answer = Discovery::Answer::Refused;
        result.unreadable = first != kRequestError;
        return result;
    }
    result.answer = Discovery::Answer::Accepted;
    const Namespace without_first(ns.begin() + 1, ns.end());
    for (const auto& message : response.messages.complete) {
        if (message.type != kNamespace) continue;
        // Section 9.16: the body is exactly a Track Namespace Suffix. One that breaks the Track Namespace
        // limits is malformed (another row's concern); it cannot be read, so it is noted, not judged.
        wire::Cursor body(message.body);
        auto suffix = read_namespace(body);
        if (!suffix || body.remaining() != 0) {
            result.unparsed_namespace = true;
            continue;
        }
        if (*suffix == ns || *suffix == without_first) result.ignored_prefix = true;
        Namespace full = prefix;
        full.insert(full.end(), suffix->begin(), suffix->end());
        if (full == ns) result.exact = true;
        else if (is_prefix(full, ns)) result.covering = true;
    }
    return result;
}

// Section 4.2 for a prefix the track namespace matches. A FIN is the end of the response (Section
// 6.4.2.2), so once it arrives without the NAMESPACE the duty was missed, provided the publisher serves
// the track (`serves`: its SUBSCRIBE was answered with SUBSCRIBE_OK, so it originally publishes a track
// in the namespace). A covering NAMESPACE (a shorter known namespace that contains the track's) is not
// proven sufficient or insufficient, so it never fails and never passes.
State matched(const Discovery& discovery, std::optional<bool> serves) {
    if (discovery.unreadable) return State::Inconclusive;
    switch (discovery.answer) {
    case Discovery::Answer::Unanswered:
        return discovery.finished || discovery.reset ? State::Inconclusive : State::Pending;
    case Discovery::Answer::Refused:
        return State::Inconclusive;
    case Discovery::Answer::Accepted:
        break;
    }
    if (discovery.exact) return State::Pass;
    if (discovery.reset) return State::Inconclusive;
    if (!discovery.finished) return State::Pending;
    // A NAMESPACE the evaluator could not read may have been the one owed: never fail on its absence.
    if (discovery.unparsed_namespace) return State::Inconclusive;
    if (discovery.covering) return State::Inconclusive;
    if (!serves) return State::Pending;
    return *serves ? State::Fail : State::Inconclusive;
}

// The nonmatching prefix: any answer settles it, a suffix that shows the prefix was not compared field by
// field blocks a pass (sending a NAMESPACE is not what this row forbids, so it is not a failure).
State unmatched(const Discovery& discovery) {
    if (discovery.unreadable || discovery.ignored_prefix || discovery.unparsed_namespace)
        return State::Inconclusive;
    if (discovery.answer != Discovery::Answer::Unanswered) return State::Pass;
    return discovery.finished || discovery.reset ? State::Pass : State::Pending;
}

// Whether the track's SUBSCRIBE was answered with SUBSCRIBE_OK; no value while unanswered.
std::optional<bool> serves_track(const RawProbeTranscript& t, const Collected& collected) {
    const auto response = response_of(t, collected, kSubscribeWrite);
    if (!response.stream || response.messages.malformed) return std::nullopt;
    if (response.messages.complete.empty()) {
        if (response.stream->fin || response.stream->reset) return false;
        return std::nullopt;
    }
    return response.messages.complete.front().type == kSubscribeOk;
}

struct Observation {
    State matching{State::Pending};
    State nonmatching{State::Pending};
    State empty{State::Pending};
};

Observation observe_streams(const RawProbeTranscript& t, const Collected& collected, const Fixture& fixture) {
    Observation result;
    const auto serves = serves_track(t, collected);
    result.empty = matched(discovery_of(t, collected, kEmptyWrite, {}, fixture.ns), serves);
    if (t.writes.size() > kNonmatchingWrite) {
        result.matching =
            matched(discovery_of(t, collected, kMatchingWrite, matching_prefix(fixture), fixture.ns), serves);
        result.nonmatching =
            unmatched(discovery_of(t, collected, kNonmatchingWrite, nonmatching_prefix(fixture), fixture.ns));
    }
    return result;
}

State observe(const RawProbeTranscript& t, const Fixture& fixture) {
    if (t.writes.size() < kWriteCount || !t.stimulus_delivered) return State::Pending;
    const auto collected = collect(t.events);
    if (!collected.bounded) return State::Inconclusive;
    const auto observed = observe_streams(t, collected, fixture);
    const State states[]{observed.matching, observed.empty, observed.nonmatching};
    const auto any = [&](State state) { return std::find(std::begin(states), std::end(states), state) != std::end(states); };
    if (observed.matching == State::Fail || observed.empty == State::Fail) return State::Fail;
    if (any(State::Pending)) return State::Pending;
    if (any(State::Inconclusive)) return State::Inconclusive;
    return State::Pass;
}

// The cancellation waits until the empty prefix is settled.
bool empty_prefix_settled(const RawProbeGateInput& input, const Fixture& fixture) {
    RawProbeTranscript t;
    t.writes.assign(input.prior_writes.begin(), input.prior_writes.end());
    t.events.assign(input.events.begin(), input.events.end());
    const auto collected = collect(t.events);
    if (!collected.bounded) return true;  // nothing more can be judged; let the probe finish
    const auto observed = observe_streams(t, collected, fixture);
    return observed.empty != State::Pending;
}

RawProbeDefinition build(std::chrono::milliseconds deadline, const Fixture& fixture) {
    require_draft22_wire("draft 22 namespace discovery");
    if (deadline.count() <= 0 || !fetch_first_object_fixture_valid(fixture.ns, fixture.name))
        throw std::invalid_argument("invalid draft 22 namespace discovery fixture or deadline");
    RawProbeDefinition definition;
    definition.id = std::string(kDraft22DiscoverNamespaces);
    definition.setup_bytes = setup_message();
    definition.deadline = deadline;
    definition.peer_setup_ready = setup_ready;
    definition.writes.push_back({RawProbeChannel::NewBidi, subscribe(fixture), false});
    definition.writes.push_back({RawProbeChannel::NewBidi, subscribe_namespace(request_id(1), {}), false});
    RawProbeWrite cancel{RawProbeChannel::NewBidi, {}, false, kEmptyWrite, {}, RawProbeOperation::StopSending,
                         kCancelled};
    cancel.evidence_ready = [fixture](const RawProbeGateInput& input) { return empty_prefix_settled(input, fixture); };
    definition.writes.push_back(std::move(cancel));
    RawProbeWrite matching{RawProbeChannel::NewBidi, subscribe_namespace(request_id(2), matching_prefix(fixture)),
                           false};
    matching.delay_after_previous = kAfterCancel;
    definition.writes.push_back(std::move(matching));
    definition.writes.push_back(
        {RawProbeChannel::NewBidi, subscribe_namespace(request_id(3), nonmatching_prefix(fixture)), false});
    definition.response_ready = [fixture](const RawProbeTranscript& t) {
        return observe(t, fixture) != State::Pending;
    };
    return definition;
}

}  // namespace

std::vector<std::byte> draft22_nonmatching_prefix_field(const std::vector<std::byte>& first) {
    if (first.size() > 1) return {first.begin(), first.end() - 1};
    auto field = first;
    field.push_back(std::byte{'-'});
    return field;
}

RawProbeDefinition draft22_namespace_discovery_probe(std::chrono::milliseconds deadline,
                                                     std::vector<std::vector<std::byte>> track_namespace,
                                                     std::vector<std::byte> track_name) {
    return build(deadline, {std::move(track_namespace), std::move(track_name)});
}

std::optional<bool> evaluate_draft22_namespace_discovery(const RawProbeTranscript& t) {
    if (t.scenario_id != kDraft22DiscoverNamespaces) return std::nullopt;
    // The stimulus is rebuilt on the draft 22 wire; on any other wire nothing is judged.
    if (current_wire_draft() != 22 || t.writes.empty() || t.harness_failed) return std::nullopt;
    const auto fixture = recover_fixture(t.writes.front().write.bytes, kSubscribe, request_id(0));
    if (!fixture) return std::nullopt;
    const auto expected = build(kRebuildDeadline, *fixture);
    // The probe settles on its evidence; a context that timed out is judged on what it holds.
    if (const auto proven = prove(t, expected, true)) {
        const auto state = observe(proven->prefix, *fixture);
        if (state == State::Pass) return true;
        if (state == State::Fail) return false;
        return std::nullopt;
    }
    // A publisher that ended the empty prefix's response with FIN keeps its cancellation from being sent (a
    // STOP_SENDING needs the stream still open), so the probe times out before its last writes. What it
    // sent until then is still proven, and can show a failure, never a pass.
    if (const auto delivered = prove_delivered(t, expected)) {
        const auto collected = collect(delivered->events);
        if (!collected.bounded) return std::nullopt;
        const auto observed = observe_streams(*delivered, collected, *fixture);
        if (observed.empty == State::Fail) return false;
    }
    return std::nullopt;
}

}  // namespace moq::interop::scenarios
