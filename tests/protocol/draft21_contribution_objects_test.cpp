#include "../support/contribution_transcript.h"

#include "moq/interop/requirements/draft21_evaluators.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <tuple>

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

// ---- wire builders for peer-originated data ---------------------------------
Bytes properties(std::initializer_list<Bytes> entries) {
    Bytes body;
    for (const auto& entry : entries) body.insert(body.end(), entry.begin(), entry.end());
    return body;
}
// Key-Value-Pair with the type given as a delta from the previous type.
Bytes numeric(std::uint64_t delta, std::uint64_t value) { return cconcat({cvi(delta), cvi(value)}); }
Bytes bytes_pair(std::uint64_t delta, const Bytes& value) {
    return cconcat({cvi(delta), cvi(value.size()), value});
}
Bytes with_length(const Bytes& block) { return cconcat({cvi(block.size()), block}); }

Bytes object_data(std::uint64_t delta, const Bytes& payload, const Bytes& props = {}, bool has_props = false) {
    Bytes result = cvi(delta);
    if (has_props) result = cconcat({result, with_length(props)});
    return cconcat({result, cvi(payload.size()), payload});
}
Bytes object_status(std::uint64_t delta, std::uint64_t status) {
    return cconcat({cvi(delta), cvi(0), cvi(status)});
}
// Subgroup stream with default priority and Subgroup ID 0.
Bytes subgroup(std::uint64_t alias, std::uint64_t group, const Bytes& objects, unsigned type = 0x30) {
    return cconcat({cvi(type), cvi(alias), cvi(group), objects});
}
Bytes fetch_stream_data(std::uint64_t request, const Bytes& payload, const Bytes& props = {},
                        bool has_props = false, bool datagram = false) {
    unsigned flags = 0x1c | (has_props ? 0x20u : 0u) | (datagram ? 0x40u : 0u);
    Bytes result = cconcat({cvi(5), cvi(request), cvi(flags), cvi(7), cvi(9), cbytes({128})});
    if (has_props) result = cconcat({result, with_length(props)});
    return cconcat({result, cvi(payload.size()), payload});
}
Bytes datagram_object(std::uint64_t alias, std::uint64_t group, std::uint64_t object, const Bytes& payload,
                      unsigned flags = 0x08) {
    return cconcat({cvi(flags), cvi(alias), cvi(group), cvi(object), payload});
}

constexpr transport::StreamId kData1 = 6;
constexpr transport::StreamId kData2 = 10;

// ---- Section 9.6: eligible Objects are delivered ---------------------------------
TEST(ContributionObjects, SubscriptionMustDeliverObjects) {
    const auto& probe = find_probe(probes(), "d21-successful-subscribe-object-delivery");
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({3, 0, 7, 1, 0, 1, 'x', 1, 0x10, 1}));
    ContributionRun run(probe);
    run.deliver(0);
    EXPECT_FALSE(probe.definition.response_ready(run.partial()));
    run.reply(run.stream_of(0), subscribe_ok(5));
    EXPECT_FALSE(probe.definition.response_ready(run.partial()));  // an idle source is not conclusive

    auto other_alias = run;
    other_alias.reply(kData1, subgroup(9, 1, object_data(0, cbytes({'a'}))));
    EXPECT_FALSE(probe.definition.response_ready(other_alias.partial()));

    auto stream = run;
    stream.reply(kData1, subgroup(5, 1, object_data(0, cbytes({'a'}))));
    EXPECT_TRUE(probe.definition.response_ready(stream.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(stream.finish(), probe), true);

    auto datagram = run;
    datagram.event(transport::DatagramEvent{datagram_object(5, 1, 0, cbytes({'a'}))});
    EXPECT_EQ(evaluate_draft21_contribution_probe(datagram.finish(), probe), true);

    // A bare stream header carries no Object yet.
    auto header_only = run;
    header_only.reply(kData1, subgroup(5, 1, {}));
    EXPECT_FALSE(probe.definition.response_ready(header_only.partial()));
}

TEST(ContributionObjects, ForwardStateZeroGatesDelivery) {
    const auto& probe = find_probe(probes(), "d21-successful-subscribe-forward-zero");
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({3, 0, 7, 1, 0, 1, 'x', 1, 0x10, 0}));
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({2, 0, 4, 3, 1, 0x10, 1}));

    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    run.deliver(1);
    EXPECT_FALSE(probe.definition.response_ready(run.partial()));
    auto resumed = run;
    resumed.reply(resumed.stream_of(0), request_ok());
    resumed.reply(kData1, subgroup(5, 1, object_data(0, cbytes({'a'}))));
    EXPECT_TRUE(probe.definition.response_ready(resumed.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(resumed.finish(), probe), true);

    // Data before the update was sent contradicts Forward State 0.
    ContributionRun early(probe);
    early.deliver(0);
    early.reply(early.stream_of(0), subscribe_ok(5));
    early.reply(kData1, subgroup(5, 1, object_data(0, cbytes({'a'}))));
    early.deliver(1);
    early.reply(early.stream_of(0), request_ok());
    EXPECT_EQ(evaluate_draft21_contribution_probe(early.finish(), probe), false);

    ContributionRun early_datagram(probe);
    early_datagram.deliver(0);
    early_datagram.reply(early_datagram.stream_of(0), subscribe_ok(5));
    early_datagram.event(transport::DatagramEvent{datagram_object(5, 1, 0, cbytes({'a'}))});
    early_datagram.deliver(1);
    EXPECT_EQ(evaluate_draft21_contribution_probe(early_datagram.finish(), probe), false);

    // Without any Objects after the update the row is unscored.
    ContributionRun idle(probe);
    idle.deliver(0);
    idle.reply(idle.stream_of(0), subscribe_ok(5));
    idle.deliver(1);
    idle.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
    EXPECT_EQ(evaluate_draft21_contribution_probe(idle.finish(), probe), std::nullopt);
}

// ---- Section 9.20: payload identity -----------------------------------------------
TEST(ContributionObjects, SubscriptionParametersDoNotChangePayloads) {
    const auto& probe = find_probe(probes(), "d21-subscribe-parameters-preserve-payload");
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    EXPECT_NE(probe.definition.writes[0].bytes, probe.definition.writes[1].bytes);
    const auto establish = [&](std::uint64_t first_alias, std::uint64_t second_alias) {
        ContributionRun run(probe);
        run.deliver(0);
        run.deliver(1);
        run.reply(run.stream_of(0), subscribe_ok(first_alias));
        run.reply(run.stream_of(1), subscribe_ok(second_alias));
        return run;
    };
    auto distinct = establish(5, 6);
    distinct.reply(kData1, subgroup(5, 7, object_data(9, cbytes({'p', 'q'}))));
    EXPECT_FALSE(probe.definition.response_ready(distinct.partial()));
    distinct.reply(kData2, subgroup(6, 7, object_data(9, cbytes({'p', 'q'}))));
    EXPECT_TRUE(probe.definition.response_ready(distinct.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(distinct.finish(), probe), true);

    auto altered = establish(5, 6);
    altered.reply(kData1, subgroup(5, 7, object_data(9, cbytes({'p', 'q'}))));
    altered.reply(kData2, subgroup(6, 7, object_data(9, cbytes({'p', 'r'}))));
    EXPECT_EQ(evaluate_draft21_contribution_probe(altered.finish(), probe), false);

    // Different Locations are unrelated and are never compared.
    auto unrelated = establish(5, 6);
    unrelated.reply(kData1, subgroup(5, 7, object_data(9, cbytes({'p'}))));
    unrelated.reply(kData2, subgroup(6, 7, object_data(10, cbytes({'z'}))));
    EXPECT_FALSE(probe.definition.response_ready(unrelated.partial()));

    // A shared Track Alias carries one copy per subscription.
    auto shared = establish(5, 5);
    shared.reply(kData1, subgroup(5, 7, object_data(9, cbytes({'p'}))));
    EXPECT_FALSE(probe.definition.response_ready(shared.partial()));
    shared.reply(kData2, subgroup(5, 7, object_data(9, cbytes({'p'}))));
    EXPECT_EQ(evaluate_draft21_contribution_probe(shared.finish(), probe), true);
    auto shared_altered = establish(5, 5);
    shared_altered.reply(kData1, subgroup(5, 7, object_data(9, cbytes({'p'}))));
    shared_altered.reply(kData2, subgroup(5, 7, object_data(9, cbytes({'x'}))));
    EXPECT_EQ(evaluate_draft21_contribution_probe(shared_altered.finish(), probe), false);
}

TEST(ContributionObjects, FetchParametersDoNotChangePayloads) {
    const auto& probe = find_probe(probes(), "d21-fetch-parameters-preserve-payload");
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    EXPECT_TRUE(probe.definition.writes[0].fin);
    EXPECT_TRUE(probe.definition.writes[1].fin);
    EXPECT_NE(probe.definition.writes[0].bytes, probe.definition.writes[1].bytes);
    const auto fetched = [&](const Bytes& first, const Bytes& second) {
        ContributionRun run(probe);
        run.deliver(0);
        run.deliver(1);
        run.reply(kData1, fetch_stream_data(1, first), false);
        run.reply(kData2, fetch_stream_data(3, second), true);
        return run;
    };
    auto same = fetched(cbytes({'a', 'b'}), cbytes({'a', 'b'}));
    EXPECT_TRUE(probe.definition.response_ready(same.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(same.finish(), probe), true);
    auto different = fetched(cbytes({'a', 'b'}), cbytes({'a', 'c'}));
    EXPECT_EQ(evaluate_draft21_contribution_probe(different.finish(), probe), false);

    ContributionRun partial(probe);
    partial.deliver(0);
    partial.deliver(1);
    partial.reply(kData1, fetch_stream_data(1, cbytes({'a'})));
    EXPECT_FALSE(probe.definition.response_ready(partial.partial()));

    ContributionRun rejected(probe);
    rejected.deliver(0);
    rejected.deliver(1);
    rejected.reply(rejected.stream_of(0), request_error(0x11), true);
    EXPECT_TRUE(probe.definition.response_ready(rejected.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(rejected.finish(), probe), std::nullopt);
}

// ---- Sections 10.8 and 10.9: prior gaps -----------------------------------------------
struct GapCase {
    const char* prefix;
    unsigned type;
    const char* stable_row;
    const char* preserved_row;
    const char* count_row;
};
const GapCase kGapCases[] = {
    {"d21-prior-group-gap", 0x3c, "D21-10-8-MUST-NOT-495", "D21-10-8-MUST-NOT-496", "D21-10-8-MUST-NOT-497"},
    {"d21-prior-object-gap", 0x3e, "D21-10-9-MUST-NOT-500", "D21-10-9-MUST-NOT-501", "D21-10-9-MUST-NOT-502"},
};

ContributionRun repeated_object(const Draft21ContributionProbe& probe, const Bytes& subscribed_properties,
                                const Bytes& fetched_properties) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    run.reply(kData1, subgroup(5, 7, object_data(9, cbytes({'o'}), subscribed_properties, true), 0x31));
    run.deliver(1);
    run.reply(kData2, fetch_stream_data(3, cbytes({'o'}), fetched_properties, true));
    return run;
}

TEST(ContributionObjects, RepeatedGapsAreStableAndPreserved) {
    for (const auto& gap : kGapCases) {
        const std::string scenario = std::string(gap.prefix) + "-repeat";
        const auto& stable = find_probe(probes(), scenario, gap.stable_row);
        const auto& preserved = find_probe(probes(), scenario, gap.preserved_row);
        EXPECT_EQ(stable.definition.writes[0].bytes,
                  cbytes({3, 0, 11, 1, 0, 1, 'x', 2, 0x10, 1, 0x11, 2, 7, 9}));
        EXPECT_EQ(stable.definition.writes[1].bytes,
                  cbytes({0x16, 0, 11, 3, 0, 1, 'x', 1, 0x21, 4, 7, 9, 0, 9}));
        EXPECT_TRUE(static_cast<bool>(stable.definition.writes[1].evidence_ready));

        const auto outcome = [&](const Bytes& subscribed, const Bytes& fetched, const Draft21ContributionProbe& row) {
            auto run = repeated_object(row, subscribed, fetched);
            return evaluate_draft21_contribution_probe(run.finish(), row);
        };
        const auto gap_value = [&](std::uint64_t value) { return properties({numeric(gap.type, value)}); };
        // Same value both times.
        EXPECT_EQ(outcome(gap_value(2), gap_value(2), stable), true) << gap.prefix;
        EXPECT_EQ(outcome(gap_value(2), gap_value(2), preserved), true) << gap.prefix;
        // Modified: not stable; still present.
        EXPECT_EQ(outcome(gap_value(2), gap_value(3), stable), false) << gap.prefix;
        EXPECT_EQ(outcome(gap_value(2), gap_value(3), preserved), true) << gap.prefix;
        // Removed: not preserved; removal is not modification.
        EXPECT_EQ(outcome(gap_value(2), {}, preserved), false) << gap.prefix;
        EXPECT_EQ(outcome(gap_value(2), {}, stable), std::nullopt) << gap.prefix;
        // Initially omitted gaps stay legal and unscored.
        EXPECT_EQ(outcome({}, {}, stable), std::nullopt) << gap.prefix;
        EXPECT_EQ(outcome({}, {}, preserved), std::nullopt) << gap.prefix;
        EXPECT_EQ(outcome({}, gap_value(2), preserved), std::nullopt) << gap.prefix;
        // Inside Immutable Properties counts the same as the mutable list.
        const auto immutable = properties({bytes_pair(0x0b, gap_value(2))});
        EXPECT_EQ(outcome(immutable, gap_value(2), stable), true) << gap.prefix;
        EXPECT_EQ(outcome(immutable, {}, preserved), false) << gap.prefix;
    }
}

TEST(ContributionObjects, RepeatedGapsNeedBothDeliveriesOfTheSameObject) {
    const auto& probe = find_probe(probes(), "d21-prior-group-gap-repeat", "D21-10-8-MUST-NOT-495");
    auto run = repeated_object(probe, properties({numeric(0x3c, 2)}), properties({numeric(0x3c, 2)}));
    EXPECT_TRUE(probe.definition.response_ready(run.partial()));
    // The FETCH stream returned a different Object.
    ContributionRun wrong(probe);
    wrong.deliver(0);
    wrong.reply(wrong.stream_of(0), subscribe_ok(5));
    wrong.reply(kData1, subgroup(5, 7, object_data(9, cbytes({'o'}), properties({numeric(0x3c, 2)}), true), 0x31));
    wrong.deliver(1);
    wrong.reply(kData2, cconcat({cvi(5), cvi(3), cvi(0x3c), cvi(8), cvi(9), cbytes({128}),
                                 with_length(properties({numeric(0x3c, 2)})), cvi(1), cbytes({'o'})}));
    EXPECT_EQ(evaluate_draft21_contribution_probe(wrong.finish(), probe), std::nullopt);
    // Rejected FETCH leaves the row unscored but ends observation.
    ContributionRun rejected(probe);
    rejected.deliver(0);
    rejected.reply(rejected.stream_of(0), subscribe_ok(5));
    rejected.reply(kData1, subgroup(5, 7, object_data(9, cbytes({'o'})), 0x30));
    rejected.deliver(1);
    rejected.reply(rejected.stream_of(1), request_error(0x11), true);
    EXPECT_TRUE(probe.definition.response_ready(rejected.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(rejected.finish(), probe), std::nullopt);
}

TEST(ContributionObjects, AnObjectHasAtMostOneOfEachGapProperty) {
    for (const auto& gap : kGapCases) {
        const auto& probe = find_probe(probes(), std::string(gap.prefix) + "-singleton", gap.count_row);
        ASSERT_EQ(probe.definition.writes.size(), 1u);
        EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({0x16, 0, 11, 1, 0, 1, 'x', 1, 0x21, 4, 7, 9, 0, 9}));
        const auto outcome = [&](const Bytes& props) {
            ContributionRun run(probe);
            run.deliver(0);
            run.reply(kData1, fetch_stream_data(1, cbytes({'o'}), props, true));
            return evaluate_draft21_contribution_probe(run.finish(), probe);
        };
        EXPECT_EQ(outcome(properties({numeric(gap.type, 1)})), true) << gap.prefix;
        EXPECT_EQ(outcome(properties({numeric(0x10, 1)})), true) << gap.prefix;
        // Two instances of the same Type in the mutable list.
        EXPECT_EQ(outcome(properties({numeric(gap.type, 1), numeric(0, 1)})), false) << gap.prefix;
        // One mutable and one inside Immutable Properties.
        EXPECT_EQ(outcome(properties({bytes_pair(0x0b, properties({numeric(gap.type, 1)})),
                                      numeric(gap.type - 0x0b, 1)})), false) << gap.prefix;
        // The other gap type is a separate property.
        EXPECT_EQ(outcome(properties({numeric(gap.type == 0x3c ? 0x3e : 0x3c, 1)})), true) << gap.prefix;
        // Different Locations are not the requested Object.
        ContributionRun elsewhere(probe);
        elsewhere.deliver(0);
        elsewhere.reply(kData1, cconcat({cvi(5), cvi(1), cvi(0x1c), cvi(1), cvi(1), cbytes({128}), cvi(1), cbytes({'o'})}));
        EXPECT_EQ(evaluate_draft21_contribution_probe(elsewhere.finish(), probe), std::nullopt);
    }
}

// ---- Sections 11.1.1 and 11.4.1.1: forwarding preference ----------------------------------
ContributionRun preference_run(const Draft21ContributionProbe& probe, bool datagram_delivery, bool fetch_flag) {
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok(5));
    if (datagram_delivery) run.event(transport::DatagramEvent{datagram_object(5, 7, 9, cbytes({'o'}))});
    else run.reply(kData1, subgroup(5, 7, object_data(9, cbytes({'o'}))));
    run.deliver(1);
    run.reply(kData2, fetch_stream_data(3, cbytes({'o'}), {}, false, fetch_flag));
    return run;
}

TEST(ContributionObjects, SubscriptionDeliveryFollowsTheFetchedPreference) {
    const auto& probe = find_probe(probes(), "d21-subscription-forwarding-preference");
    for (const auto& [datagram, flag, expected] : std::vector<std::tuple<bool, bool, std::optional<bool>>>{
             {true, true, true}, {false, false, true}, {false, true, false}, {true, false, std::nullopt}}) {
        auto run = preference_run(probe, datagram, flag);
        EXPECT_TRUE(probe.definition.response_ready(run.partial()));
        EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), expected)
            << datagram << flag;
    }
}

TEST(ContributionObjects, FetchMustFlagDatagramPreference) {
    const auto& probe = find_probe(probes(), "d21-fetch-datagram-preference");
    for (const auto& [datagram, flag, expected] : std::vector<std::tuple<bool, bool, std::optional<bool>>>{
             {true, true, true}, {true, false, false}, {false, false, std::nullopt}, {false, true, std::nullopt}}) {
        auto run = preference_run(probe, datagram, flag);
        EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), expected) << datagram << flag;
    }
    // A FETCH that is rejected leaves nothing to compare.
    ContributionRun rejected(probe);
    rejected.deliver(0);
    rejected.reply(rejected.stream_of(0), subscribe_ok(5));
    rejected.event(transport::DatagramEvent{datagram_object(5, 7, 9, cbytes({'o'}))});
    rejected.deliver(1);
    rejected.reply(rejected.stream_of(1), request_error(0x11), true);
    EXPECT_EQ(evaluate_draft21_contribution_probe(rejected.finish(), probe), std::nullopt);
}

// ---- Sections 11.2.1 and 11.3.1: reserved bits --------------------------------------------
TEST(ContributionObjects, DatagramReservedBitMustBeZero) {
    const auto& probe = find_probe(probes(), "d21-object-datagram-flags");
    const auto outcome = [&](std::initializer_list<Bytes> datagrams) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok(5));
        for (const auto& datagram : datagrams) run.event(transport::DatagramEvent{datagram});
        return run;
    };
    auto valid = outcome({datagram_object(5, 1, 0, cbytes({'a'}), 0x08)});
    EXPECT_TRUE(probe.definition.response_ready(valid.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(valid.finish(), probe), true);
    auto reserved = outcome({datagram_object(5, 1, 0, cbytes({'a'}), 0x08), datagram_object(5, 1, 1, cbytes({'a'}), 0x18)});
    EXPECT_EQ(evaluate_draft21_contribution_probe(reserved.finish(), probe), false);
    // Other aliases are not this subscription's Objects.
    auto foreign = outcome({datagram_object(9, 1, 0, cbytes({'a'}), 0x18)});
    EXPECT_FALSE(probe.definition.response_ready(foreign.partial()));
    auto none = outcome({});
    EXPECT_FALSE(probe.definition.response_ready(none.partial()));
}

TEST(ContributionObjects, SubgroupHeaderMustSetBitFour) {
    const auto& probe = find_probe(probes(), "d21-subgroup-header-flags");
    const auto outcome = [&](const std::vector<std::pair<transport::StreamId, Bytes>>& streams) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok(5));
        for (const auto& [id, data] : streams) run.reply(id, data);
        return run;
    };
    auto valid = outcome({{kData1, subgroup(5, 1, object_data(0, cbytes({'a'})))}});
    EXPECT_TRUE(probe.definition.response_ready(valid.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(valid.finish(), probe), true);
    for (const unsigned type : {0x20u, 0x00u, 0x61u}) {
        auto missing = outcome({{kData1, subgroup(5, 1, object_data(0, cbytes({'a'})), type)}});
        EXPECT_EQ(evaluate_draft21_contribution_probe(missing.finish(), probe), false) << type;
    }
    // FETCH and padding streams are legitimate peer streams without bit four.
    Bytes padding = cvi(0x132b3e28);
    padding.resize(40, std::byte{0});
    auto others = outcome({{kData1, fetch_stream_data(9, cbytes({'a'}))}, {kData2, padding}});
    EXPECT_FALSE(probe.definition.response_ready(others.partial()));
    auto mixed = outcome({{kData2, padding}, {14, subgroup(5, 1, object_data(0, cbytes({'a'})))}});
    EXPECT_EQ(evaluate_draft21_contribution_probe(mixed.finish(), probe), true);
}

// ---- Section 11.3.2: closing Subgroup streams -----------------------------------------------
TEST(ContributionObjects, CompleteSubgroupsEndWithFin) {
    for (const char* scenario : {"d21-complete-subgroup-fin", "d21-subgroup-start-location-fin"}) {
        const auto& probe = find_probe(probes(), scenario);
        if (std::string(scenario).find("start-location") != std::string::npos)
            EXPECT_EQ(probe.definition.writes[0].bytes,
                      cbytes({3, 0, 11, 1, 0, 1, 'x', 2, 0x10, 1, 0x11, 2, 7, 9}));
        const auto outcome = [&](const Bytes& stream, bool fin, bool reset) {
            ContributionRun run(probe);
            run.deliver(0);
            run.reply(run.stream_of(0), subscribe_ok(5));
            run.reply(kData1, stream, fin);
            if (reset) run.event(transport::PeerResetEvent{kData1, 0});
            return run;
        };
        const auto finished = cconcat({object_data(0, cbytes({'a'})), object_status(0, 3)});
        auto clean = outcome(subgroup(5, 1, finished), true, false);
        EXPECT_TRUE(probe.definition.response_ready(clean.partial())) << scenario;
        EXPECT_EQ(evaluate_draft21_contribution_probe(clean.finish(), probe), true) << scenario;

        // All Objects, including End of Group, were delivered but no FIN followed.
        auto reset = outcome(subgroup(5, 1, finished), false, true);
        EXPECT_EQ(evaluate_draft21_contribution_probe(reset.finish(), probe), false) << scenario;

        // END_OF_GROUP in the header plus FIN also marks a complete Subgroup.
        auto header_flag = outcome(subgroup(5, 1, object_data(0, cbytes({'a'})), 0x38), true, false);
        EXPECT_EQ(evaluate_draft21_contribution_probe(header_flag.finish(), probe), true) << scenario;

        // An ordinary open stream proves nothing yet.
        auto open = outcome(subgroup(5, 1, object_data(0, cbytes({'a'}))), false, false);
        EXPECT_FALSE(probe.definition.response_ready(open.partial())) << scenario;
        // A reset before the end of the Subgroup is not this obligation.
        auto early = outcome(subgroup(5, 1, object_data(0, cbytes({'a'}))), false, true);
        EXPECT_FALSE(probe.definition.response_ready(early.partial())) << scenario;
    }
}

TEST(ContributionObjects, EarlySubgroupClosureMustReset) {
    const auto& probe = find_probe(probes(), "d21-subgroup-premature-close-reset");
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({2, 0, 4, 3, 1, 0x10, 0}));
    EXPECT_TRUE(static_cast<bool>(probe.definition.writes[1].evidence_ready));

    const auto established = [&](bool open_stream) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok(5));
        if (open_stream) run.reply(kData1, subgroup(5, 1, object_data(0, cbytes({'a'}))));
        return run;
    };
    // The update waits for an open Subgroup stream of this subscription.
    auto nothing_open = established(false);
    const auto gate = probe.definition.writes[1].evidence_ready;
    EXPECT_FALSE(gate({std::span(nothing_open.snapshot().writes).first(1), nothing_open.snapshot().events}));
    auto open = established(true);
    EXPECT_TRUE(gate({std::span(open.snapshot().writes).first(1), open.snapshot().events}));
    auto closed_first = established(true);
    closed_first.reply(kData1, {}, true);
    EXPECT_FALSE(gate({std::span(closed_first.snapshot().writes).first(1), closed_first.snapshot().events}));

    auto reset = established(true);
    reset.deliver(1);
    reset.event(transport::PeerResetEvent{kData1, 1});
    EXPECT_TRUE(probe.definition.response_ready(reset.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(reset.finish(), probe), true);

    // A FIN cannot show that Objects were omitted.
    auto fin = established(true);
    fin.deliver(1);
    fin.reply(kData1, {}, true);
    EXPECT_EQ(evaluate_draft21_contribution_probe(fin.finish(), probe), std::nullopt);

    // Silence leaves the row unscored.
    auto silent = established(true);
    silent.deliver(1);
    EXPECT_FALSE(probe.definition.response_ready(silent.partial()));

    // A stream the publisher closed before the update was sent is not a target.
    auto earlier = established(true);
    earlier.event(transport::PeerResetEvent{kData1, 1});
    earlier.deliver(1);
    EXPECT_FALSE(probe.definition.response_ready(earlier.partial()));
}

// ---- Catalog rows ------------------------------------------------------------------------------
TEST(ContributionObjects, DeliveryRowNeedsBothContexts) {
    using namespace requirements;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, root / "requirements/draft21.json");
    const auto state = [&](const std::vector<RawProbeTranscript>& transcripts) {
        const auto outcomes = evaluate_draft21_raw_probes(catalog, transcripts);
        const auto found = std::find_if(outcomes.begin(), outcomes.end(),
                                        [](const auto& outcome) { return outcome.requirement_id == "D21-9-6-MUST-355"; });
        return found == outcomes.end() ? OutcomeState::NotRun : found->state;
    };
    const auto delivered = [&] {
        const auto& probe = find_probe(probes(), "d21-successful-subscribe-object-delivery");
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok(5));
        run.reply(kData1, subgroup(5, 1, object_data(0, cbytes({'a'}))));
        return run.finish();
    }();
    const auto forward_zero = [&](bool early) {
        const auto& probe = find_probe(probes(), "d21-successful-subscribe-forward-zero");
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok(5));
        if (early) run.reply(kData1, subgroup(5, 1, object_data(0, cbytes({'a'}))));
        run.deliver(1);
        if (!early) run.reply(kData1, subgroup(5, 1, object_data(0, cbytes({'a'}))));
        return run.finish();
    };
    EXPECT_EQ(state({delivered}), OutcomeState::NotRun);
    EXPECT_EQ(state({delivered, forward_zero(false)}), OutcomeState::Pass);
    EXPECT_EQ(state({delivered, forward_zero(true)}), OutcomeState::Fail);
}

}  // namespace
}  // namespace moq::interop::scenarios
