#include "moq/interop/scenarios/draft18_contribution.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;
using std::chrono::milliseconds;

constexpr milliseconds kDeadline{60};

std::vector<Draft18ContributionProbe> probes() { return draft18_contribution_probes(kDeadline); }

Bytes track_status_request() {
    return encode(d18::TrackStatusMessage{0, d18::TrackNamespace{{text("n")}}, d18::TrackName{text("t")}, {}});
}
Bytes publish_request() {
    return encode(d18::PublishMessage{0, d18::TrackNamespace{{text("n")}}, d18::TrackName{text("t")}, 9, {}, {}});
}
Bytes publish_namespace_request() {
    return encode(d18::PublishNamespaceMessage{0, d18::TrackNamespace{{text("n")}}, {}});
}

struct Outcome {
    std::optional<bool> result;
    std::map<transport::StreamId, Bytes> output;
    std::set<transport::StreamId> fins;
    std::vector<std::pair<transport::StreamId, std::uint64_t>> stops;
};

// The publisher opens `request` on stream 0 after its SETUP; `after` reacts
// once the runner's follow-up request is on its first bidirectional stream (1).
Outcome run(const Draft18ContributionProbe& p, const Bytes& request,
            const std::function<void(PeerView&)>& after) {
    Outcome outcome;
    const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        v.when("request", v.step > 1, [&] { v.data(0, request); });
        after(v);
        outcome.output = v.transport.output;
        outcome.fins = v.transport.fins;
        outcome.stops = v.transport.stops;
    });
    outcome.result = evaluate_draft18_contribution_probe(transcript, p);
    return outcome;
}

const d18::TrackProperties& properties_of(const d18::Message& message) {
    return std::get<d18::RequestOkMessage>(message).track_properties;
}
std::vector<std::uint64_t> types(const d18::TrackProperties& properties) {
    std::vector<std::uint64_t> result;
    for (const auto& entry : properties.entries) result.push_back(entry.type);
    return result;
}

// Sections 2.5, 14 and 15.8: unknown optional Properties are skipped.
TEST(Draft18ContributionPublisher, RecoveryTrackStatusReplyCarriesUnknownOptionalProperties) {
    const auto all = probes();
    for (const char* requirement : {"D18-14-MUST-002", "D18-14-MUST-009", "D18-15-8-MUST-001"}) {
        SCOPED_TRACE(requirement);
        const auto& p = probe(all, "publisher-recovery-track-status-unknown-optional-properties", requirement);
        EXPECT_EQ(p.evaluator_id, "track-status-unknown-optional-properties-skipped");
        EXPECT_TRUE(p.definition.peer_request_ready);
        ASSERT_EQ(p.definition.writes.size(), 2u);
        EXPECT_EQ(p.definition.writes[0].channel, RawProbeChannel::PeerBidi);
        // Only a TRACK_STATUS with an even Request ID opens the exchange.
        EXPECT_TRUE(p.definition.peer_request_ready(track_status_request()));
        EXPECT_FALSE(p.definition.peer_request_ready(publish_request()));
        EXPECT_FALSE(p.definition.peer_request_ready(encode(d18::TrackStatusMessage{
            1, d18::TrackNamespace{{text("n")}}, d18::TrackName{text("t")}, {}})));
        const auto reply = decode_request(p.definition.writes[0].bytes);
        ASSERT_TRUE(reply);
        const auto& properties = properties_of(*reply);
        // Ascending: unknown 0x00 and 0x01, known 0x22 = 1, then greased 0x9D and 0x11C.
        EXPECT_EQ(types(properties), (std::vector<std::uint64_t>{0x00, 0x01, 0x22, 0x9d, 0x11c}));
        EXPECT_EQ(std::get<d18::VarIntValue>(properties.entries[2].value).value, 1u);

        const auto survive = run(p, track_status_request(), [](PeerView& v) {
            v.when("reply", v.sent(1), [&] { v.data(1, ok()); });
        });
        EXPECT_EQ(survive.result, std::optional<bool>{true});
        EXPECT_EQ(run(p, track_status_request(), [](PeerView& v) {
            v.when("close", v.sent(1), [&] {
                v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
            });
        }).result, std::optional<bool>{false});
        EXPECT_EQ(run(p, track_status_request(), [](PeerView&) {}).result, std::nullopt);
    }
}

// Section 12.5 after the unknown Properties: value 0 must close the session
// with PROTOCOL_VIOLATION, which shows parsing reached the known Property.
TEST(Draft18ContributionPublisher, InvalidGroupOrderAfterUnknownPropertiesClosesWithProtocolViolation) {
    const auto all = probes();
    for (const char* requirement : {"D18-14-MUST-002", "D18-14-MUST-009", "D18-15-8-MUST-001"}) {
        SCOPED_TRACE(requirement);
        const auto& p = probe(all, "publisher-recovery-track-status-unknown-before-invalid-property", requirement);
        EXPECT_EQ(p.evaluator_id, "track-status-unknown-optional-properties-skipped");
        const auto reply = decode_request(p.definition.writes[0].bytes);
        ASSERT_TRUE(reply);
        const auto& properties = properties_of(*reply);
        EXPECT_EQ(types(properties), (std::vector<std::uint64_t>{0x00, 0x01, 0x22}));
        EXPECT_EQ(std::get<d18::VarIntValue>(properties.entries[2].value).value, 0u);
        const auto closes = [&](transport::CloseErrorSpace space, std::uint64_t code) {
            return run(p, track_status_request(), [&](PeerView& v) {
                v.when("close", v.sent(0), [&] { v.push(transport::PeerCloseEvent{space, code, {}}); });
            }).result;
        };
        EXPECT_EQ(closes(transport::CloseErrorSpace::Application, 3), std::optional<bool>{true});
        // KEY_VALUE_FORMATTING_ERROR means the unknown Properties were not skipped.
        EXPECT_EQ(closes(transport::CloseErrorSpace::Application, 6), std::optional<bool>{false});
        EXPECT_EQ(closes(transport::CloseErrorSpace::Transport, 3), std::nullopt);
        // Carrying on as if the invalid value were fine violates section 12.5.
        EXPECT_EQ(run(p, track_status_request(), [](PeerView& v) {
            v.when("reply", v.sent(1), [&] { v.data(1, ok()); });
        }).result, std::optional<bool>{false});
        EXPECT_EQ(run(p, track_status_request(), [](PeerView&) {}).result, std::nullopt);
    }
}

// Section 14: REQUEST_ERROR codes the publisher does not know.
TEST(Draft18ContributionPublisher, UnknownRequestErrorCodeDoesNotCloseTheSession) {
    const auto all = probes();
    for (const char* requirement : {"D18-14-MUST-004", "D18-14-MUST-NOT-002"}) {
        SCOPED_TRACE(requirement);
        const auto& p = probe(all, "publisher-request-rejected-with-unknown-error", requirement);
        EXPECT_EQ(p.evaluator_id, "session-survives-unknown-request-error");
        EXPECT_TRUE(p.definition.peer_request_ready(publish_request()));
        EXPECT_TRUE(p.definition.peer_request_ready(publish_namespace_request()));
        EXPECT_FALSE(p.definition.peer_request_ready(track_status_request()));
        const auto survive = run(p, publish_request(), [](PeerView& v) {
            v.when("reply", v.sent(1), [&] { v.data(1, ok()); });
        });
        EXPECT_EQ(survive.result, std::optional<bool>{true});
        // REQUEST_ERROR 0x9D, retry interval 0, empty reason, then FIN.
        const auto rejected = decode_request(survive.output.at(0));
        ASSERT_TRUE(rejected);
        const auto* rejection = std::get_if<d18::RequestErrorMessage>(&*rejected);
        ASSERT_NE(rejection, nullptr);
        EXPECT_EQ(rejection->error_code, 0x9du);
        EXPECT_EQ(rejection->retry_interval, 0u);
        EXPECT_TRUE(rejection->reason_phrase.bytes.empty());
        EXPECT_TRUE(survive.fins.contains(0));
        EXPECT_EQ(run(p, publish_namespace_request(), [](PeerView& v) {
            v.when("reply", v.sent(1), [&] { v.data(1, error(0x3)); });
        }).result, std::optional<bool>{true});
        EXPECT_EQ(run(p, publish_request(), [](PeerView& v) {
            v.when("close", v.sent(1), [&] {
                v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 1, {}});
            });
        }).result, std::optional<bool>{false});
        EXPECT_EQ(run(p, publish_request(), [](PeerView&) {}).result, std::nullopt);
    }
}

// Section 3.3.3: an unknown Stream Reset Error Code on a request stream.
TEST(Draft18ContributionPublisher, UnknownStreamResetCodeOnRequestDoesNotCloseTheSession) {
    const auto all = probes();
    const auto& p = probe(all, "publisher-request-stream-reset-with-unknown-code", "D18-14-MUST-006");
    EXPECT_EQ(p.evaluator_id, "session-survives-unknown-request-stream-reset-code");
    ASSERT_EQ(p.definition.writes.size(), 3u);
    EXPECT_EQ(p.definition.writes[1].operation, RawProbeOperation::StopSending);
    EXPECT_EQ(p.definition.writes[1].application_error, 0x9du);
    EXPECT_EQ(p.definition.writes[1].reuse_write_stream, 0u);
    const auto survive = run(p, publish_request(), [](PeerView& v) {
        v.when("reply", v.sent(1), [&] { v.data(1, ok()); });
    });
    EXPECT_EQ(survive.result, std::optional<bool>{true});
    const auto accepted = decode_request(survive.output.at(0));
    ASSERT_TRUE(accepted);
    EXPECT_TRUE(std::holds_alternative<d18::RequestOkMessage>(*accepted));
    ASSERT_EQ(survive.stops.size(), 1u);
    EXPECT_EQ(survive.stops[0], (std::pair<transport::StreamId, std::uint64_t>{0, 0x9d}));
    EXPECT_EQ(run(p, publish_request(), [](PeerView& v) {
        v.when("close", v.sent(1), [&] {
            v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
        });
    }).result, std::optional<bool>{false});
    EXPECT_EQ(run(p, publish_request(), [](PeerView&) {}).result, std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
