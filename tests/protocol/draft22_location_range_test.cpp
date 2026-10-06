// Draft 22 own scenarios for D22-3-3-1-MUST-NOT-069 (Section 3.3.1): each case builds the transcript a
// publisher exchange would produce and checks the evaluator's verdict.
#include "moq/interop/scenarios/draft22_location_range.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <gtest/gtest.h>

#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace moq::interop::scenarios {
namespace {

using Bytes = std::vector<std::byte>;
using FilterType = wire::draft22::LocationFilterType;

Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

// The runner opens its requests on server-initiated bidirectional streams 1, 5, 9, ...
constexpr transport::StreamId request_stream(std::size_t index) { return 1 + 4 * index; }

class Draft22LocationRange : public ::testing::Test {
protected:
    ScopedWireDraft wire{22};
};

RawProbeDefinition subscribe_probe() {
    return draft22_subscribe_location_range_probe(std::chrono::milliseconds(1000), {b({'n'})}, b({'t'}));
}

RawProbeTranscript start(const RawProbeDefinition& p) {
    RawProbeTranscript t;
    t.scenario_id = p.id;
    t.setup = {{RawProbeChannel::NewUni, b({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
    t.events = {transport::ConnectionEstablishedEvent{{}, {}, {}, 1200},
                transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false}};
    t.transport_established = t.peer_setup_received = true;
    t.max_datagram_payload = 1200;
    return t;
}

void accept(RawProbeTranscript& t, const RawProbeDefinition& p, std::size_t index, transport::StreamId stream) {
    const auto& write = p.writes[index];
    t.writes.push_back({write, stream, write.bytes.size(), write.fin, t.events.size()});
    t.delivery_event_count = t.events.size();
    t.stimulus_delivered = true;
}

void data(RawProbeTranscript& t, transport::StreamId stream, Bytes payload, bool fin = false) {
    t.events.push_back(transport::StreamDataEvent{stream, std::move(payload), fin});
}

// SUBSCRIBE_OK (Section 9.7): Track Alias, then LARGEST_OBJECT (0x09) when given, and no Track Properties.
Bytes subscribe_ok(unsigned alias, std::optional<std::pair<unsigned, unsigned>> largest = std::pair{7u, 9u}) {
    if (!largest) return b({4, 0, 2, alias, 0});
    return b({4, 0, 5, alias, 1, 9, largest->first, largest->second});
}
Bytes request_error() { return b({5, 0, 3, 0x10, 0, 0}); }

// Subgroup stream: flags 0x30 (Subgroup ID 0, default priority), alias, group, then Objects (delta,
// payload length 1, 'x').
Bytes subgroup(unsigned alias, unsigned group, const std::vector<unsigned>& object_ids) {
    Bytes result = b({0x30, alias, group});
    unsigned previous = 0;
    bool first = true;
    for (const auto id : object_ids) {
        result.push_back(static_cast<std::byte>(first ? id : id - previous - 1));
        result.push_back(std::byte{1});
        result.push_back(std::byte{'x'});
        previous = id;
        first = false;
    }
    return result;
}

struct Delivery {
    unsigned alias;
    unsigned group;
    std::vector<unsigned> objects;
};

// All five SUBSCRIBEs accepted and answered with `oks`, then the subgroup streams, then the window ends
// (or the context stops early when `window_ended` is false).
RawProbeTranscript subscribed(const RawProbeDefinition& p, const std::vector<Bytes>& oks,
                              const std::vector<Delivery>& deliveries, bool window_ended = true) {
    auto t = start(p);
    for (std::size_t index = 0; index < p.writes.size(); ++index) accept(t, p, index, request_stream(index));
    for (std::size_t index = 0; index < oks.size(); ++index)
        if (!oks[index].empty()) data(t, request_stream(index), oks[index]);
    transport::StreamId stream = 6;
    for (const auto& delivery : deliveries) {
        data(t, stream, subgroup(delivery.alias, delivery.group, delivery.objects), true);
        stream += 4;
    }
    t.timed_out = window_ended;
    t.complete = !window_ended;
    return t;
}

// Distinct aliases 1..5 in filter order, every SUBSCRIBE_OK reporting Largest Object {7, 9}.
std::vector<Bytes> distinct_oks() {
    return {subscribe_ok(1), subscribe_ok(2), subscribe_ok(3), subscribe_ok(4), subscribe_ok(5)};
}

// One conforming delivery per Type: 0x01 from {7, 0}, 0x02 from {7, 9}, 0x03 Group 7 from Object 9,
// 0x04 exactly {7, 9}, 0x05 from {7, 10}. Inclusive boundaries are delivered.
std::vector<Delivery> conforming() {
    return {{1, 7, {0, 9}}, {2, 7, {9, 10}}, {2, 8, {0}}, {3, 7, {9, 12}}, {4, 7, {9}}, {5, 7, {10, 11}}};
}

// ------------------------------------------------------------------ wiring

TEST_F(Draft22LocationRange, FiltersCoverEveryExplicitTypeInOrder) {
    const auto filters = draft22_location_range_filters();
    ASSERT_EQ(filters.size(), 5u);
    EXPECT_EQ(filters[0].type, FilterType::RelativeGroup);
    EXPECT_EQ(filters[0].start_group, 1u);
    EXPECT_EQ(filters[1].type, FilterType::Absolute);
    EXPECT_EQ(filters[2].type, FilterType::AbsoluteBounded);
    EXPECT_EQ(filters[3].type, FilterType::AbsoluteRange);
    EXPECT_EQ(filters[4].type, FilterType::NextObject);
}

TEST_F(Draft22LocationRange, SubscribeProbeWritesOneDraft22FilterPerType) {
    const auto p = subscribe_probe();
    EXPECT_EQ(p.id, kDraft22SubscribeLocationRange);
    ASSERT_EQ(p.writes.size(), 5u);
    // SUBSCRIBE (0x3), Request IDs 1, 3, 5, 7, 9, track (n)/t, FORWARD=1, then LOCATION_FILTER (delta 0x11)
    // as the draft 22 Type and exactly its fields.
    EXPECT_EQ(p.writes[0].bytes, b({3, 0, 12, 1, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x01, 1}));
    EXPECT_EQ(p.writes[1].bytes, b({3, 0, 13, 3, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x02, 7, 9}));
    EXPECT_EQ(p.writes[2].bytes, b({3, 0, 14, 5, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x03, 7, 9, 0}));
    EXPECT_EQ(p.writes[3].bytes, b({3, 0, 15, 7, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x04, 7, 9, 0, 9}));
    EXPECT_EQ(p.writes[4].bytes, b({3, 0, 11, 9, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 0x05}));
    for (const auto& write : p.writes) {
        EXPECT_EQ(write.channel, RawProbeChannel::NewBidi);
        EXPECT_FALSE(write.fin);
    }
}

TEST(Draft22LocationRangeWire, ProbesAreBuiltOnTheDraft22WireOnly) {
    const ScopedWireDraft wire(21);
    EXPECT_THROW(subscribe_probe(), std::logic_error);
}

// -------------------------------------------------------------- subscribe

TEST_F(Draft22LocationRange, ObjectsInsideEveryRequestedRangePass) {
    const auto p = subscribe_probe();
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, distinct_oks(), conforming())),
              std::optional<bool>{true});
}

TEST_F(Draft22LocationRange, AnObjectOutsideEitherBoundaryOfAnyTypeFails) {
    const auto p = subscribe_probe();
    struct Case {
        const char* name;
        Delivery delivery;
    };
    // The open-ended Types (0x01, 0x02, 0x05) have only a start boundary.
    for (const auto& violation : std::vector<Case>{
             {"0x01 before the Largest Object's Group", {1, 6, {5}}},
             {"0x02 before the start", {2, 7, {8}}},
             {"0x03 before the start", {3, 7, {8}}},
             {"0x03 after the end Group", {3, 8, {0}}},
             {"0x04 before the start", {4, 7, {8}}},
             {"0x04 after the end", {4, 7, {10}}},
             {"0x05 at the Largest Object, before the Next Object", {5, 7, {9}}},
         }) {
        SCOPED_TRACE(violation.name);
        auto deliveries = conforming();
        deliveries.push_back(violation.delivery);
        EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, distinct_oks(), deliveries)),
                  std::optional<bool>{false});
        // A violation is proven before the window ends.
        EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, distinct_oks(), deliveries, false)),
                  std::optional<bool>{false});
    }
}

TEST_F(Draft22LocationRange, RelativeRangesFollowTheReportedLargestObject) {
    const auto p = subscribe_probe();
    // Largest Object {9, 3}: 0x01 (StartGroup 1) starts at {9, 0}, 0x05 at {9, 4}.
    std::vector<Bytes> oks = distinct_oks();
    oks[0] = subscribe_ok(1, std::pair{9u, 3u});
    oks[4] = subscribe_ok(5, std::pair{9u, 3u});
    const std::vector<Delivery> base{{1, 9, {0, 3}}, {4, 7, {9}}, {5, 9, {4}}};
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, oks, base)), std::optional<bool>{true});
    auto early = base;
    early.push_back({1, 8, {5}});
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, oks, early)), std::optional<bool>{false});
    auto largest = base;
    largest.push_back({5, 9, {3}});
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, oks, largest)), std::optional<bool>{false});
    // Without LARGEST_OBJECT (nothing published, Section 9.20.17) the Next Object is {0, 0}.
    oks[4] = subscribe_ok(5, std::nullopt);
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, oks, {{4, 7, {9}}, {5, 0, {0}}})),
              std::optional<bool>{true});
}

TEST_F(Draft22LocationRange, ASharedAliasIsJudgedAgainstEverySubscriptionCarryingIt) {
    const auto p = subscribe_probe();
    const std::vector<Bytes> shared{subscribe_ok(1), subscribe_ok(1), subscribe_ok(1), subscribe_ok(1),
                                    subscribe_ok(1)};
    // {7, 0} fits only 0x01, {8, 4} only the open-ended Types: each belongs to some subscription.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, shared, {{1, 7, {0, 9}}, {1, 8, {4}}})),
              std::optional<bool>{true});
    // {6, 9} fits none of them.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, shared, {{1, 7, {9}}, {1, 6, {9}}})),
              std::optional<bool>{false});
    // Objects of another alias belong to no subscription of this probe.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, shared, {{1, 7, {9}}, {9, 6, {9}}})),
              std::optional<bool>{true});
}

TEST_F(Draft22LocationRange, MissingOrInsufficientEvidenceHasNoVerdict) {
    const auto p = subscribe_probe();
    // Nothing delivered: the inclusive boundaries were never exercised.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, distinct_oks(), {})), std::nullopt);
    // The context stopped before the window ended.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, distinct_oks(), conforming(), false)),
              std::nullopt);
    // A rejected subscription leaves its Type unexercised.
    auto rejected = distinct_oks();
    rejected[3] = request_error();
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, rejected, conforming())), std::nullopt);
    // ... but a violation on another subscription still fails.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, rejected, {{2, 7, {8}}})),
              std::optional<bool>{false});
    // An unanswered subscription could own any alias: nothing is judged.
    auto unanswered = distinct_oks();
    unanswered[2].clear();
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, unanswered, {{2, 7, {8}}})), std::nullopt);
    // A Relative Start whose Largest Object is unknown cannot be judged and blocks a pass, while the
    // other Types are still judged.
    auto unknown = distinct_oks();
    unknown[0] = subscribe_ok(1, std::nullopt);
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, unknown, conforming())), std::nullopt);
    auto unknown_violation = conforming();
    unknown_violation.push_back({1, 2, {0}});
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, unknown, unknown_violation)), std::nullopt);
    unknown_violation.push_back({4, 7, {10}});
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, unknown, unknown_violation)),
              std::optional<bool>{false});
}

TEST_F(Draft22LocationRange, OnlyAProvenDraft22StimulusIsJudged) {
    const auto p = subscribe_probe();
    const auto passing = subscribed(p, distinct_oks(), conforming());
    // An incomplete context that did not time out.
    auto incomplete = passing;
    incomplete.timed_out = false;
    EXPECT_EQ(evaluate_draft22_subscription_location_range(incomplete), std::nullopt);
    auto failed = passing;
    failed.harness_failed = true;
    EXPECT_EQ(evaluate_draft22_subscription_location_range(failed), std::nullopt);
    // An altered filter is not this probe's stimulus.
    auto altered = passing;
    altered.writes[3].write.bytes.back() = std::byte{10};
    EXPECT_EQ(evaluate_draft22_subscription_location_range(altered), std::nullopt);
    // Another scenario's transcript.
    auto other = passing;
    other.scenario_id = "d22-fetch-bounded-location-range";
    EXPECT_EQ(evaluate_draft22_subscription_location_range(other), std::nullopt);
    // Evidence cut at a recording limit.
    auto limited = passing;
    limited.event_limit_reached = true;
    EXPECT_EQ(evaluate_draft22_subscription_location_range(limited), std::nullopt);
    // On another wire nothing is judged.
    const ScopedWireDraft draft21(21);
    EXPECT_EQ(evaluate_draft22_subscription_location_range(passing), std::nullopt);
}

TEST_F(Draft22LocationRange, ResponseReadyStopsEarlyOnlyOnAViolation) {
    const auto p = subscribe_probe();
    ASSERT_TRUE(p.response_ready);
    auto t = subscribed(p, distinct_oks(), conforming(), false);
    t.complete = false;
    EXPECT_FALSE(p.response_ready(t));
    data(t, 30, subgroup(4, 7, {10}), true);
    EXPECT_TRUE(p.response_ready(t));
}

}  // namespace
}  // namespace moq::interop::scenarios
