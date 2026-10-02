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

Bytes object_datagram(std::uint64_t alias, std::uint64_t group, std::uint64_t object) {
    auto bytes = concat(vi(0x00), vi(alias));
    bytes = concat(bytes, vi(group));
    bytes = concat(bytes, vi(object));
    bytes.push_back(std::byte{128});
    return concat(bytes, text("hi"));
}

// Section 11.4.4.1: fetched Objects previously sent as datagrams set bit 0x40.
TEST(Draft18ContributionExchange, FetchedDatagramObjectSetsTheDatagramBit) {
    const auto all = probes();
    const auto& p = probe(all, "fetch-object-previously-observed-as-datagram", "D18-11-4-4-1-MUST-007");
    EXPECT_EQ(p.evaluator_id, "fetch-datagram-object-sets-bit-0x40");
    ASSERT_EQ(p.definition.writes.size(), 2u);
    ASSERT_TRUE(p.definition.writes[1].prepare_bytes);
    const auto fetch_stream = [&](std::uint64_t flags, std::uint64_t group, std::uint64_t object) {
        auto bytes = concat(vi(5), vi(3));
        bytes = concat(bytes, vi(flags));
        bytes = concat(bytes, vi(group));
        if ((flags & 0x40u) == 0u && (flags & 3u) == 3u) bytes = concat(bytes, vi(0));
        bytes = concat(bytes, vi(object));
        // The first Object carries every field itself, including its priority.
        bytes.push_back(std::byte{128});
        bytes = concat(bytes, vi(1));
        return concat(bytes, text("p"));
    };
    Bytes request;
    const auto run = [&](bool datagram, const Bytes& stream_bytes) {
        const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup_with({})); });
            v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); });
            v.when("datagram", datagram && v.sent(1) && v.step > 3,
                   [&] { v.push(transport::DatagramEvent{object_datagram(5, 3, 4)}); });
            v.when("fetch", v.sent(5), [&] {
                request = v.transport.output[5];
                v.data(5, encode(d18::FetchOkMessage{0, {3, 5}, {}, {}}));
                v.data(10, stream_bytes, true);
            });
        });
        return evaluate_draft18_contribution_probe(transcript, p);
    };
    EXPECT_EQ(run(true, fetch_stream(0x5C, 3, 4)), std::optional<bool>{true});
    // Subgroup ID bits are ignored when 0x40 is set.
    EXPECT_EQ(run(true, fetch_stream(0x5F, 3, 4)), std::optional<bool>{true});
    EXPECT_EQ(run(true, fetch_stream(0x1F, 3, 4)), std::optional<bool>{false});
    // The first fetched Object must be the datagram Object itself.
    EXPECT_EQ(run(true, fetch_stream(0x5C, 3, 5)), std::nullopt);
    // Without an observed datagram there is no Object to fetch.
    request.clear();
    EXPECT_EQ(run(false, fetch_stream(0x5C, 3, 4)), std::nullopt);
    EXPECT_TRUE(request.empty());
    run(true, fetch_stream(0x5C, 3, 4));
    const auto message = decode_request(request);
    ASSERT_TRUE(message);
    const auto* fetch = std::get_if<d18::FetchMessage>(&*message);
    ASSERT_NE(fetch, nullptr);
    EXPECT_EQ(fetch->request_id, 3u);
    const auto& standalone = std::get<d18::StandaloneFetch>(fetch->fetch);
    EXPECT_EQ(standalone.start, (d18::Location{3, 4}));
    EXPECT_EQ(standalone.end, (d18::Location{3, 5}));
    EXPECT_EQ(standalone.track_name.bytes, text("t"));
}

// Section 11.1: a Track Alias never names two different Tracks at once.
TEST(Draft18ContributionExchange, DistinctTracksNeedDistinctAliases) {
    const auto all = probes();
    const auto& p = probe(all, "publish-two-simultaneous-tracks", "D18-11-1-MUST-NOT-001");
    EXPECT_EQ(p.evaluator_id, "no-simultaneous-track-alias-reuse-for-distinct-tracks");
    EXPECT_TRUE(p.definition.peer_request_ready(encode(d18::PublishMessage{
        0, d18::TrackNamespace{{text("n")}}, d18::TrackName{text("u")}, 9, {}, {}})));
    const auto run = [&](std::uint64_t subscribe_alias, std::uint64_t publish_alias, std::string_view publish_name,
                         bool done_on_publish, bool done_on_subscribe) {
        const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup_with({})); });
            v.when("publish", v.step > 1, [&] {
                auto bytes = encode(d18::PublishMessage{0, d18::TrackNamespace{{text("n")}},
                                                        d18::TrackName{text(publish_name)}, publish_alias, {}, {}});
                if (done_on_publish) bytes = concat(bytes, encode(d18::PublishDoneMessage{0x3, 0, d18::ReasonPhrase{}}));
                v.data(0, bytes);
            });
            v.when("ok", v.sent(1), [&] {
                auto bytes = subscribe_ok(subscribe_alias);
                if (done_on_subscribe) bytes = concat(bytes, encode(d18::PublishDoneMessage{0x3, 0, d18::ReasonPhrase{}}));
                v.data(1, bytes);
            });
        });
        EXPECT_EQ(transcript.writes.size(), 2u);
        const auto reply = decode_request(transcript.writes[1].write.bytes);
        EXPECT_TRUE(reply && std::holds_alternative<d18::RequestOkMessage>(*reply));
        return evaluate_draft18_contribution_probe(transcript, p);
    };
    EXPECT_EQ(run(5, 9, "u", false, false), std::optional<bool>{true});
    EXPECT_EQ(run(5, 5, "u", false, false), std::optional<bool>{false});
    // The same Track under one alias is not two tracks.
    EXPECT_EQ(run(5, 5, "t", false, false), std::nullopt);
    // A retired subscription frees its alias.
    EXPECT_EQ(run(5, 5, "u", true, false), std::nullopt);
    EXPECT_EQ(run(5, 5, "u", false, true), std::nullopt);
}

// Section 11.2.1: an Object keeps its Forwarding Preference on later delivery.
TEST(Draft18ContributionExchange, RedeliveredObjectKeepsItsForwardingPreference) {
    const auto all = probes();
    const auto& p = probe(all, "redeliver-previously-observed-object-in-later-subscription", "D18-11-2-1-MUST-001");
    EXPECT_EQ(p.evaluator_id, "subscription-delivery-preserves-observed-object-forwarding-preference");
    ASSERT_EQ(p.definition.writes.size(), 4u);
    const auto run = [&](bool first_datagram, std::optional<bool> second_datagram, bool end_first,
                         Bytes* second_request = nullptr) {
        const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup_with({})); });
            v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); });
            v.when("first", v.sent(1) && v.step > 3, [&] {
                if (first_datagram) v.push(transport::DatagramEvent{object_datagram(5, 2, 0)});
                else v.data(6, concat(subgroup_header(5, 2), subgroup_object(0, text("hi"))));
            });
            v.when("end", end_first && !v.transport.stops.empty(), [&] { v.data(1, {}, true); });
            v.when("second-ok", v.sent(5), [&] {
                if (second_request) *second_request = v.transport.output[5];
                v.data(5, subscribe_ok(7));
                if (second_datagram) {
                    if (*second_datagram) v.push(transport::DatagramEvent{object_datagram(7, 2, 0)});
                    else v.data(10, concat(subgroup_header(7, 2), subgroup_object(0, text("hi"))));
                }
            });
        });
        return evaluate_draft18_contribution_probe(transcript, p);
    };
    Bytes second;
    EXPECT_EQ(run(true, true, true, &second), std::optional<bool>{true});
    EXPECT_EQ(run(false, false, true), std::optional<bool>{true});
    EXPECT_EQ(run(true, false, true), std::optional<bool>{false});
    EXPECT_EQ(run(false, true, true), std::optional<bool>{false});
    // The object is not delivered again: nothing is established.
    EXPECT_EQ(run(true, std::nullopt, true), std::nullopt);
    // The second SUBSCRIBE waits for the first subscription to end.
    Bytes early;
    EXPECT_EQ(run(true, true, false, &early), std::nullopt);
    EXPECT_TRUE(early.empty());
    const auto message = decode_request(second);
    ASSERT_TRUE(message);
    const auto* subscribe = std::get_if<d18::SubscribeMessage>(&*message);
    ASSERT_NE(subscribe, nullptr);
    EXPECT_EQ(subscribe->request_id, 3u);
    ASSERT_EQ(subscribe->parameters.size(), 2u);
    const auto& filter = std::get<d18::SubscriptionFilter>(subscribe->parameters[1].value);
    EXPECT_EQ(filter.type, d18::SubscriptionFilterType::AbsoluteStart);
    EXPECT_EQ(filter.start, (d18::Location{2, 0}));
}

// Sections 10.18 and 10.19: an unauthorized subscriber is not accepted.
TEST(Draft18ContributionExchange, UnauthorizedNamespaceSubscriptionsAreNotAccepted) {
    const auto all = probes();
    struct Row { const char* scenario; const char* requirement; bool tracks; };
    for (const auto& row : {
             Row{"receive-subscribe-namespace-denied-by-configured-authorization-policy", "D18-10-18-MUST-004", false},
             Row{"receive-subscribe-tracks-denied-by-configured-authorization-policy", "D18-10-19-MUST-004", true}}) {
        SCOPED_TRACE(row.requirement);
        const auto& p = probe(all, row.scenario, row.requirement);
        EXPECT_EQ(p.evaluator_id, "unauthorized-namespace-subscription-not-accepted");
        const auto message = decode_request(p.definition.writes.front().bytes);
        ASSERT_TRUE(message);
        const d18::Parameters* parameters = nullptr;
        if (row.tracks) {
            const auto* request = std::get_if<d18::SubscribeTracksMessage>(&*message);
            ASSERT_NE(request, nullptr);
            EXPECT_EQ(request->track_namespace_prefix.fields, (std::vector<Bytes>{text("n")}));
            parameters = &request->parameters;
        } else {
            const auto* request = std::get_if<d18::SubscribeNamespaceMessage>(&*message);
            ASSERT_NE(request, nullptr);
            EXPECT_EQ(request->track_namespace_prefix.fields, (std::vector<Bytes>{text("n")}));
            parameters = &request->parameters;
        }
        ASSERT_EQ(parameters->size(), 1u);
        const auto& token = std::get<d18::Token>(parameters->front().value);
        EXPECT_EQ(token.alias_type, d18::TokenAliasType::UseValue);
        EXPECT_EQ(token.token_type, 0u);
        EXPECT_EQ(token.token_value, text("interop-denied"));
        const auto reply = [&](Bytes bytes) {
            const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
                v.when("setup", true, [&] { v.data(2, setup_with({})); });
                v.when("reply", v.sent(1), [&] { v.data(1, bytes); });
            });
            auto configured = transcript;
            configured.denied_authorization_token = "interop-denied";
            return evaluate_draft18_contribution_probe(configured, p);
        };
        EXPECT_EQ(reply(error(0x1)), std::optional<bool>{true});
        EXPECT_EQ(reply(error(0x3)), std::optional<bool>{true});
        EXPECT_EQ(reply(ok()), std::optional<bool>{false});
        EXPECT_EQ(reply(subscribe_ok()), std::nullopt);
    }
}

// Without --denied-authorization-token the publisher may legitimately grant the request, so a
// REQUEST_OK proves nothing; with it, the configured value is what is sent and judged.
TEST(Draft18ContributionExchange, UnauthorizedSubscriptionsAreUnscoredWithoutAConfiguredDenial) {
    for (const auto* scenario : {"receive-subscribe-namespace-denied-by-configured-authorization-policy",
                                 "receive-subscribe-tracks-denied-by-configured-authorization-policy"}) {
        SCOPED_TRACE(scenario);
        const auto all = probes();
        const auto* found = static_cast<const Draft18ContributionProbe*>(nullptr);
        for (const auto& p : all) if (p.definition.id == scenario) found = &p;
        ASSERT_NE(found, nullptr);
        const auto transcript = drive_probe(found->definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup_with({})); });
            v.when("reply", v.sent(1), [&] { v.data(1, ok()); });
        });
        EXPECT_EQ(evaluate_draft18_contribution_probe(transcript, *found), std::nullopt);
        auto custom = transcript;
        custom.denied_authorization_token = "other";
        EXPECT_EQ(evaluate_draft18_contribution_probe(custom, *found), std::nullopt);  // stimulus carried the default value
        Draft18TokenCredentials credentials;
        credentials.denied = "other";
        const auto rebuilt = draft18_contribution_probes(milliseconds{60}, {text("n")}, text("t"), credentials);
        for (const auto& p : rebuilt) if (p.definition.id == scenario) found = &p;
        auto driven = drive_probe(found->definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup_with({})); });
            v.when("reply", v.sent(1), [&] { v.data(1, ok()); });
        });
        driven.denied_authorization_token = "other";
        EXPECT_EQ(evaluate_draft18_contribution_probe(driven, *found), std::optional<bool>{false});
    }
}

}  // namespace
}  // namespace moq::interop::scenarios
