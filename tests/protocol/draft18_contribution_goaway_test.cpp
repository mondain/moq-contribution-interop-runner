#include "moq/interop/scenarios/draft18_contribution.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;
using std::chrono::milliseconds;

std::vector<Draft18ContributionProbe> probes() { return draft18_contribution_probes(milliseconds{60}); }

Bytes control_goaway(Bytes uri, std::uint64_t cutoff) {
    return encode(d18::GoawayMessage{std::move(uri), 0, cutoff});
}
Bytes request_goaway(Bytes uri) { return encode(d18::GoawayMessage{std::move(uri), 0, std::nullopt}); }

// Section 10.4: a client sends a zero-length New Session URI in any GOAWAY.
TEST(Draft18ContributionGoaway, ClientGoawaysCarryNoNewSessionUri) {
    const auto all = probes();
    const auto& p = probe(all, "observe-publisher-client-goaway", "D18-10-4-MUST-001");
    EXPECT_EQ(p.evaluator_id, "client-goaway-new-session-uri-empty");
    EXPECT_TRUE(p.definition.writes.empty());
    const auto run = [&](const std::function<void(PeerView&)>& peer) {
        const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup_with({})); });
            peer(v);
        });
        return evaluate_draft18_contribution_probe(transcript, p);
    };
    const auto on_control = [&](Bytes goaway) {
        return run([&](PeerView& v) { v.when("goaway", v.step > 1, [&] { v.data(2, goaway); }); });
    };
    EXPECT_EQ(on_control(control_goaway({}, 3)), std::optional<bool>{true});
    EXPECT_EQ(on_control(control_goaway(text("moqt://elsewhere"), 3)), std::optional<bool>{false});
    // A GOAWAY on a request stream the publisher opened is covered as well.
    const auto publish = encode(d18::PublishMessage{0, d18::TrackNamespace{{text("n")}}, d18::TrackName{text("t")}, 9, {}, {}});
    const auto on_request = [&](Bytes goaway) {
        return run([&](PeerView& v) {
            v.when("goaway", v.step > 1, [&] { v.data(0, concat(publish, goaway)); });
        });
    };
    EXPECT_EQ(on_request(request_goaway({})), std::optional<bool>{true});
    EXPECT_EQ(on_request(request_goaway(text("moqt://elsewhere"))), std::optional<bool>{false});
    // One non-empty URI among several GOAWAYs is still a violation.
    EXPECT_EQ(run([&](PeerView& v) {
        v.when("goaways", v.step > 1, [&] {
            v.data(2, control_goaway({}, 3));
            v.data(0, concat(publish, request_goaway(text("x"))));
        });
    }), std::optional<bool>{false});
    EXPECT_EQ(run([](PeerView&) {}), std::nullopt);
}

// Section 10.4: requests arriving after a control GOAWAY, and those at or
// above its cutoff, are rejected with REQUEST_ERROR GOING_AWAY (0x6).
TEST(Draft18ContributionGoaway, RequestsAfterAControlGoawayAreRejectedAsGoingAway) {
    const auto all = probes();
    struct Row { const char* scenario; const char* requirement; };
    for (const auto& row : {Row{"send-new-request-after-publisher-control-goaway", "D18-10-4-MUST-009"},
                            Row{"publisher-control-goaway-with-pending-request-at-cutoff", "D18-10-4-MUST-008"}}) {
        SCOPED_TRACE(row.requirement);
        const auto& p = probe(all, row.scenario, row.requirement);
        EXPECT_EQ(p.evaluator_id, "request-error-going-away");
        ASSERT_EQ(p.definition.writes.size(), 1u);
        const auto run = [&](std::optional<Bytes> goaway, const std::function<void(PeerView&)>& reply,
                             Bytes* sent = nullptr) {
            const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
                v.when("setup", true, [&] { v.data(2, setup_with({})); });
                if (goaway) v.when("goaway", v.step > 1, [&] { v.data(2, *goaway); });
                reply(v);
                if (sent && v.sent(1)) *sent = v.transport.output[1];
            });
            return evaluate_draft18_contribution_probe(transcript, p);
        };
        const auto answer = [](Bytes bytes) { return [bytes](PeerView& v) {
            v.when("reply", v.sent(1), [&] { v.data(1, bytes); }); }; };
        Bytes sent;
        EXPECT_EQ(run(control_goaway({}, 1), answer(error(0x6)), &sent), std::optional<bool>{true});
        EXPECT_EQ(run(control_goaway({}, 1), answer(error(0x10))), std::optional<bool>{false});
        EXPECT_EQ(run(control_goaway({}, 1), answer(ok())), std::optional<bool>{false});
        // Nothing is sent, and nothing is established, until the publisher's GOAWAY.
        EXPECT_EQ(run(std::nullopt, answer(error(0x6))), std::nullopt);
        EXPECT_EQ(run(control_goaway({}, 1), [](PeerView&) {}), std::nullopt);
        const auto message = decode_request(sent);
        ASSERT_TRUE(message);
        EXPECT_TRUE(std::holds_alternative<d18::SubscribeNamespaceMessage>(*message));
    }
    // The cutoff probe targets the GOAWAY's own Request ID; the other uses ID 1.
    const auto& cutoff = probe(all, "publisher-control-goaway-with-pending-request-at-cutoff", "D18-10-4-MUST-008");
    ASSERT_TRUE(cutoff.definition.writes[0].prepare_bytes);
    Bytes sent;
    const auto transcript = drive_probe(cutoff.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        v.when("goaway", v.step > 1, [&] { v.data(2, control_goaway({}, 7)); });
        v.when("reply", v.sent(1), [&] { v.data(1, error(0x6)); sent = v.transport.output[1]; });
    });
    EXPECT_EQ(evaluate_draft18_contribution_probe(transcript, cutoff), std::optional<bool>{true});
    const auto message = decode_request(sent);
    ASSERT_TRUE(message);
    EXPECT_EQ(std::get<d18::SubscribeNamespaceMessage>(*message).request_id, 7u);
    // An even cutoff names no Request ID the runner can send: no stimulus.
    const auto even = drive_probe(cutoff.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        v.when("goaway", v.step > 1, [&] { v.data(2, control_goaway({}, 4)); });
    });
    EXPECT_EQ(evaluate_draft18_contribution_probe(even, cutoff), std::nullopt);
    const auto& after = probe(all, "send-new-request-after-publisher-control-goaway", "D18-10-4-MUST-009");
    Bytes after_sent;
    const auto later = drive_probe(after.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        v.when("goaway", v.step > 1, [&] { v.data(2, control_goaway({}, 7)); });
        v.when("reply", v.sent(1), [&] { v.data(1, error(0x6)); after_sent = v.transport.output[1]; });
    });
    EXPECT_EQ(evaluate_draft18_contribution_probe(later, after), std::optional<bool>{true});
    EXPECT_EQ(std::get<d18::SubscribeNamespaceMessage>(*decode_request(after_sent)).request_id, 1u);
}

}  // namespace
}  // namespace moq::interop::scenarios
