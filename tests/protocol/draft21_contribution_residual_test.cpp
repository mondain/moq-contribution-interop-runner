#include "../support/contribution_transcript.h"

#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <gtest/gtest.h>

#include <filesystem>
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
          "d21-forward-location-and-range-filter-conjunction",
          "d21-fill-fails-before-first-object", "d21-cancel-subscription-with-concurrent-fill-streams",
          "d21-subscribe-tracks-publish-skipped-then-capacity-recovers",
          "d21-concurrent-distinct-track-subscriptions", "d21-publish-distinct-tracks-in-one-scope",
          "d21-reject-publish-before-object-production", "d21-rejected-subscribe-no-delivery",
          "d21-publisher-update-credit-limit", "d21-publisher-update-credit-per-stream",
          "d21-publisher-update-zero-unlimited", "d21-publisher-client-goaway-control",
          "d21-publisher-client-goaway-request", "d21-publisher-delete-with-pending-alias-uses",
          "d21-subgroup-completion-withheld-acknowledgments", "d21-request-well-formed-invalid-token",
          "d21-expired-token-alias-lifetime"}) {
        EXPECT_TRUE(app::draft21_contribution_scenario(21, scenario)) << scenario;
        EXPECT_TRUE(app::raw_probe_scenario(21, scenario)) << scenario;
        EXPECT_NO_THROW(find_probe(probes(), scenario)) << scenario;
        // Every context acknowledges a PUBLISH_NAMESPACE (the publisher-initiated ones say so below).
        EXPECT_TRUE(static_cast<bool>(find_probe(probes(), scenario).definition.auto_accept_ready)) << scenario;
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

// D21-3-1-MUST-041 lists both alias-assignment scenarios; the publisher's own Track Alias
// choice decides which one applies, so the other is unscored and one Pass settles the row.
TEST(ContributionResidual, CopyPerSubscriptionRowIsSettledByTheScenarioThatApplies) {
    using namespace requirements;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, root / "requirements/draft21.json");
    const auto state = [&](const std::vector<RawProbeTranscript>& transcripts) {
        const auto outcomes = evaluate_draft21_raw_probes(catalog, transcripts);
        const auto found = std::find_if(outcomes.begin(), outcomes.end(),
                                        [](const auto& outcome) { return outcome.requirement_id == "D21-3-1-MUST-041"; });
        return found == outcomes.end() ? OutcomeState::NotRun : found->state;
    };
    const auto& shared = find_probe(probes(), "d21-overlapping-subscriptions-shared-alias");
    const auto& distinct = find_probe(probes(), "d21-overlapping-subscriptions-distinct-aliases");
    const auto shared_run = [&](int copies) {
        ContributionRun run(shared);
        run.deliver(0);
        run.deliver(1);
        run.reply(run.stream_of(0), subscribe_ok(5));
        run.reply(run.stream_of(1), subscribe_ok(5));
        const transport::StreamId streams[] = {kData1, kData2};
        for (int copy = 0; copy < copies; ++copy)
            run.reply(streams[copy], subgroup(5, 0, objects_with_ids({1})), true);
        return windowed(std::move(run));
    };
    const auto distinct_run = [&](int copies) {
        ContributionRun run(distinct);
        run.deliver(0);
        run.deliver(1);
        run.reply(run.stream_of(0), subscribe_ok(5));
        run.reply(run.stream_of(1), subscribe_ok(6));
        if (copies > 0) run.reply(kData1, subgroup(5, 0, objects_with_ids({1})), true);
        if (copies > 1) run.reply(kData2, subgroup(6, 0, objects_with_ids({1})), true);
        return windowed(std::move(run));
    };
    // The only scenario present passes; the other one was not run.
    EXPECT_EQ(state({shared_run(2)}), OutcomeState::Pass);
    EXPECT_EQ(state({distinct_run(2)}), OutcomeState::Pass);
    // Both contexts ran, but only the one matching the publisher's alias choice is scored.
    EXPECT_EQ(state({shared_run(2), distinct_run(0)}), OutcomeState::Pass);
    // The only scenario present fails.
    EXPECT_EQ(state({shared_run(1)}), OutcomeState::Fail);
    EXPECT_EQ(state({distinct_run(1)}), OutcomeState::Fail);
    EXPECT_EQ(state({}), OutcomeState::NotRun);
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
    // Only the stream header has to have arrived: held credit can stop an Object short.
    ContributionRun header_only(probe);
    header_only.deliver(0);
    header_only.reply(header_only.stream_of(0), subscribe_ok(5));
    header_only.reply(kData1, cbytes({0x30, 5, 0}));
    transcript = header_only.partial();
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
    // Stream credit is held at 64 bytes so a fill longer than that stays open.
    EXPECT_EQ(probe.definition.initial_peer_uni_stream_data, std::optional<std::uint64_t>{64});
    EXPECT_TRUE(probe.definition.hold_uni_stream_credit);
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

// ---- Section 4.1 lines 1536-1545: PUBLISH_SKIPPED is not undone by a later PUBLISH ----------
Bytes publish_for(const Bytes& name, std::uint64_t request_id = 0) {
    return cframe(0x1d, cconcat({cvi(request_id), cbytes({1, 1, 'n'}), cvi(name.size()), name, cbytes({5, 0})}));
}
Bytes publish_skipped(const Bytes& name) {
    return cframe(0xf, cconcat({cbytes({1, 1, 'n'}), cvi(name.size()), name}));
}

TEST(ContributionResidual, SkippedPublishStimulusLimitsStreamsThenRejectsTheFirstPublish) {
    const auto& probe = find_probe(probes(), "d21-subscribe-tracks-publish-skipped-then-capacity-recovers");
    EXPECT_EQ(probe.requirement_id, "D21-4-1-MUST-NOT-084");
    EXPECT_EQ(probe.definition.initial_peer_bidi_streams, std::optional<std::uint64_t>{1});
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    // SUBSCRIBE_TRACKS (0x51), Request ID 1, empty namespace prefix, no parameters.
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({0x51, 0, 3, 1, 0, 0}));
    // REQUEST_ERROR UNINTERESTED (0x20) on the publisher's stream, then FIN.
    EXPECT_EQ(probe.definition.writes[1].channel, RawProbeChannel::PeerBidi);
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({5, 0, 3, 0x20, 0, 0}));
    EXPECT_TRUE(probe.definition.writes[1].fin);
    EXPECT_TRUE(probe.definition.peer_request_ready(publish_for(cbytes({'a'}))));
    EXPECT_FALSE(probe.definition.peer_request_ready(cbytes({0x1d, 0, 8})));
}

ContributionRun skipped_run(const Draft21ContributionProbe& probe) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), cconcat({request_ok(), publish_skipped(cbytes({'s'}))}));
    // The one PUBLISH the publisher could open, for another track, which the runner rejects.
    run.reply(0, publish_for(cbytes({'t'})));
    run.deliver(1);
    return run;
}

TEST(ContributionResidual, SkippedTrackMustNotBePublishedLater) {
    const auto& probe = find_probe(probes(), "d21-subscribe-tracks-publish-skipped-then-capacity-recovers");
    // Nothing further for the skipped track: the capacity hand-back is exercised and passes.
    EXPECT_EQ(judge(probe, windowed(skipped_run(probe))), true);
    // A PUBLISH for the skipped track after PUBLISH_SKIPPED breaks the rule.
    auto late = skipped_run(probe);
    late.reply(4, publish_for(cbytes({'s'}), 2));
    EXPECT_EQ(judge(probe, windowed(late)), false);
    EXPECT_TRUE(probe.definition.response_ready(late.partial()));
    // Before the window ends the absence is not proven; the violation needs no window.
    EXPECT_EQ(judge(probe, skipped_run(probe).finish()), std::nullopt);
    EXPECT_EQ(judge(probe, late.finish()), false);
    // A PUBLISH for a different track is permitted.
    auto other = skipped_run(probe);
    other.reply(4, publish_for(cbytes({'u'}), 2));
    EXPECT_EQ(judge(probe, windowed(other)), true);
    // A track published before it was skipped is not a later PUBLISH.
    ContributionRun before(probe);
    before.deliver(0);
    before.reply(0, publish_for(cbytes({'s'})));
    before.reply(before.stream_of(0), cconcat({request_ok(), publish_skipped(cbytes({'s'}))}));
    before.deliver(1);
    EXPECT_EQ(judge(probe, windowed(before)), true);
    // No PUBLISH_SKIPPED at all means the capacity limit never bit.
    ContributionRun silent(probe);
    silent.deliver(0);
    silent.reply(silent.stream_of(0), request_ok());
    EXPECT_FALSE(probe.definition.response_ready(silent.partial()));
    // A rejected SUBSCRIBE_TRACKS ends the context unscored.
    ContributionRun rejected(probe);
    rejected.deliver(0);
    rejected.reply(rejected.stream_of(0), request_error(0x30), true);
    EXPECT_TRUE(probe.definition.response_ready(rejected.partial()));
    EXPECT_EQ(judge(probe, rejected.finish()), std::nullopt);
}

// ---- Requests the publisher opens ----------------------------------------------------------
// Peer-initiated bidirectional streams are 0, 4, 8; the runner's volunteered answers are
// stamped with RawProbeCourtesyKind in the order the controller would write them.
using Kind = RawProbeCourtesyKind;

Bytes publish_body(std::uint64_t request_id, const Bytes& name, std::uint64_t alias, const Bytes& parameters) {
    return cconcat({cvi(request_id), cbytes({1, 1, 'n'}), cvi(name.size()), name, cvi(alias), parameters});
}
Bytes publish_with(const Bytes& name, std::uint64_t alias, std::uint64_t request_id = 0,
                   const Bytes& parameters = cbytes({0})) {
    return cframe(0x1d, publish_body(request_id, name, alias, parameters));
}
Bytes update_with(std::uint64_t request_id, const Bytes& parameters = cbytes({0})) {
    return cframe(0x2, cconcat({cvi(request_id), parameters}));
}
Bytes publish_done() { return cframe(0xb, cbytes({0, 0, 0})); }
// One AUTHORIZATION TOKEN parameter (0x03, length-prefixed), `token` being the Token structure.
Bytes token_parameter(const Bytes& token) {
    return cconcat({cbytes({1, 3}), cvi(token.size()), token});
}
Bytes register_token(std::uint64_t alias) { return cconcat({cvi(1), cvi(alias), cvi(0), cbytes({'k'})}); }
Bytes use_alias_token(std::uint64_t alias) { return cconcat({cvi(2), cvi(alias)}); }
Bytes delete_token(std::uint64_t alias) { return cconcat({cvi(0), cvi(alias)}); }

ContributionRun observing_run(const Draft21ContributionProbe& probe, Bytes peer_setup = cbytes({0xaf, 0, 0, 0})) {
    return ContributionRun(probe, std::move(peer_setup));
}

TEST(ContributionResidual, PublisherInitiatedContextsSendNothingAndUseTheCourtesy) {
    struct Case { const char* scenario; RawProbePublishResponse publish; };
    for (const auto& item : {Case{"d21-concurrent-distinct-track-subscriptions", RawProbePublishResponse::Accept},
                             Case{"d21-publish-distinct-tracks-in-one-scope", RawProbePublishResponse::Accept},
                             Case{"d21-reject-publish-before-object-production", RawProbePublishResponse::Reject},
                             Case{"d21-rejected-subscribe-no-delivery", RawProbePublishResponse::RejectAfterObject},
                             Case{"d21-publisher-update-credit-limit", RawProbePublishResponse::Accept},
                             Case{"d21-publisher-update-credit-per-stream", RawProbePublishResponse::Accept},
                             Case{"d21-publisher-update-zero-unlimited", RawProbePublishResponse::Accept},
                             Case{"d21-publisher-client-goaway-control", RawProbePublishResponse::Accept},
                             Case{"d21-publisher-client-goaway-request", RawProbePublishResponse::Accept},
                             Case{"d21-publisher-delete-with-pending-alias-uses", RawProbePublishResponse::Accept}}) {
        const auto& probe = find_probe(probes(), item.scenario);
        EXPECT_TRUE(probe.definition.writes.empty()) << item.scenario;
        EXPECT_TRUE(static_cast<bool>(probe.definition.auto_accept_ready)) << item.scenario;
        EXPECT_EQ(probe.definition.courtesy.publish, item.publish) << item.scenario;
    }
    const auto& hold = find_probe(probes(), "d21-publisher-delete-with-pending-alias-uses");
    EXPECT_EQ(hold.definition.courtesy.update, RawProbeUpdateResponse::HoldAliasUses);
    EXPECT_EQ(find_probe(probes(), "d21-publisher-update-credit-limit").definition.courtesy.update,
              RawProbeUpdateResponse::Ignore);
}

// ---- Section 3.1.2 lines 1080-1084 ------------------------------------------------------------
ContributionRun two_tracks(const Draft21ContributionProbe& probe, std::uint64_t first_alias,
                           std::uint64_t second_alias, const Bytes& second_name = cbytes({'v'})) {
    auto run = observing_run(probe);
    run.reply(0, publish_with(cbytes({'c'}), first_alias, 2));
    run.courtesy(0, Kind::PublishOk);
    run.reply(4, publish_with(second_name, second_alias, 4));
    run.courtesy(4, Kind::PublishOk);
    return run;
}

TEST(ContributionResidual, ConcurrentTracksNeedDistinctAliases) {
    const auto& probe = find_probe(probes(), "d21-concurrent-distinct-track-subscriptions");
    EXPECT_EQ(probe.requirement_id, "D21-3-1-2-MUST-NOT-048");
    EXPECT_EQ(judge(probe, windowed(two_tracks(probe, 0, 1))), true);
    // Two different Tracks sharing an alias while both are Established.
    EXPECT_EQ(judge(probe, windowed(two_tracks(probe, 3, 3))), false);
    EXPECT_TRUE(probe.definition.response_ready(two_tracks(probe, 3, 3).partial()));
    EXPECT_FALSE(probe.definition.response_ready(two_tracks(probe, 0, 1).partial()));
    // The same Track under two aliases is not two Tracks sharing one.
    EXPECT_EQ(judge(probe, windowed(two_tracks(probe, 3, 3, cbytes({'c'})))), std::nullopt);
    // The first subscription ended before the second began, so they never overlapped.
    auto sequential = observing_run(probe);
    sequential.reply(0, publish_with(cbytes({'c'}), 3, 2));
    sequential.courtesy(0, Kind::PublishOk);
    sequential.reply(0, publish_done(), true);
    sequential.reply(4, publish_with(cbytes({'v'}), 3, 4));
    sequential.courtesy(4, Kind::PublishOk);
    EXPECT_EQ(judge(probe, windowed(sequential)), std::nullopt);
    // A PUBLISH the runner rejected, or never answered, is not Established.
    auto rejected = observing_run(probe);
    rejected.reply(0, publish_with(cbytes({'c'}), 3, 2));
    rejected.courtesy(0, Kind::PublishError);
    rejected.reply(4, publish_with(cbytes({'v'}), 3, 4));
    rejected.courtesy(4, Kind::PublishOk);
    EXPECT_EQ(judge(probe, windowed(rejected)), std::nullopt);
    auto single = observing_run(probe);
    single.reply(0, publish_with(cbytes({'c'}), 3, 2));
    single.courtesy(0, Kind::PublishOk);
    EXPECT_EQ(judge(probe, windowed(single)), std::nullopt);
    // Before the window ends a pass is not yet settled.
    EXPECT_EQ(judge(probe, two_tracks(probe, 0, 1).finish()), std::nullopt);
}

// ---- Section 2.5 lines 885-890 ----------------------------------------------------------------------
TEST(ContributionResidual, DifferentContentNeedsDifferentFullTrackNames) {
    const auto& probe = find_probe(probes(), "d21-publish-distinct-tracks-in-one-scope");
    EXPECT_EQ(probe.requirement_id, "D21-2-5-MUST-032");
    const auto with_objects = [&](const Bytes& second_name, const Bytes& first_payload, const Bytes& second_payload) {
        auto run = two_tracks(probe, 0, 1, second_name);
        run.reply(kData1, subgroup(0, 0, object_data(0, first_payload)), true);
        run.reply(kData2, subgroup(1, 0, object_data(0, second_payload)), true);
        return run;
    };
    // Two names, two contents at the same Location: the names distinguish the content.
    EXPECT_EQ(judge(probe, windowed(with_objects(cbytes({'v'}), cbytes({'a'}), cbytes({'b'})))), true);
    // One name carrying two different contents at once.
    EXPECT_EQ(judge(probe, windowed(with_objects(cbytes({'c'}), cbytes({'a'}), cbytes({'b'})))), false);
    // The same content under two names (or one) shows nothing about different content.
    EXPECT_EQ(judge(probe, windowed(with_objects(cbytes({'v'}), cbytes({'a'}), cbytes({'a'})))), std::nullopt);
    // Objects at no shared Location cannot be compared.
    auto apart = two_tracks(probe, 0, 1);
    apart.reply(kData1, subgroup(0, 0, object_data(0, cbytes({'a'}))), true);
    apart.reply(kData2, subgroup(1, 0, object_data(1, cbytes({'b'}))), true);
    EXPECT_EQ(judge(probe, windowed(apart)), std::nullopt);
    EXPECT_EQ(judge(probe, windowed(two_tracks(probe, 0, 1))), std::nullopt);
}

// ---- Section 3.1.1 lines 1068-1071 ----------------------------------------------------------------------
TEST(ContributionResidual, NothingStartsForARejectedPublishAfterThePublisherReacted) {
    const auto& probe = find_probe(probes(), "d21-reject-publish-before-object-production");
    EXPECT_EQ(probe.requirement_id, "D21-3-1-1-MUST-NOT-047");
    const auto run = [&](bool publisher_closes, bool object_after, bool object_before) {
        auto result = observing_run(probe);
        result.reply(0, publish_with(cbytes({'v'}), 3, 2));
        if (object_before) result.reply(kData1, subgroup(3, 0, object_data(0, cbytes({'a'}))));
        result.courtesy(0, Kind::PublishError);
        if (publisher_closes) result.reply(0, {}, true);
        if (object_after) result.reply(kData2, subgroup(3, 1, object_data(0, cbytes({'b'}))));
        return result;
    };
    // The publisher ended the rejected request and then produced nothing for it.
    EXPECT_EQ(judge(probe, windowed(run(true, false, false))), true);
    // Objects already in flight before it reacted are allowed (Section 3.1).
    EXPECT_EQ(judge(probe, windowed(run(true, false, true))), true);
    // A new stream for the rejected alias after the publisher reacted.
    EXPECT_EQ(judge(probe, windowed(run(true, true, false))), false);
    // Without a visible reaction the publisher may simply not have learned yet.
    EXPECT_EQ(judge(probe, windowed(run(false, true, false))), std::nullopt);
    // A reset is a reaction too.
    auto reset = run(false, false, false);
    reset.event(transport::PeerResetEvent{0, 0});
    reset.reply(kData2, subgroup(3, 1, object_data(0, cbytes({'b'}))));
    EXPECT_EQ(judge(probe, windowed(reset)), false);
    // Datagrams for the alias count as production as well.
    auto datagram = run(true, false, false);
    datagram.event(transport::DatagramEvent{cconcat({cvi(0x08), cvi(3), cvi(1), cvi(0), payload()})});
    EXPECT_EQ(judge(probe, windowed(datagram)), false);
    // Another alias is unrelated.
    auto other = run(true, false, false);
    other.reply(kData2, subgroup(9, 1, object_data(0, cbytes({'b'}))));
    EXPECT_EQ(judge(probe, windowed(other)), true);
    // A FIN seen before the rejection was sent says nothing about having learned of it.
    auto early = observing_run(probe);
    early.reply(0, publish_with(cbytes({'v'}), 3, 2));
    early.reply(0, {}, true);
    early.courtesy(0, Kind::PublishError);
    early.reply(kData2, subgroup(3, 1, object_data(0, cbytes({'b'}))));
    EXPECT_EQ(judge(probe, windowed(early)), std::nullopt);
}

TEST(ContributionResidual, TheSecondRejectionScenarioNeedsThepublisherToBeSeenProducing) {
    const auto& probe = find_probe(probes(), "d21-rejected-subscribe-no-delivery");
    const auto run = [&](bool object_before_rejection, bool production_after) {
        auto result = observing_run(probe);
        result.reply(0, publish_with(cbytes({'v'}), 3, 2));
        if (object_before_rejection) result.reply(kData1, subgroup(3, 0, object_data(0, cbytes({'a'}))));
        result.courtesy(0, Kind::PublishError);
        result.reply(0, {}, true);
        if (production_after) result.reply(kData2, subgroup(3, 1, object_data(0, cbytes({'b'}))));
        return result;
    };
    EXPECT_EQ(judge(probe, windowed(run(true, false))), true);
    EXPECT_EQ(judge(probe, windowed(run(true, true))), false);
    // The publisher was never seen producing, so the rejection came before any production.
    EXPECT_EQ(judge(probe, windowed(run(false, false))), std::nullopt);
    EXPECT_EQ(judge(probe, windowed(run(false, true))), std::nullopt);
}

// ---- Section 9.1.7 lines 3611-3618 ------------------------------------------------------------------------
ContributionRun updates_run(const Draft21ContributionProbe& probe, std::initializer_list<std::size_t> per_stream) {
    auto run = observing_run(probe);
    transport::StreamId stream = 0;
    std::uint64_t request = 2;
    for (const auto count : per_stream) {
        run.reply(stream, publish_with(cbytes({'t'}), stream + 1, request));
        run.courtesy(stream, Kind::PublishOk);
        for (std::size_t index = 0; index < count; ++index) run.reply(stream, update_with(3 + 2 * index));
        stream += 4;
        request += 2;
    }
    return run;
}

TEST(ContributionResidual, UpdateCreditIsAnnouncedAndNeverExceeded) {
    const auto& limit = find_probe(probes(), "d21-publisher-update-credit-limit");
    EXPECT_EQ(limit.requirement_id, "D21-9-1-7-MUST-NOT-316");
    EXPECT_EQ(limit.definition.setup_bytes, cbytes({0xaf, 0, 0, 2, 8, 2}));
    EXPECT_EQ(find_probe(probes(), "d21-publisher-update-credit-per-stream").definition.setup_bytes,
              cbytes({0xaf, 0, 0, 2, 8, 2}));
    // MAX_REQUEST_UPDATES is omitted-equivalent zero for the unlimited scenario.
    EXPECT_EQ(find_probe(probes(), "d21-publisher-update-zero-unlimited").definition.setup_bytes,
              cbytes({0xaf, 0, 0, 2, 8, 0}));
    // Two outstanding updates is the whole credit.
    EXPECT_EQ(judge(limit, windowed(updates_run(limit, {2}))), true);
    // One more than the credit with no response between breaks the limit at once.
    EXPECT_EQ(judge(limit, windowed(updates_run(limit, {3}))), false);
    EXPECT_TRUE(limit.definition.response_ready(updates_run(limit, {3}).partial()));
    EXPECT_FALSE(limit.definition.response_ready(updates_run(limit, {2}).partial()));
    // Spending less than the credit never exercises the limit.
    EXPECT_EQ(judge(limit, windowed(updates_run(limit, {1}))), std::nullopt);
    EXPECT_EQ(judge(limit, windowed(updates_run(limit, {}))), std::nullopt);
}

TEST(ContributionResidual, UpdateCreditIsPerRequestStream) {
    const auto& probe = find_probe(probes(), "d21-publisher-update-credit-per-stream");
    // Each of two streams spends all of its credit, so the credit is not shared.
    EXPECT_EQ(judge(probe, windowed(updates_run(probe, {2, 2}))), true);
    EXPECT_EQ(judge(probe, windowed(updates_run(probe, {2}))), std::nullopt);
    EXPECT_EQ(judge(probe, windowed(updates_run(probe, {2, 1}))), std::nullopt);
    EXPECT_EQ(judge(probe, windowed(updates_run(probe, {2, 3}))), false);
}

TEST(ContributionResidual, ZeroOrOmittedUpdateLimitMeansUnlimited) {
    const auto& probe = find_probe(probes(), "d21-publisher-update-zero-unlimited");
    EXPECT_EQ(judge(probe, windowed(updates_run(probe, {3}))), true);
    EXPECT_EQ(judge(probe, windowed(updates_run(probe, {2}))), true);
    // At most one update never shows more than a single credit was assumed.
    EXPECT_EQ(judge(probe, windowed(updates_run(probe, {1}))), std::nullopt);
    EXPECT_EQ(judge(probe, windowed(updates_run(probe, {}))), std::nullopt);
}

// ---- Section 9.2 lines 3650-3652 ---------------------------------------------------------------------------
Bytes goaway(const Bytes& uri) { return cframe(0x10, cconcat({cvi(uri.size()), uri, cvi(0)})); }

TEST(ContributionResidual, ClientGoawayCarriesNoUri) {
    const auto& control = find_probe(probes(), "d21-publisher-client-goaway-control");
    const auto& request = find_probe(probes(), "d21-publisher-client-goaway-request");
    EXPECT_EQ(control.requirement_id, "D21-9-2-MUST-318");
    const auto on_control = [&](const Bytes& uri) {
        auto run = observing_run(control);
        run.reply(2, goaway(uri));
        return run;
    };
    EXPECT_EQ(judge(control, windowed(on_control({}))), true);
    EXPECT_EQ(judge(control, windowed(on_control(cbytes({'m', 'o', 'q'})))), false);
    EXPECT_TRUE(control.definition.response_ready(on_control(cbytes({'m'})).partial()));
    EXPECT_FALSE(control.definition.response_ready(on_control({}).partial()));
    // A GOAWAY on a request stream is the other scenario's.
    auto on_request = observing_run(request);
    on_request.reply(0, publish_with(cbytes({'t'}), 1, 2));
    on_request.courtesy(0, Kind::PublishOk);
    on_request.reply(0, goaway({}));
    EXPECT_EQ(judge(request, windowed(on_request)), true);
    auto bad_request = observing_run(request);
    bad_request.reply(0, publish_with(cbytes({'t'}), 1, 2));
    bad_request.reply(0, goaway(cbytes({'x'})));
    EXPECT_EQ(judge(request, windowed(bad_request)), false);
    EXPECT_EQ(judge(control, windowed(on_request)), std::nullopt);
    EXPECT_EQ(judge(request, windowed(on_control({}))), std::nullopt);
    // No GOAWAY: nothing to score.
    EXPECT_EQ(judge(control, windowed(observing_run(control))), std::nullopt);
}

// ---- Section 8.9 lines 3336-3337 -------------------------------------------------------------------------------
TEST(ContributionResidual, DeleteWaitsForEveryAnswerToAnAliasUse) {
    const auto& probe = find_probe(probes(), "d21-publisher-delete-with-pending-alias-uses");
    EXPECT_EQ(probe.requirement_id, "D21-8-9-MUST-NOT-281");
    // MAX_AUTH_TOKEN_CACHE_SIZE of 4096 (Option 0x04) so the publisher may register.
    EXPECT_EQ(probe.definition.setup_bytes, cbytes({0xaf, 0, 0, 3, 4, 0x90, 0}));
    const auto sequence = [&](bool delete_after_answer) {
        auto run = observing_run(probe);
        // Frame 0: PUBLISH registering Alias 1. Frame 1: REQUEST_UPDATE using it.
        run.reply(0, publish_with(cbytes({'t'}), 1, 2, token_parameter(register_token(1))));
        run.courtesy(0, Kind::PublishOk);
        run.reply(0, update_with(3, token_parameter(use_alias_token(1))));
        if (delete_after_answer) {
            run.courtesy(0, Kind::UpdateOk);
            run.reply(0, update_with(5, token_parameter(delete_token(1))));
        } else {
            run.reply(0, update_with(5, token_parameter(delete_token(1))));
            run.courtesy(0, Kind::UpdateOk);
        }
        return run;
    };
    EXPECT_EQ(judge(probe, windowed(sequence(true))), true);
    // The DELETE arrived before the answer to the earlier use was even written.
    EXPECT_EQ(judge(probe, windowed(sequence(false))), false);
    // A DELETE with no earlier use of the alias has nothing to wait for and proves no waiting.
    auto unused = observing_run(probe);
    unused.reply(0, publish_with(cbytes({'t'}), 1, 2));
    unused.courtesy(0, Kind::PublishOk);
    unused.reply(0, update_with(3, token_parameter(delete_token(1))));
    EXPECT_EQ(judge(probe, windowed(unused)), std::nullopt);
    // An alias use that is never answered while a DELETE follows is a violation.
    auto unanswered = observing_run(probe);
    unanswered.reply(0, publish_with(cbytes({'t'}), 1, 2));
    unanswered.courtesy(0, Kind::PublishOk);
    unanswered.reply(0, update_with(3, token_parameter(use_alias_token(1))));
    unanswered.reply(0, update_with(5, token_parameter(delete_token(1))));
    EXPECT_EQ(judge(probe, windowed(unanswered)), false);
    // Other aliases do not interfere.
    auto other = observing_run(probe);
    other.reply(0, publish_with(cbytes({'t'}), 1, 2));
    other.courtesy(0, Kind::PublishOk);
    other.reply(0, update_with(3, token_parameter(use_alias_token(2))));
    other.reply(0, update_with(5, token_parameter(delete_token(1))));
    EXPECT_EQ(judge(probe, windowed(other)), std::nullopt);
}

// ---- Section 5.2 lines 1880-1890 -----------------------------------------------------------------------
TEST(ContributionResidual, SubgroupTimeoutScenarioHoldsStreamCreditAndAsksForATimeout) {
    const auto& probe = find_probe(probes(), "d21-subgroup-completion-withheld-acknowledgments");
    EXPECT_EQ(probe.requirement_id, "D21-5-2-MUST-130");
    EXPECT_EQ(probe.definition.initial_peer_uni_stream_data, std::optional<std::uint64_t>{64});
    EXPECT_TRUE(probe.definition.hold_uni_stream_credit);
    ASSERT_EQ(probe.definition.writes.size(), 1u);
    // SUBGROUP_DELIVERY_TIMEOUT (0x06) of 200 ms (vi64 0x80c8), FORWARD=1 (delta 0x0a)
    // and a Location filter for all of Group 0 (delta 0x11, three fields).
    EXPECT_EQ(probe.definition.writes[0].bytes,
              cbytes({3, 0, 15, 1, 0, 1, 'x', 3, 6, 0x80, 0xc8, 0x0a, 1, 0x11, 3, 0, 0, 0}));
}

TEST(ContributionResidual, AnUncommittedSubgroupMustBeResetWhenItsTimerExpires) {
    const auto& probe = find_probe(probes(), "d21-subgroup-completion-withheld-acknowledgments");
    const auto stalled = [&](bool reset, bool fin) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok(5));
        // Group 0's first Object arrives truncated at the held credit.
        run.reply(kData1, subgroup(5, 0, object_data(0, Bytes(100, std::byte{'o'}))), fin);
        if (reset) run.event(transport::PeerResetEvent{kData1, 0x0});
        return run;
    };
    // Reset while unfinished: the timer was honoured, whatever the code.
    EXPECT_EQ(judge(probe, stalled(true, false).finish()), true);
    EXPECT_TRUE(probe.definition.response_ready(stalled(true, false).partial()));
    // Still open when the window ends, long after the 200 ms timer: never reset.
    EXPECT_FALSE(probe.definition.response_ready(stalled(false, false).partial()));
    EXPECT_EQ(judge(probe, windowed(stalled(false, false))), false);
    EXPECT_EQ(judge(probe, stalled(false, false).finish()), std::nullopt);
    // A stream that finished was fully committed; there is nothing to time out.
    EXPECT_EQ(judge(probe, windowed(stalled(false, true))), std::nullopt);
    // Another Track Alias is not this subscription.
    ContributionRun other(probe);
    other.deliver(0);
    other.reply(other.stream_of(0), subscribe_ok(5));
    other.reply(kData1, subgroup(9, 0, object_data(0, Bytes(100, std::byte{'o'}))));
    EXPECT_EQ(judge(probe, windowed(other)), std::nullopt);
    // No data stream at all: the fixture produced nothing to time out.
    ContributionRun silent(probe);
    silent.deliver(0);
    silent.reply(silent.stream_of(0), subscribe_ok(5));
    EXPECT_EQ(judge(probe, windowed(silent)), std::nullopt);
    ContributionRun refused(probe);
    refused.deliver(0);
    refused.reply(refused.stream_of(0), request_error(0x10), true);
    EXPECT_TRUE(probe.definition.response_ready(refused.partial()));
    EXPECT_EQ(judge(probe, refused.finish()), std::nullopt);
}

// ---- Subgroup timer: wire draft 21 pins and the draft 22 judgement ---------------------------------
// The shapes the draft 22 evaluator tells apart (see the next tests), pinned on wire draft 21 first:
// draft 21 is frozen, so there every stream still open at the end of the window stays a FAIL.

// PUBLISH_DONE (0xB): Status Code 2, Stream Count 1, empty Reason Phrase (moqxr's bytes).
Bytes subgroup_publish_done() { return cframe(0xb, cbytes({2, 1, 0})); }

enum class Subgroup { Truncated, MoqPubLike };

// SUBSCRIBE_OK, optionally PUBLISH_DONE, then Group 0's Subgroup stream: either one Object
// truncated at the held credit (moqxr: a complete Group whose first Object is 1174 bytes) or
// eight 2-byte Objects, one per event, the stream still open (imquic's moq-pub: a Group per
// minute, an Object per second, so the Group is still being published when the window ends).
ContributionRun withheld_run(const Draft21ContributionProbe& probe, Subgroup shape, bool done, bool reset) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    if (done) run.reply(run.stream_of(0), subgroup_publish_done());
    if (shape == Subgroup::Truncated) {
        run.reply(kData1, subgroup(5, 0, object_data(0, Bytes(100, std::byte{'o'}))));
    } else {
        run.reply(kData1, subgroup(5, 0, object_data(1, cbytes({'3', '5'}))));
        for (int object = 2; object <= 8; ++object) run.reply(kData1, object_data(0, cbytes({'3', '6'})));
    }
    if (reset) run.event(transport::PeerResetEvent{kData1, 0x2});
    return run;
}

TEST(ContributionResidual, SubgroupTimerVerdictsOnWireDraft21AreUnchanged) {
    ASSERT_EQ(current_wire_draft(), 21u);
    const auto& probe = find_probe(probes(), "d21-subgroup-completion-withheld-acknowledgments");
    for (const auto shape : {Subgroup::Truncated, Subgroup::MoqPubLike}) {
        for (const bool done : {false, true}) {
            // A reset of the open stream passes, with or without PUBLISH_DONE.
            EXPECT_EQ(judge(probe, withheld_run(probe, shape, done, true).finish()), true);
            // A stream still open when the window ends fails: complete Group or not.
            EXPECT_EQ(judge(probe, windowed(withheld_run(probe, shape, done, false))), false);
            // Before the window ends there is no verdict.
            EXPECT_EQ(judge(probe, withheld_run(probe, shape, done, false).finish()), std::nullopt);
        }
    }
    // Arrival times do not matter on wire draft 21.
    auto late = windowed(withheld_run(probe, Subgroup::Truncated, true, false));
    const auto start = RawProbeClock::time_point{} + std::chrono::seconds(1);
    late.event_times.assign(late.events.size(), start);
    late.last_poll_at = start + std::chrono::milliseconds(10);
    EXPECT_EQ(judge(probe, late), false);
}

// ---- Draft 22 Section 5.2 lines 2301-2312 (D22-5-2-MUST-144) -------------------------------------------
// "... MUST start a timer of SUBGROUP_DELIVERY_TIMEOUT duration once it becomes aware that all of
// the objects on the subgroup have been published ... If the timer expires before the underlying
// transport stream reaches 'all data committed' state ..., the implementation MUST reset the
// stream." A publisher still publishing the Subgroup owes no reset yet. The runner learns that the
// publisher knows the Subgroup is complete from PUBLISH_DONE, which (Section 9.9 lines 4613-4615)
// "MUST NOT [be sent] until it has closed all streams it will ever open" for the subscription.
std::vector<Draft21ContributionProbe> wire22_probes() {
    const ScopedWireDraft wire(22);
    return draft21_contribution_probes();
}

std::optional<bool> judge22(const Draft21ContributionProbe& probe, const RawProbeTranscript& transcript) {
    const ScopedWireDraft wire(22);
    return evaluate_draft21_contribution_probe(transcript, probe);
}

// Every event arrives at `start`; the window ends `after` later.
RawProbeTranscript timed(RawProbeTranscript transcript, std::chrono::milliseconds after) {
    const auto start = RawProbeClock::time_point{} + std::chrono::seconds(1);
    transcript.event_times.assign(transcript.events.size(), start);
    transcript.last_poll_at = start + after;
    return transcript;
}

TEST(ContributionResidual, Draft22SubgroupStillBeingPublishedIsNotJudged) {
    const auto probes22 = wire22_probes();
    const auto& probe = find_probe(probes22, "d21-subgroup-completion-withheld-acknowledgments");
    // imquic's moq-pub: Objects keep arriving and no PUBLISH_DONE says the Subgroup is complete,
    // so the timer need not have started. No verdict instead of a FAIL.
    EXPECT_EQ(judge22(probe, windowed(withheld_run(probe, Subgroup::MoqPubLike, false, false))), std::nullopt);
    EXPECT_EQ(judge22(probe, timed(windowed(withheld_run(probe, Subgroup::MoqPubLike, false, false)),
                                   std::chrono::seconds(4))), std::nullopt);
    // The same for a truncated Object without PUBLISH_DONE: completion is not observable.
    EXPECT_EQ(judge22(probe, windowed(withheld_run(probe, Subgroup::Truncated, false, false))), std::nullopt);
}

TEST(ContributionResidual, Draft22CompletedSubgroupLeftOpenFails) {
    const auto probes22 = wire22_probes();
    const auto& probe = find_probe(probes22, "d21-subgroup-completion-withheld-acknowledgments");
    for (const auto shape : {Subgroup::Truncated, Subgroup::MoqPubLike}) {
        // PUBLISH_DONE, then nothing until the window ends: never reset. A transcript without
        // arrival times (hand-built) is judged by order alone.
        EXPECT_EQ(judge22(probe, windowed(withheld_run(probe, shape, true, false))), false);
        // Window ended well after the 200 ms timer (plus the allowance for the reset to arrive).
        EXPECT_EQ(judge22(probe, timed(windowed(withheld_run(probe, shape, true, false)),
                                       std::chrono::seconds(2))), false);
        // PUBLISH_DONE arrived too close to the end of the window for the timer to have run out.
        EXPECT_EQ(judge22(probe, timed(windowed(withheld_run(probe, shape, true, false)),
                                       std::chrono::milliseconds(100))), std::nullopt);
        // Before the window ends there is no verdict.
        EXPECT_EQ(judge22(probe, withheld_run(probe, shape, true, false).finish()), std::nullopt);
    }
    // A window ended by the publisher's close is measured to the close.
    const auto closed = [&](std::chrono::milliseconds after) {
        auto run = withheld_run(probe, Subgroup::Truncated, true, false);
        run.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
        auto transcript = run.finish();
        const auto start = RawProbeClock::time_point{} + std::chrono::seconds(1);
        transcript.event_times.assign(transcript.events.size(), start);
        transcript.event_times.back() = start + after;
        transcript.last_poll_at = start + std::chrono::seconds(30);
        return transcript;
    };
    EXPECT_EQ(judge22(probe, closed(std::chrono::seconds(3))), false);
    EXPECT_EQ(judge22(probe, closed(std::chrono::milliseconds(50))), std::nullopt);
}

TEST(ContributionResidual, Draft22ResetSubgroupPasses) {
    const auto probes22 = wire22_probes();
    const auto& probe = find_probe(probes22, "d21-subgroup-completion-withheld-acknowledgments");
    for (const auto shape : {Subgroup::Truncated, Subgroup::MoqPubLike}) {
        for (const bool done : {false, true}) {
            EXPECT_EQ(judge22(probe, withheld_run(probe, shape, done, true).finish()), true);
            EXPECT_TRUE(probe.definition.response_ready(withheld_run(probe, shape, done, true).partial()));
        }
    }
    // A finished stream was committed: nothing to time out, PUBLISH_DONE or not.
    ContributionRun finished(probe);
    finished.deliver(0);
    finished.reply(finished.stream_of(0), subscribe_ok(5));
    finished.reply(finished.stream_of(0), subgroup_publish_done());
    finished.reply(kData1, subgroup(5, 0, objects_with_ids({0})), true);
    EXPECT_EQ(judge22(probe, windowed(finished)), std::nullopt);
}

// ---- Section 8.9: operator-configured credentials ----------------------------------------------------------
const std::vector<Draft21ContributionProbe>& token_probes() {
    static const auto value = draft21_contribution_probes(
        std::chrono::milliseconds{1000}, {}, cbytes({'x'}), std::string{},
        Draft21TokenCredentials{Draft21TokenCredential{4, cbytes({'b', 'a', 'd'})},
                                Draft21TokenCredential{4, cbytes({'o', 'l', 'd'})}});
    return value;
}

TEST(ContributionResidual, TokenScenariosSendNothingWithoutAConfiguredCredential) {
    for (const char* scenario : {"d21-request-well-formed-invalid-token", "d21-expired-token-alias-lifetime"}) {
        const auto& probe = find_probe(probes(), scenario);
        EXPECT_TRUE(probe.definition.writes.empty()) << scenario;
        ContributionRun run(probe);
        // Ready at once and never scored: there is no stimulus to judge.
        EXPECT_TRUE(probe.definition.response_ready(run.partial())) << scenario;
        EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), std::nullopt) << scenario;
    }
}

TEST(ContributionResidual, InvalidTokenIsSentAsUseValueOfTheConfiguredType) {
    const auto& probe = find_probe(token_probes(), "d21-request-well-formed-invalid-token");
    EXPECT_EQ(probe.requirement_id, "D21-8-9-MUST-270");
    ASSERT_EQ(probe.definition.writes.size(), 1u);
    // TRACK_STATUS (0xd), Request ID 1, track "x", one AUTHORIZATION TOKEN (0x03) holding a
    // USE_VALUE token (Alias Type 3, Token Type 4, Value "bad").
    EXPECT_EQ(probe.definition.writes[0].bytes,
              cbytes({0xd, 0, 12, 1, 0, 1, 'x', 1, 3, 5, 3, 4, 'b', 'a', 'd'}));
}

std::optional<bool> invalid_token_outcome(const Bytes& response, bool fin = true) {
    const auto& probe = find_probe(token_probes(), "d21-request-well-formed-invalid-token");
    ContributionRun run(probe);
    run.deliver(0);
    if (!response.empty()) run.reply(run.stream_of(0), response, fin);
    return judge(probe, run.finish());
}

TEST(ContributionResidual, WellFormedInvalidTokenIsRejectedWithMalformedAuthToken) {
    // REQUEST_ERROR MALFORMED_AUTH_TOKEN (0x4).
    EXPECT_EQ(invalid_token_outcome(request_error(0x4)), true);
    // The invalid credential was accepted, or rejected as something else.
    EXPECT_EQ(invalid_token_outcome(request_ok()), false);
    EXPECT_EQ(invalid_token_outcome(request_error(0x1)), false);
    EXPECT_EQ(invalid_token_outcome(request_error(0x10)), false);
    // NOT_SUPPORTED means the Token Type is not understood: the precondition is unmet.
    EXPECT_EQ(invalid_token_outcome(request_error(0x3)), std::nullopt);
    EXPECT_EQ(invalid_token_outcome({}), std::nullopt);
    const auto& probe = find_probe(token_probes(), "d21-request-well-formed-invalid-token");
    ContributionRun pending(probe);
    pending.deliver(0);
    EXPECT_FALSE(probe.definition.response_ready(pending.partial()));
    // A session close (the structural error) is not the message-level rejection.
    ContributionRun closed(probe);
    closed.deliver(0);
    closed.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x16, {}});
    EXPECT_EQ(judge(probe, closed.finish()), std::nullopt);
}

TEST(ContributionResidual, ExpiredTokenScenarioRegistersUsesAndRegistersAgain) {
    const auto& probe = find_probe(token_probes(), "d21-expired-token-alias-lifetime");
    EXPECT_EQ(probe.requirement_id, "D21-8-9-MUST-273");
    ASSERT_EQ(probe.definition.writes.size(), 3u);
    // REGISTER Alias 1 (Alias Type 1, Token Type 4, Value "old"), then USE_ALIAS Alias 1
    // (Alias Type 2), then REGISTER Alias 1 again.
    EXPECT_EQ(probe.definition.writes[0].bytes,
              cbytes({0xd, 0, 13, 1, 0, 1, 'x', 1, 3, 6, 1, 1, 4, 'o', 'l', 'd'}));
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({0xd, 0, 9, 3, 0, 1, 'x', 1, 3, 2, 2, 1}));
    EXPECT_EQ(probe.definition.writes[2].bytes,
              cbytes({0xd, 0, 13, 5, 0, 1, 'x', 1, 3, 6, 1, 1, 4, 'o', 'l', 'd'}));
    EXPECT_TRUE(static_cast<bool>(probe.definition.writes[1].evidence_ready));
    EXPECT_TRUE(static_cast<bool>(probe.definition.writes[2].evidence_ready));
}

struct ExpiredRun {
    Bytes registration = request_error(0x5);
    Bytes use = request_error(0x5);
    std::optional<Bytes> second_registration;
    std::optional<std::uint64_t> close_code;
    std::optional<std::uint64_t> unknown_code;
    bool second_sent = true;
};

std::optional<bool> expired_outcome(const ExpiredRun& spec) {
    const auto& probe = find_probe(token_probes(), "d21-expired-token-alias-lifetime");
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), spec.registration, true);
    if (!spec.use.empty()) {
        run.deliver(1);
        run.reply(run.stream_of(1), spec.use, true);
        if (spec.second_sent) {
            run.deliver(2);
            if (spec.second_registration) run.reply(run.stream_of(2), *spec.second_registration, true);
            if (spec.close_code)
                run.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, *spec.close_code, {}});
        }
    }
    auto transcript = run.finish();
    transcript.unknown_auth_token_alias_compatibility_code = spec.unknown_code;
    return judge(probe, transcript);
}

TEST(ContributionResidual, ExpiredTokenAliasIsRetainedUntilDeleted) {
    ExpiredRun retained;
    retained.close_code = 0x14;
    // EXPIRED_AUTH_TOKEN for the Alias use, then DUPLICATE_AUTH_TOKEN_ALIAS for the repeat.
    EXPECT_EQ(expired_outcome(retained), true);
    // The Alias use fails as something else: not retained as an expired token.
    ExpiredRun other = retained;
    other.use = request_error(0x1);
    EXPECT_EQ(expired_outcome(other), false);
    // A compatibility profile that maps UNKNOWN_AUTH_TOKEN_ALIAS to a code: it was dropped.
    ExpiredRun dropped = retained;
    dropped.use = request_error(0x31);
    dropped.unknown_code = 0x31;
    EXPECT_EQ(expired_outcome(dropped), false);
    // Registering the Alias again succeeded or failed softly instead of closing the session.
    ExpiredRun reregistered = retained;
    reregistered.close_code = std::nullopt;
    reregistered.second_registration = request_error(0x5);
    EXPECT_EQ(expired_outcome(reregistered), false);
    // The credential did not expire, so nothing is shown.
    ExpiredRun fresh = retained;
    fresh.registration = request_ok();
    EXPECT_EQ(expired_outcome(fresh), std::nullopt);
    // The registration failed for another reason (UNAUTHORIZED, NOT_SUPPORTED): the
    // credential is not shown to be expired, whatever the Alias use then returns.
    for (const std::uint8_t code : {0x1, 0x3}) {
        ExpiredRun unrelated = retained;
        unrelated.registration = request_error(code);
        unrelated.use = request_error(0x1);
        EXPECT_EQ(expired_outcome(unrelated), std::nullopt) << "registration error " << int(code);
    }
    // The Alias use succeeded or went unanswered.
    ExpiredRun accepted = retained;
    accepted.use = request_ok();
    EXPECT_EQ(expired_outcome(accepted), std::nullopt);
    // No decision about the repeat registration yet.
    ExpiredRun undecided = retained;
    undecided.close_code = std::nullopt;
    EXPECT_EQ(expired_outcome(undecided), std::nullopt);
    // A close with another code is not a duplicate-alias close.
    ExpiredRun elsewhere = retained;
    elsewhere.close_code = 0x3;
    EXPECT_EQ(expired_outcome(elsewhere), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
