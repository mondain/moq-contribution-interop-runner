// Draft 22 own scenarios for D22-3-3-1-MUST-NOT-069 (Section 3.3.1): each case builds the transcript a
// publisher exchange would produce and checks the evaluator's verdict.
#include "moq/interop/scenarios/draft22_location_range.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <gtest/gtest.h>

#include <algorithm>
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

// One Track Alias for all five subscriptions (Section 3.1 allows it). With Largest Object {7, 9} the ranges
// are 0x01 {7, 0}.., 0x02 {7, 9}.., 0x03 {7, 9}..{7, max}, 0x04 {7, 9}..{7, 9}, 0x05 {7, 10}..
std::vector<Bytes> shared_oks() {
    return {subscribe_ok(1), subscribe_ok(1), subscribe_ok(1), subscribe_ok(1), subscribe_ok(1)};
}

TEST_F(Draft22LocationRange, ASharedAliasFailsOnlyAnObjectThatFitsNoSubscriptionCarryingIt) {
    const auto p = subscribe_probe();
    // {6, 9} fits none of the ranges.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, shared_oks(), {{1, 7, {9}}, {1, 6, {9}}})),
              std::optional<bool>{false});
}

TEST_F(Draft22LocationRange, ASharedAliasWithDifferentRangesNeverPasses) {
    const auto p = subscribe_probe();
    // A filter-ignoring publisher sends {7, 10} and {8, 4} to the 0x03 and 0x04 subscriptions too (past their
    // ends). On one alias every copy also fits an open-ended range, so no copy can be shown to be outside: the
    // scenario settles without a verdict instead of passing.
    std::vector<Delivery> ignoring;
    for (int copy = 0; copy < 5; ++copy) {
        ignoring.push_back({1, 7, {9, 10}});
        ignoring.push_back({1, 8, {4}});
    }
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, shared_oks(), ignoring)), std::nullopt);
    // A conforming publisher on one alias sends each Object once per matching subscription: {7, 0} to 0x01
    // only, {7, 9} to 0x01..0x04, {8, 4} to 0x01, 0x02 and 0x05. It is not failed, and (the evidence cannot
    // tell it from the publisher above) not passed either.
    std::vector<Delivery> conforming_shared{{1, 7, {0}}};
    for (int copy = 0; copy < 4; ++copy) conforming_shared.push_back({1, 7, {9}});
    for (int copy = 0; copy < 3; ++copy) conforming_shared.push_back({1, 8, {4}});
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, shared_oks(), conforming_shared)),
              std::nullopt);
    // Two aliases, each carrying subscriptions with different ranges: still no pass.
    const std::vector<Bytes> pairs{subscribe_ok(1), subscribe_ok(2), subscribe_ok(3), subscribe_ok(3), subscribe_ok(1)};
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, pairs, conforming())), std::nullopt);
}

TEST_F(Draft22LocationRange, AnAliasAnnouncedInAPublishIsNotJudgedAgainstTheseRanges) {
    const auto p = subscribe_probe();
    // PUBLISH (0x1d) on the publisher's request stream 0: Request ID 0, track (n)/t, Track Alias 4, no
    // parameters. Its subscription shares alias 4 with the 0x04 subscription.
    const auto publish = b({0x1d, 0, 8, 0, 1, 1, 'n', 1, 't', 4, 0});
    auto deliveries = conforming();
    deliveries.push_back({4, 7, {12}});
    auto t = subscribed(p, distinct_oks(), deliveries);
    t.events.insert(t.events.begin() + 2, transport::StreamDataEvent{0, publish, false});
    for (auto& write : t.writes) ++*write.delivery_event_count;
    ++*t.delivery_event_count;
    EXPECT_EQ(evaluate_draft22_subscription_location_range(t), std::nullopt);
    // Without the PUBLISH the same Object fails the 0x04 subscription.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(subscribed(p, distinct_oks(), deliveries)),
              std::optional<bool>{false});
    // Other aliases are still judged.
    deliveries.push_back({2, 7, {8}});
    auto other = subscribed(p, distinct_oks(), deliveries);
    other.events.insert(other.events.begin() + 2, transport::StreamDataEvent{0, publish, false});
    for (auto& write : other.writes) ++*write.delivery_event_count;
    ++*other.delivery_event_count;
    EXPECT_EQ(evaluate_draft22_subscription_location_range(other), std::optional<bool>{false});
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

// ------------------------------------------------------------------ update

RawProbeDefinition update_probe() {
    return draft22_update_location_range_probe(std::chrono::milliseconds(1000), {b({'n'})}, b({'t'}));
}

// REQUEST_OK (REQUEST_UPDATE_OK, Section 9.3) with LARGEST_OBJECT when given.
Bytes request_ok(std::optional<std::pair<unsigned, unsigned>> largest = std::pair{7u, 9u}) {
    if (!largest) return b({7, 0, 1, 0});
    return b({7, 0, 4, 1, 9, largest->first, largest->second});
}

// Five SUBSCRIBEs answered with `oks`, then the five updates answered with `updates` (empty: no answer),
// then the subgroup streams.
RawProbeTranscript updated(const RawProbeDefinition& p, const std::vector<Bytes>& oks,
                           const std::vector<Bytes>& updates, const std::vector<Delivery>& deliveries,
                           bool window_ended = true) {
    auto t = start(p);
    for (std::size_t index = 0; index < 5; ++index) accept(t, p, index, request_stream(index));
    for (std::size_t index = 0; index < 5; ++index) data(t, request_stream(index), oks[index]);
    for (std::size_t index = 0; index < 5; ++index) accept(t, p, 5 + index, request_stream(index));
    for (std::size_t index = 0; index < updates.size(); ++index)
        if (!updates[index].empty()) data(t, request_stream(index), updates[index]);
    transport::StreamId stream = 6;
    for (const auto& delivery : deliveries) {
        data(t, stream, subgroup(delivery.alias, delivery.group, delivery.objects), true);
        stream += 4;
    }
    t.timed_out = window_ended;
    t.complete = !window_ended;
    return t;
}

std::vector<Bytes> acknowledged() { return {request_ok(), request_ok(), request_ok(), request_ok(), request_ok()}; }

TEST_F(Draft22LocationRange, UpdateProbeSetsEachTypeByRequestUpdateOnItsSubscription) {
    const auto p = update_probe();
    EXPECT_EQ(p.id, kDraft22UpdateLocationRange);
    ASSERT_EQ(p.writes.size(), 10u);
    // SUBSCRIBEs with FORWARD=0 and no filter, so nothing is sent before the update.
    for (std::size_t index = 0; index < 5; ++index) {
        EXPECT_EQ(p.writes[index].bytes, b({3, 0, 9, static_cast<unsigned>(1 + 2 * index), 1, 1, 'n', 1, 't', 1,
                                            0x10, 0}));
        EXPECT_FALSE(p.writes[index].reuse_write_stream.has_value());
    }
    // REQUEST_UPDATE (0x2), Request IDs 11..19, FORWARD=1 and the draft 22 LOCATION_FILTER.
    EXPECT_EQ(p.writes[5].bytes, b({2, 0, 7, 11, 2, 0x10, 1, 0x11, 0x01, 1}));
    EXPECT_EQ(p.writes[6].bytes, b({2, 0, 8, 13, 2, 0x10, 1, 0x11, 0x02, 7, 9}));
    EXPECT_EQ(p.writes[7].bytes, b({2, 0, 9, 15, 2, 0x10, 1, 0x11, 0x03, 7, 9, 0}));
    EXPECT_EQ(p.writes[8].bytes, b({2, 0, 10, 17, 2, 0x10, 1, 0x11, 0x04, 7, 9, 0, 9}));
    EXPECT_EQ(p.writes[9].bytes, b({2, 0, 6, 19, 2, 0x10, 1, 0x11, 0x05}));
    for (std::size_t index = 0; index < 5; ++index) {
        const auto& update = p.writes[5 + index];
        EXPECT_EQ(update.reuse_write_stream, std::optional<std::size_t>{index});
        ASSERT_TRUE(update.peer_response_ready);
        EXPECT_TRUE(update.peer_response_ready(subscribe_ok(1)));
        EXPECT_FALSE(update.peer_response_ready(request_error()));
    }
    const ScopedWireDraft draft21(21);
    EXPECT_THROW(update_probe(), std::logic_error);
}

TEST_F(Draft22LocationRange, UpdatedObjectsInsideEveryRange) {
    const auto p = update_probe();
    EXPECT_EQ(evaluate_draft22_subscription_location_range(updated(p, distinct_oks(), acknowledged(), conforming())),
              std::optional<bool>{true});
}

TEST_F(Draft22LocationRange, UpdatedObjectOutsideEitherBoundaryOfAnyTypeFails) {
    const auto p = update_probe();
    for (const auto& violation : std::vector<Delivery>{
             {1, 6, {5}}, {2, 7, {8}}, {3, 7, {8}}, {3, 8, {0}}, {4, 7, {8}}, {4, 7, {10}}, {5, 7, {9}}}) {
        SCOPED_TRACE(::testing::Message() << "alias " << violation.alias << " group " << violation.group);
        auto deliveries = conforming();
        deliveries.push_back(violation);
        EXPECT_EQ(evaluate_draft22_subscription_location_range(updated(p, distinct_oks(), acknowledged(), deliveries)),
                  std::optional<bool>{false});
    }
}

TEST_F(Draft22LocationRange, UpdatedRelativeRangesFollowTheUpdatesLargestObject) {
    const auto p = update_probe();
    // SUBSCRIBE_OK reported {7, 9}; by the update the Largest Object is {9, 3}, which the REQUEST_OK carries.
    auto updates = acknowledged();
    updates[0] = request_ok(std::pair{9u, 3u});
    updates[4] = request_ok(std::pair{9u, 3u});
    EXPECT_EQ(evaluate_draft22_subscription_location_range(
                  updated(p, distinct_oks(), updates, {{1, 9, {0}}, {4, 7, {9}}, {5, 9, {4}}})),
              std::optional<bool>{true});
    EXPECT_EQ(evaluate_draft22_subscription_location_range(
                  updated(p, distinct_oks(), updates, {{1, 7, {9}}, {4, 7, {9}}})),
              std::optional<bool>{false});
    EXPECT_EQ(evaluate_draft22_subscription_location_range(
                  updated(p, distinct_oks(), updates, {{4, 7, {9}}, {5, 9, {3}}})),
              std::optional<bool>{false});
}

TEST_F(Draft22LocationRange, AnUpdateMustBeAcknowledgedBeforeItsObjectsAreJudged) {
    const auto p = update_probe();
    // A refused update never applied: its Type is unexercised.
    auto refused = acknowledged();
    refused[3] = request_error();
    EXPECT_EQ(evaluate_draft22_subscription_location_range(updated(p, distinct_oks(), refused, conforming())),
              std::nullopt);
    // An update still unanswered at the window's end: its alias is not judged, the others are.
    auto unanswered = acknowledged();
    unanswered[3].clear();
    EXPECT_EQ(evaluate_draft22_subscription_location_range(updated(p, distinct_oks(), unanswered, conforming())),
              std::nullopt);
    EXPECT_EQ(evaluate_draft22_subscription_location_range(
                  updated(p, distinct_oks(), unanswered, {{4, 7, {10}}, {2, 7, {9}}})),
              std::nullopt);
    EXPECT_EQ(evaluate_draft22_subscription_location_range(
                  updated(p, distinct_oks(), unanswered, {{4, 7, {10}}, {2, 7, {8}}})),
              std::optional<bool>{false});
    // Nothing delivered under the updated filters: no verdict.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(updated(p, distinct_oks(), acknowledged(), {})),
              std::nullopt);
    // The stimulus is the update probe's own: a subscribe transcript is not mistaken for it.
    auto other = updated(p, distinct_oks(), acknowledged(), conforming());
    other.scenario_id = std::string(kDraft22SubscribeLocationRange);
    EXPECT_EQ(evaluate_draft22_subscription_location_range(other), std::nullopt);
}

// ------------------------------------------------------------------- FETCH

RawProbeDefinition fetch_probe() {
    return draft22_fetch_location_range_probe(std::chrono::milliseconds(1000), {b({'n'})}, b({'t'}));
}

// FETCH_OK (Section 9.12): End Of Track 0, End Location, no parameters, no Track Properties.
Bytes fetch_ok(unsigned group, unsigned object) { return b({0x18, 0, 4, 0, group, object, 0}); }

struct At {
    unsigned group;
    unsigned object;
};

// FETCH data stream (Section 11.4.1): FETCH_HEADER, then Objects in Ascending order. The first carries
// absolute Group and Object IDs and its priority (flags 0x1c); later ones use deltas.
Bytes fetch_stream(unsigned request_id, const std::vector<At>& objects) {
    Bytes result = b({5, request_id});
    std::optional<At> prior;
    for (const auto& at : objects) {
        if (!prior) {
            const auto first = b({0x1c, at.group, at.object, 99});
            result.insert(result.end(), first.begin(), first.end());
        } else if (at.group == prior->group) {
            const auto next = b({0x04, at.object - prior->object});
            result.insert(result.end(), next.begin(), next.end());
        } else {
            const auto next = b({0x0c, at.group - prior->group - 1, at.object});
            result.insert(result.end(), next.begin(), next.end());
        }
        result.push_back(std::byte{1});
        result.push_back(std::byte{42});
        prior = at;
    }
    return result;
}

// The five FETCHes accepted, answered with `answers` (empty: no answer), then one data stream per entry
// of `streams` (Request ID, Objects, FIN).
struct FetchData {
    unsigned request_id;
    std::vector<At> objects;
    bool fin{true};
};
RawProbeTranscript fetched(const RawProbeDefinition& p, const std::vector<Bytes>& answers,
                           const std::vector<FetchData>& streams, bool window_ended = false) {
    auto t = start(p);
    for (std::size_t index = 0; index < p.writes.size(); ++index) accept(t, p, index, request_stream(index));
    for (std::size_t index = 0; index < answers.size(); ++index)
        if (!answers[index].empty()) data(t, request_stream(index), answers[index], true);
    transport::StreamId stream = 6;
    for (const auto& entry : streams) {
        data(t, stream, fetch_stream(entry.request_id, entry.objects), entry.fin);
        stream += 4;
    }
    t.timed_out = window_ended;
    t.complete = !window_ended;
    return t;
}

// Largest Object {7, 9}: 0x01 and 0x02 end there (FETCH_OK End Location {7, 9}), 0x03 and 0x04 report
// their own end, 0x05 is refused (INVALID_RANGE).
std::vector<Bytes> fetch_answers() {
    return {fetch_ok(7, 9), fetch_ok(7, 9), fetch_ok(7, 9), fetch_ok(7, 9), request_error()};
}

// 0x01 {7, 0}..{7, 9}, 0x02 {7, 9}..{7, 9}, 0x03 {7, 9}..end of Group 7, 0x04 {7, 9}: boundaries delivered.
std::vector<FetchData> fetch_conforming() {
    return {{1, {{7, 0}, {7, 9}}}, {3, {{7, 9}}}, {5, {{7, 9}}}, {7, {{7, 9}}}};
}

TEST_F(Draft22LocationRange, FetchProbeCarriesEachTypeAsItsRange) {
    const auto p = fetch_probe();
    EXPECT_EQ(p.id, kDraft22FetchLocationRange);
    ASSERT_EQ(p.writes.size(), 5u);
    // FETCH (0x16), Request IDs 1..9, track (n)/t, one parameter: LOCATION_FILTER (0x21) as Type + fields.
    EXPECT_EQ(p.writes[0].bytes, b({0x16, 0, 10, 1, 1, 1, 'n', 1, 't', 1, 0x21, 0x01, 1}));
    EXPECT_EQ(p.writes[1].bytes, b({0x16, 0, 11, 3, 1, 1, 'n', 1, 't', 1, 0x21, 0x02, 7, 9}));
    EXPECT_EQ(p.writes[2].bytes, b({0x16, 0, 12, 5, 1, 1, 'n', 1, 't', 1, 0x21, 0x03, 7, 9, 0}));
    EXPECT_EQ(p.writes[3].bytes, b({0x16, 0, 13, 7, 1, 1, 'n', 1, 't', 1, 0x21, 0x04, 7, 9, 0, 9}));
    EXPECT_EQ(p.writes[4].bytes, b({0x16, 0, 9, 9, 1, 1, 'n', 1, 't', 1, 0x21, 0x05}));
    for (const auto& write : p.writes) {
        EXPECT_EQ(write.channel, RawProbeChannel::NewBidi);
        EXPECT_TRUE(write.fin);
    }
    const ScopedWireDraft draft21(21);
    EXPECT_THROW(fetch_probe(), std::logic_error);
}

TEST_F(Draft22LocationRange, FetchedObjectsInsideEveryRequestedRangePass) {
    const auto p = fetch_probe();
    // Every FETCH settled: no need to wait for the window.
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, fetch_answers(), fetch_conforming())),
              std::optional<bool>{true});
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, fetch_answers(), fetch_conforming(), true)),
              std::optional<bool>{true});
    // A FETCH_OK for 0x05 is not this row's concern as long as nothing is delivered for it.
    auto accepted = fetch_answers();
    accepted[4] = fetch_ok(7, 9);
    auto streams = fetch_conforming();
    streams.push_back({9, {}});
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, accepted, streams)), std::optional<bool>{true});
    // The subscription evaluator says nothing about a FETCH transcript, and the reverse.
    EXPECT_EQ(evaluate_draft22_subscription_location_range(fetched(p, fetch_answers(), fetch_conforming())),
              std::nullopt);
    EXPECT_EQ(evaluate_draft22_fetch_location_range(subscribed(subscribe_probe(), distinct_oks(), conforming())),
              std::nullopt);
}

TEST_F(Draft22LocationRange, AFetchedObjectOutsideEitherBoundaryOfAnyTypeFails) {
    const auto p = fetch_probe();
    struct Case {
        const char* name;
        std::size_t index;
        At extra;
    };
    for (const auto& violation : std::vector<Case>{
             {"0x01 before the Largest Object's Group", 0, {6, 5}},
             {"0x01 past the Largest Object", 0, {7, 10}},
             {"0x02 before the start", 1, {7, 8}},
             {"0x02 past the Largest Object", 1, {7, 10}},
             {"0x03 before the start", 2, {7, 8}},
             {"0x03 after the end Group", 2, {8, 0}},
             {"0x04 before the start", 3, {7, 8}},
             {"0x04 after the end", 3, {7, 10}},
         }) {
        SCOPED_TRACE(violation.name);
        auto streams = fetch_conforming();
        auto& objects = streams[violation.index].objects;
        objects.push_back(violation.extra);
        std::sort(objects.begin(), objects.end(), [](const At& left, const At& right) {
            return left.group != right.group ? left.group < right.group : left.object < right.object;
        });
        EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, fetch_answers(), streams)),
                  std::optional<bool>{false});
    }
    // Next Object selects nothing: any Object delivered for it is outside, answered or not.
    auto accepted = fetch_answers();
    accepted[4] = fetch_ok(7, 9);
    auto streams = fetch_conforming();
    streams.push_back({9, {{7, 9}}});
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, accepted, streams)), std::optional<bool>{false});
    auto unanswered = fetch_answers();
    unanswered[4].clear();
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, unanswered, streams, true)),
              std::optional<bool>{false});
}

TEST_F(Draft22LocationRange, FetchRangesEndingAtTheLargestObjectFollowTheFetchOk) {
    const auto p = fetch_probe();
    // Largest Object {9, 3}: 0x01 is {9, 0}..{9, 3}, 0x02 {7, 9}..{9, 3}.
    auto answers = fetch_answers();
    answers[0] = fetch_ok(9, 3);
    answers[1] = fetch_ok(9, 3);
    auto streams = fetch_conforming();
    streams[0].objects = {{9, 0}, {9, 3}};
    streams[1].objects = {{7, 9}, {8, 0}, {9, 3}};
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, answers, streams)), std::optional<bool>{true});
    streams[0].objects = {{8, 7}, {9, 0}};
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, answers, streams)), std::optional<bool>{false});
}

TEST_F(Draft22LocationRange, MissingOrInsufficientFetchEvidenceHasNoVerdict) {
    const auto p = fetch_probe();
    // Nothing delivered at all.
    EXPECT_EQ(evaluate_draft22_fetch_location_range(
                  fetched(p, fetch_answers(), {{1, {}}, {3, {}}, {5, {}}, {7, {}}})),
              std::nullopt);
    // A refused bounded FETCH leaves its Type unexercised.
    auto refused = fetch_answers();
    refused[3] = request_error();
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, refused, fetch_conforming())), std::nullopt);
    // An unanswered FETCH: its relative range is unknown, so its Objects are not judged...
    auto unanswered = fetch_answers();
    unanswered[0].clear();
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, unanswered, fetch_conforming(), true)), std::nullopt);
    auto early = fetch_conforming();
    early[0].objects = {{2, 0}};
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, unanswered, early, true)), std::nullopt);
    // ... but an Absolute start is known from the request alone.
    unanswered[1].clear();
    early[1].objects = {{7, 8}};
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, unanswered, early, true)),
              std::optional<bool>{false});
    // A data stream that has not ended before the context stopped.
    auto open = fetch_conforming();
    open[2].fin = false;
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, fetch_answers(), open)), std::nullopt);
    // ... is judged once the window ended.
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, fetch_answers(), open, true)),
              std::optional<bool>{true});
    // Altered stimulus, failed harness, other wire.
    auto altered = fetched(p, fetch_answers(), fetch_conforming());
    altered.writes[2].write.bytes.back() = std::byte{1};
    EXPECT_EQ(evaluate_draft22_fetch_location_range(altered), std::nullopt);
    auto failed = fetched(p, fetch_answers(), fetch_conforming());
    failed.harness_failed = true;
    EXPECT_EQ(evaluate_draft22_fetch_location_range(failed), std::nullopt);
    const ScopedWireDraft draft21(21);
    EXPECT_EQ(evaluate_draft22_fetch_location_range(fetched(p, fetch_answers(), fetch_conforming())), std::nullopt);
}

TEST_F(Draft22LocationRange, FetchResponseReadyStopsOnceEveryFetchSettledOrOnAViolation) {
    const auto p = fetch_probe();
    ASSERT_TRUE(p.response_ready);
    auto open = fetch_conforming();
    open[0].fin = false;
    EXPECT_FALSE(p.response_ready(fetched(p, fetch_answers(), open)));
    EXPECT_TRUE(p.response_ready(fetched(p, fetch_answers(), fetch_conforming())));
    open[1].objects = {{7, 8}};
    EXPECT_TRUE(p.response_ready(fetched(p, fetch_answers(), open)));
}

TEST_F(Draft22LocationRange, UpdatedSubscriptionsSharingAnAliasNeverPass) {
    const auto u = update_probe();
    std::vector<Delivery> ignoring;
    for (int copy = 0; copy < 5; ++copy) ignoring.push_back({1, 7, {9, 10}});
    EXPECT_EQ(evaluate_draft22_subscription_location_range(updated(u, shared_oks(), acknowledged(), ignoring)),
              std::nullopt);
    EXPECT_EQ(evaluate_draft22_subscription_location_range(
                  updated(u, shared_oks(), acknowledged(), {{1, 7, {9}}, {1, 6, {9}}})),
              std::optional<bool>{false});
}

}  // namespace
}  // namespace moq::interop::scenarios
