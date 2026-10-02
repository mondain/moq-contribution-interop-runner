#include "moq/interop/scenarios/draft18_contribution.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;
using std::chrono::milliseconds;

std::vector<Draft18ContributionProbe> probes() {
    return draft18_contribution_probes(milliseconds{60}, {text("n")}, text("t"));
}

// An open subgroup stream (id 6) with one Object and no final status.
Bytes open_subgroup() { return concat(subgroup_header(5, 2), subgroup_object(0, text("a"))); }

struct Result {
    std::optional<bool> verdict;
    std::set<transport::StreamId> fins;
    std::vector<std::pair<transport::StreamId, std::uint64_t>> stops;
    Bytes request_stream;
};

// Establishes the subscription, opens a subgroup, then lets `after` react.
Result run(const Draft18ContributionProbe& p, bool open_stream,
           const std::function<void(PeerView&)>& after) {
    Result result;
    const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); });
        v.when("open", open_stream && v.sent(1) && v.step > 3, [&] { v.data(6, open_subgroup()); });
        after(v);
        result.fins = v.transport.fins;
        result.stops = v.transport.stops;
        result.request_stream = v.transport.output[1];
    });
    result.verdict = evaluate_draft18_contribution_probe(transcript, p);
    return result;
}

// Section 11.4.3: closing an incomplete subgroup on a trigger takes a reset.
TEST(Draft18ContributionClosure, CancelResetsTheOpenSubgroup) {
    const auto all = probes();
    const auto& p = probe(all, "cancel-subscription-before-next-subgroup-object-is-produced", "D18-11-4-3-MUST-002");
    EXPECT_EQ(p.evaluator_id, "incomplete-subgroup-closure-uses-reset");
    ASSERT_EQ(p.definition.writes.size(), 3u);
    EXPECT_TRUE(p.definition.writes[1].fin);
    EXPECT_TRUE(p.definition.writes[1].bytes.empty());
    EXPECT_EQ(p.definition.writes[2].operation, RawProbeOperation::StopSending);
    EXPECT_EQ(p.definition.writes[2].application_error, 1u);
    const auto reset = run(p, true, [](PeerView& v) {
        v.when("reset", !v.transport.stops.empty(), [&] { v.push(transport::PeerResetEvent{6, 1}); });
    });
    EXPECT_EQ(reset.verdict, std::optional<bool>{true});
    EXPECT_TRUE(reset.fins.contains(1));
    ASSERT_EQ(reset.stops.size(), 1u);
    EXPECT_EQ(reset.stops[0], (std::pair<transport::StreamId, std::uint64_t>{1, 1}));
    // A FIN cannot show whether every Object was delivered.
    EXPECT_EQ(run(p, true, [](PeerView& v) {
        v.when("fin", !v.transport.stops.empty(), [&] { v.data(6, {}, true); });
    }).verdict, std::nullopt);
    EXPECT_EQ(run(p, true, [](PeerView&) {}).verdict, std::nullopt);
    // Nothing is cancelled until a subgroup is open.
    const auto early = run(p, false, [](PeerView&) {});
    EXPECT_EQ(early.verdict, std::nullopt);
    EXPECT_TRUE(early.stops.empty());
    EXPECT_FALSE(early.fins.contains(1));
    // A subgroup that already ended is not an open one.
    EXPECT_EQ(run(p, false, [](PeerView& v) {
        v.when("closed", v.sent(1) && v.step > 3, [&] { v.data(6, open_subgroup(), true); });
        v.when("reset", !v.transport.stops.empty(), [&] { v.push(transport::PeerResetEvent{6, 1}); });
    }).verdict, std::nullopt);
}

TEST(Draft18ContributionClosure, AdvancingTheStartLocationResetsTheOpenSubgroup) {
    const auto all = probes();
    const auto& p = probe(all, "advance-start-location-while-subgroup-remains-incomplete", "D18-11-4-3-MUST-002");
    ASSERT_EQ(p.definition.writes.size(), 2u);
    EXPECT_EQ(p.definition.writes[1].reuse_write_stream, 0u);
    ASSERT_TRUE(p.definition.writes[1].prepare_bytes);
    const auto reset = run(p, true, [](PeerView& v) {
        v.when("reset", v.transport.output[1].size() > 20, [&] { v.push(transport::PeerResetEvent{6, 4}); });
    });
    EXPECT_EQ(reset.verdict, std::optional<bool>{true});
    // REQUEST_UPDATE (id 3) with an AbsoluteStart filter after the open Group (2).
    const auto subscribe_size = p.definition.writes[0].bytes.size();
    ASSERT_GT(reset.request_stream.size(), subscribe_size);
    const auto update = decode_request(Bytes(reset.request_stream.begin() + static_cast<std::ptrdiff_t>(subscribe_size),
                                              reset.request_stream.end()));
    ASSERT_TRUE(update);
    const auto* message = std::get_if<d18::RequestUpdateMessage>(&*update);
    ASSERT_NE(message, nullptr);
    EXPECT_EQ(message->request_id, 3u);
    ASSERT_EQ(message->parameters.size(), 1u);
    EXPECT_EQ(message->parameters[0].type, 0x21u);
    const auto& filter = std::get<d18::SubscriptionFilter>(message->parameters[0].value);
    EXPECT_EQ(filter.type, d18::SubscriptionFilterType::AbsoluteStart);
    EXPECT_EQ(filter.start, (d18::Location{3, 0}));
    EXPECT_EQ(run(p, true, [](PeerView& v) {
        v.when("fin", v.transport.output[1].size() > 20, [&] { v.data(6, {}, true); });
    }).verdict, std::nullopt);
    // No update is sent while no subgroup is open.
    EXPECT_EQ(run(p, false, [](PeerView&) {}).request_stream.size(), subscribe_size);
}

TEST(Draft18ContributionClosure, PausingForwardingResetsTheOpenSubgroup) {
    const auto all = probes();
    const auto& p = probe(all, "pause-forwarding-with-an-unsent-subgroup-object", "D18-11-4-3-MUST-002");
    ASSERT_EQ(p.definition.writes.size(), 2u);
    const auto update = decode_request(p.definition.writes[1].bytes);
    const auto* message = std::get_if<d18::RequestUpdateMessage>(&*update);
    ASSERT_NE(message, nullptr);
    ASSERT_EQ(message->parameters.size(), 1u);
    EXPECT_EQ(message->parameters[0].type, 0x10u);
    EXPECT_EQ(std::get<d18::Uint8ParameterValue>(message->parameters[0].value).value, 0u);
    const auto both = p.definition.writes[0].bytes.size() + p.definition.writes[1].bytes.size();
    EXPECT_EQ(run(p, true, [&](PeerView& v) {
        v.when("reset", v.transport.output[1].size() == both, [&] { v.push(transport::PeerResetEvent{6, 0x4}); });
    }).verdict, std::optional<bool>{true});
    EXPECT_EQ(run(p, true, [&](PeerView& v) {
        v.when("fin", v.transport.output[1].size() == both, [&] { v.data(6, {}, true); });
    }).verdict, std::nullopt);
    EXPECT_EQ(run(p, true, [](PeerView&) {}).verdict, std::nullopt);
}

// A subgroup closed with FIN that then continues on another stream was closed early.
TEST(Draft18ContributionClosure, TerminatedSubgroupThatContinuesMustHaveBeenReset) {
    const auto all = probes();
    const auto& p = probe(all, "publisher-terminates-subgroup-before-final-object-production", "D18-11-4-3-MUST-005");
    EXPECT_EQ(p.evaluator_id, "incomplete-subgroup-closure-uses-reset");
    EXPECT_TRUE(p.requires_track);
    ASSERT_EQ(p.definition.writes.size(), 1u);
    const auto run_pair = [&](bool first_fin, bool first_reset, std::uint64_t later_delta, bool later_before_close) {
        return run(p, false, [&](PeerView& v) {
            v.when("first", v.sent(1) && v.step > 3, [&] {
                v.data(6, concat(subgroup_header(5, 2), subgroup_object(0, text("a"))));
                if (later_before_close) v.data(10, concat(subgroup_header(5, 2), subgroup_object(later_delta, text("c"))));
                if (first_fin) v.data(6, {}, true);
                if (first_reset) v.push(transport::PeerResetEvent{6, 0x4});
                if (!later_before_close)
                    v.data(10, concat(subgroup_header(5, 2), subgroup_object(later_delta, text("c"))));
            });
        }).verdict;
    };
    // Object 0 on the first stream, object 2 on the second (the first Object ID is the delta).
    EXPECT_EQ(run_pair(true, false, 2, false), std::optional<bool>{false});
    EXPECT_EQ(run_pair(false, true, 2, false), std::optional<bool>{true});
    // Not a continuation: the second stream repeats or precedes the first's Objects.
    EXPECT_EQ(run_pair(true, false, 0, false), std::nullopt);
    // Two streams open together are a different problem and prove nothing about closure.
    EXPECT_EQ(run_pair(true, false, 2, true), std::nullopt);
    // Without a later stream there is no evidence the subgroup was cut short.
    EXPECT_EQ(run(p, false, [](PeerView& v) {
        v.when("only", v.sent(1) && v.step > 3, [&] { v.data(6, open_subgroup(), true); });
    }).verdict, std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
