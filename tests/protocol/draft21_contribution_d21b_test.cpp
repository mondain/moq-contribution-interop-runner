// Draft-21 contribution profiles, slice B: hand-derived expectations from the
// draft text. Every verdict rests on bytes the scripted publisher put on the
// wire; a missing or ambiguous observation must stay unscored.

#include "../support/contribution_harness.h"
#include "../support/contribution_transcript.h"

#include "moq/interop/app/scenario_registry.h"

#include <gtest/gtest.h>

#include <string>

namespace moq::interop::scenarios {
namespace {
using test::Bytes;
using test::cbytes;
using test::cconcat;
using test::cframe;
using test::ContributionRun;
using test::cvi;
using test::find_probe;
using test::request_error;
using test::request_ok;
using test::subscribe_ok;

const std::vector<Draft21ContributionProbe>& probes() {
    static const auto value = draft21_contribution_probes();
    return value;
}

Bytes peer_setup(unsigned type, unsigned value) { return cbytes({0xaf, 0, 0, 2, type, value}); }

// ---- wire builders ----------------------------------------------------------
// Parameter blocks: count, then (type delta, value) pairs.
Bytes ok_with(const Bytes& block) { return cframe(7, block); }
const Bytes kLargest = cbytes({1, 9, 1, 2});  // LARGEST_OBJECT {1, 2}
Bytes notify(const Bytes& block) { return cframe(0x22, block); }
Bytes publish_done(std::uint64_t status, std::uint64_t streams) {
    return cframe(0xb, cconcat({cvi(status), cvi(streams), cvi(0)}));
}
Bytes with_length(const Bytes& block) { return cconcat({cvi(block.size()), block}); }
Bytes object_data(std::uint64_t delta, const Bytes& payload, const Bytes& props = {}, bool has_props = false) {
    Bytes result = cvi(delta);
    if (has_props) result = cconcat({result, with_length(props)});
    return cconcat({result, cvi(payload.size()), payload});
}
Bytes subgroup(std::uint64_t alias, std::uint64_t group, const Bytes& objects, unsigned type = 0x30) {
    return cconcat({cvi(type), cvi(alias), cvi(group), objects});
}
Bytes datagram_object(std::uint64_t alias, std::uint64_t group, std::uint64_t object) {
    return cconcat({cvi(0x08), cvi(alias), cvi(group), cvi(object), cbytes({'a'})});
}
Bytes text(const std::string& value) {
    Bytes result;
    for (const char c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}
constexpr transport::StreamId kData1 = 6;
constexpr transport::StreamId kData2 = 10;
const Bytes kObject = object_data(0, cbytes({'a'}));

std::optional<bool> evaluate(ContributionRun run, const Draft21ContributionProbe& probe,
                             std::optional<std::string> denied = std::nullopt) {
    auto transcript = run.finish();
    transcript.denied_authorization_token = std::move(denied);
    return evaluate_draft21_contribution_probe(transcript, probe);
}

void close_with(ContributionRun& run, std::uint64_t code) {
    run.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, code, {}});
}

TEST(ContributionD21b, RegistryAndAlternativesAreDeclared) {
    for (const char* scenario : {"d21-largest-object-required-after-publication",
                                 "d21-publish-done-datagram-only", "d21-grease-stop-sending",
                                 "d21-namespace-discovery-authorization"})
        EXPECT_TRUE(app::draft21_contribution_scenario(21, scenario)) << scenario;
    EXPECT_TRUE(draft21_contribution_scenarios_are_alternatives("D21-9-9-MUST-365"));
    EXPECT_FALSE(draft21_contribution_scenarios_are_alternatives("D21-9-20-18-MUST-456"));
}

// ---- Section 9.20.18: LARGEST_OBJECT after publication -----------------------------
ContributionRun largest_run(const Draft21ContributionProbe& probe) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    return run;
}

TEST(ContributionD21b, LargestObjectIsRequiredInEveryAcceptanceAfterObservedPublication) {
    const auto& probe = find_probe(probes(), "d21-largest-object-required-after-publication");
    ASSERT_EQ(probe.definition.writes.size(), 4u);
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({3, 0, 7, 3, 0, 1, 'x', 1, 0x10, 0}));
    EXPECT_EQ(probe.definition.writes[2].bytes, cbytes({2, 0, 4, 5, 1, 0x20, 100}));
    EXPECT_EQ(probe.definition.writes[3].bytes, cbytes({0xd, 0, 5, 7, 0, 1, 'x', 0}));
    EXPECT_TRUE(probe.definition.writes[3].fin);

    auto full = largest_run(probe);
    // Nothing is sent before an Object's first byte is observed.
    EXPECT_FALSE(probe.definition.writes[1].evidence_ready({full.snapshot().writes, full.snapshot().events}));
    full.reply(kData1, subgroup(5, 1, kObject));
    full.deliver(1);
    full.deliver(2);
    full.reply(full.stream_of(1), subscribe_ok(5, kLargest));
    EXPECT_FALSE(probe.definition.response_ready(full.partial()));
    full.reply(full.stream_of(0), ok_with(kLargest));
    // TRACK_STATUS goes last, once the other two have been answered.
    full.deliver(3);
    full.reply(full.stream_of(3), ok_with(kLargest), true);
    EXPECT_TRUE(probe.definition.response_ready(full.partial()));
    EXPECT_EQ(evaluate(full, probe), true);

    // REQUEST_UPDATE_OK without LARGEST_OBJECT after publication is a violation.
    auto update_lacks = largest_run(probe);
    update_lacks.reply(kData1, subgroup(5, 1, kObject));
    update_lacks.deliver(1);
    update_lacks.deliver(2);
    update_lacks.reply(update_lacks.stream_of(1), subscribe_ok(5, kLargest));
    update_lacks.reply(update_lacks.stream_of(0), request_ok());
    update_lacks.deliver(3);
    EXPECT_TRUE(probe.definition.response_ready(update_lacks.partial()));
    EXPECT_EQ(evaluate(update_lacks, probe), false);

    auto subscribe_lacks = largest_run(probe);
    subscribe_lacks.reply(kData1, subgroup(5, 1, kObject));
    subscribe_lacks.deliver(1);
    subscribe_lacks.deliver(2);
    subscribe_lacks.reply(subscribe_lacks.stream_of(1), subscribe_ok(5));
    subscribe_lacks.reply(subscribe_lacks.stream_of(0), ok_with(kLargest));
    subscribe_lacks.deliver(3);
    EXPECT_EQ(evaluate(subscribe_lacks, probe), false);

    auto status_lacks = largest_run(probe);
    status_lacks.reply(kData1, subgroup(5, 1, kObject));
    status_lacks.deliver(1);
    status_lacks.deliver(2);
    status_lacks.reply(status_lacks.stream_of(1), subscribe_ok(5, kLargest));
    status_lacks.reply(status_lacks.stream_of(0), ok_with(kLargest));
    status_lacks.deliver(3);
    status_lacks.reply(status_lacks.stream_of(3), request_ok(), true);
    EXPECT_EQ(evaluate(status_lacks, probe), false);

    // A publisher that ends the session on TRACK_STATUS leaves the earlier
    // acceptances to decide; with both carrying the parameter the row passes.
    auto closed = largest_run(probe);
    closed.reply(kData1, subgroup(5, 1, kObject));
    closed.deliver(1);
    closed.deliver(2);
    closed.reply(closed.stream_of(1), subscribe_ok(5, kLargest));
    closed.reply(closed.stream_of(0), ok_with(kLargest));
    closed.deliver(3);
    close_with(closed, 3);
    EXPECT_EQ(evaluate(closed, probe), true);

    // Rejections prove nothing.
    auto rejected = largest_run(probe);
    rejected.reply(kData1, subgroup(5, 1, kObject));
    rejected.deliver(1);
    rejected.deliver(2);
    rejected.reply(rejected.stream_of(1), request_error(0x10));
    rejected.reply(rejected.stream_of(0), request_error(0x10));
    rejected.deliver(3);
    rejected.reply(rejected.stream_of(3), request_error(0x10), true);
    EXPECT_EQ(evaluate(rejected, probe), std::nullopt);
}

TEST(ContributionD21b, LargestObjectControlOnlyShowsTheSubscriptionWasAnswered) {
    const auto& probe = find_probe(probes(), "d21-largest-object-before-publication");
    ASSERT_EQ(probe.definition.writes.size(), 1u);
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({3, 0, 7, 1, 0, 1, 'x', 1, 0x10, 0}));
    for (const Bytes& response : {subscribe_ok(5), subscribe_ok(5, kLargest)}) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), response);
        EXPECT_EQ(evaluate(run, probe), true);
    }
    ContributionRun rejected(probe);
    rejected.deliver(0);
    rejected.reply(rejected.stream_of(0), request_error(0x10));
    EXPECT_EQ(evaluate(rejected, probe), std::nullopt);
}

// ---- Section 9.9: PUBLISH_DONE Stream Count --------------------------------------
ContributionRun done_run(const Draft21ContributionProbe& probe, const Bytes& done, bool fence = true) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    run.deliver(1);
    run.reply(run.stream_of(0), cconcat({request_error(0x1), done}));
    if (fence) {
        run.deliver(2);
        run.reply(run.stream_of(2), subscribe_ok(6));
    }
    return run;
}

TEST(ContributionD21b, PublishDoneCountsZeroWhenNoDataStreamWasOpened) {
    const auto& probe = find_probe(probes(), "d21-publish-done-without-data-streams");
    ASSERT_EQ(probe.definition.writes.size(), 3u);
    // The subscription's start is far in the future: no Object can match.
    EXPECT_EQ(probe.definition.writes[0].bytes,
              cbytes({3, 0, 13, 1, 0, 1, 'x', 2, 0x10, 1, 0x11, 4, 0xcf, 0x42, 0x40, 0}));
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({2, 0, 6, 3, 1, 3, 2, 2, 0}));
    EXPECT_EQ(evaluate(done_run(probe, publish_done(0x2, 0)), probe), true);
    // A nonzero count without any data stream contradicts the rule.
    EXPECT_EQ(evaluate(done_run(probe, publish_done(0x2, 3)), probe), false);
    EXPECT_EQ(evaluate(done_run(probe, publish_done(0x2, std::numeric_limits<std::uint64_t>::max())), probe), false);
    // Waiting for the later round trip: not ready (and unscored) before it.
    auto early = done_run(probe, publish_done(0x2, 3), false);
    EXPECT_FALSE(probe.definition.response_ready(early.partial()));
    // A data stream means the precondition does not hold.
    auto streamed = done_run(probe, publish_done(0x2, 1));
    streamed.reply(kData1, subgroup(5, 1, kObject));
    EXPECT_EQ(evaluate(streamed, probe), std::nullopt);
    auto datagram = done_run(probe, publish_done(0x2, 0));
    datagram.event(transport::DatagramEvent{datagram_object(5, 1, 0)});
    EXPECT_EQ(evaluate(datagram, probe), std::nullopt);
    // Without PUBLISH_DONE nothing is scored.
    ContributionRun silent(probe);
    silent.deliver(0);
    silent.reply(silent.stream_of(0), subscribe_ok(5));
    silent.deliver(1);
    close_with(silent, 0);
    EXPECT_EQ(evaluate(silent, probe), std::nullopt);
}

TEST(ContributionD21b, DatagramOnlyDeliveryNeedsNoStreamsEither) {
    const auto& probe = find_probe(probes(), "d21-publish-done-datagram-only");
    const auto datagram_gate = [&](ContributionRun& run) {
        return probe.definition.writes[1].evidence_ready({run.snapshot().writes, run.snapshot().events});
    };
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    EXPECT_FALSE(datagram_gate(run));
    run.event(transport::DatagramEvent{datagram_object(5, 1, 0)});
    EXPECT_TRUE(datagram_gate(run));
    auto copy = run;
    copy.deliver(1);
    copy.reply(copy.stream_of(0), cconcat({request_error(0x1), publish_done(0x2, 0)}));
    copy.deliver(2);
    copy.reply(copy.stream_of(2), subscribe_ok(6));
    EXPECT_EQ(evaluate(copy, probe), true);
    auto wrong = run;
    wrong.deliver(1);
    wrong.reply(wrong.stream_of(0), cconcat({request_error(0x1), publish_done(0x2, 4)}));
    wrong.deliver(2);
    wrong.reply(wrong.stream_of(2), subscribe_ok(6));
    EXPECT_EQ(evaluate(wrong, probe), false);
    // A stream carrying data keeps the update from being sent.
    auto streamed = run;
    streamed.reply(kData1, subgroup(5, 1, kObject));
    EXPECT_FALSE(datagram_gate(streamed));
}

// ---- Section 9.4.1: namespace-scoped Redirect --------------------------------------
Bytes redirect_error(const Bytes& track_name) {
    // REDIRECT: retry 0, no reason, empty Connect URI, namespace (a), Track Name.
    return cframe(5, cconcat({cvi(0x34), cvi(0), cvi(0), cvi(0), cvi(1), cvi(1), cbytes({'a'}),
                              cvi(track_name.size()), track_name}));
}

TEST(ContributionD21b, NamespaceRedirectsMustLeaveTheTrackNameEmpty) {
    for (const auto& [scenario, type] : {std::pair<std::string, unsigned>{"d21-publisher-namespace-redirect", 0x50},
                                        {"d21-publisher-subscribe-tracks-redirect", 0x51}}) {
        const auto& probe = find_probe(probes(), scenario);
        EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({type, 0, 3, 1, 0, 0})) << scenario;
        ContributionRun empty(probe);
        empty.deliver(0);
        empty.reply(empty.stream_of(0), redirect_error({}), true);
        EXPECT_EQ(evaluate(empty, probe), true) << scenario;
        ContributionRun named(probe);
        named.deliver(0);
        named.reply(named.stream_of(0), redirect_error(cbytes({'t'})), true);
        EXPECT_EQ(evaluate(named, probe), false) << scenario;
        // Other errors and acceptances are not Redirects.
        ContributionRun other(probe);
        other.deliver(0);
        other.reply(other.stream_of(0), request_error(0x30), true);
        EXPECT_EQ(evaluate(other, probe), std::nullopt) << scenario;
        ContributionRun accepted(probe);
        accepted.deliver(0);
        accepted.reply(accepted.stream_of(0), request_ok());
        EXPECT_EQ(evaluate(accepted, probe), std::nullopt) << scenario;
    }
}

// ---- Section 9.10: PUBLISH_STATE_NOTIFY --------------------------------------------
ContributionRun notify_run(const Draft21ContributionProbe& probe) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    return run;
}

TEST(ContributionD21b, NotificationIncludesLargestObjectOnceAnObjectIsKnown) {
    const auto& probe = find_probe(probes(), "d21-publish-state-notify-known-largest-object");
    EXPECT_EQ(probe.definition.writes[0].bytes,
              cbytes({3, 0, 11, 1, 0, 1, 'x', 3, 0x10, 1, 0x10, 100, 2, 1}));
    auto with = notify_run(probe);
    with.reply(kData1, subgroup(5, 1, kObject));
    with.reply(with.stream_of(0), notify(kLargest));
    EXPECT_TRUE(probe.definition.response_ready(with.partial()));
    EXPECT_EQ(evaluate(with, probe), true);
    auto without = notify_run(probe);
    without.reply(kData1, subgroup(5, 1, kObject));
    without.reply(without.stream_of(0), notify(cbytes({1, 0x10, 1})));
    EXPECT_EQ(evaluate(without, probe), false);
    // A notification that arrived before any Object was observed owes nothing here.
    auto early = notify_run(probe);
    early.reply(early.stream_of(0), notify(cbytes({1, 0x10, 1})));
    early.reply(kData1, subgroup(5, 1, kObject));
    EXPECT_FALSE(probe.definition.response_ready(early.partial()));
    EXPECT_EQ(evaluate(early, probe), std::nullopt);
    EXPECT_EQ(evaluate(notify_run(probe), probe), std::nullopt);
}

TEST(ContributionD21b, NotificationBeforeAnyObjectIsOnlyRecorded) {
    const auto& probe = find_probe(probes(), "d21-publish-state-notify-before-first-object");
    auto early = notify_run(probe);
    early.reply(early.stream_of(0), notify(cbytes({1, 0x10, 1})));
    EXPECT_EQ(evaluate(early, probe), true);
    auto late = notify_run(probe);
    late.reply(kData1, subgroup(5, 1, kObject));
    late.reply(late.stream_of(0), notify(cbytes({1, 0x10, 1})));
    EXPECT_EQ(evaluate(late, probe), std::nullopt);
}

TEST(ContributionD21b, SubscriberControlledValuesAreNotChangedByTheNotification) {
    const auto& probe = find_probe(probes(), "d21-publish-state-notify-preserves-subscriber-control");
    for (const Bytes& harmless : {cbytes({1, 0x10, 1}), cbytes({1, 9, 1, 2}), cbytes({2, 0x10, 1, 0x10, 100})}) {
        auto run = notify_run(probe);
        run.reply(run.stream_of(0), notify(harmless));
        EXPECT_EQ(evaluate(run, probe), true);
    }
    // FORWARD 0 or another priority is a change nobody asked for.
    for (const Bytes& changed : {cbytes({1, 0x10, 0}), cbytes({1, 0x20, 7}), cbytes({1, 0x22, 2})}) {
        auto run = notify_run(probe);
        run.reply(run.stream_of(0), notify(changed));
        EXPECT_EQ(evaluate(run, probe), false);
    }
    EXPECT_EQ(evaluate(notify_run(probe), probe), std::nullopt);
}

TEST(ContributionD21b, RequestedForwardChangeMayBeReportedButNothingElse) {
    const auto& probe = find_probe(probes(), "d21-publish-state-notify-requested-forward-change");
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({2, 0, 4, 3, 1, 0x10, 0}));
    const auto run_with = [&](const Bytes& before_ack, const Bytes& after_ack) {
        auto run = notify_run(probe);
        run.deliver(1);
        if (!before_ack.empty()) run.reply(run.stream_of(0), before_ack);
        run.reply(run.stream_of(0), request_ok());
        if (!after_ack.empty()) run.reply(run.stream_of(0), after_ack);
        return run;
    };
    // After the acknowledged update FORWARD 0 is what the subscriber asked for.
    EXPECT_EQ(evaluate(run_with({}, notify(cbytes({1, 0x10, 0}))), probe), true);
    EXPECT_EQ(evaluate(run_with({}, notify(cbytes({1, 0x10, 1}))), probe), false);
    // Before the acknowledgement either state is legitimate.
    EXPECT_EQ(evaluate(run_with(notify(cbytes({1, 0x10, 1})), notify(cbytes({1, 0x10, 0}))), probe), true);
    EXPECT_EQ(evaluate(run_with(notify(cbytes({1, 0x10, 1})), {}), probe), std::nullopt);
    EXPECT_EQ(evaluate(run_with({}, {}), probe), std::nullopt);
    // An update that was not accepted fixes no requested value.
    auto rejected = notify_run(probe);
    rejected.deliver(1);
    rejected.reply(rejected.stream_of(0), request_error(0x1));
    EXPECT_EQ(evaluate(rejected, probe), std::nullopt);
}

// ---- Section 11.5: padding emitted by the publisher ---------------------------------
Bytes padding_stream(std::size_t zeros, std::optional<std::size_t> nonzero_at = std::nullopt) {
    Bytes result = cvi(0x132b3e28);
    result.resize(result.size() + zeros, std::byte{0});
    if (nonzero_at) result[result.size() - 1 - *nonzero_at] = std::byte{1};
    return result;
}

TEST(ContributionD21b, PaddingStreamBytesAfterTheTypeAreZero) {
    const auto& probe = find_probe(probes(), "d21-padding-stream-emission");
    ContributionRun clean(probe);
    clean.deliver(0);
    clean.reply(clean.stream_of(0), subscribe_ok(5));
    clean.reply(kData1, padding_stream(40), true);
    EXPECT_TRUE(probe.definition.response_ready(clean.partial()));
    EXPECT_EQ(evaluate(clean, probe), true);
    // Empty padding is valid.
    ContributionRun empty(probe);
    empty.deliver(0);
    empty.reply(kData1, padding_stream(0), true);
    EXPECT_EQ(evaluate(empty, probe), true);
    // One non-zero byte anywhere after the type fails, even before the stream ends.
    ContributionRun dirty(probe);
    dirty.deliver(0);
    dirty.reply(kData1, padding_stream(40, 17));
    EXPECT_TRUE(probe.definition.response_ready(dirty.partial()));
    EXPECT_EQ(evaluate(dirty, probe), false);
    // An unfinished all-zero stream is not yet a verdict.
    ContributionRun open(probe);
    open.deliver(0);
    open.reply(kData1, padding_stream(8));
    EXPECT_FALSE(probe.definition.response_ready(open.partial()));
    EXPECT_EQ(evaluate(open, probe), std::nullopt);
    ContributionRun none(probe);
    none.deliver(0);
    EXPECT_EQ(evaluate(none, probe), std::nullopt);
}

TEST(ContributionD21b, PaddingDatagramBytesAfterTheTypeAreZero) {
    const auto& probe = find_probe(probes(), "d21-padding-datagram-emission");
    const auto datagram = [](std::size_t zeros, bool dirty) {
        Bytes bytes = cvi(0x132b3e29);
        bytes.resize(bytes.size() + zeros, std::byte{0});
        if (dirty) bytes.back() = std::byte{9};
        return transport::DatagramEvent{bytes};
    };
    ContributionRun clean(probe);
    clean.deliver(0);
    clean.event(datagram(20, false));
    EXPECT_EQ(evaluate(clean, probe), true);
    ContributionRun dirty(probe);
    dirty.deliver(0);
    dirty.event(datagram(20, true));
    EXPECT_EQ(evaluate(dirty, probe), false);
    ContributionRun other(probe);
    other.deliver(0);
    other.event(transport::DatagramEvent{datagram_object(5, 1, 0)});
    EXPECT_EQ(evaluate(other, probe), std::nullopt);
}

// ---- Sections 9.15, 9.18: discovery authorization -----------------------------------
TEST(ContributionD21b, DiscoveryIsGrantedOnlyToCredentialsThePolicyAccepts) {
    for (const auto& [scenario, type] : {std::pair<std::string, unsigned>{"d21-namespace-discovery-authorization", 0x50},
                                        {"d21-track-discovery-authorization", 0x51}}) {
        const auto& probe = find_probe(probes(), scenario);
        // AUTHORIZATION_TOKEN (3): USE_VALUE (3), Token Type 0, the contract value.
        const std::string value = "interop-denied";
        const auto expected = cconcat({cvi(type), cbytes({0, static_cast<unsigned>(value.size() + 7)}),
                                       cbytes({1, 0, 1, 3, static_cast<unsigned>(value.size() + 2), 3, 0}), text(value)});
        EXPECT_EQ(probe.definition.writes[0].bytes, expected) << scenario;

        ContributionRun denied(probe);
        denied.deliver(0);
        denied.reply(denied.stream_of(0), request_error(0x1), true);
        EXPECT_EQ(evaluate(denied, probe, value), true) << scenario;
        ContributionRun granted(probe);
        granted.deliver(0);
        granted.reply(granted.stream_of(0), request_ok());
        EXPECT_EQ(evaluate(granted, probe, value), false) << scenario;
        // Without an operator-controlled policy a grant proves nothing.
        EXPECT_EQ(evaluate(granted, probe), std::nullopt) << scenario;
        EXPECT_EQ(evaluate(denied, probe), std::nullopt) << scenario;
    }
}

TEST(ContributionD21b, ConfiguredDeniedTokenIsTheOneSent) {
    const auto custom = draft21_contribution_probes(std::chrono::milliseconds{1000}, {}, cbytes({'x'}), "custom-denied");
    const auto& probe = find_probe(custom, "d21-namespace-discovery-authorization");
    const auto& bytes = probe.definition.writes[0].bytes;
    const std::string needle = "custom-denied";
    const auto needle_bytes = text(needle);
    EXPECT_NE(std::search(bytes.begin(), bytes.end(), needle_bytes.begin(), needle_bytes.end()), bytes.end());
    ContributionRun granted(probe);
    granted.deliver(0);
    granted.reply(granted.stream_of(0), request_ok());
    EXPECT_EQ(evaluate(granted, probe, needle), false);
    // A transcript naming another token does not match this stimulus.
    EXPECT_EQ(evaluate(granted, probe, "other"), std::nullopt);
}

// ---- Section 9.20.3: the subscriber's credential is not copied ----------------------
Bytes peer_publish_with(const Bytes& params_block) {
    // PUBLISH: request 2, namespace (a), track "t", alias 7, Parameters.
    return cframe(0x1d, cconcat({cvi(2), cvi(1), cvi(1), cbytes({'a'}), cvi(1), cbytes({'t'}), cvi(7), params_block}));
}
Bytes token_param(unsigned alias_type, std::optional<unsigned> alias, std::optional<unsigned> type, const std::string& value) {
    Bytes token = cvi(alias_type);
    if (alias) token = cconcat({token, cvi(*alias)});
    if (type) token = cconcat({token, cvi(*type), text(value)});
    return cconcat({cvi(token.size()), token});
}

TEST(ContributionD21b, PublishFromSubscribeTracksDoesNotCarryTheSubscribersToken) {
    const auto& probe = find_probe(probes(), "d21-track-discovery-does-not-copy-authorization");
    const std::string credential = "interop-subscriber-credential";
    const auto request = probe.definition.writes[0].bytes;
    const auto credential_bytes = text(credential);
    EXPECT_NE(std::search(request.begin(), request.end(), credential_bytes.begin(), credential_bytes.end()),
              request.end());
    const auto run_with = [&](const Bytes& params, bool accepted) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), accepted ? request_ok() : request_error(0x1));
        run.reply(0, peer_publish_with(params));
        return run;
    };
    // No token, a publisher-owned token, or another value: not a copy.
    EXPECT_EQ(evaluate(run_with(cbytes({0}), true), probe), true);
    EXPECT_EQ(evaluate(run_with(cconcat({cbytes({1, 3}), token_param(3, std::nullopt, 0, "publisher-token")}), true), probe), true);
    // The same credential by value or through registration is a copy.
    EXPECT_EQ(evaluate(run_with(cconcat({cbytes({1, 3}), token_param(3, std::nullopt, 0, credential)}), true), probe), false);
    EXPECT_EQ(evaluate(run_with(cconcat({cbytes({1, 3}), token_param(1, 9, 0, credential)}), true), probe), false);
    // Same alias integer but another value: aliases are per direction, so no copy.
    EXPECT_EQ(evaluate(run_with(cconcat({cbytes({1, 3}), token_param(1, 1, 0, "other")}), true), probe), true);
    // An unresolved alias proves nothing either way.
    EXPECT_EQ(evaluate(run_with(cconcat({cbytes({1, 3}), token_param(2, 1, std::nullopt, "")}), true), probe), true);
    // No PUBLISH without an accepted SUBSCRIBE_TRACKS: unscored.
    EXPECT_EQ(evaluate(run_with(cbytes({0}), false), probe), std::nullopt);
    ContributionRun silent(probe);
    silent.deliver(0);
    silent.reply(silent.stream_of(0), request_ok());
    EXPECT_EQ(evaluate(silent, probe), std::nullopt);
}

// ---- Section 11.3.2: early Subgroup termination -------------------------------------
ContributionRun early_run(const Draft21ContributionProbe& probe, const Bytes& first_stream, bool fin) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    run.reply(kData1, first_stream, fin);
    return run;
}

TEST(ContributionD21b, StreamEndedEarlyIsResetNeverFinished) {
    const auto& probe = find_probe(probes(), "d21-subgroup-early-handoff-reset");
    ASSERT_EQ(probe.definition.writes.size(), 3u);
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({2, 0, 4, 3, 1, 0x10, 0}));
    EXPECT_EQ(probe.definition.writes[2].bytes, cbytes({2, 0, 4, 5, 1, 0x10, 1}));
    const Bytes open_stream = subgroup(5, 1, kObject);
    // No stream open yet: the pause is not sent.
    ContributionRun gated(probe);
    gated.deliver(0);
    gated.reply(gated.stream_of(0), subscribe_ok(5));
    EXPECT_FALSE(probe.definition.writes[1].evidence_ready({gated.snapshot().writes, gated.snapshot().events}));

    const auto paused = [&](ContributionRun run) {
        EXPECT_TRUE(probe.definition.writes[1].evidence_ready({run.snapshot().writes, run.snapshot().events}));
        run.deliver(1);
        run.reply(run.stream_of(0), request_ok());
        EXPECT_TRUE(probe.definition.writes[2].evidence_ready({run.snapshot().writes, run.snapshot().events}));
        run.deliver(2);
        return run;
    };
    // The open stream is reset after the pause: the rule holds.
    auto reset = paused(early_run(probe, open_stream, false));
    reset.event(transport::PeerResetEvent{kData1, 0});
    EXPECT_EQ(evaluate(reset, probe), true);
    // It is closed with FIN although the same Subgroup continues on a later stream.
    auto finished = paused(early_run(probe, open_stream, false));
    finished.reply(kData1, {}, true);
    finished.reply(kData2, subgroup(5, 1, object_data(2, cbytes({'b'}))));
    EXPECT_EQ(evaluate(finished, probe), false);
    // A FIN with no continuation proves nothing: the Subgroup may have been complete.
    auto quiet = paused(early_run(probe, open_stream, false));
    quiet.reply(kData1, {}, true);
    EXPECT_EQ(evaluate(quiet, probe), std::nullopt);
    // Another Subgroup or Group is not a continuation.
    auto other = paused(early_run(probe, open_stream, false));
    other.reply(kData1, {}, true);
    other.reply(kData2, subgroup(5, 2, object_data(2, cbytes({'b'}))));
    EXPECT_EQ(evaluate(other, probe), std::nullopt);
    // A stream that already carried End of Group is complete, not terminated early.
    const Bytes complete = subgroup(5, 1, cconcat({kObject, cconcat({cvi(0), cvi(0), cvi(3)})}));
    EXPECT_FALSE(probe.definition.writes[1].evidence_ready(
        {early_run(probe, complete, false).snapshot().writes, early_run(probe, complete, false).snapshot().events}));
}

// ---- Sections 3.3.2, 10.7: property filters -----------------------------------------
Bytes numeric(std::uint64_t delta, std::uint64_t value) { return cconcat({cvi(delta), cvi(value)}); }
Bytes bytes_pair(std::uint64_t delta, const Bytes& value) { return cconcat({cvi(delta), cvi(value.size()), value}); }

ContributionRun filter_run(const Draft21ContributionProbe& probe) { return ContributionRun(probe, peer_setup(6, 4)); }

TEST(ContributionD21b, PropertyFilterNeedsAdvertisedRangesAndSearchesBothLists) {
    for (const bool immutable : {false, true}) {
        const auto& probe = find_probe(probes(),
            immutable ? "d21-filter-immutable-property" : "d21-filter-mutable-property");
        // The publisher must advertise MAX_FILTER_RANGES.
        EXPECT_FALSE(probe.definition.peer_setup_ready(cbytes({0xaf, 0, 0, 0})));
        EXPECT_FALSE(probe.definition.peer_setup_ready(peer_setup(6, 0)));
        EXPECT_TRUE(probe.definition.peer_setup_ready(peer_setup(6, 4)));

        // Property 16 (even, one integer) with value 6 in exactly one list.
        const Bytes properties = immutable ? bytes_pair(11, numeric(16, 6)) : numeric(16, 6);
        const Bytes object = object_data(0, cbytes({'a'}), properties, true);
        auto run = filter_run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok(5));
        run.reply(kData1, subgroup(5, 1, object, 0x31));
        run.deliver(1);
        // SetID 0, Property Type 16, Range 6..6.
        const auto& filter = probe.definition.writes[0].bytes;
        EXPECT_EQ(run.snapshot().writes[1].write.bytes,
                  cbytes({3, 0, 13, 3, 0, 1, 'x', 2, 0x10, 1, 0x18, 4, 0, 16, 6, 0}));
        (void)filter;
        run.reply(run.stream_of(1), subscribe_ok(6));
        run.reply(kData2, subgroup(6, 1, object, 0x31));
        EXPECT_EQ(evaluate(run, probe), true);

        // The matching Objects keep flowing on the unfiltered subscription but
        // none passes the filter, and the session then ends.
        auto starved = filter_run(probe);
        starved.deliver(0);
        starved.reply(starved.stream_of(0), subscribe_ok(5));
        starved.reply(kData1, subgroup(5, 1, object, 0x31));
        starved.deliver(1);
        starved.reply(starved.stream_of(1), subscribe_ok(6));
        starved.reply(14, subgroup(5, 2, object, 0x31));
        starved.reply(18, subgroup(5, 3, object, 0x31));
        close_with(starved, 0);
        EXPECT_EQ(evaluate(starved, probe), false);
        // Without that evidence a quiet filtered subscription is unscored.
        auto quiet = filter_run(probe);
        quiet.deliver(0);
        quiet.reply(quiet.stream_of(0), subscribe_ok(5));
        quiet.reply(kData1, subgroup(5, 1, object, 0x31));
        quiet.deliver(1);
        quiet.reply(quiet.stream_of(1), subscribe_ok(6));
        close_with(quiet, 0);
        EXPECT_EQ(evaluate(quiet, probe), std::nullopt);
        // The property sits in the other list only for the sibling scenario.
        const Bytes other_properties = immutable ? numeric(16, 6) : bytes_pair(11, numeric(16, 6));
        auto wrong_list = filter_run(probe);
        wrong_list.deliver(0);
        wrong_list.reply(wrong_list.stream_of(0), subscribe_ok(5));
        wrong_list.reply(kData1, subgroup(5, 1, object_data(0, cbytes({'a'}), other_properties, true), 0x31));
        EXPECT_FALSE(probe.definition.writes[1].prepare_bytes != nullptr &&
                     probe.definition.writes[1].prepare_bytes(
                         {wrong_list.snapshot().writes, wrong_list.snapshot().events}).has_value());
    }
}

// ---- Section 13: unknown GREASE values ------------------------------------------------
TEST(ContributionD21b, UnknownAuthTokenTypeIsHandledWithoutEndingTheSession) {
    const auto& probe = find_probe(probes(), "d21-grease-auth-token-type", "D21-13-MUST-593");
    const auto& strict = find_probe(probes(), "d21-grease-auth-token-type", "D21-13-MUST-NOT-594");
    EXPECT_EQ(probe.evaluator_id, "d21-grease-publisher-context-handling");
    EXPECT_EQ(strict.evaluator_id, "d21-grease-no-unknown-value-session-close");
    // Token Type 0x9D by value, then a fresh request.
    EXPECT_EQ(probe.definition.writes[0].bytes,
              cbytes({3, 0, 19, 1, 0, 1, 'x', 2, 3, 0x0a, 3, 0x80, 0x9d, 'i', 'n', 't', 'e', 'r', 'o', 'p', 0x0d, 1}));
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({3, 0, 7, 3, 0, 1, 'x', 1, 0x10, 0}));
    for (const Bytes& rejection : {request_error(0x4), request_error(0x1), subscribe_ok(5)}) {
        for (const auto* row : {&probe, &strict}) {
            ContributionRun run(*row);
            run.deliver(0);
            run.deliver(1);
            run.reply(run.stream_of(0), rejection);
            EXPECT_FALSE(row->definition.response_ready(run.partial()));
            run.reply(run.stream_of(1), subscribe_ok(6));
            EXPECT_TRUE(row->definition.response_ready(run.partial()));
            EXPECT_EQ(evaluate(run, *row), true);
        }
    }
    // The session ending with an error is not a pass, and for the closing rule a failure.
    ContributionRun closed(probe);
    closed.deliver(0);
    closed.deliver(1);
    close_with(closed, 3);
    EXPECT_EQ(evaluate(closed, probe), std::nullopt);
    EXPECT_EQ(evaluate(closed, strict), false);
    // NO_ERROR (0) closes prove nothing about the unknown value.
    ContributionRun graceful(strict);
    graceful.deliver(0);
    graceful.deliver(1);
    close_with(graceful, 0);
    EXPECT_EQ(evaluate(graceful, strict), std::nullopt);
}

TEST(ContributionD21b, UnknownStopSendingCodeCancelsTheStreamAndKeepsTheSession) {
    const auto& handled = find_probe(probes(), "d21-grease-stop-sending", "D21-13-MUST-593");
    const auto& kept = find_probe(probes(), "d21-grease-stop-sending", "D21-13-MUST-NOT-594");
    EXPECT_EQ(handled.evaluator_id, "d21-unknown-stop-sending-graceful-handling");
    EXPECT_EQ(kept.evaluator_id, "d21-unknown-stop-sending-preserves-session");
    ASSERT_EQ(kept.definition.writes.size(), 3u);
    const auto& stop = kept.definition.writes[1];
    EXPECT_EQ(stop.operation, RawProbeOperation::StopSending);
    EXPECT_EQ(stop.application_error, 0x9du);
    EXPECT_TRUE(static_cast<bool>(stop.select_peer_stream));

    const auto prepared = [&](const Draft21ContributionProbe& probe) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok(5));
        // No data stream yet: nothing to cancel.
        EXPECT_FALSE(stop.select_peer_stream({run.snapshot().writes, run.snapshot().events}).has_value());
        run.reply(kData1, subgroup(5, 1, kObject));
        EXPECT_EQ(stop.select_peer_stream({run.snapshot().writes, run.snapshot().events}), kData1);
        run.deliver(1);
        run.deliver(2);
        return run;
    };
    for (const auto* row : {&handled, &kept}) {
        auto run = prepared(*row);
        run.event(transport::PeerResetEvent{kData1, 0x9d});
        run.reply(run.stream_of(2), subscribe_ok(6));
        EXPECT_TRUE(row->definition.response_ready(run.partial()));
        EXPECT_EQ(evaluate(run, *row), true);
        // The affected stream ending is not required; the session staying usable is.
        auto still_open = prepared(*row);
        still_open.reply(still_open.stream_of(2), request_error(0x10));
        EXPECT_EQ(evaluate(still_open, *row), true);
    }
    // An error close after the STOP_SENDING and before any answer ends the session
    // over the unknown code; a close from before it does not count.
    auto closed = prepared(kept);
    close_with(closed, 3);
    EXPECT_EQ(evaluate(closed, kept), false);
    auto closed_handled = prepared(handled);
    close_with(closed_handled, 3);
    EXPECT_EQ(evaluate(closed_handled, handled), std::nullopt);
    // A finished stream is never targeted.
    ContributionRun finished(kept);
    finished.deliver(0);
    finished.reply(finished.stream_of(0), subscribe_ok(5));
    finished.reply(kData1, subgroup(5, 1, kObject), true);
    EXPECT_FALSE(stop.select_peer_stream({finished.snapshot().writes, finished.snapshot().events}).has_value());
}

TEST(ContributionD21b, GreaseSetupOptionsAndRequestErrorContextsFeedTheSessionRows) {
    const auto& setup_handled = find_probe(probes(), "d21-grease-setup-options", "D21-13-MUST-593");
    const auto& setup_kept = find_probe(probes(), "d21-grease-setup-options", "D21-13-MUST-NOT-594");
    for (const auto* row : {&setup_handled, &setup_kept}) {
        ContributionRun run(*row);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok());
        EXPECT_EQ(evaluate(run, *row), true);
    }
    ContributionRun closed(setup_kept);
    closed.deliver(0);
    close_with(closed, 3);
    EXPECT_EQ(evaluate(closed, setup_kept), false);
    ContributionRun closed_handled(setup_handled);
    closed_handled.deliver(0);
    close_with(closed_handled, 3);
    EXPECT_EQ(evaluate(closed_handled, setup_handled), std::nullopt);

    const auto& error_handled = find_probe(probes(), "d21-grease-request-error", "D21-13-MUST-593");
    const auto& error_kept = find_probe(probes(), "d21-grease-request-error", "D21-13-MUST-NOT-594");
    for (const auto* row : {&error_handled, &error_kept}) {
        ContributionRun run(*row);
        run.reply(0, cframe(0x1d, cbytes({0, 1, 1, 'n', 1, 't', 5, 0})));
        run.deliver(0);
        run.deliver(1);
        run.reply(run.stream_of(1), request_error(0x10));
        EXPECT_EQ(evaluate(run, *row), true);
    }
    ContributionRun error_closed(error_kept);
    error_closed.reply(0, cframe(0x1d, cbytes({0, 1, 1, 'n', 1, 't', 5, 0})));
    error_closed.deliver(0);
    error_closed.deliver(1);
    close_with(error_closed, 3);
    EXPECT_EQ(evaluate(error_closed, error_kept), false);
}

}  // namespace
}  // namespace moq::interop::scenarios
