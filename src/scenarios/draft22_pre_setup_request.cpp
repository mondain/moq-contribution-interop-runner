#include "moq/interop/scenarios/draft22_pre_setup_request.h"

#include "draft22_probe_support.h"

#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {

using namespace d22support;

// Section 9 message types.
constexpr std::uint64_t kSubscribe = 0x03;
constexpr std::uint64_t kSubscribeOk = 0x04;
constexpr std::uint64_t kRequestError = 0x05;
constexpr std::uint64_t kForward = 0x10;

constexpr std::size_t kRequestWrite = 0;
constexpr std::size_t kCompletionWrite = 1;

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

// The runner's SETUP split in two: its first byte opens the control stream, the rest completes it.
Bytes setup_head() {
    const auto whole = setup_message();
    return {whole.begin(), whole.begin() + 1};
}
Bytes setup_tail() {
    const auto whole = setup_message();
    return {whole.begin() + 1, whole.end()};
}

enum class State { Pending, Inconclusive, Pass };

State observe(const RawProbeTranscript& t, bool window_ended) {
    const auto pending = window_ended ? State::Inconclusive : State::Pending;
    if (t.writes.size() <= kCompletionWrite || !t.stimulus_delivered) return State::Pending;
    const auto& completion = t.writes[kCompletionWrite];
    if (!completion.delivery_event_count) return State::Pending;
    // Events before this index were observed before the rest of the SETUP was handed to the transport, so
    // the publisher acted on them without a complete SETUP from the runner.
    const auto completed_at = *completion.delivery_event_count;
    const auto collected = collect(t.events);
    if (!collected.bounded) return State::Inconclusive;
    const auto response = response_of(t, collected, kRequestWrite);
    if (!response.stream) return pending;
    const auto& stream = *response.stream;
    // The permitted reset: the request direction stopped, or the response direction reset, before the
    // session could be established.
    const bool reset_early = (stream.reset && stream.reset_event < completed_at) ||
                             (stream.stop_sending && stream.stop_event < completed_at);
    const bool answered_early = !stream.bytes.empty() && stream.first_event < completed_at;
    if (reset_early && !answered_early) return State::Pass;
    if (answered_early || stream.reset || stream.stop_sending || response.messages.malformed)
        return State::Inconclusive;
    if (response.messages.complete.empty()) return pending;
    const auto first = response.messages.complete.front().type;
    // Buffered: the answer began only once the SETUP was complete.
    return first == kSubscribeOk || first == kRequestError ? State::Pass : State::Inconclusive;
}

RawProbeDefinition build(std::chrono::milliseconds deadline, const Fixture& fixture) {
    require_draft22_wire("draft 22 pre-setup request");
    if (deadline.count() <= 0 || !fetch_first_object_fixture_valid(fixture.ns, fixture.name))
        throw std::invalid_argument("invalid draft 22 pre-setup request fixture or deadline");
    RawProbeDefinition definition;
    definition.id = std::string(kDraft22RequestBeforeSetup);
    definition.setup_bytes = setup_head();
    definition.deadline = deadline;
    // The early request does not wait for the publisher's SETUP either: a publisher that sends its own
    // only after the runner's is exercised too.
    definition.start_after_peer_setup = false;
    definition.peer_setup_ready = setup_ready;
    definition.writes.push_back({RawProbeChannel::NewBidi, subscribe(fixture), false});
    RawProbeWrite completion{RawProbeChannel::Control, setup_tail(), false};
    completion.delay_after_previous = kDraft22PreSetupHold;
    definition.writes.push_back(std::move(completion));
    definition.response_ready = [](const RawProbeTranscript& t) { return observe(t, false) != State::Pending; };
    return definition;
}

}  // namespace

RawProbeDefinition draft22_pre_setup_request_probe(std::chrono::milliseconds deadline,
                                                   std::vector<std::vector<std::byte>> track_namespace,
                                                   std::vector<std::byte> track_name) {
    return build(deadline, {std::move(track_namespace), std::move(track_name)});
}

std::optional<bool> evaluate_draft22_pre_setup_request(const RawProbeTranscript& t) {
    if (t.scenario_id != kDraft22RequestBeforeSetup) return std::nullopt;
    // The stimulus is rebuilt on the draft 22 wire; on any other wire nothing is judged.
    if (current_wire_draft() != 22 || t.writes.empty() || t.harness_failed) return std::nullopt;
    const auto fixture = recover_fixture(t.writes.front().write.bytes, kSubscribe, request_id(0));
    if (!fixture) return std::nullopt;
    const auto proven = prove(t, build(kRebuildDeadline, *fixture), true);
    if (!proven) return std::nullopt;
    if (observe(proven->prefix, proven->ended) == State::Pass) return true;
    return std::nullopt;
}

}  // namespace moq::interop::scenarios
