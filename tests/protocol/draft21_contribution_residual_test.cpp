#include "../support/contribution_transcript.h"

#include "moq/interop/app/scenario_registry.h"

#include <gtest/gtest.h>

#include <string>

// Hand-derived expectations for the remaining-rows profiles. The expected bytes
// and verdicts come from draft-ietf-moq-transport-21, not from the code under
// test; each test names the draft lines it relies on.
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

// Subgroup stream: type 0x30 (no priority field, Subgroup ID 0) or 0x34
// (explicit Subgroup ID), Track Alias, Group, then Objects.
Bytes subgroup(std::uint64_t alias, std::uint64_t group, const Bytes& objects,
               std::optional<std::uint64_t> subgroup_id = std::nullopt) {
    if (!subgroup_id) return cconcat({cvi(0x30), cvi(alias), cvi(group), objects});
    return cconcat({cvi(0x34), cvi(alias), cvi(group), cvi(*subgroup_id), objects});
}
Bytes object_data(std::uint64_t delta, const Bytes& payload) {
    return cconcat({cvi(delta), cvi(payload.size()), payload});
}
Bytes payload() { return cbytes({'o'}); }

// A Subgroup stream carrying the listed Object IDs in order.
Bytes objects_with_ids(std::initializer_list<std::uint64_t> ids) {
    Bytes result;
    std::optional<std::uint64_t> previous;
    for (const auto id : ids) {
        result = cconcat({result, object_data(previous ? id - *previous - 1 : id, payload())});
        previous = id;
    }
    return result;
}

constexpr transport::StreamId kData1 = 6;
constexpr transport::StreamId kData2 = 10;
constexpr transport::StreamId kData3 = 14;

// A context whose observation window has ended.
RawProbeTranscript windowed(ContributionRun run) {
    auto result = run.finish();
    result.complete = false;
    result.timed_out = true;
    return result;
}

std::optional<bool> judge(const Draft21ContributionProbe& probe, const RawProbeTranscript& transcript) {
    return evaluate_draft21_contribution_probe(transcript, probe);
}

TEST(ContributionResidual, RegistryAndBindingsCoverTheNewScenarios) {
    for (const char* scenario :
         {"d21-overlapping-subscriptions-shared-alias", "d21-overlapping-subscriptions-distinct-aliases",
          "d21-forward-location-and-range-filter-conjunction", "d21-subscribe-multiple-subgroups",
          "d21-fill-fails-before-first-object", "d21-cancel-subscription-with-concurrent-fill-streams"}) {
        EXPECT_TRUE(app::draft21_contribution_scenario(21, scenario)) << scenario;
        EXPECT_TRUE(app::raw_probe_scenario(21, scenario)) << scenario;
        EXPECT_NO_THROW(find_probe(probes(), scenario)) << scenario;
        // Every context asks for the unscored PUBLISH_NAMESPACE acknowledgement.
        EXPECT_TRUE(find_probe(probes(), scenario).definition.acknowledge_publisher_namespaces) << scenario;
    }
}

// ---- Section 3.1 lines 1021-1028: one copy per matching subscription -----------
TEST(ContributionResidual, ConcurrentSubscriptionsAskForExactlyTheSameObject) {
    const auto& probe = find_probe(probes(), "d21-overlapping-subscriptions-shared-alias");
    EXPECT_EQ(probe.requirement_id, "D21-3-1-MUST-041");
    EXPECT_EQ(probe.evaluator_id, "d21-object-per-matching-subscription");
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    // Request IDs 1 and 3, FORWARD=1 and a Location filter of Group 0, Object 1 to itself.
    EXPECT_EQ(probe.definition.writes[0].bytes,
              cbytes({3, 0, 13, 1, 0, 1, 'x', 2, 0x10, 1, 0x11, 4, 0, 1, 0, 1}));
    EXPECT_EQ(probe.definition.writes[1].bytes,
              cbytes({3, 0, 13, 3, 0, 1, 'x', 2, 0x10, 1, 0x11, 4, 0, 1, 0, 1}));
}

TEST(ContributionResidual, SharedAliasNeedsTwoCopies) {
    const auto& probe = find_probe(probes(), "d21-overlapping-subscriptions-shared-alias");
    const auto run = [&](int copies) {
        ContributionRun result(probe);
        result.deliver(0);
        result.deliver(1);
        result.reply(result.stream_of(0), subscribe_ok(5));
        result.reply(result.stream_of(1), subscribe_ok(5));
        const transport::StreamId streams[] = {kData1, kData2, kData3};
        for (int copy = 0; copy < copies; ++copy)
            result.reply(streams[copy], subgroup(5, 0, objects_with_ids({1})), true);
        return result;
    };
    EXPECT_EQ(judge(probe, windowed(run(2))), true);
    // One subscription never received its copy.
    EXPECT_EQ(judge(probe, windowed(run(1))), false);
    // Nothing delivered at all means the fixture was unavailable.
    EXPECT_EQ(judge(probe, windowed(run(0))), std::nullopt);
    // A surplus copy is conclusive without waiting for the window.
    EXPECT_FALSE(probe.definition.response_ready(run(2).partial()));
    EXPECT_TRUE(probe.definition.response_ready(run(3).partial()));
    EXPECT_EQ(judge(probe, run(3).finish()), false);
    // Before the window ends two copies are not yet a verdict.
    EXPECT_EQ(judge(probe, run(2).finish()), std::nullopt);

    // Distinct aliases are the other scenario's case.
    ContributionRun distinct(probe);
    distinct.deliver(0);
    distinct.deliver(1);
    distinct.reply(distinct.stream_of(0), subscribe_ok(5));
    distinct.reply(distinct.stream_of(1), subscribe_ok(6));
    EXPECT_TRUE(probe.definition.response_ready(distinct.partial()));
    EXPECT_EQ(judge(probe, distinct.finish()), std::nullopt);

    ContributionRun refused(probe);
    refused.deliver(0);
    refused.deliver(1);
    refused.reply(refused.stream_of(0), subscribe_ok(5));
    refused.reply(refused.stream_of(1), request_error(0x11), true);
    EXPECT_EQ(judge(probe, windowed(refused)), std::nullopt);
}

TEST(ContributionResidual, DistinctAliasesNeedOneCopyEach) {
    const auto& probe = find_probe(probes(), "d21-overlapping-subscriptions-distinct-aliases");
    const auto run = [&](std::initializer_list<std::pair<std::uint64_t, transport::StreamId>> deliveries) {
        ContributionRun result(probe);
        result.deliver(0);
        result.deliver(1);
        result.reply(result.stream_of(0), subscribe_ok(5));
        result.reply(result.stream_of(1), subscribe_ok(6));
        for (const auto& [alias, stream] : deliveries)
            result.reply(stream, subgroup(alias, 0, objects_with_ids({1})), true);
        return result;
    };
    EXPECT_EQ(judge(probe, windowed(run({{5, kData1}, {6, kData2}}))), true);
    EXPECT_EQ(judge(probe, windowed(run({{5, kData1}}))), false);
    EXPECT_EQ(judge(probe, windowed(run({{6, kData2}}))), false);
    EXPECT_EQ(judge(probe, windowed(run({}))), std::nullopt);
    // One alias carrying the Object twice is a surplus for that subscription.
    EXPECT_EQ(judge(probe, run({{5, kData1}, {5, kData2}}).finish()), false);
    // Objects at another Location do not count.
    ContributionRun elsewhere(probe);
    elsewhere.deliver(0);
    elsewhere.deliver(1);
    elsewhere.reply(elsewhere.stream_of(0), subscribe_ok(5));
    elsewhere.reply(elsewhere.stream_of(1), subscribe_ok(6));
    elsewhere.reply(kData1, subgroup(5, 0, objects_with_ids({0})), true);
    elsewhere.reply(kData2, subgroup(6, 0, objects_with_ids({0})), true);
    EXPECT_EQ(judge(probe, windowed(elsewhere)), std::nullopt);
    // A shared alias is the other scenario's case.
    ContributionRun shared(probe);
    shared.deliver(0);
    shared.deliver(1);
    shared.reply(shared.stream_of(0), subscribe_ok(5));
    shared.reply(shared.stream_of(1), subscribe_ok(5));
    EXPECT_EQ(judge(probe, shared.finish()), std::nullopt);
}

TEST(ContributionResidual, DatagramCopiesCountToo) {
    const auto& probe = find_probe(probes(), "d21-overlapping-subscriptions-shared-alias");
    ContributionRun run(probe);
    run.deliver(0);
    run.deliver(1);
    run.reply(run.stream_of(0), subscribe_ok(5));
    run.reply(run.stream_of(1), subscribe_ok(5));
    // Object Datagram: type 0x08 (no priority), alias, group, object, payload.
    run.event(transport::DatagramEvent{cconcat({cvi(0x08), cvi(5), cvi(0), cvi(1), payload()})});
    run.reply(kData1, subgroup(5, 0, objects_with_ids({1})), true);
    EXPECT_EQ(judge(probe, windowed(run)), true);
}

// ---- Section 3.3.3 lines 1264-1272: Forward AND Location AND Range ------------------
TEST(ContributionResidual, FilterConjunctionStimulus) {
    const auto& probe = find_probe(probes(), "d21-forward-location-and-range-filter-conjunction");
    EXPECT_EQ(probe.requirement_id, "D21-3-3-3-MUST-066");
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    // Request 1: FORWARD=0, LOCATION_FILTER {0,0} to {1,1} (Group delta 1).
    EXPECT_EQ(probe.definition.writes[0].bytes,
              cbytes({3, 0, 13, 1, 0, 1, 'x', 2, 0x10, 0, 0x11, 4, 0, 0, 1, 1}));
    // Request 3 is built from the peer's SETUP: FORWARD=1, LOCATION_FILTER {0,0} to
    // {1,0}, plus an OBJECTID_FILTER (type delta 5 from 0x21) of SetID 0 and Range
    // 1-1 (Start 1, End delta 0, Section 8.6) only when MAX_FILTER_RANGES is non-zero.
    const auto prepared = [&](const Bytes& peer_setup) {
        ContributionRun run(probe, peer_setup);
        run.deliver(0);
        run.deliver(1);
        return run.snapshot().writes[1].write.bytes;
    };
    EXPECT_EQ(prepared(cbytes({0xaf, 0, 0, 2, 6, 2})),
              cbytes({3, 0, 18, 3, 0, 1, 'x', 3, 0x10, 1, 0x11, 4, 0, 0, 1, 0, 5, 3, 0, 1, 0}));
    EXPECT_EQ(prepared(cbytes({0xaf, 0, 0, 0})),
              cbytes({3, 0, 13, 3, 0, 1, 'x', 2, 0x10, 1, 0x11, 4, 0, 0, 1, 0}));
    EXPECT_EQ(prepared(cbytes({0xaf, 0, 0, 2, 6, 0})),
              cbytes({3, 0, 13, 3, 0, 1, 'x', 2, 0x10, 1, 0x11, 4, 0, 0, 1, 0}));
}

ContributionRun conjunction_run(const Draft21ContributionProbe& probe, const Bytes& peer_setup,
                                std::uint64_t forward_off_alias, std::uint64_t filtered_alias) {
    ContributionRun run(probe, peer_setup);
    run.deliver(0);
    run.deliver(1);
    run.reply(run.stream_of(0), subscribe_ok(forward_off_alias));
    run.reply(run.stream_of(1), subscribe_ok(filtered_alias));
    return run;
}

TEST(ContributionResidual, OnlyObjectsPassingEveryFilterAreForwarded) {
    const auto& probe = find_probe(probes(), "d21-forward-location-and-range-filter-conjunction");
    const auto ranged = cbytes({0xaf, 0, 0, 2, 6, 2});
    const auto run = [&](const Bytes& setup, std::uint64_t group, std::initializer_list<std::uint64_t> ids,
                         std::uint64_t alias = 6) {
        auto result = conjunction_run(probe, setup, 5, 6);
        result.reply(kData1, subgroup(alias, group, objects_with_ids(ids)), true);
        return result;
    };
    // Only {0,1} passes Forward, the Location filter and the OBJECTID_FILTER.
    EXPECT_EQ(judge(probe, windowed(run(ranged, 0, {1}))), true);
    // {0,0} passes the Location filter but not the range filter.
    EXPECT_EQ(judge(probe, windowed(run(ranged, 0, {0, 1}))), false);
    // {1,0} is the same, in the Location filter's last Group.
    EXPECT_EQ(judge(probe, windowed(run(ranged, 1, {0}))), false);
    // {1,1} passes the range filter but lies past the Location filter's end.
    EXPECT_EQ(judge(probe, windowed(run(ranged, 1, {1}))), false);
    // Without range filters (MAX_FILTER_RANGES omitted) the Location filter alone applies.
    const auto plain = cbytes({0xaf, 0, 0, 0});
    EXPECT_EQ(judge(probe, windowed(run(plain, 0, {0, 1}))), true);
    EXPECT_EQ(judge(probe, windowed(run(plain, 1, {0}))), true);
    EXPECT_EQ(judge(probe, windowed(run(plain, 1, {1}))), false);
    // Forward is a filter: the FORWARD=0 subscription's own alias must stay silent.
    auto leaked = conjunction_run(probe, ranged, 5, 6);
    leaked.reply(kData1, subgroup(5, 0, objects_with_ids({1})), true);
    leaked.reply(kData2, subgroup(6, 0, objects_with_ids({1})), true);
    EXPECT_EQ(judge(probe, windowed(leaked)), false);
    // A shared alias cannot attribute, but {1,1} is outside every pass set.
    auto shared = conjunction_run(probe, ranged, 6, 6);
    shared.reply(kData1, subgroup(6, 0, objects_with_ids({1})), true);
    EXPECT_EQ(judge(probe, windowed(shared)), true);
    shared.reply(kData2, subgroup(6, 1, objects_with_ids({1})), true);
    EXPECT_EQ(judge(probe, windowed(shared)), false);
    // A violation ends the context at once; success waits for the window.
    EXPECT_TRUE(probe.definition.response_ready(run(ranged, 1, {1}).partial()));
    EXPECT_FALSE(probe.definition.response_ready(run(ranged, 0, {1}).partial()));
    EXPECT_EQ(judge(probe, run(ranged, 0, {1}).finish()), std::nullopt);
    // Nothing forwarded is not evidence, and a rejected request ends the context.
    EXPECT_EQ(judge(probe, windowed(conjunction_run(probe, ranged, 5, 6))), std::nullopt);
    ContributionRun rejected(probe, ranged);
    rejected.deliver(0);
    rejected.deliver(1);
    rejected.reply(rejected.stream_of(0), subscribe_ok(5));
    rejected.reply(rejected.stream_of(1), request_error(0x10), true);
    EXPECT_TRUE(probe.definition.response_ready(rejected.partial()));
    EXPECT_EQ(judge(probe, rejected.finish()), std::nullopt);
}

// ---- Section 2.2 lines 719-722: one Subgroup per subscription stream -------------------
TEST(ContributionResidual, SubgroupStimulusIsTheWholeGroup) {
    const auto& probe = find_probe(probes(), "d21-subscribe-multiple-subgroups");
    EXPECT_EQ(probe.requirement_id, "D21-2-2-MUST-NOT-017");
    ASSERT_EQ(probe.definition.writes.size(), 1u);
    // Start {0,0}, End Group delta 0 and no End Object: all of Group 0 (Section 9.20.10).
    EXPECT_EQ(probe.definition.writes[0].bytes,
              cbytes({3, 0, 12, 1, 0, 1, 'x', 2, 0x10, 1, 0x11, 3, 0, 0, 0}));
}

TEST(ContributionResidual, ObjectsOfTwoSubgroupsNeverShareAStream) {
    const auto& probe = find_probe(probes(), "d21-subscribe-multiple-subgroups");
    const auto run = [&](const Bytes& first, const Bytes& second) {
        ContributionRun result(probe);
        result.deliver(0);
        result.reply(result.stream_of(0), subscribe_ok(5));
        if (!first.empty()) result.reply(kData1, first, true);
        if (!second.empty()) result.reply(kData2, second, true);
        return result;
    };
    // Objects 0-4 in Subgroup 0 and 5-9 in Subgroup 1, each on its own stream.
    EXPECT_EQ(judge(probe, windowed(run(subgroup(5, 0, objects_with_ids({0, 1, 2, 3, 4})),
                                        subgroup(5, 0, objects_with_ids({5, 6, 7, 8, 9}), 1)))), true);
    // Subgroup ID given by the first Object (type 0x32) is equally fine.
    EXPECT_EQ(judge(probe, windowed(run(subgroup(5, 0, objects_with_ids({0, 1})),
                                        cconcat({cvi(0x32), cvi(5), cvi(0), objects_with_ids({5, 6})})))), true);
    // Objects from both Subgroups on one stream mix them.
    EXPECT_EQ(judge(probe, windowed(run(subgroup(5, 0, objects_with_ids({3, 6})), {}))), false);
    EXPECT_TRUE(probe.definition.response_ready(run(subgroup(5, 0, objects_with_ids({3, 6})), {}).partial()));
    EXPECT_EQ(judge(probe, run(subgroup(5, 0, objects_with_ids({3, 6})), {}).finish()), false);
    // A single Subgroup never exercises the split.
    EXPECT_EQ(judge(probe, windowed(run(subgroup(5, 0, objects_with_ids({0, 1, 2})), {}))), std::nullopt);
    // Another Track Alias is not this subscription.
    EXPECT_EQ(judge(probe, windowed(run(subgroup(9, 0, objects_with_ids({3, 6})), {}))), std::nullopt);
    // Objects outside the fixture's Subgroup map are not placed.
    EXPECT_EQ(judge(probe, windowed(run(subgroup(5, 0, objects_with_ids({3, 12})),
                                        subgroup(5, 0, objects_with_ids({6}), 1)))), true);
}

// ---- Section 3.4 lines 1272-1370: fill fetch streams ---------------------------------------
// A plain SUBSCRIBE (Request ID 1) makes the track live; the fill SUBSCRIBE
// (Request ID 3) carries FILL_PARAMETERS with a zero-length LOCATION_FILTER.
const Bytes kLargest = cbytes({1, 9, 0, 1});  // LARGEST_OBJECT {0, 1}

TEST(ContributionResidual, FillRequestWarmsUpThenCarriesFillParameters) {
    const auto& probe = find_probe(probes(), "d21-fill-fails-before-first-object", "D21-3-4-1-MUST-068");
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({3, 0, 7, 1, 0, 1, 'x', 1, 0x10, 1}));
    // FORWARD=1 and FILL_PARAMETERS (0x23, delta 0x13) holding a zero-length
    // LOCATION_FILTER (Section 9.20.16, Section 3.4 lines 1282-1288).
    EXPECT_EQ(probe.definition.writes[1].bytes,
              cbytes({3, 0, 11, 3, 0, 1, 'x', 2, 0x10, 1, 0x13, 2, 0x21, 0}));
    EXPECT_TRUE(static_cast<bool>(probe.definition.writes[1].evidence_ready));
    EXPECT_EQ(find_probe(probes(), "d21-fill-fails-before-first-object", "D21-3-4-1-MUST-069")
                  .evaluator_id, "d21-fill-failure-resets-after-fetch-header");
}

ContributionRun failed_fill_run(const Draft21ContributionProbe& probe, const Bytes& fill, bool fin,
                                std::optional<std::uint64_t> reset, const Bytes& response) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    run.reply(kData1, subgroup(5, 0, objects_with_ids({0})));
    run.deliver(1);
    run.reply(run.stream_of(1), response);
    if (!fill.empty()) run.reply(kData2, fill, fin);
    if (reset) run.event(transport::PeerResetEvent{kData2, reset});
    return run;
}

TEST(ContributionResidual, FailedFillIsAFetchHeaderThenAReset) {
    for (const char* row : {"D21-3-4-1-MUST-068", "D21-3-4-1-MUST-069"}) {
        const auto& probe = find_probe(probes(), "d21-fill-fails-before-first-object", row);
        const auto with_largest = subscribe_ok(5, kLargest);
        // FETCH_HEADER (type 5) for Request ID 3, then RESET_STREAM and nothing else.
        const auto header = cbytes({5, 3});
        EXPECT_EQ(judge(probe, failed_fill_run(probe, header, false, 0, with_largest).finish()), true) << row;
        EXPECT_TRUE(probe.definition.response_ready(
            failed_fill_run(probe, header, false, 0, with_largest).partial())) << row;
        // The stream is still open: no verdict yet.
        EXPECT_FALSE(probe.definition.response_ready(
            failed_fill_run(probe, header, false, std::nullopt, with_largest).partial())) << row;
        // A FIN is a completed fill; Objects before the reset are a failure after delivery began.
        EXPECT_EQ(judge(probe, failed_fill_run(probe, header, true, std::nullopt, with_largest).finish()),
                  std::nullopt) << row;
        EXPECT_EQ(judge(probe, failed_fill_run(probe, cconcat({header, cbytes({0x1c, 7, 0, 128, 1, 'o'})}),
                                               false, 0, with_largest).finish()), std::nullopt) << row;
        // The header must name the fill SUBSCRIBE's Request ID, not the warm-up's.
        EXPECT_EQ(judge(probe, failed_fill_run(probe, cbytes({5, 1}), false, 0, with_largest).finish()),
                  std::nullopt) << row;
        // No fill stream at all proves nothing: the failure may not have been necessary.
        EXPECT_EQ(judge(probe, failed_fill_run(probe, {}, false, std::nullopt, with_largest).finish()),
                  std::nullopt) << row;
        // Without a Largest Object the fill range is empty and no stream is owed.
        EXPECT_EQ(judge(probe, failed_fill_run(probe, header, false, 0, subscribe_ok(5)).finish()),
                  std::nullopt) << row;
        // Subgroup streams are not fill streams.
        EXPECT_EQ(judge(probe, failed_fill_run(probe, subgroup(5, 7, objects_with_ids({0})), false, 0,
                                               with_largest).finish()), std::nullopt) << row;
        EXPECT_EQ(judge(probe, failed_fill_run(probe, header, false, 0, request_error(0x11)).finish()),
                  std::nullopt) << row;
    }
}

TEST(ContributionResidual, FillWaitsForTheWarmUpSubscriptionToDeliver) {
    const auto& probe = find_probe(probes(), "d21-fill-fails-before-first-object");
    const auto& gate = probe.definition.writes[1].evidence_ready;
    ContributionRun silent(probe);
    silent.deliver(0);
    silent.reply(silent.stream_of(0), subscribe_ok(5));
    auto transcript = silent.partial();
    const auto prior = std::span<const RawProbeAcceptedWrite>(transcript.writes).first(1);
    EXPECT_FALSE(gate({prior, transcript.events}));
    silent.reply(kData1, subgroup(5, 0, objects_with_ids({0})));
    transcript = silent.partial();
    EXPECT_TRUE(gate({std::span<const RawProbeAcceptedWrite>(transcript.writes).first(1), transcript.events}));
    // An Object on another Track Alias is not this subscription's delivery.
    ContributionRun other(probe);
    other.deliver(0);
    other.reply(other.stream_of(0), subscribe_ok(5));
    other.reply(kData1, subgroup(9, 0, objects_with_ids({0})));
    transcript = other.partial();
    EXPECT_FALSE(gate({std::span<const RawProbeAcceptedWrite>(transcript.writes).first(1), transcript.events}));
}

// Lines 1352-1353: cancelling the subscription resets any open fill stream.
TEST(ContributionResidual, CancellationScenarioOpensTwoFillsThenStopsTheRequest) {
    const auto& probe = find_probe(probes(), "d21-cancel-subscription-with-concurrent-fill-streams");
    EXPECT_EQ(probe.requirement_id, "D21-3-4-1-MUST-067");
    ASSERT_EQ(probe.definition.writes.size(), 4u);
    EXPECT_EQ(probe.definition.writes[1].bytes,
              cbytes({3, 0, 11, 3, 0, 1, 'x', 2, 0x10, 1, 0x13, 2, 0x21, 0}));
    // REQUEST_UPDATE with Request ID 5, FORWARD=1 and the same fill, FIN afterwards.
    EXPECT_EQ(probe.definition.writes[2].bytes, cbytes({2, 0, 8, 5, 2, 0x10, 1, 0x13, 2, 0x21, 0}));
    EXPECT_TRUE(probe.definition.writes[2].fin);
    EXPECT_EQ(probe.definition.writes[3].operation, RawProbeOperation::StopSending);
    EXPECT_EQ(probe.definition.writes[3].reuse_write_stream, std::optional<std::size_t>{1});
}

ContributionRun cancelled_run(const Draft21ContributionProbe& probe, bool both_open) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    run.reply(kData3, subgroup(5, 0, objects_with_ids({0})));
    run.deliver(1);
    run.reply(run.stream_of(1), subscribe_ok(5, kLargest));
    run.reply(kData1, cbytes({5, 3, 0x1c, 7, 0, 128, 1, 'a'}));
    run.deliver(2);
    run.reply(run.stream_of(1), request_ok());
    if (both_open) run.reply(kData2, cbytes({5, 5, 0x1c, 7, 1, 128, 1, 'b'}));
    if (both_open) run.deliver(3);
    return run;
}

TEST(ContributionResidual, CancellationWaitsForBothFillStreamsToBeOpen) {
    const auto& probe = find_probe(probes(), "d21-cancel-subscription-with-concurrent-fill-streams");
    const auto& gate = probe.definition.writes[3].evidence_ready;
    ASSERT_TRUE(static_cast<bool>(gate));
    const auto readiness = [&](ContributionRun run) {
        const auto transcript = run.partial();
        return gate({std::span<const RawProbeAcceptedWrite>(transcript.writes).first(3), transcript.events});
    };
    EXPECT_FALSE(readiness(cancelled_run(probe, false)));
    EXPECT_TRUE(readiness(cancelled_run(probe, true)));
    // A fill that already ended is not open.
    auto closed = cancelled_run(probe, false);
    closed.reply(kData2, cbytes({5, 5}), true);
    EXPECT_FALSE(readiness(closed));
}

TEST(ContributionResidual, EveryOpenFillMustBeResetAfterCancellation) {
    const auto& probe = find_probe(probes(), "d21-cancel-subscription-with-concurrent-fill-streams");
    const auto all_reset = [&] {
        auto run = cancelled_run(probe, true);
        run.event(transport::PeerResetEvent{kData1, 0});
        run.event(transport::PeerResetEvent{kData2, 0});
        return run;
    };
    EXPECT_EQ(judge(probe, all_reset().finish()), true);
    EXPECT_TRUE(probe.definition.response_ready(all_reset().partial()));
    // One fill left open when the window ends: "MUST reset any open fill fetch streams".
    auto one_open = cancelled_run(probe, true);
    one_open.event(transport::PeerResetEvent{kData1, 0});
    EXPECT_FALSE(probe.definition.response_ready(one_open.partial()));
    EXPECT_EQ(judge(probe, windowed(one_open)), false);
    EXPECT_EQ(judge(probe, one_open.finish()), std::nullopt);
    // A FIN after the cancellation may have been committed before it arrived.
    auto finished = cancelled_run(probe, true);
    finished.event(transport::PeerResetEvent{kData1, 0});
    finished.reply(kData2, cbytes({0x1c, 7, 1, 128, 1, 'c'}), true);
    EXPECT_EQ(judge(probe, windowed(finished)), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
