// Draft 22 own scenario for D22-6-3-MAY-159 (Section 6.3): each case builds the transcript a publisher
// exchange would produce and checks the evaluator's verdict.
#include "moq/interop/scenarios/draft22_pre_setup_request.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace moq::interop::scenarios {
namespace {

using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;

Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

constexpr transport::StreamId kRequest = 1;
constexpr transport::StreamId kControl = 3;

class Draft22PreSetupRequest : public ::testing::Test {
protected:
    ScopedWireDraft wire{22};
};

RawProbeDefinition probe() { return draft22_pre_setup_request_probe(1000ms, {b({'n'})}, b({'t'})); }

const RawProbeClock::time_point kStart{};

// What the publisher does before the rest of the SETUP goes out (`early`) and after it (`late`).
RawProbeTranscript transcript(std::vector<transport::TransportEvent> early, std::vector<transport::TransportEvent> late,
                              bool window_ended = false) {
    const auto p = probe();
    RawProbeTranscript t;
    t.scenario_id = p.id;
    t.setup = {{RawProbeChannel::NewUni, b({0xaf}), false}, kControl, 1, false, 1};
    t.setup.accepted_at = kStart;
    t.events = {transport::ConnectionEstablishedEvent{{}, {}, {}, 1200},
                transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false}};
    t.transport_established = t.peer_setup_received = true;
    t.max_datagram_payload = 1200;
    t.writes.push_back({p.writes[0], kRequest, p.writes[0].bytes.size(), false, t.events.size()});
    t.writes[0].accepted_at = kStart;
    for (auto& event : early) t.events.push_back(std::move(event));
    t.writes.push_back({p.writes[1], kControl, p.writes[1].bytes.size(), false, t.events.size()});
    t.writes[1].accepted_at = kStart + 500ms;
    t.delivery_event_count = t.events.size();
    t.stimulus_delivered = true;
    for (auto& event : late) t.events.push_back(std::move(event));
    t.timed_out = window_ended;
    t.complete = !window_ended;
    return t;
}

transport::TransportEvent answer(Bytes payload) { return transport::StreamDataEvent{kRequest, std::move(payload), false}; }
transport::TransportEvent reset() { return transport::PeerResetEvent{kRequest, 1}; }
transport::TransportEvent stop() { return transport::PeerStopSendingEvent{kRequest, 1}; }

Bytes subscribe_ok() { return b({4, 0, 2, 1, 0}); }
Bytes request_error() { return b({5, 0, 3, 0x10, 0, 0}); }

std::optional<bool> verdict(const RawProbeTranscript& t) { return evaluate_draft22_pre_setup_request(t); }

// ------------------------------------------------------------------ wiring

TEST_F(Draft22PreSetupRequest, ProbeSendsTheRequestBetweenTheTwoHalvesOfItsSetup) {
    const auto p = probe();
    EXPECT_EQ(p.id, kDraft22RequestBeforeSetup);
    EXPECT_EQ(p.setup_bytes, b({0xaf})) << "the first byte of SETUP's two-byte Type";
    EXPECT_FALSE(p.start_after_peer_setup);
    ASSERT_EQ(p.writes.size(), 2u);
    EXPECT_EQ(p.writes[0].channel, RawProbeChannel::NewBidi);
    EXPECT_EQ(p.writes[0].bytes, b({3, 0, 9, 1, 1, 1, 'n', 1, 't', 1, 0x10, 0}));
    EXPECT_EQ(p.writes[1].channel, RawProbeChannel::Control);
    EXPECT_EQ(p.writes[1].bytes, b({0, 0, 0})) << "the rest of SETUP: Type byte, Length 0";
    EXPECT_EQ(p.writes[1].delay_after_previous, kDraft22PreSetupHold);
}

TEST(Draft22PreSetupRequestWire, ProbeIsBuiltOnTheDraft22WireOnly) {
    const ScopedWireDraft wire(21);
    EXPECT_THROW(probe(), std::logic_error);
}

// -------------------------------------------------------------------- verdicts

TEST_F(Draft22PreSetupRequest, ResetBeforeTheSetupCompletesPasses) {
    EXPECT_EQ(verdict(transcript({reset()}, {})), std::optional<bool>{true});
    EXPECT_EQ(verdict(transcript({stop()}, {})), std::optional<bool>{true});
    EXPECT_EQ(verdict(transcript({stop(), reset()}, {})), std::optional<bool>{true});
}

TEST_F(Draft22PreSetupRequest, AnswerAfterTheSetupCompletesPasses) {
    EXPECT_EQ(verdict(transcript({}, {answer(subscribe_ok())})), std::optional<bool>{true});
    EXPECT_EQ(verdict(transcript({}, {answer(request_error())})), std::optional<bool>{true});
}

TEST_F(Draft22PreSetupRequest, NoBehaviourFailsThePermission) {
    // Processed before the session existed, cancelled after it, another first message, or a close.
    for (const auto& t : {transcript({answer(subscribe_ok())}, {}), transcript({}, {reset()}),
                          transcript({}, {stop()}), transcript({}, {answer(b({0x22, 0, 1, 0}))}),
                          transcript({answer(subscribe_ok()), reset()}, {}),
                          transcript({}, {transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}}}),
                          transcript({}, {}, true)}) {
        EXPECT_EQ(verdict(t), std::nullopt);
    }
}

TEST_F(Draft22PreSetupRequest, UnprovenEvidenceGivesNoVerdict) {
    const auto t = transcript({reset()}, {});
    auto other = t;
    other.scenario_id = "d22-subscribe-bounded-location-range";
    EXPECT_EQ(verdict(other), std::nullopt);
    auto whole = t;
    whole.setup.write.bytes = b({0xaf, 0, 0, 0});
    whole.setup.accepted = 4;
    EXPECT_EQ(verdict(whole), std::nullopt) << "the SETUP was complete before the request";
    auto hurried = t;
    hurried.writes[1].accepted_at = kStart + 100ms;
    EXPECT_EQ(verdict(hurried), std::nullopt);
    auto failed = t;
    failed.harness_failed = true;
    EXPECT_EQ(verdict(failed), std::nullopt);
    auto truncated = t;
    truncated.event_limit_reached = true;
    EXPECT_EQ(verdict(truncated), std::nullopt);
    auto unfinished = t;
    unfinished.complete = false;
    EXPECT_EQ(verdict(unfinished), std::nullopt);
    const ScopedWireDraft wire21(21);
    EXPECT_EQ(verdict(t), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
