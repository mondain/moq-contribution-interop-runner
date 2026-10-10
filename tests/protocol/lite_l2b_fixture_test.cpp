// Fixture-level tests for the L2b behavior of ConformingLitePublisher: datagrams (draft 6.4). The real
// LiteProbeController drives the scripted peer; no evaluators are involved.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/session/lite_session.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using moq::interop::scenarios::LiteProbeDefinition;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
using namespace moq::interop::test::lite;
namespace scen = moq::interop::scenarios;
namespace sess = moq::interop::session;

const std::string kBroadcast = "demo/live";
const std::string kTrack = "video";

ConformingLitePublisherConfig datagram_config() {
    ConformingLitePublisherConfig config;
    config.datagrams = true;
    config.frames_per_group = 1;  // a group of exactly one frame is eligible for datagram delivery
    return config;
}

LiteTranscript run(ConformingLitePublisherConfig config) {
    ConformingLitePublisher publisher(std::move(config));
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 3000ms;
    definition.steps.push_back(scen::lite_open_bidi(
        scen::lite_subscribe_stream_bytes(l06::Subscribe{1, kBroadcast, kTrack, l06::SubscribeRange{}}), false, "sub"));
    definition.observation_window = 300ms;
    scen::LiteProbeController controller(std::move(definition), peer, clock);
    for (std::size_t i = 0; i < 100000 && controller.poll(); ++i) clock.advance(1ms);
    return controller.transcript();
}

std::size_t group_streams(const LiteTranscript& t) {
    std::size_t count = 0;
    for (const auto& record : t.streams) {
        if (record.origin == sess::LiteOrigin::Peer && record.kind == sess::LiteStreamKind::Group) ++count;
    }
    return count;
}

TEST(LiteL2bFixture, DatagramsAreOffByDefault) {
    ConformingLitePublisherConfig config;
    config.frames_per_group = 1;
    const auto t = run(config);
    EXPECT_TRUE(t.datagrams.empty());
    EXPECT_GT(group_streams(t), 0u);
}

TEST(LiteL2bFixture, ASingleFrameGroupIsAlsoSentAsADatagramCarryingTheSameFrame) {
    const auto t = run(datagram_config());
    ASSERT_FALSE(t.datagrams.empty());
    EXPECT_GT(group_streams(t), 0u) << "in addition to the Group stream";
    for (const auto& datagram : t.datagrams) {
        ASSERT_TRUE(datagram.body.has_value()) << datagram.issue;
        EXPECT_EQ(datagram.body->subscribe_id, 1u);
        EXPECT_LE(datagram.size, 1200u);
        // The same group on a stream with the same payload.
        bool matched = false;
        for (const auto& record : t.streams) {
            if (record.kind != sess::LiteStreamKind::Group) continue;
            const auto messages = sess::peer_messages(record);
            ASSERT_GE(messages.size(), 2u);
            const auto* header = std::get_if<l06::GroupHeader>(&messages[0]->message);
            const auto* frame = std::get_if<l06::Frame>(&messages[1]->message);
            if (header && frame && header->group_sequence == datagram.body->group_sequence) {
                EXPECT_EQ(frame->payload, datagram.body->payload);
                matched = true;
            }
        }
        EXPECT_TRUE(matched) << "group " << datagram.body->group_sequence;
    }
}

TEST(LiteL2bFixture, AMultiFrameGroupIsNeverSentAsADatagram) {
    auto config = datagram_config();
    config.frames_per_group = 3;
    const auto t = run(config);
    EXPECT_TRUE(t.datagrams.empty());
    EXPECT_GT(group_streams(t), 0u);
}

TEST(LiteL2bFixture, DefectDatagramOversizeSendsABodyAbove1200Bytes) {
    auto config = datagram_config();
    config.defect = LiteDefect::DatagramOversize;
    const auto t = run(config);
    ASSERT_FALSE(t.datagrams.empty());
    EXPECT_EQ(t.datagrams.front().issue, sess::kIssueDatagramOverLimit);
    EXPECT_GT(t.datagrams.front().size, 1200u);
}

TEST(LiteL2bFixture, DefectDatagramUnknownSubscribeIdUsesAnotherId) {
    auto config = datagram_config();
    config.defect = LiteDefect::DatagramUnknownSubscribeId;
    const auto t = run(config);
    ASSERT_FALSE(t.datagrams.empty());
    ASSERT_TRUE(t.datagrams.front().body.has_value());
    EXPECT_NE(t.datagrams.front().body->subscribe_id, 1u);
}

TEST(LiteL2bFixture, DefectDatagramDiffersFromStreamChangesThePayload) {
    auto config = datagram_config();
    config.defect = LiteDefect::DatagramDiffersFromStream;
    const auto t = run(config);
    ASSERT_FALSE(t.datagrams.empty());
    ASSERT_TRUE(t.datagrams.front().body.has_value());
    bool differs = false;
    for (const auto& record : t.streams) {
        if (record.kind != sess::LiteStreamKind::Group) continue;
        const auto messages = sess::peer_messages(record);
        const auto* header = std::get_if<l06::GroupHeader>(&messages[0]->message);
        const auto* frame = std::get_if<l06::Frame>(&messages[1]->message);
        if (header && frame && header->group_sequence == t.datagrams.front().body->group_sequence)
            differs = frame->payload != t.datagrams.front().body->payload;
    }
    EXPECT_TRUE(differs);
}

TEST(LiteL2bFixture, DefectDatagramOnlyOpensNoGroupStreamForTheGroup) {
    auto config = datagram_config();
    config.defect = LiteDefect::DatagramOnly;
    const auto t = run(config);
    EXPECT_FALSE(t.datagrams.empty());
    EXPECT_EQ(group_streams(t), 0u);
}

}  // namespace
