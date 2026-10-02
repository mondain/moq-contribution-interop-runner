#include "moq/interop/scenarios/draft18_contribution.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

// Expectations are derived from the draft-18 text cited at each test, not from
// any publisher's behavior.
namespace moq::interop::scenarios {
namespace {
using namespace test;
using std::chrono::milliseconds;

std::vector<Draft18ContributionProbe> probes() {
    return draft18_contribution_probes(milliseconds{60}, {text("n")}, text("t"));
}

Bytes publish_namespace(std::vector<std::string_view> fields, std::uint64_t id = 0) {
    d18::TrackNamespace ns;
    for (const auto field : fields) ns.fields.push_back(text(field));
    return encode(d18::PublishNamespaceMessage{id, std::move(ns), {}});
}
Bytes publish_track(std::vector<std::string_view> fields, std::string_view name, std::uint64_t alias,
                    std::uint64_t id = 0) {
    d18::TrackNamespace ns;
    for (const auto field : fields) ns.fields.push_back(text(field));
    return encode(d18::PublishMessage{id, std::move(ns), d18::TrackName{text(name)}, alias, {}, {}});
}
Bytes subscribe_namespace(std::vector<std::string_view> fields, std::uint64_t id = 0) {
    d18::TrackNamespace ns;
    for (const auto field : fields) ns.fields.push_back(text(field));
    return encode(d18::SubscribeNamespaceMessage{id, std::move(ns), {}});
}

using Script = std::function<void(PeerView&)>;

std::optional<bool> run(const char* scenario, const char* requirement, const Script& script,
                        std::optional<std::string> uri = std::nullopt, bool webtransport = false,
                        bool default_setup = true) {
    const auto all = probes();
    const auto& p = probe(all, scenario, requirement);
    auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        if (default_setup) v.when("setup", true, [&] { v.data(2, setup_with({})); });
        script(v);
    });
    if (uri) transcript.connection_uri = *uri;
    return evaluate_draft18_contribution_probe(transcript, p, webtransport);
}

// Sends `messages`, one per peer request stream, once the runner has seen SETUP.
Script requests(std::vector<Bytes> messages) {
    return [messages](PeerView& v) {
        for (std::size_t i = 0; i < messages.size(); ++i)
            v.when("request" + std::to_string(i), v.step > 1, [&] { v.data(4 * i, messages[i]); });
    };
}

TEST(Draft18ContributionOrigination, RegistersEveryScenarioOfTheSlice) {
    const std::vector<std::pair<const char*, const char*>> rows{
        {"publish-key-value-type-boundary", "D18-1-4-3-MUST-NOT-001"},
        {"publish-distinct-content-tracks-in-same-scope", "D18-2-4-3-MUST-001"},
        {"publish-track-with-mandatory-property", "D18-2-5-1-MUST-001"},
        {"publish-under-unregistered-period-prefixed-namespace", "D18-3-2-1-MUST-NOT-001"},
        {"originate-publisher-operations-under-single-period-namespace", "D18-3-2-1-MUST-NOT-002"},
        {"publish-track-under-single-period-namespace", "D18-3-2-1-MUST-NOT-003"},
        {"publish-namespace-under-single-period-namespace", "D18-3-2-1-MUST-NOT-004"},
        {"application-publish-track-in-session-namespace", "D18-3-2-2-MUST-NOT-001"},
        {"application-publish-namespace-in-session-namespace", "D18-3-2-2-MUST-NOT-002"},
        {"finish-subscription-with-open-object-streams", "D18-5-1-1-MUST-NOT-001"},
    };
    const auto all = probes();
    for (const auto& [scenario, requirement] : rows) {
        const auto& p = probe(all, scenario, requirement);
        EXPECT_TRUE(draft18_contribution_scenario(scenario)) << scenario;
        EXPECT_FALSE(p.evaluator_id.empty());
    }
}

// Section 3.2.1: period-prefixed namespaces are reserved and must not be used
// unless registered; section 3.2.2 defines .session.
TEST(Draft18ContributionOrigination, UnregisteredReservedNamespaceIsNotOriginated) {
    const auto row = [](Bytes message) {
        return run("publish-under-unregistered-period-prefixed-namespace", "D18-3-2-1-MUST-NOT-001",
                   requests({std::move(message)}));
    };
    EXPECT_EQ(row(publish_namespace({"media"})), std::optional<bool>{true});
    EXPECT_EQ(row(publish_namespace({".x"})), std::optional<bool>{false});
    EXPECT_EQ(row(publish_namespace({".", "media"})), std::optional<bool>{false});
    EXPECT_EQ(row(publish_track({".private"}, "t", 1)), std::optional<bool>{false});
    EXPECT_EQ(row(publish_namespace({".session"})), std::optional<bool>{true});
    // Only the first tuple field is reserved.
    EXPECT_EQ(row(publish_namespace({"media", ".x"})), std::optional<bool>{true});
    // Without any publication the rule was not exercised.
    EXPECT_EQ(run("publish-under-unregistered-period-prefixed-namespace", "D18-3-2-1-MUST-NOT-001",
                  requests({})), std::nullopt);
}

// Section 3.2.1: a namespace whose first field is exactly "." is not to be used
// for any purpose.
TEST(Draft18ContributionOrigination, SinglePeriodNamespaceIsNotUsedByAnyOriginatedRequest) {
    const auto row = [](Bytes message) {
        return run("originate-publisher-operations-under-single-period-namespace", "D18-3-2-1-MUST-NOT-002",
                   requests({std::move(message)}));
    };
    EXPECT_EQ(row(publish_namespace({"media"})), std::optional<bool>{true});
    EXPECT_EQ(row(publish_namespace({"."})), std::optional<bool>{false});
    EXPECT_EQ(row(publish_track({"."}, "t", 1)), std::optional<bool>{false});
    EXPECT_EQ(row(encode(d18::TrackStatusMessage{0, d18::TrackNamespace{{text(".")}}, d18::TrackName{text("t")}, {}})),
              std::optional<bool>{false});
    EXPECT_EQ(row(encode(d18::SubscribeMessage{0, d18::TrackNamespace{{text(".")}}, d18::TrackName{text("t")}, {}})),
              std::optional<bool>{false});
    EXPECT_EQ(row(encode(d18::SubscribeTracksMessage{0, d18::TrackNamespace{{text(".")}}, {}})),
              std::optional<bool>{false});
    EXPECT_EQ(row(encode(d18::FetchMessage{0, d18::StandaloneFetch{d18::TrackNamespace{{text(".")}},
                  d18::TrackName{text("t")}, {0, 0}, {1, 0}}, {}})), std::optional<bool>{false});
    EXPECT_EQ(row(subscribe_namespace({"."})), std::optional<bool>{false});
    // Another reserved name, or "." that is not the first field, is a different rule.
    EXPECT_EQ(row(publish_namespace({".x"})), std::optional<bool>{true});
    EXPECT_EQ(row(publish_namespace({"media", "."})), std::optional<bool>{true});
    EXPECT_EQ(row(publish_namespace({".."})), std::optional<bool>{true});
}

TEST(Draft18ContributionOrigination, SinglePeriodTrackIsNotPublished) {
    const auto row = [](std::vector<Bytes> messages) {
        return run("publish-track-under-single-period-namespace", "D18-3-2-1-MUST-NOT-003",
                   requests(std::move(messages)));
    };
    EXPECT_EQ(row({publish_track({"media"}, "t", 1)}), std::optional<bool>{true});
    EXPECT_EQ(row({publish_track({"."}, "t", 1)}), std::optional<bool>{false});
    // A namespace announcement is not a track publication: no evidence for this row.
    EXPECT_EQ(row({publish_namespace({"."})}), std::nullopt);
    EXPECT_EQ(row({publish_namespace({"."}), publish_track({"media"}, "t", 1, 2)}), std::optional<bool>{true});
}

TEST(Draft18ContributionOrigination, SinglePeriodNamespaceIsNotPublished) {
    const auto row = [](std::vector<Bytes> messages) {
        return run("publish-namespace-under-single-period-namespace", "D18-3-2-1-MUST-NOT-004",
                   requests(std::move(messages)));
    };
    EXPECT_EQ(row({publish_namespace({"media"})}), std::optional<bool>{true});
    EXPECT_EQ(row({publish_namespace({"."})}), std::optional<bool>{false});
    EXPECT_EQ(row({publish_track({"."}, "t", 1)}), std::nullopt);
}

// Section 3.2.2: the Application publishes no track or namespace whose first
// field is .session. The runner offers no extension, so any such name on the
// wire is the application's.
TEST(Draft18ContributionOrigination, ApplicationPublishesNothingUnderSessionNamespace) {
    const auto track = [](std::vector<Bytes> messages) {
        return run("application-publish-track-in-session-namespace", "D18-3-2-2-MUST-NOT-001",
                   requests(std::move(messages)));
    };
    const auto announcement = [](std::vector<Bytes> messages) {
        return run("application-publish-namespace-in-session-namespace", "D18-3-2-2-MUST-NOT-002",
                   requests(std::move(messages)));
    };
    EXPECT_EQ(track({publish_track({"media"}, "t", 1)}), std::optional<bool>{true});
    EXPECT_EQ(track({publish_track({".session"}, "t", 1)}), std::optional<bool>{false});
    EXPECT_EQ(track({publish_track({".sessions"}, "t", 1)}), std::optional<bool>{true});
    EXPECT_EQ(track({publish_track({"media", ".session"}, "t", 1)}), std::optional<bool>{true});
    EXPECT_EQ(track({publish_namespace({".session"})}), std::nullopt);
    EXPECT_EQ(announcement({publish_namespace({"media"})}), std::optional<bool>{true});
    EXPECT_EQ(announcement({publish_namespace({".session", "x"})}), std::optional<bool>{false});
    EXPECT_EQ(announcement({publish_namespace({".Session"})}), std::optional<bool>{true});
    EXPECT_EQ(announcement({publish_track({".session"}, "t", 1)}), std::nullopt);
}

// The runner acknowledges the publisher's PUBLISH and PUBLISH_NAMESPACE so that a
// publisher that waits for each answer carries on and originates more.
TEST(Draft18ContributionOrigination, AcceptsEveryPublisherPublicationRequest) {
    const auto all = probes();
    const auto& p = probe(all, "publish-track-under-single-period-namespace", "D18-3-2-1-MUST-NOT-003");
    const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        v.when("first", v.step > 1, [&] { v.data(0, publish_namespace({"media"})); });
        v.when("second", v.sent(0), [&] { v.data(4, publish_track({"media"}, "t", 1, 2)); });
    });
    EXPECT_TRUE(transcript.complete);
    ASSERT_EQ(transcript.auto_replies.size(), 2u);
    EXPECT_EQ(transcript.auto_replies[0].stream_id, 0u);
    EXPECT_EQ(transcript.auto_replies[1].stream_id, 4u);
    EXPECT_EQ(evaluate_draft18_contribution_probe(transcript, p), std::optional<bool>{true});
}

// Section 1.4.3: previous Type plus Delta Type must not exceed 2^64 - 1.
TEST(Draft18ContributionOrigination, KeyValueTypeSumsStayWithinSixtyFourBits) {
    const auto row = [](Bytes setup) {
        return run("publish-key-value-type-boundary", "D18-1-4-3-MUST-NOT-001",
                   [setup](PeerView& v) { v.when("setup-bytes", true, [&] { v.data(2, setup); }); },
                   std::nullopt, false, false);
    };
    // Types 1 and 3 (odd, so each carries a length): the second sums to 4.
    EXPECT_EQ(row(setup_with({odd_option(1, text("a")), odd_option(3, text("b"))})), std::optional<bool>{true});
    // No pair, or a single pair, has no sum to check.
    EXPECT_EQ(row(setup_with({})), std::nullopt);
    EXPECT_EQ(row(setup_with({odd_option(1, text("a"))})), std::nullopt);
    // An even Type carries a varint value after it.
    const auto raw = [](std::uint64_t first, std::uint64_t second) {
        Bytes payload = concat(vi(first), vi(0));
        payload = concat(payload, vi(second));
        return raw_setup(concat(payload, vi(0)));
    };
    // Largest even Type 2^64 - 2, then a delta of 2: the sum is 2^64 and does not fit.
    EXPECT_EQ(row(raw(0xFFFFFFFFFFFFFFFEull, 2)), std::optional<bool>{false});
    // A delta of 1 would reach 2^64 - 1 (odd, so a length follows): it fits exactly.
    Bytes boundary = concat(vi(0xFFFFFFFFFFFFFFFEull), vi(0));
    boundary = concat(boundary, vi(1));    // delta 1: Type 2^64 - 1
    boundary = concat(boundary, vi(0));    // zero-length value
    EXPECT_EQ(row(raw_setup(boundary)), std::optional<bool>{true});
    // A truncated pair cannot be summed.
    EXPECT_EQ(row(raw_setup(concat(vi(1), vi(5)))), std::nullopt);
}

// Section 1.4.3 for Parameters and Track Properties on request streams.
TEST(Draft18ContributionOrigination, KeyValueTypeSumsInRequestMessagesAreChecked) {
    const auto row = [](Bytes message) {
        return run("publish-key-value-type-boundary", "D18-1-4-3-MUST-NOT-001", requests({std::move(message)}));
    };
    d18::TrackProperties properties;
    properties.entries.push_back(even_option(2, 1));
    properties.entries.push_back(odd_option(5, text("p")));
    EXPECT_EQ(row(encode(d18::PublishMessage{0, d18::TrackNamespace{{text("n")}}, d18::TrackName{text("t")}, 3, {},
                                             properties})), std::optional<bool>{true});
    EXPECT_EQ(row(publish_namespace({"n"})), std::nullopt);
    // PUBLISH_NAMESPACE body: request id 0, one namespace field "n", two Parameters. The first is a
    // valid AUTHORIZATION TOKEN (Type 3: Delete alias 1); the second has a Type delta of 2^64 - 1,
    // so the running Type would exceed 2^64 - 1.
    Bytes body = concat(vi(0), vi(1));
    body = concat(body, vi(1));
    body = concat(body, bytes_of({'n'}));
    body = concat(body, vi(2));
    body = concat(body, vi(3));
    body = concat(body, vi(2));
    body = concat(body, vi(0));
    body = concat(body, vi(1));
    body = concat(body, vi(0xFFFFFFFFFFFFFFFFull));
    body = concat(body, vi(0));
    Bytes message = vi(0x06);
    message = concat(message, bytes_of({static_cast<unsigned>(body.size() >> 8), static_cast<unsigned>(body.size() & 255)}));
    message = concat(message, body);
    EXPECT_EQ(row(message), std::optional<bool>{false});
}

// Section 5.1.1 and 10.11: the publisher closes the subscription's streams before it
// sends PUBLISH_DONE. Only an ordered close or a stream that never ends is decisive.
Bytes subgroup_stream(std::uint64_t alias, std::uint64_t group) {
    return concat(subgroup_header(alias, group), subgroup_object(0, text("p")));
}
Bytes publish_done() { return encode(d18::PublishDoneMessage{0x2, 1, d18::ReasonPhrase{}}); }

TEST(Draft18ContributionOrigination, StreamsEndBeforePublishDone) {
    const auto row = [](bool fin_first, bool fin_at_all, bool close_session) {
        return run("finish-subscription-with-open-object-streams", "D18-5-1-1-MUST-NOT-001", [=](PeerView& v) {
            v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); });
            v.when("data", v.fired.count("ok") != 0, [&] { v.data(6, subgroup_stream(5, 0), fin_first && fin_at_all); });
            v.when("done", v.fired.count("data") != 0, [&] { v.data(1, publish_done()); });
            v.when("late-fin", v.fired.count("done") != 0 && !fin_first && fin_at_all, [&] { v.data(6, {}, true); });
            v.when("close", v.fired.count("done") != 0 && close_session,
                   [&] { v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}}); });
        });
    };
    EXPECT_EQ(row(true, true, false), std::optional<bool>{true});
    // PUBLISH_DONE can overtake the end of a stream without being sent first.
    EXPECT_EQ(row(false, true, false), std::nullopt);
    // A stream that stays open throughout the observation window was open when PUBLISH_DONE was sent.
    EXPECT_EQ(row(false, false, false), std::optional<bool>{false});
    // A session close ends every stream, so an open stream proves nothing.
    EXPECT_EQ(row(false, false, true), std::nullopt);
}

TEST(Draft18ContributionOrigination, ResetStreamCountsAsClosedBeforePublishDone) {
    const auto result = run("finish-subscription-with-open-object-streams", "D18-5-1-1-MUST-NOT-001", [](PeerView& v) {
        v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); });
        v.when("data", v.fired.count("ok") != 0, [&] { v.data(6, subgroup_stream(5, 0)); });
        v.when("reset", v.fired.count("data") != 0, [&] { v.push(transport::PeerResetEvent{6, 0}); });
        v.when("done", v.fired.count("reset") != 0, [&] { v.data(1, publish_done()); });
    });
    EXPECT_EQ(result, std::optional<bool>{true});
}

TEST(Draft18ContributionOrigination, PublishDoneWithoutObjectStreamsIsNotEvidence) {
    const auto result = run("finish-subscription-with-open-object-streams", "D18-5-1-1-MUST-NOT-001", [](PeerView& v) {
        v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); });
        v.when("done", v.fired.count("ok") != 0, [&] { v.data(1, publish_done()); });
    });
    EXPECT_EQ(result, std::nullopt);
}

// Section 2.5.1: Mandatory Track Properties (0x4000-0x7FFF) have Track scope; one on an
// Object is malformed.
TEST(Draft18ContributionOrigination, MandatoryPropertyIsOnlyUsedAtTrackScope) {
    const auto row = [](std::optional<d18::KeyValuePairs> properties, bool track_scope_property = false) {
        return run("publish-track-with-mandatory-property", "D18-2-5-1-MUST-001", [=](PeerView& v) {
            v.when("ok", v.sent(1), [&] {
                d18::TrackProperties track;
                if (track_scope_property) track.entries.push_back(even_option(0x4000, 1));
                v.data(1, encode(d18::SubscribeOkMessage{5, {}, track}));
            });
            v.when("data", v.fired.count("ok") != 0, [&] {
                Bytes stream = subgroup_header(5, 0, properties ? 0x11 : 0x10);
                stream = concat(stream, subgroup_object(0, text("p"), std::nullopt,
                                                        properties ? std::optional<Bytes>{kvp(*properties)}
                                                                   : std::nullopt));
                v.data(6, stream);
            });
        });
    };
    EXPECT_EQ(row(std::nullopt), std::optional<bool>{true});
    EXPECT_EQ(row(d18::KeyValuePairs{even_option(2, 1), odd_option(5, text("a"))}), std::optional<bool>{true});
    EXPECT_EQ(row(d18::KeyValuePairs{even_option(0x3FFE, 1), even_option(0x8000, 1)}), std::optional<bool>{true});
    EXPECT_EQ(row(d18::KeyValuePairs{even_option(0x4000, 1)}), std::optional<bool>{false});
    EXPECT_EQ(row(d18::KeyValuePairs{odd_option(0x4001, text("a"))}), std::optional<bool>{false});
    EXPECT_EQ(row(d18::KeyValuePairs{even_option(2, 1), even_option(0x7FFE, 1)}), std::optional<bool>{false});
    // Inside the Immutable Properties container an Object Property is still an Object Property.
    EXPECT_EQ(row(d18::KeyValuePairs{odd_option(0x0B, kvp({even_option(0x4002, 1)}))}), std::optional<bool>{false});
    EXPECT_EQ(row(d18::KeyValuePairs{odd_option(0x0B, kvp({even_option(0x3000, 1)}))}), std::optional<bool>{true});
    // On the Track itself it is allowed.
    EXPECT_EQ(row(std::nullopt, true), std::optional<bool>{true});
    // Without any Object there is nothing to inspect.
    const auto idle = run("publish-track-with-mandatory-property", "D18-2-5-1-MUST-001",
                          [](PeerView& v) { v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); }); });
    EXPECT_EQ(idle, std::nullopt);
}

// Section 2.4.3: tracks that hold different data at the same time have different Full Track Names.
TEST(Draft18ContributionOrigination, DistinctContentUsesDistinctFullTrackNames) {
    const auto row = [](std::string_view publish_name, std::string_view published_payload,
                        std::string_view subscribed_payload = "AA") {
        return run("publish-distinct-content-tracks-in-same-scope", "D18-2-4-3-MUST-001", [=](PeerView& v) {
            v.when("publish", v.step > 1, [&] { v.data(0, publish_track({"n"}, publish_name, 9)); });
            v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); });
            v.when("subscribed", v.fired.count("ok") != 0, [&] {
                v.data(6, concat(subgroup_header(5, 0), subgroup_object(0, text(subscribed_payload))));
            });
            v.when("published", v.fired.count("publish") != 0 && v.fired.count("ok") != 0, [&] {
                v.data(10, concat(subgroup_header(9, 0), subgroup_object(0, text(published_payload))));
            });
        });
    };
    EXPECT_EQ(row("u", "BB"), std::optional<bool>{true});
    // The same Full Track Name (n, t) with different data at the same Location.
    EXPECT_EQ(row("t", "BB"), std::optional<bool>{false});
    // The same Full Track Name with the same data is one Track.
    EXPECT_EQ(row("t", "AA"), std::nullopt);
    // Different names with identical data do not exercise the rule.
    EXPECT_EQ(row("u", "AA"), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
