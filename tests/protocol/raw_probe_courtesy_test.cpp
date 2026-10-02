// The opt-in courtesy responses of the raw probe controller: what it answers,
// what it leaves alone, and how it orders and times its answers (draft 21,
// Sections 6.4.2.3, 8.9, 9.3, 9.4 and 9.5).
#include "moq/interop/scenarios/raw_probe.h"

#include <gtest/gtest.h>

#include <map>

namespace moq::interop::scenarios {
namespace {

using Bytes = std::vector<std::byte>;
using Clock = RawProbeClock;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

struct Sent {
    Bytes data;
    bool fin{false};
};

class Transport final : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::InvalidState, 0}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, 3}; }
    transport::OperationResult write(transport::StreamId stream, std::span<const std::byte> data,
                                     bool fin) override {
        auto& sent = writes[stream];
        sent.data.insert(sent.data.end(), data.begin(), data.end());
        sent.fin = sent.fin || fin;
        return {transport::TransportStatus::Success, data.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult stop_sending(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult send_datagram(std::span<const std::byte>) override { return {}; }
    transport::OperationResult close(std::uint64_t, std::span<const std::byte>) override { return {}; }
    std::vector<transport::TransportEvent> poll(std::size_t) override {
        auto result = std::move(inbound);
        inbound.clear();
        return result;
    }
    std::vector<transport::TransportEvent> inbound;
    std::map<transport::StreamId, Sent> writes;
};

RawProbeDefinition definition(RawProbeCourtesy courtesy) {
    RawProbeDefinition result;
    result.id = "courtesy";
    result.setup_bytes = bytes({0xaf, 0, 0, 0});
    result.start_after_peer_setup = false;
    result.deadline = std::chrono::milliseconds(1000);
    result.courtesy = courtesy;
    return result;
}

transport::StreamDataEvent data(transport::StreamId stream, Bytes payload, bool fin = false) {
    return {stream, std::move(payload), fin};
}

Bytes frame(unsigned type, const Bytes& body) {
    Bytes result{static_cast<std::byte>(type), static_cast<std::byte>(body.size() >> 8),
                 static_cast<std::byte>(body.size() & 255u)};
    result.insert(result.end(), body.begin(), body.end());
    return result;
}
// PUBLISH: Request ID 0, namespace {"n"}, name "t", Track Alias `alias`, no parameters.
Bytes publish(unsigned alias, const Bytes& parameters = bytes({0})) {
    Bytes body = bytes({0, 1, 1, 'n', 1, 't', alias});
    body.insert(body.end(), parameters.begin(), parameters.end());
    return frame(0x1d, body);
}
Bytes publish_namespace() { return frame(0x06, bytes({0, 1, 1, 'n', 0})); }
Bytes request_update(const Bytes& parameters = bytes({0})) {
    Bytes body = bytes({3});
    body.insert(body.end(), parameters.begin(), parameters.end());
    return frame(0x02, body);
}
// One AUTHORIZATION TOKEN (0x03, length-prefixed) using Alias 1: Alias Type 2.
Bytes use_alias_parameter() { return bytes({1, 3, 2, 2, 1}); }

struct Harness {
    explicit Harness(RawProbeCourtesy courtesy) : controller(transport, definition(courtesy)) {
        transport.inbound.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
        poll();
    }
    const RawProbeTranscript& poll(std::chrono::milliseconds advance = std::chrono::milliseconds{0}) {
        now += advance;
        return controller.poll(now);
    }
    const RawProbeTranscript& feed(transport::TransportEvent event,
                                   std::chrono::milliseconds advance = std::chrono::milliseconds{0}) {
        transport.inbound.push_back(std::move(event));
        return poll(advance);
    }
    Transport transport;
    RawProbeController controller;
    Clock::time_point now{Clock::now()};
};

// Streams other than the runner's own SETUP stream (3) carry courtesy responses.
bool nothing_answered(const Transport& transport) {
    for (const auto& [stream, sent] : transport.writes)
        if (stream != 3) return false;
    return true;
}

const Bytes kOk = bytes({7, 0, 1, 0});
Bytes oks(std::size_t count) {
    Bytes result;
    for (std::size_t index = 0; index < count; ++index) result.insert(result.end(), kOk.begin(), kOk.end());
    return result;
}
const Bytes kReject = bytes({5, 0, 3, 0x20, 0, 0});

TEST(RawProbeCourtesy, IsOffUnlessAPolicyAsksForIt) {
    Harness idle(RawProbeCourtesy{});
    idle.feed(data(0, publish_namespace()));
    idle.feed(data(4, publish(2)));
    EXPECT_TRUE(nothing_answered(idle.transport));
    EXPECT_TRUE(idle.poll().courtesy_writes.empty());
}

TEST(RawProbeCourtesy, AcknowledgesPublishNamespaceWithAnEmptyRequestOk) {
    RawProbeCourtesy policy;
    policy.acknowledge_namespaces = true;
    Harness harness(policy);
    // A partial message is not answered.
    auto partial = publish_namespace();
    const Bytes tail(partial.begin() + 4, partial.end());
    partial.resize(4);
    harness.feed(data(0, partial));
    EXPECT_TRUE(nothing_answered(harness.transport));
    const auto& transcript = harness.feed(data(0, tail));
    EXPECT_EQ(harness.transport.writes[0].data, kOk);
    EXPECT_FALSE(harness.transport.writes[0].fin);
    ASSERT_EQ(transcript.courtesy_writes.size(), 1u);
    EXPECT_EQ(transcript.courtesy_writes[0].stream_id, 0u);
    EXPECT_EQ(transcript.courtesy_writes[0].kind, RawProbeCourtesyKind::NamespaceOk);
    // The acknowledgement is not a stimulus write and a PUBLISH is left alone.
    EXPECT_TRUE(transcript.writes.empty());
    harness.feed(data(4, publish(2)));
    EXPECT_EQ(harness.transport.writes.count(4), 0u);
    // Request streams the runner itself opened are not the publisher's requests.
    harness.feed(data(1, publish_namespace()));
    EXPECT_EQ(harness.transport.writes.count(1), 0u);
}

TEST(RawProbeCourtesy, AcceptsOrRejectsPublish) {
    RawProbeCourtesy accept;
    accept.publish = RawProbePublishResponse::Accept;
    Harness accepting(accept);
    const auto& accepted = accepting.feed(data(4, publish(2)));
    EXPECT_EQ(accepting.transport.writes[4].data, kOk);
    EXPECT_FALSE(accepting.transport.writes[4].fin);
    ASSERT_EQ(accepted.courtesy_writes.size(), 1u);
    EXPECT_EQ(accepted.courtesy_writes[0].kind, RawProbeCourtesyKind::PublishOk);
    // Each PUBLISH on its own request stream is answered once.
    accepting.feed(data(8, publish(3)));
    accepting.poll();
    EXPECT_EQ(accepting.transport.writes[8].data, kOk);
    EXPECT_EQ(accepting.poll().courtesy_writes.size(), 2u);

    RawProbeCourtesy reject;
    reject.publish = RawProbePublishResponse::Reject;
    Harness rejecting(reject);
    const auto& rejected = rejecting.feed(data(4, publish(2)));
    // REQUEST_ERROR UNINTERESTED, then FIN (Section 6.4.2.3).
    EXPECT_EQ(rejecting.transport.writes[4].data, kReject);
    EXPECT_TRUE(rejecting.transport.writes[4].fin);
    ASSERT_EQ(rejected.courtesy_writes.size(), 1u);
    EXPECT_EQ(rejected.courtesy_writes[0].kind, RawProbeCourtesyKind::PublishError);
}

TEST(RawProbeCourtesy, RejectsAfterTheTrackAliasHasDeliveredAnObject) {
    RawProbeCourtesy policy;
    policy.publish = RawProbePublishResponse::RejectAfterObject;
    Harness harness(policy);
    harness.feed(data(4, publish(2)));
    EXPECT_TRUE(nothing_answered(harness.transport));
    // A Subgroup stream (type 0x30, alias, group) for another alias changes nothing.
    harness.feed(data(6, bytes({0x30, 9, 0})));
    EXPECT_TRUE(nothing_answered(harness.transport));
    // Not a Subgroup stream type.
    harness.feed(data(10, bytes({0x05, 2, 0})));
    EXPECT_TRUE(nothing_answered(harness.transport));
    harness.feed(data(14, bytes({0x30, 2, 0})));
    EXPECT_EQ(harness.transport.writes[4].data, kReject);
    EXPECT_TRUE(harness.transport.writes[4].fin);
    // An Object Datagram for the alias also counts as production.
    Harness datagram(policy);
    datagram.feed(data(4, publish(5)));
    datagram.feed(transport::DatagramEvent{bytes({0x08, 5, 0, 0, 'o'})});
    EXPECT_EQ(datagram.transport.writes[4].data, kReject);
}

TEST(RawProbeCourtesy, AnswersUpdatesInOrderAndHoldsAliasUsesBack) {
    RawProbeCourtesy accept;
    accept.publish = RawProbePublishResponse::Accept;
    accept.update = RawProbeUpdateResponse::Accept;
    Harness accepting(accept);
    accepting.feed(data(4, publish(2)));
    accepting.feed(data(4, request_update()));
    EXPECT_EQ(accepting.transport.writes[4].data, oks(2));
    EXPECT_EQ(accepting.poll().courtesy_writes.back().kind, RawProbeCourtesyKind::UpdateOk);

    RawProbeCourtesy ignore;
    ignore.publish = RawProbePublishResponse::Accept;
    Harness ignoring(ignore);
    ignoring.feed(data(4, publish(2)));
    ignoring.feed(data(4, request_update()));
    EXPECT_EQ(ignoring.transport.writes[4].data, kOk);  // only the PUBLISH is answered

    RawProbeCourtesy hold;
    hold.publish = RawProbePublishResponse::Accept;
    hold.update = RawProbeUpdateResponse::HoldAliasUses;
    hold.hold = std::chrono::milliseconds(300);
    Harness holding(hold);
    holding.feed(data(4, publish(2)));
    holding.feed(data(4, request_update(use_alias_parameter())));
    // The USE_ALIAS update waits; a later plain update waits behind it (responses are ordered).
    holding.feed(data(4, request_update()));
    EXPECT_EQ(holding.transport.writes[4].data, kOk);
    holding.poll(std::chrono::milliseconds(299));
    EXPECT_EQ(holding.transport.writes[4].data, kOk);
    holding.poll(std::chrono::milliseconds(2));
    EXPECT_EQ(holding.transport.writes[4].data, oks(3));
    // A plain update on its own is answered at once.
    Harness plain(hold);
    plain.feed(data(4, publish(2)));
    plain.feed(data(4, request_update()));
    EXPECT_EQ(plain.transport.writes[4].data, oks(2));
}

}  // namespace
}  // namespace moq::interop::scenarios
