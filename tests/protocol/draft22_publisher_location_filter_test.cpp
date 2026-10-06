// Draft 22 own scenario for D22-9-20-9-MAY-422 (Section 9.20.9): each case builds the transcript a
// publisher exchange would produce and checks the evaluator's verdict.
#include "moq/interop/scenarios/draft22_publisher_location_filter.h"
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

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes result;
    for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}

// Section 9 framing: type, 16-bit length, body (single-byte types only here).
Bytes message(unsigned type, const Bytes& body) {
    return concat({b({type, static_cast<unsigned>(body.size() >> 8u), static_cast<unsigned>(body.size() & 255u)}),
                   body});
}

constexpr transport::StreamId kSubscription = 1;  // the runner's request stream
constexpr transport::StreamId kPublishStream = 0;  // the publisher's first request stream

class Draft22PublisherLocationFilter : public ::testing::Test {
protected:
    ScopedWireDraft wire{22};
};

RawProbeDefinition probe() { return draft22_publisher_location_filter_probe(1000ms, {b({'n'})}, b({'t'})); }

// The SUBSCRIBE accepted; `events` follow it; the window ends unless `window_ended` is false.
RawProbeTranscript transcript(std::vector<transport::TransportEvent> events, bool window_ended = true) {
    const auto p = probe();
    RawProbeTranscript t;
    t.scenario_id = p.id;
    t.setup = {{RawProbeChannel::NewUni, b({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
    t.events = {transport::ConnectionEstablishedEvent{{}, {}, {}, 1200},
                transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false}};
    t.transport_established = t.peer_setup_received = true;
    t.max_datagram_payload = 1200;
    t.writes.push_back({p.writes[0], kSubscription, p.writes[0].bytes.size(), false, t.events.size()});
    t.delivery_event_count = t.events.size();
    t.stimulus_delivered = true;
    for (auto& event : events) t.events.push_back(std::move(event));
    t.timed_out = window_ended;
    t.complete = !window_ended;
    return t;
}

transport::TransportEvent on(transport::StreamId stream, Bytes payload) {
    return transport::StreamDataEvent{stream, std::move(payload), false};
}

Bytes subscribe_ok() { return message(4, b({1, 0})); }
// PUBLISH_STATE_NOTIFY (0x22) carrying `parameters` (count first).
Bytes notify(const Bytes& parameters) { return message(0x22, parameters); }
// PUBLISH (0x1D) of (n)/t, Request ID 0, Track Alias 2, `parameters` (count first), no Track Properties.
Bytes publish(const Bytes& parameters) { return message(0x1d, concat({b({0, 1, 1, 'n', 1, 't', 2}), parameters})); }
// REQUEST_UPDATE (0x2) for the publisher's Request ID 2 with `parameters`.
Bytes update(const Bytes& parameters) { return message(2, concat({b({2}), parameters})); }

// LOCATION_FILTER as the only parameter: count 1, type 0x21, then the filter bytes.
Bytes only_filter(const Bytes& filter) { return concat({b({1, 0x21}), filter}); }
// LARGEST_OBJECT {7, 9}, then LOCATION_FILTER (delta 0x18).
Bytes largest_and_filter(const Bytes& filter) { return concat({b({2, 0x09, 7, 9, 0x18}), filter}); }

Bytes requested() { return b({2, 7, 0}); }  // Absolute Start {7, 0}

std::optional<bool> verdict(const RawProbeTranscript& t) { return evaluate_draft22_publisher_location_filter(t); }

// ------------------------------------------------------------------ wiring

TEST_F(Draft22PublisherLocationFilter, ProbeSubscribesWithAnAbsoluteStartFilter) {
    const auto p = probe();
    EXPECT_EQ(p.id, kDraft22PublisherLocationFilter);
    ASSERT_EQ(p.writes.size(), 1u);
    // SUBSCRIBE, Request ID 1, (n)/t, FORWARD=1, LOCATION_FILTER (delta 0x11) Type 0x02 {7, 0}.
    EXPECT_EQ(p.writes[0].bytes, b({3, 0, 13, 1, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 2, 7, 0}));
    EXPECT_EQ(p.courtesy.publish, RawProbePublishResponse::Accept);
    EXPECT_EQ(p.courtesy.update, RawProbeUpdateResponse::Accept);
}

TEST(Draft22PublisherLocationFilterWire, ProbeIsBuiltOnTheDraft22WireOnly) {
    const ScopedWireDraft wire(21);
    EXPECT_THROW(probe(), std::logic_error);
}

// -------------------------------------------------------------------- verdicts

TEST_F(Draft22PublisherLocationFilter, DecodableFiltersInEveryPublisherMessagePass) {
    // The notification reports the requested filter, alone or after LARGEST_OBJECT.
    EXPECT_EQ(verdict(transcript({on(kSubscription, subscribe_ok()), on(kSubscription, notify(only_filter(requested())))})),
              std::optional<bool>{true});
    EXPECT_EQ(verdict(transcript({on(kSubscription, subscribe_ok()),
                                  on(kSubscription, notify(largest_and_filter(requested())))})),
              std::optional<bool>{true});
    // On the publisher's own PUBLISH every defined Type is its own setting: 0x00 to 0x05 all decode.
    for (const auto& filter : {b({0}), b({1, 1}), b({2, 0, 0}), b({3, 7, 9, 2}), b({4, 7, 9, 0, 9}), b({5})}) {
        EXPECT_EQ(verdict(transcript({on(kPublishStream, publish(only_filter(filter)))})), std::optional<bool>{true});
        EXPECT_EQ(verdict(transcript({on(kPublishStream, concat({publish(b({0})), update(only_filter(filter)),
                                                                 notify(only_filter(filter))}))})),
                  std::optional<bool>{true});
    }
}

TEST_F(Draft22PublisherLocationFilter, UndecodableFiltersFail) {
    const std::vector<Bytes> invalid{
        b({6}),                                                               // undefined Type
        b({0x7f}),                                                            // undefined Type
        b({4, 7, 9, 0}),                                                      // 0x04 without EndObject
        b({3, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 1}),   // StartGroup + EndGroupDelta overflow
    };
    for (const auto& filter : invalid) {
        SCOPED_TRACE(::testing::PrintToString(filter));
        EXPECT_EQ(verdict(transcript({on(kPublishStream, publish(only_filter(filter)))})), std::optional<bool>{false});
        EXPECT_EQ(verdict(transcript({on(kPublishStream, concat({publish(b({0})), update(only_filter(filter))}))})),
                  std::optional<bool>{false});
        EXPECT_EQ(verdict(transcript({on(kSubscription, subscribe_ok()), on(kSubscription, notify(only_filter(filter)))})),
                  std::optional<bool>{false});
        // Shown before the window ends.
        EXPECT_EQ(verdict(transcript({on(kPublishStream, publish(only_filter(filter)))}, false)),
                  std::optional<bool>{false});
    }
}

TEST_F(Draft22PublisherLocationFilter, NotificationChangingTheRequestedFilterFails) {
    for (const auto& filter : {b({0}), b({2, 8, 0}), b({2, 7, 1}), b({3, 7, 0, 0}), b({4, 7, 0, 1, 2})}) {
        SCOPED_TRACE(::testing::PrintToString(filter));
        EXPECT_EQ(verdict(transcript({on(kSubscription, subscribe_ok()), on(kSubscription, notify(only_filter(filter)))})),
                  std::optional<bool>{false});
    }
    // The same change on the publisher's own PUBLISH stream is not judged against the probe's request.
    EXPECT_EQ(verdict(transcript({on(kPublishStream, concat({publish(only_filter(requested())),
                                                             notify(only_filter(b({2, 8, 0})))}))})),
              std::optional<bool>{true});
}

TEST_F(Draft22PublisherLocationFilter, InsufficientEvidenceGivesNoVerdict) {
    // Nothing sent with a LOCATION_FILTER: the permission was not exercised.
    EXPECT_EQ(verdict(transcript({on(kSubscription, subscribe_ok())})), std::nullopt);
    EXPECT_EQ(verdict(transcript({on(kSubscription, subscribe_ok()), on(kSubscription, notify(b({1, 0x09, 7, 9})))})),
              std::nullopt);
    EXPECT_EQ(verdict(transcript({on(kPublishStream, publish(b({0})))})), std::nullopt);
    // A relative filter on the probe's subscription cannot be compared without its Largest Object.
    for (const auto& filter : {b({1, 1}), b({5})})
        EXPECT_EQ(verdict(transcript({on(kSubscription, subscribe_ok()), on(kSubscription, notify(only_filter(filter)))})),
                  std::nullopt);
    // A judged message with a parameter that cannot be read (type 0x01 is not defined) hides what follows.
    EXPECT_EQ(verdict(transcript({on(kPublishStream, publish(only_filter(requested()))),
                                  on(kSubscription, subscribe_ok()), on(kSubscription, notify(b({1, 0x01, 0})))})),
              std::nullopt);
    // Before SUBSCRIBE_OK there is no subscription to report on.
    EXPECT_EQ(verdict(transcript({on(kSubscription, notify(only_filter(b({6}))))})), std::nullopt);
    // The window did not end.
    EXPECT_EQ(verdict(transcript({on(kPublishStream, publish(only_filter(requested())))}, false)), std::nullopt);
}

TEST_F(Draft22PublisherLocationFilter, MediaDoesNotExhaustTheEvidenceBound) {
    Bytes media(70000, std::byte{0});
    media[0] = std::byte{0x30};
    EXPECT_EQ(verdict(transcript({on(kPublishStream, publish(only_filter(requested()))), on(6, media)})),
              std::optional<bool>{true});
}

TEST_F(Draft22PublisherLocationFilter, UnprovenEvidenceGivesNoVerdict) {
    const auto t = transcript({on(kPublishStream, publish(only_filter(b({6}))))});
    auto other = t;
    other.scenario_id = "d22-subscribe-bounded-location-range";
    EXPECT_EQ(verdict(other), std::nullopt);
    auto altered = t;
    altered.writes[0].write.bytes.back() = std::byte{1};
    EXPECT_EQ(verdict(altered), std::nullopt);
    auto failed = t;
    failed.harness_failed = true;
    EXPECT_EQ(verdict(failed), std::nullopt);
    auto truncated = t;
    truncated.event_limit_reached = true;
    EXPECT_EQ(verdict(truncated), std::nullopt);
    const ScopedWireDraft wire21(21);
    EXPECT_EQ(verdict(t), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
