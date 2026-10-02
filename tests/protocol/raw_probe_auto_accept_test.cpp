#include "moq/interop/scenarios/raw_probe.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;

Bytes announce(std::uint64_t id) {
    return encode(d18::PublishNamespaceMessage{id, d18::TrackNamespace{{text("n")}}, {}});
}
bool is_announcement(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    return message && cursor.remaining() == 0 && std::holds_alternative<d18::PublishNamespaceMessage>(*message);
}

RawProbeDefinition definition(std::size_t limit) {
    RawProbeDefinition result{"auto-accept", setup_with({}), {}, true,
        [](std::span<const std::byte> input) { return !input.empty(); }, std::chrono::milliseconds{50},
        [](const RawProbeTranscript& transcript) { return transcript.auto_replies.size() >= 2; }, {}};
    result.auto_accept_ready = is_announcement;
    result.auto_accept_reply = ok();
    result.auto_accept_limit = limit;
    return result;
}

TEST(RawProbeAutoAccept, AnswersEveryMatchingPeerRequestOnceAfterPeerSetup) {
    const auto def = definition(8);
    ScriptTransport* observed = nullptr;
    (void)observed;
    const auto transcript = drive_probe(def, [&](PeerView& v) {
        // A request that arrives before SETUP is answered only once SETUP has been seen.
        v.when("early", v.step == 0, [&] { v.data(0, announce(0)); });
        v.when("setup", v.step == 1, [&] { v.data(2, setup_with({})); });
        v.when("second", v.step == 3, [&] { v.data(4, announce(2)); });
        // Not a PUBLISH_NAMESPACE: no reply.
        v.when("other", v.step == 4, [&] { v.data(8, encode(d18::SubscribeNamespaceMessage{4, d18::TrackNamespace{{text("n")}}, {}})); });
    });
    EXPECT_TRUE(transcript.complete);
    ASSERT_EQ(transcript.auto_replies.size(), 2u);
    EXPECT_EQ(transcript.auto_replies[0].stream_id, 0u);
    EXPECT_EQ(transcript.auto_replies[1].stream_id, 4u);
    EXPECT_TRUE(raw_probe_stimulus_valid(transcript, def));
}

TEST(RawProbeAutoAccept, RepliesWithTheConfiguredBytesAndStopsAtTheLimit) {
    auto def = definition(1);
    def.response_ready = [](const RawProbeTranscript& transcript) {
        return transcript.auto_replies.size() == 1 && transcript.events.size() > 4;
    };
    ScriptTransport transport;
    transport.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
    RawProbeController controller(transport, def);
    transport.events.push_back(transport::StreamDataEvent{2, setup_with({}), false});
    transport.events.push_back(transport::StreamDataEvent{0, announce(0), false});
    transport.events.push_back(transport::StreamDataEvent{4, announce(2), false});
    const auto start = RawProbeClock::now();
    for (int step = 0; step < 6; ++step) {
        transport.events.push_back(transport::DatagramEvent{{std::byte{0}}});
        controller.poll(start + std::chrono::milliseconds(step));
    }
    EXPECT_EQ(transport.output[0], ok());
    EXPECT_TRUE(transport.output[4].empty());
    EXPECT_EQ(controller.transcript().auto_replies.size(), 1u);
}

TEST(RawProbeAutoAccept, ProofRejectsAReplyThatTheDefinitionWouldNotHaveSent) {
    const auto def = definition(8);
    auto transcript = drive_probe(def, [&](PeerView& v) {
        v.when("setup", v.step == 0, [&] { v.data(2, setup_with({})); });
        v.when("a", v.step == 1, [&] { v.data(0, announce(0)); });
        v.when("b", v.step == 2, [&] { v.data(4, announce(2)); });
    });
    ASSERT_TRUE(raw_probe_stimulus_valid(transcript, def));
    auto forged = transcript;
    forged.auto_replies.push_back({8, forged.events.size()});  // no request was ever received there
    EXPECT_FALSE(raw_probe_stimulus_valid(forged, def));
    forged = transcript;
    forged.auto_replies[0].stream_id = 3;  // not a peer-opened bidirectional stream
    EXPECT_FALSE(raw_probe_stimulus_valid(forged, def));
    forged = transcript;
    forged.auto_replies.push_back(forged.auto_replies.front());  // a stream answered twice
    EXPECT_FALSE(raw_probe_stimulus_valid(forged, def));
    // A definition without auto-accept never records replies.
    auto plain = def;
    plain.auto_accept_ready = nullptr;
    EXPECT_FALSE(raw_probe_stimulus_valid(transcript, plain));
}

}  // namespace
}  // namespace moq::interop::scenarios
