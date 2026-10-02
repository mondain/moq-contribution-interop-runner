#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/scenarios/raw_probe_liveness.h"
#include "moq/interop/scenarios/draft18_close.h"
#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/request_goaway.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <set>

namespace moq::interop::scenarios {
namespace {
using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;

Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
Bytes text(std::string_view value) {
    Bytes result;
    for (const auto c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}

class ScriptedPeer : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override {
        if (open_status != transport::TransportStatus::Success) return {open_status, 0};
        return {transport::TransportStatus::Success, next_bidi += 4};
    }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, next_uni += 4}; }
    transport::OperationResult write(transport::StreamId id, std::span<const std::byte> bytes, bool fin) override {
        if (write_status != transport::TransportStatus::Success) return {write_status, 0, {}};
        output[id].insert(output[id].end(), bytes.begin(), bytes.end());
        order.push_back(id);
        if (fin) fins.push_back(id);
        return {transport::TransportStatus::Success, bytes.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult stop_sending(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult send_datagram(std::span<const std::byte>) override { return {}; }
    transport::OperationResult close(std::uint64_t, std::span<const std::byte>) override { return {}; }
    std::vector<transport::TransportEvent> poll(std::size_t) override {
        auto result = std::move(events);
        events.clear();
        return result;
    }
    std::uint64_t next_bidi = static_cast<std::uint64_t>(-3);
    std::uint64_t next_uni = static_cast<std::uint64_t>(-1);
    std::map<std::uint64_t, Bytes> output;
    std::vector<std::uint64_t> order;
    std::vector<std::uint64_t> fins;
    std::vector<transport::TransportEvent> events;
    transport::TransportStatus open_status{transport::TransportStatus::Success};
    transport::TransportStatus write_status{transport::TransportStatus::Success};
};

constexpr auto kSubscribeOk = std::initializer_list<unsigned>{4, 0, 2, 0, 0};

struct Case {
    unsigned draft;
    std::string scenario;
    std::uint64_t expected_close;
    // The definition the evaluator sees: policy present, no request bound.
    RawProbeDefinition unbound;
};

Case make(unsigned draft, const std::string& id) {
    if (draft == 18) {
        const auto profiles = draft18_close_profiles();
        const auto found = std::find_if(profiles.begin(), profiles.end(),
                                        [&](const auto& profile) { return profile.scenario_id == id; });
        EXPECT_NE(found, profiles.end());
        return {18, id, found->expected_close.value_or(3), draft18_close_probe(id, 5000ms)};
    }
    for (auto& probe : draft21_close_probes(5000ms))
        if (probe.definition.id == id) return {21, id, probe.expected_close.value_or(3), probe.definition};
    for (auto& probe : draft21_request_goaway_probes(5000ms))
        if (probe.definition.id == id) return {21, id, 3, probe.definition};
    ADD_FAILURE() << "unknown scenario " << id;
    return {};
}
RawProbeDefinition bound(const Case& scenario) {
    auto definition = scenario.unbound;
    bind_liveness_track(definition, {text("media")}, text("video_1"));
    return definition;
}

// One scripted session against a RawProbeController, with a manual clock.
class Session {
public:
    explicit Session(RawProbeDefinition definition)
        : definition_(std::move(definition)),
          controller_(std::make_unique<RawProbeController>(peer, definition_)),
          t0_(RawProbeClock::time_point{} + 1000s) {
        peer.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
        peer.events.push_back(transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false});
    }
    const RawProbeTranscript& poll(std::chrono::milliseconds at) { return controller_->poll(t0_ + at); }
    void send(transport::TransportEvent event) { peer.events.push_back(std::move(event)); }
    // The stream the follow-up went out on (the stimulus may have used others).
    transport::StreamId follow_up_stream() const { return *transcript().liveness->write.stream_id; }
    void answer(std::initializer_list<unsigned> message, bool fin = false) {
        send(transport::StreamDataEvent{follow_up_stream(), b(message), fin});
    }
    const RawProbeTranscript& transcript() const { return controller_->transcript(); }
    // Drives the standard sequence up to the follow-up having been sent.
    void run_to_follow_up() {
        poll(0ms);
        poll(499ms);
        poll(500ms);
    }
    ScriptedPeer peer;

private:
    RawProbeDefinition definition_;
    std::unique_ptr<RawProbeController> controller_;
    RawProbeClock::time_point t0_;
};

class Liveness : public testing::TestWithParam<std::pair<unsigned, const char*>> {};

TEST_P(Liveness, ServedFollowUpWithoutCloseIsFailAndStimulusIsUnchanged) {
    const auto scenario = make(GetParam().first, GetParam().second);
    ASSERT_TRUE(scenario.unbound.liveness.has_value());
    const auto definition = bound(scenario);
    ASSERT_FALSE(definition.liveness->request.empty());
    // Binding a track changes no stimulus byte.
    ASSERT_EQ(definition.writes.size(), scenario.unbound.writes.size());
    for (std::size_t i = 0; i < definition.writes.size(); ++i)
        EXPECT_EQ(definition.writes[i].bytes, scenario.unbound.writes[i].bytes);
    EXPECT_EQ(definition.setup_bytes, scenario.unbound.setup_bytes);

    Session session(definition);
    session.poll(0ms);
    // Too soon: nothing besides the stimulus has been sent.
    EXPECT_FALSE(session.transcript().liveness.has_value());
    session.poll(499ms);
    EXPECT_FALSE(session.transcript().liveness.has_value());
    session.poll(500ms);
    ASSERT_TRUE(session.transcript().liveness.has_value());
    const auto stream = *session.transcript().liveness->write.stream_id;
    EXPECT_EQ(session.peer.output[stream], definition.liveness->request);
    // Not answered yet: unscored.
    EXPECT_FALSE(evaluate_raw_probe_close(session.transcript(), scenario.unbound, scenario.expected_close).has_value());
    session.send(transport::StreamDataEvent{stream, b(kSubscribeOk), false});
    session.poll(520ms);
    EXPECT_FALSE(session.transcript().complete);
    session.poll(1019ms);
    EXPECT_FALSE(session.transcript().complete);
    const auto& done = session.poll(1020ms);
    ASSERT_TRUE(done.complete);
    EXPECT_TRUE(raw_probe_liveness_proven(done, scenario.unbound));
    EXPECT_EQ(evaluate_raw_probe_close(done, scenario.unbound, scenario.expected_close), std::optional<bool>{false});
}

TEST_P(Liveness, CloseWithTheRequiredCodeStillPasses) {
    const auto scenario = make(GetParam().first, GetParam().second);
    for (const auto when : {10ms, 700ms, 1100ms}) {
        Session session(bound(scenario));
        session.poll(0ms);
        if (when > 500ms) session.poll(500ms);
        if (when > 1000ms) {
            session.answer(kSubscribeOk);
            session.poll(520ms);
        }
        session.send(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, scenario.expected_close, {}});
        const auto& done = session.poll(when);
        ASSERT_TRUE(done.complete);
        EXPECT_EQ(evaluate_raw_probe_close(done, scenario.unbound, scenario.expected_close), std::optional<bool>{true});
        EXPECT_FALSE(raw_probe_liveness_proven(done, scenario.unbound));
    }
}

TEST_P(Liveness, ACloseNeverBecomesAFailureByLiveness) {
    const auto scenario = make(GetParam().first, GetParam().second);
    // A transport-level close is unscored, as before; a wrong application code is
    // judged by the existing rule; neither is a liveness verdict.
    Session session(bound(scenario));
    session.run_to_follow_up();
    session.answer(kSubscribeOk);
    session.poll(520ms);
    session.send(transport::PeerCloseEvent{transport::CloseErrorSpace::Transport, 0, {}});
    const auto& done = session.poll(700ms);
    EXPECT_FALSE(raw_probe_liveness_proven(done, scenario.unbound));
    EXPECT_FALSE(evaluate_raw_probe_close(done, scenario.unbound, scenario.expected_close).has_value());
}

TEST_P(Liveness, UnansweredOrRefusedFollowUpIsUnscored) {
    const auto scenario = make(GetParam().first, GetParam().second);
    {   // Silence.
        Session session(bound(scenario));
        session.run_to_follow_up();
        const auto& end = session.poll(5000ms + 1000ms + 1ms);
        EXPECT_TRUE(end.timed_out);
        EXPECT_FALSE(evaluate_raw_probe_close(end, scenario.unbound, scenario.expected_close).has_value());
    }
    // A refusal proves the session is open but is confounded (a publisher may
    // refuse requests after a GOAWAY), so it is not scored.
    for (const auto& reply : {std::initializer_list<unsigned>{7, 0, 1, 0},
                              std::initializer_list<unsigned>{5, 0, 3, 0x10, 0, 0}}) {
        Session session(bound(scenario));
        session.run_to_follow_up();
        session.answer(reply);
        session.poll(520ms);
        const auto& end = session.poll(1600ms);
        EXPECT_FALSE(session.transcript().liveness->answered_at.has_value());
        EXPECT_FALSE(raw_probe_liveness_proven(end, scenario.unbound));
        EXPECT_FALSE(evaluate_raw_probe_close(end, scenario.unbound, scenario.expected_close).has_value());
    }
    {   // The follow-up stream is reset.
        Session session(bound(scenario));
        session.run_to_follow_up();
        session.send(transport::PeerResetEvent{session.follow_up_stream(), 7});
        session.poll(520ms);
        session.answer(kSubscribeOk);
        session.poll(530ms);
        const auto& end = session.poll(2000ms);
        EXPECT_FALSE(raw_probe_liveness_proven(end, scenario.unbound));
    }
    {   // The answer is cut off mid-message.
        Session session(bound(scenario));
        session.run_to_follow_up();
        session.answer({4, 0, 2, 0});
        session.poll(520ms);
        const auto& end = session.poll(2000ms);
        EXPECT_FALSE(raw_probe_liveness_proven(end, scenario.unbound));
    }
}

TEST_P(Liveness, ForgedTranscriptsAreNotScored) {
    const auto scenario = make(GetParam().first, GetParam().second);
    Session session(bound(scenario));
    session.run_to_follow_up();
    session.answer(kSubscribeOk);
    session.poll(520ms);
    session.poll(1020ms);
    const auto good = session.transcript();
    ASSERT_TRUE(raw_probe_liveness_proven(good, scenario.unbound));
    const auto score = [&](const RawProbeTranscript& t) { return raw_probe_liveness_proven(t, scenario.unbound); };
    {   auto t = good; t.liveness.reset(); EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->answered_at.reset(); EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->settled_at.reset(); EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->settled_at = *t.liveness->answered_at + 100ms; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->write.accepted_at = *t.liveness->anchor_at + 100ms; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->anchor_at = *t.setup.accepted_at - 1ms; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->write.stream_id = t.setup.stream_id; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->write.stream_id = *t.liveness->write.stream_id + 40; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->write.write.bytes.back() = std::byte{1}; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->write.delivery_event_count = 0; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->anchor_event_count = 0; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.liveness->write.write.bytes[3] = std::byte{9}; EXPECT_FALSE(score(t)); }  // Request ID
    {   auto t = good; t.timed_out = true; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.complete = false; EXPECT_FALSE(score(t)); }
    {   auto t = good; t.harness_failed = true; EXPECT_FALSE(score(t)); }
    {   // A close anywhere defeats it, whatever its code.
        auto t = good;
        t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
        EXPECT_FALSE(score(t));
    }
    {   // The reply sits before the request it would answer.
        auto t = good;
        auto reply = std::find_if(t.events.begin(), t.events.end(), [&good](const auto& e) {
            const auto* d = std::get_if<transport::StreamDataEvent>(&e);
            return d && d->stream_id == *good.liveness->write.stream_id;
        });
        ASSERT_NE(reply, t.events.end());
        auto moved = *reply;
        t.events.erase(reply);
        t.events.insert(t.events.begin() + 1, moved);
        EXPECT_FALSE(score(t));
    }
    {   // Without its policy the definition never scores a follow-up.
        auto definition = scenario.unbound;
        definition.liveness.reset();
        EXPECT_FALSE(raw_probe_liveness_proven(good, definition));
        EXPECT_FALSE(evaluate_raw_probe_close(good, definition, scenario.expected_close).has_value());
    }
    {   // A follow-up sent before the policy's delay is not one the definition allows.
        auto early = bound(scenario);
        early.liveness->delay = 10ms;
        Session fast(early);
        fast.poll(0ms);
        fast.poll(20ms);
        fast.answer(kSubscribeOk);
        fast.poll(30ms);
        const auto& t = fast.poll(600ms);
        ASSERT_TRUE(t.complete);
        EXPECT_FALSE(score(t));
    }
}

INSTANTIATE_TEST_SUITE_P(
    Families, Liveness,
    testing::Values(std::pair{18u, "receive-two-goaways-on-control-stream"},
                    std::pair{18u, "receive-goaway-uri-length-8193"},
                    std::pair{18u, "receive-control-goaway-with-wrong-receiver-request-id-parity"},
                    std::pair{18u, "receive-server-setup-with-authority"},
                    std::pair{18u, "receive-server-setup-with-path"},
                    std::pair{18u, "receive-understood-key-value-invalid-serialization"},
                    std::pair{18u, "receive-zero-length-namespace-field"},
                    std::pair{18u, "receive-undecodable-authorization-token-structure"},
                    std::pair{21u, "d21-duplicate-control-goaway"},
                    std::pair{21u, "d21-goaway-uri-length-boundary"},
                    std::pair{21u, "d21-setup-known-key-value-malformed-value"},
                    std::pair{21u, "d21-fill-recursive-parameter"},
                    std::pair{21u, "d21-request-undecodable-authorization-token"},
                    std::pair{21u, "d21-message-body-length-mismatch"}));

TEST(LivenessAnswers, EachDraftRecognisesItsOwnSubscribeOkOnly) {
    // d21 SUBSCRIBE_OK is decoded by the d21 rules, d18 by the d18 rules.
    for (const unsigned draft : {18u, 21u}) {
        auto scenario = draft == 18 ? make(18, "receive-two-goaways-on-control-stream")
                                    : make(21, "d21-duplicate-control-goaway");
        Session session(bound(scenario));
        session.run_to_follow_up();
        EXPECT_EQ(liveness_answer(session.transcript(), draft), LivenessAnswer::None);
        session.answer(kSubscribeOk);
        session.poll(520ms);
        EXPECT_EQ(liveness_answer(session.transcript(), draft), LivenessAnswer::Serving);
    }
}

TEST(LivenessGuards, NoFollowUpUntilPeerSetupAndTrackAreKnown) {
    const auto scenario = make(18, "receive-two-goaways-on-control-stream");
    {   // Unbound: no track, so nothing is ever sent or scored.
        Session session(scenario.unbound);
        session.poll(0ms);
        session.poll(5000ms);
        EXPECT_FALSE(session.transcript().liveness.has_value());
        EXPECT_EQ(session.peer.order.size(), 2u);   // SETUP and the stimulus only
    }
    {   // The publisher's SETUP never arrives (the stimulus waits for it too).
        ScriptedPeer peer;
        peer.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
        RawProbeController controller(peer, bound(scenario));
        const auto t0 = RawProbeClock::time_point{} + 1000s;
        controller.poll(t0);
        controller.poll(t0 + 3000ms);
        EXPECT_FALSE(controller.transcript().liveness.has_value());
    }
}

TEST(LivenessGuards, ClosingSessionIsNotAHarnessFault) {
    const auto scenario = make(18, "receive-two-goaways-on-control-stream");
    for (const auto status : {transport::TransportStatus::ConnectionClosed, transport::TransportStatus::InvalidState}) {
        Session session(bound(scenario));
        session.poll(0ms);
        session.peer.open_status = status;
        const auto& t = session.poll(600ms);
        EXPECT_FALSE(t.harness_failed);
        EXPECT_FALSE(t.liveness.has_value());
    }
    {
        Session session(bound(scenario));
        session.poll(0ms);
        session.peer.write_status = transport::TransportStatus::ConnectionClosed;
        const auto& t = session.poll(600ms);
        EXPECT_FALSE(t.harness_failed);
        EXPECT_FALSE(raw_probe_liveness_proven(t, scenario.unbound));
    }
    {   // Stream credit exhausted: wait rather than fail.
        Session session(bound(scenario));
        session.poll(0ms);
        session.peer.open_status = transport::TransportStatus::StreamLimit;
        const auto& t = session.poll(600ms);
        EXPECT_FALSE(t.harness_failed);
        EXPECT_FALSE(t.liveness.has_value());
    }
}

TEST(LivenessGuards, UnsoundDefinitionsAreRefused) {
    ScriptedPeer peer;
    auto datagram = make(18, "receive-two-goaways-on-control-stream");
    auto definition = bound(datagram);
    definition.writes.push_back({RawProbeChannel::Datagram, b({0x40}), false});
    EXPECT_THROW(RawProbeController(peer, definition), std::invalid_argument);
    EXPECT_FALSE(liveness_definition_eligible(definition));
    definition = bound(datagram);
    definition.writes.front().channel = RawProbeChannel::PeerBidi;
    EXPECT_FALSE(liveness_definition_eligible(definition));
    definition = bound(datagram);
    definition.offer_replacement_session = true;
    EXPECT_FALSE(liveness_definition_eligible(definition));
    definition = bound(datagram);
    definition.liveness->request.back() = std::byte{1};   // not a parameter-free SUBSCRIBE
    EXPECT_THROW(RawProbeController(peer, definition), std::invalid_argument);
    definition = bound(datagram);
    definition.liveness->draft = 19;
    EXPECT_THROW(RawProbeController(peer, definition), std::invalid_argument);
    definition = bound(datagram);
    definition.liveness->request_id = 8;   // even: wrong parity for a server request
    EXPECT_THROW(RawProbeController(peer, definition), std::invalid_argument);
}

TEST(LivenessPolicy, OnlyListedScenariosAreOptedIn) {
    // Families judged not sound, or not a bare MUST close, never get a follow-up.
    for (const char* id : {"receive-unknown-datagram-type", "receive-invalid-object-datagram-type-0x10",
                           "receive-datagram-types-0x22-0x23-0x26-0x27-0x2a-0x2b-0x2e-0x2f",
                           "receive-subgroup-types-with-subgroup-id-mode-three",
                           "receive-invalid-subgroup-header-type-0x80",
                           "receive-duplicate-request-id-across-request-streams",
                           "receive-duplicate-nonrepeatable-message-parameter",
                           "receive-message-parameter-on-disallowed-message-native-quic"}) {
        EXPECT_FALSE(liveness_follow_up_sound(18, id)) << id;
        EXPECT_FALSE(make(18, id).unbound.liveness.has_value()) << id;
    }
    for (const char* id : {"d21-unknown-datagram-type", "d21-duplicate-request-id-across-streams",
                           "d21-unexpected-duplicate-message-parameter", "d21-update-on-track-status",
                           "d21-group-order-in-subscription-update", "d21-discovery-update-invalid-forward",
                           "d21-duplicate-request-update-id", "d21-publish-state-notify-on-namespace-request",
                           "d21-publish-state-notify-on-fetch", "d21-subscriber-sends-publish-state-notify",
                           "d21-publish-established-subscriber-sends-publish-state-notify",
                           "d21-goaway-on-distinct-request-streams",
                           "d21-unknown-request-stream-message"}) {
        EXPECT_FALSE(liveness_follow_up_sound(21, id)) << id;
    }
    for (const auto& probe : draft21_close_probes()) {
        const auto sound = liveness_follow_up_sound(21, probe.definition.id);
        EXPECT_EQ(probe.definition.liveness.has_value(), sound) << probe.definition.id;
        if (probe.definition.liveness) {
            EXPECT_EQ(probe.definition.liveness->draft, 21u);
            EXPECT_TRUE(probe.definition.liveness->request.empty());
        }
    }
    for (const auto& profile : draft18_close_profiles()) {
        const auto definition = draft18_close_probe(profile.scenario_id, 100ms);
        EXPECT_EQ(definition.liveness.has_value(), liveness_follow_up_sound(18, profile.scenario_id))
            << profile.scenario_id;
    }
    for (const auto& probe : draft18_request_goaway_probes())
        EXPECT_EQ(probe.definition.liveness.has_value(), probe.duplicate) << probe.definition.id;
    for (const auto& probe : draft21_request_goaway_probes())
        EXPECT_EQ(probe.definition.liveness.has_value(), probe.duplicate) << probe.definition.id;
    // Every row this feature is for is covered.
    for (const char* id : {"receive-two-goaways-on-control-stream", "receive-goaway-uri-length-8193",
                           "receive-control-goaway-with-wrong-receiver-request-id-parity",
                           "receive-server-setup-with-authority", "receive-webtransport-setup-with-authority",
                           "receive-server-setup-with-path", "receive-webtransport-setup-with-path",
                           "receive-understood-key-value-invalid-serialization",
                           "receive-undecodable-authorization-token-structure",
                           "register-request-token-exceeding-advertised-cache-size"})
        EXPECT_TRUE(draft18_close_probe(id, 100ms).liveness.has_value()) << id;
    for (const char* id : {"d21-duplicate-control-goaway", "d21-goaway-uri-length-boundary",
                           "d21-message-body-length-mismatch", "d21-setup-known-key-value-malformed-value",
                           "d21-request-undecodable-authorization-token", "d21-request-token-cache-overflow",
                           "d21-fill-forbidden-nested-authorization", "d21-fill-recursive-parameter",
                           "d21-duplicate-request-goaway"})
        EXPECT_TRUE(liveness_follow_up_sound(21, id)) << id;
}

TEST(LivenessPolicy, FollowUpRequestIdNeverCollidesWithAStimulus) {
    // The follow-up's Request ID (7) must be fresh in every opted-in stimulus.
    const auto first_frame_request_id = [](const Bytes& bytes) -> std::optional<std::uint64_t> {
        if (bytes.size() < 4) return std::nullopt;
        return std::to_integer<std::uint64_t>(bytes[3]);
    };
    std::vector<RawProbeDefinition> all;
    for (const auto& probe : draft21_close_probes()) all.push_back(probe.definition);
    for (const auto& profile : draft18_close_profiles()) all.push_back(draft18_close_probe(profile.scenario_id, 100ms));
    for (const auto& probe : draft21_request_goaway_probes()) all.push_back(probe.definition);
    for (const auto& probe : draft18_request_goaway_probes()) all.push_back(probe.definition);
    for (const auto& definition : all) {
        if (!definition.liveness) continue;
        for (const auto& write : definition.writes) {
            if (write.channel != RawProbeChannel::NewBidi) continue;
            const auto id = first_frame_request_id(write.bytes);
            if (id) EXPECT_LT(*id, 7u) << definition.id;
        }
    }
}
}  // namespace
}  // namespace moq::interop::scenarios
