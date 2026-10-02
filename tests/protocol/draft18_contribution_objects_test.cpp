#include "moq/interop/scenarios/draft18_contribution.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;
using std::chrono::milliseconds;

constexpr milliseconds kDeadline{60};

std::vector<Draft18ContributionProbe> probes() {
    return draft18_contribution_probes(kDeadline, {text("n")}, text("t"));
}

std::optional<bool> run(const Draft18ContributionProbe& p, const std::function<void(PeerView&)>& peer) {
    const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        peer(v);
    });
    return evaluate_draft18_contribution_probe(transcript, p);
}
// Answers the SUBSCRIBE with alias 5 and then runs `after` once established.
std::optional<bool> run_subscribed(const Draft18ContributionProbe& p, const std::function<void(PeerView&)>& after) {
    return run(p, [&](PeerView& v) {
        v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); });
        v.when("after", v.sent(1) && v.step > 3, [&] { after(v); });
    });
}

Bytes zeros(std::size_t count) { return Bytes(count, std::byte{0}); }

// Section 11.5.1: every padding byte after the type is zero.
TEST(Draft18ContributionObjects, PaddingStreamBytesAreAllZero) {
    const auto all = probes();
    const auto& p = probe(all, "observe-publisher-padding-stream", "D18-11-5-1-MUST-001");
    EXPECT_EQ(p.evaluator_id, "padding-data-bytes-all-zero");
    EXPECT_FALSE(p.requires_track);
    EXPECT_TRUE(p.definition.writes.empty());
    const auto stream = [&](Bytes padding, bool fin) {
        return run(p, [&](PeerView& v) {
            v.when("padding", v.step > 1, [&] { v.data(6, concat(vi(0x132B3E28), padding), fin); });
        });
    };
    EXPECT_EQ(stream(zeros(8), true), std::optional<bool>{true});
    EXPECT_EQ(stream({}, true), std::optional<bool>{true});
    auto dirty = zeros(8);
    dirty[5] = std::byte{1};
    EXPECT_EQ(stream(dirty, true), std::optional<bool>{false});
    EXPECT_EQ(stream(dirty, false), std::optional<bool>{false});
    // Zeros without an end of stream are not yet a complete observation.
    EXPECT_EQ(stream(zeros(8), false), std::nullopt);
    EXPECT_EQ(run(p, [](PeerView&) {}), std::nullopt);
    // Other unidirectional streams are not padding.
    EXPECT_EQ(run(p, [](PeerView& v) {
        v.when("other", v.step > 1, [&] { v.data(6, concat(subgroup_header(5, 1), zeros(3)), true); });
    }), std::nullopt);
}

TEST(Draft18ContributionObjects, PaddingDatagramBytesAreAllZero) {
    const auto all = probes();
    const auto& p = probe(all, "observe-publisher-padding-datagram", "D18-11-5-2-MUST-001");
    EXPECT_EQ(p.evaluator_id, "padding-data-bytes-all-zero");
    const auto datagram = [&](Bytes padding) {
        return run(p, [&](PeerView& v) {
            v.when("padding", v.step > 1, [&] { v.push(transport::DatagramEvent{concat(vi(0x132B3E29), padding)}); });
        });
    };
    EXPECT_EQ(datagram(zeros(16)), std::optional<bool>{true});
    auto dirty = zeros(16);
    dirty[15] = std::byte{0xff};
    EXPECT_EQ(datagram(dirty), std::optional<bool>{false});
    EXPECT_EQ(run(p, [](PeerView&) {}), std::nullopt);
}

d18::KeyValuePairs gap(std::uint64_t type, std::uint64_t value) { return {even_option(type, value)}; }
d18::KeyValuePair immutable(const d18::KeyValuePairs& nested) { return odd_option(0x0B, kvp(nested)); }

// Sections 12.8 and 12.9: at most one gap Property per Object, counting both
// the mutable list and Immutable Properties.
TEST(Draft18ContributionObjects, ObjectCarriesAtMostOneGapProperty) {
    const auto all = probes();
    struct Row { const char* scenario; const char* requirement; const char* evaluator; std::uint64_t type; };
    for (const auto& row : {
             Row{"publish-object-with-prior-group-id-gap", "D18-12-8-MUST-NOT-004",
                 "object-has-at-most-one-prior-group-id-gap", 0x3c},
             Row{"publish-object-with-prior-object-id-gap", "D18-12-9-MUST-NOT-004",
                 "object-has-at-most-one-prior-object-id-gap", 0x3e}}) {
        SCOPED_TRACE(row.requirement);
        const auto& p = probe(all, row.scenario, row.requirement);
        EXPECT_EQ(p.evaluator_id, row.evaluator);
        EXPECT_TRUE(p.requires_track);
        const auto other = row.type == 0x3c ? 0x3eu : 0x3cu;
        const auto object = [&](d18::KeyValuePairs properties, std::uint64_t group = 2) {
            return run_subscribed(p, [&](PeerView& v) {
                v.data(6, concat(subgroup_header(5, group, 0x11),
                                 subgroup_object(0, text("p"), std::nullopt, kvp(properties))));
            });
        };
        EXPECT_EQ(object(gap(row.type, 2)), std::optional<bool>{true});
        EXPECT_EQ(object({even_option(row.type, 1), even_option(row.type, 2)}), std::optional<bool>{false});
        EXPECT_EQ(object({immutable(gap(row.type, 2)), even_option(row.type, 1)}), std::optional<bool>{false});
        EXPECT_EQ(object({immutable({even_option(row.type, 1), even_option(row.type, 2)})}), std::optional<bool>{false});
        // One gap in the mutable list and a different gap kind elsewhere is fine.
        EXPECT_EQ(object({immutable(gap(other, 2)), even_option(row.type, 1)}), std::optional<bool>{true});
        // An Object without the Property exercises nothing.
        EXPECT_EQ(object(gap(other, 2)), std::nullopt);
        EXPECT_EQ(object({even_option(0x22, 1)}), std::nullopt);
        EXPECT_EQ(run_subscribed(p, [](PeerView&) {}), std::nullopt);
    }
}

// Section 11.2.1.1: Objects with a nonzero status have an empty payload.
TEST(Draft18ContributionObjects, StatusObjectsHaveNoPayload) {
    const auto all = probes();
    const auto& p = probe(all, "publish-end-of-group-and-end-of-track-status-objects", "D18-11-2-1-1-MUST-001");
    EXPECT_EQ(p.evaluator_id, "non-normal-object-status-has-empty-payload");
    for (const std::uint64_t status : {3u, 4u}) {
        EXPECT_EQ(run_subscribed(p, [&](PeerView& v) {
            v.data(6, concat(subgroup_header(5, 2), subgroup_object(0, {}, status)));
        }), std::optional<bool>{true}) << status;
    }
    // STATUS datagram: type 0x20, alias, group, object id, priority, status.
    EXPECT_EQ(run_subscribed(p, [&](PeerView& v) {
        auto datagram = concat(vi(0x20), vi(5));
        datagram = concat(datagram, vi(2));
        datagram = concat(datagram, vi(7));
        datagram.push_back(std::byte{128});
        datagram = concat(datagram, vi(4));
        v.push(transport::DatagramEvent{datagram});
    }), std::optional<bool>{true});
    // Zero-length Normal objects and ordinary payloads are not status objects.
    EXPECT_EQ(run_subscribed(p, [&](PeerView& v) {
        v.data(6, concat(subgroup_header(5, 2), subgroup_object(0, {}, 0)));
    }), std::nullopt);
    EXPECT_EQ(run_subscribed(p, [&](PeerView& v) {
        v.data(6, concat(subgroup_header(5, 2), subgroup_object(0, text("p"))));
    }), std::nullopt);
    // Another Track Alias is not this subscription.
    EXPECT_EQ(run_subscribed(p, [&](PeerView& v) {
        v.data(6, concat(subgroup_header(9, 2), subgroup_object(0, {}, 3)));
    }), std::nullopt);
}

// Section 11.4.3: a subgroup whose final Object was delivered is closed with FIN.
TEST(Draft18ContributionObjects, CompleteSubgroupIsClosedWithFin) {
    const auto all = probes();
    const auto& p = probe(all, "publish-complete-finite-subgroup-with-start-location-filter", "D18-11-4-3-MUST-001");
    EXPECT_EQ(p.evaluator_id, "complete-subgroup-ends-with-fin");
    const auto message = decode_request(p.definition.writes.front().bytes);
    const auto* subscribe = std::get_if<d18::SubscribeMessage>(&*message);
    ASSERT_NE(subscribe, nullptr);
    ASSERT_EQ(subscribe->parameters.size(), 2u);
    EXPECT_EQ(subscribe->parameters[1].type, 0x21u);
    EXPECT_EQ(std::get<d18::SubscriptionFilter>(subscribe->parameters[1].value).type,
              d18::SubscriptionFilterType::LargestObject);
    const auto closed = [&](Bytes objects, bool fin, bool reset) {
        return run_subscribed(p, [&](PeerView& v) {
            v.data(6, concat(subgroup_header(5, 2), objects), fin);
            if (reset) v.push(transport::PeerResetEvent{6, 0x1});
        });
    };
    const auto complete = concat(subgroup_object(3, text("a")), subgroup_object(0, {}, 3));
    EXPECT_EQ(closed(complete, true, false), std::optional<bool>{true});
    // FIN after the delivered End of Group, then a late reset, still counts as FIN.
    EXPECT_EQ(closed(complete, true, true), std::optional<bool>{true});
    EXPECT_EQ(closed(complete, false, true), std::optional<bool>{false});
    // Without a final Object there is no proof that delivery was complete.
    EXPECT_EQ(closed(subgroup_object(3, text("a")), true, false), std::nullopt);
    EXPECT_EQ(closed(subgroup_object(3, text("a")), false, true), std::nullopt);
    EXPECT_EQ(closed(complete, false, false), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
