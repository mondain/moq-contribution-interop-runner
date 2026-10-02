// Raw probe family for draft-21 completeness-gap slice A. Each case builds the
// transcript an actual publisher exchange would produce and checks the verdict.
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/requirements/draft21_gap_a.h"
#include "moq/interop/scenarios/draft21_gap_a.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace moq::interop::scenarios {
namespace {

using Bytes = std::vector<std::byte>;

Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

const Draft21GapProbe& probe(const char* scenario, std::vector<Draft21GapProbe>& storage,
                             std::vector<Bytes> ns = {b({'n'})}, Bytes name = b({'t'})) {
    storage = draft21_gap_a_probes(std::chrono::milliseconds(1000), std::move(ns), std::move(name));
    const auto found = std::find_if(storage.begin(), storage.end(),
                                    [&](const auto& candidate) { return candidate.definition.id == scenario; });
    EXPECT_NE(found, storage.end()) << scenario;
    return *found;
}

// Setup and the first request are accepted after the publisher's SETUP.
RawProbeTranscript start(const Draft21GapProbe& p) {
    RawProbeTranscript t;
    t.scenario_id = p.definition.id;
    t.setup = {{RawProbeChannel::NewUni, b({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
    t.events = {transport::ConnectionEstablishedEvent{{}, {}, {}, 1200},
                transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false}};
    t.transport_established = t.peer_setup_received = true;
    t.max_datagram_payload = 1200;
    return t;
}

// Appends write `index` on `stream` as accepted now.
void accept(RawProbeTranscript& t, const Draft21GapProbe& p, std::size_t index, transport::StreamId stream) {
    const auto& write = p.definition.writes[index];
    t.writes.push_back({write, stream, write.bytes.size(), write.fin, t.events.size()});
    t.delivery_event_count = t.events.size();
    t.stimulus_delivered = true;
}

void data(RawProbeTranscript& t, transport::StreamId stream, Bytes payload, bool fin = false) {
    t.events.push_back(transport::StreamDataEvent{stream, std::move(payload), fin});
}

void finish(RawProbeTranscript& t) { t.complete = true; }

Bytes subscribe_ok(unsigned alias = 4) { return b({4, 0, 2, alias, 0}); }
Bytes request_error() { return b({5, 0, 3, 0x10, 0, 0}); }
Bytes request_ok() { return b({7, 0, 1, 0}); }
Bytes publish_done() { return b({0x0b, 0, 3, 8, 0, 0}); }
Bytes fetch_ok() { return b({0x18, 0, 4, 0, 7, 9, 0}); }
Bytes concat(Bytes first, const Bytes& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

// Subgroup stream for alias 4: flags, alias, group, then Objects (delta, len, 'x').
Bytes subgroup(unsigned flags, unsigned group, std::initializer_list<unsigned> object_ids,
               unsigned alias = 4, Bytes properties = {}) {
    Bytes result = b({flags, alias, group});
    unsigned previous = 0;
    bool first = true;
    for (const auto id : object_ids) {
        result.push_back(static_cast<std::byte>(first ? id : id - previous - 1));
        if ((flags & 1u) != 0u) {
            result.push_back(static_cast<std::byte>(properties.size()));
            result.insert(result.end(), properties.begin(), properties.end());
        }
        result.push_back(std::byte{1});
        result.push_back(std::byte{'x'});
        previous = id;
        first = false;
    }
    return result;
}

std::optional<bool> verdict(const char* scenario, const RawProbeTranscript& t) {
    std::vector<Draft21GapProbe> storage;
    return evaluate_draft21_gap_a_probe(t, probe(scenario, storage));
}

// ------------------------------------------------------------------ wiring

TEST(Draft21GapAProbe, CanonicalStimuliAreExactlyTheDocumentedRequests) {
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-publisher-request-response-before-fin", storage);
    ASSERT_EQ(p.definition.writes.size(), 1u);
    // SUBSCRIBE id 1, FORWARD=1, absolute LOCATION_FILTER starting Group 7, Object 9.
    EXPECT_EQ(p.definition.writes[0].bytes,
              b({3, 0, 13, 1, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 2, 7, 9}));
    EXPECT_FALSE(p.definition.writes[0].fin);

    const auto& whole = probe("d21-original-publisher-opens-new-subgroup", storage);
    // StartGroup 7, StartObject 0, EndGroupDelta 0: every Object of Group 7.
    EXPECT_EQ(whole.definition.writes[0].bytes,
              b({3, 0, 14, 1, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 3, 7, 0, 0}));

    const auto& updated = probe("d21-update-subscription-location-range", storage);
    ASSERT_EQ(updated.definition.writes.size(), 2u);
    // FORWARD=0 so nothing is sent before the update sets filter and FORWARD=1.
    EXPECT_EQ(updated.definition.writes[0].bytes, b({3, 0, 9, 1, 1, 1, 'n', 1, 't', 1, 0x10, 0}));
    EXPECT_EQ(updated.definition.writes[1].bytes,
              b({2, 0, 10, 3, 2, 0x10, 1, 0x11, 4, 7, 9, 0, 9}));
    EXPECT_EQ(updated.definition.writes[1].reuse_write_stream, std::optional<std::size_t>{0});

    const auto& terminal = probe("d21-request-stream-terminal-message-order", storage);
    ASSERT_EQ(terminal.definition.writes.size(), 3u);
    EXPECT_EQ(terminal.definition.writes[1].bytes, b({2, 0, 6, 3, 1, 3, 2, 2, 0}));
    EXPECT_EQ(terminal.definition.writes[2].bytes,
              b({0x16, 0, 13, 5, 1, 1, 'n', 1, 't', 1, 0x21, 4, 7, 9, 0, 9}));
    EXPECT_TRUE(terminal.definition.writes[2].fin);

    const auto& discovery = probe("d21-discover-original-publisher-namespaces", storage);
    ASSERT_EQ(discovery.definition.writes.size(), 2u);
    EXPECT_EQ(discovery.definition.writes[1].bytes, b({0x50, 0, 3, 3, 0, 0}));
}

TEST(Draft21GapAProbe, RowsMapToTheCatalogScenariosAndEvaluators) {
    const auto probes = draft21_gap_a_probes();
    std::set<std::string> rows;
    for (const auto& candidate : probes) rows.insert(candidate.requirement_id);
    EXPECT_EQ(rows, (std::set<std::string>{
        "D21-2-2-MUST-020", "D21-3-3-1-MUST-NOT-057", "D21-3-6-MUST-070", "D21-4-2-MUST-089",
        "D21-6-2-MUST-139", "D21-6-3-MUST-NOT-146", "D21-6-4-2-2-MUST-157",
        "D21-6-4-2-2-MUST-158", "D21-6-4-2-2-MUST-NOT-156"}));
    EXPECT_EQ(probes.size(), 11u);
}

// ----------------------------------------------------- response before FIN

TEST(Draft21GapAProbe, ResponseMustPrecedeFin) {
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-publisher-request-response-before-fin", storage);
    auto passing = start(p);
    accept(passing, p, 0, 1);
    data(passing, 1, subscribe_ok());
    data(passing, 1, {}, true);
    finish(passing);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(passing, p), std::optional<bool>{true});

    auto rejected = start(p);
    accept(rejected, p, 0, 1);
    data(rejected, 1, concat(request_error(), {}), true);
    finish(rejected);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(rejected, p), std::optional<bool>{true});

    auto bare_fin = start(p);
    accept(bare_fin, p, 0, 1);
    data(bare_fin, 1, {}, true);
    finish(bare_fin);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(bare_fin, p), std::optional<bool>{false});

    // A truncated message followed by FIN is still a FIN before any response.
    auto truncated = start(p);
    accept(truncated, p, 0, 1);
    data(truncated, 1, b({4, 0, 2, 4}), true);
    finish(truncated);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(truncated, p), std::optional<bool>{false});

    // A reset is not a FIN.
    auto reset = start(p);
    accept(reset, p, 0, 1);
    reset.events.push_back(transport::PeerResetEvent{1, 1});
    finish(reset);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(reset, p), std::nullopt);

    // Nothing received: no verdict.
    auto silent = start(p);
    accept(silent, p, 0, 1);
    finish(silent);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(silent, p), std::nullopt);

    // Response bytes that predate the accepted request prove nothing.
    auto early = start(p);
    data(early, 1, subscribe_ok());
    accept(early, p, 0, 1);
    finish(early);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(early, p), std::nullopt);

    // An incomplete context is never evaluated.
    auto incomplete = passing;
    incomplete.complete = false;
    EXPECT_EQ(evaluate_draft21_gap_a_probe(incomplete, p), std::nullopt);
}

// -------------------------------------------------- PUBLISH_DONE before FIN

RawProbeTranscript failed_update(const Draft21GapProbe& p) {
    auto t = start(p);
    accept(t, p, 0, 1);
    data(t, 1, subscribe_ok());
    accept(t, p, 1, 1);
    return t;
}

TEST(Draft21GapAProbe, EstablishedSubscriptionNeedsPublishDoneBeforeFin) {
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-established-subscription-publisher-fin", storage);
    auto passing = failed_update(p);
    data(passing, 1, concat(request_error(), publish_done()), true);
    finish(passing);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(passing, p), std::optional<bool>{true});

    auto without_done = failed_update(p);
    data(without_done, 1, request_error(), true);
    finish(without_done);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(without_done, p), std::optional<bool>{false});

    auto split = failed_update(p);
    data(split, 1, request_error());
    data(split, 1, publish_done());
    data(split, 1, {}, true);
    finish(split);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(split, p), std::optional<bool>{true});

    // PUBLISH_DONE but no FIN: the ordering was never observed.
    auto open = failed_update(p);
    data(open, 1, concat(request_error(), publish_done()));
    finish(open);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(open, p), std::nullopt);

    // A reset instead of FIN is not the rule's subject.
    auto reset = failed_update(p);
    data(reset, 1, request_error());
    reset.events.push_back(transport::PeerResetEvent{1, 1});
    finish(reset);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(reset, p), std::nullopt);

    // A subscription that was never established has no PUBLISH_DONE duty.
    auto rejected = start(p);
    accept(rejected, p, 0, 1);
    data(rejected, 1, request_error(), true);
    finish(rejected);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(rejected, p), std::nullopt);
}

TEST(Draft21GapAProbe, EveryRequestTypeNeedsItsRequiredMessagesBeforeFin) {
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-request-stream-terminal-message-order", storage);
    const auto build = [&](Bytes subscription, bool subscription_fin, Bytes fetch_response, bool fetch_fin) {
        auto t = failed_update(p);
        accept(t, p, 2, 5);
        data(t, 1, std::move(subscription), subscription_fin);
        data(t, 5, std::move(fetch_response), fetch_fin);
        finish(t);
        return t;
    };
    EXPECT_EQ(evaluate_draft21_gap_a_probe(
                  build(concat(request_error(), publish_done()), true, fetch_ok(), true), p),
              std::optional<bool>{true});
    // FETCH finished before FETCH_OK.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(
                  build(concat(request_error(), publish_done()), true, {}, true), p),
              std::optional<bool>{false});
    // Subscription finished before PUBLISH_DONE.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(request_error(), true, fetch_ok(), true), p),
              std::optional<bool>{false});
    // One side unfinished: neither a pass nor a failure.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(
                  build(concat(request_error(), publish_done()), false, fetch_ok(), true), p),
              std::nullopt);
    // A violation on one stream fails even when the other has not finished.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(request_error(), true, fetch_ok(), false), p),
              std::optional<bool>{false});
}

// ------------------------------------------------------------ FIRST_OBJECT

TEST(Draft21GapAProbe, FirstStreamOfEachSubgroupSetsFirstObject) {
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-original-publisher-opens-new-subgroup", storage);
    const auto build = [&](std::initializer_list<std::pair<unsigned, Bytes>> streams) {
        auto t = start(p);
        accept(t, p, 0, 1);
        data(t, 1, subscribe_ok());
        for (const auto& [id, payload] : streams) data(t, id, payload);
        finish(t);
        return t;
    };
    // flags: 0x10 always, 0x20 default priority, 0x40 FIRST_OBJECT, subgroup 0.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build({{6, subgroup(0x70, 7, {0, 1})}}), p),
              std::optional<bool>{true});
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build({{6, subgroup(0x30, 7, {0, 1})}}), p),
              std::optional<bool>{false});
    // A restart of the same Subgroup on a later stream need not set the bit.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(
                  build({{6, subgroup(0x70, 7, {0})}, {10, subgroup(0x30, 7, {1})}}), p),
              std::optional<bool>{true});
    // Streams are ordered by the publisher's open order (stream ID), not arrival.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(
                  build({{10, subgroup(0x30, 7, {1})}, {6, subgroup(0x70, 7, {0})}}), p),
              std::optional<bool>{true});
    // Another Subgroup (explicit ID, mode 0b10) is judged on its own first stream.
    auto second = subgroup(0x34, 7, {});
    second.insert(second.begin() + 3, std::byte{5});
    second.push_back(std::byte{0});
    second.push_back(std::byte{1});
    second.push_back(std::byte{'x'});
    EXPECT_EQ(evaluate_draft21_gap_a_probe(
                  build({{6, subgroup(0x70, 7, {0})}, {10, second}}), p),
              std::optional<bool>{false});
    // Another alias is not this subscription's delivery.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build({{6, subgroup(0x30, 7, {0}, 9)}}), p), std::nullopt);
    // Without a rejected-free establishment there is nothing to judge.
    auto rejected = start(p);
    accept(rejected, p, 0, 1);
    data(rejected, 1, request_error(), true);
    finish(rejected);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(rejected, p), std::nullopt);
    // No stream yet.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build({}), p), std::nullopt);
}

// ----------------------------------------------------- mandatory properties

TEST(Draft21GapAProbe, MandatoryTrackPropertiesNeverAppearOnObjects) {
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-publish-track-with-mandatory-property", storage);
    const auto build = [&](Bytes stream, bool fin) {
        auto t = start(p);
        accept(t, p, 0, 1);
        data(t, 1, subscribe_ok());
        data(t, 6, std::move(stream), fin);
        finish(t);
        return t;
    };
    // PROPERTIES flag set. Property: delta 2 (type 2, even) value 1.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(subgroup(0x31, 7, {9}, 4, b({2, 1})), true), p),
              std::optional<bool>{true});
    // Type 0x4000 (even): delta 0x4000 as a two-byte vi64 (0x80|0x40, 0x00), value 1.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(subgroup(0x31, 7, {9}, 4, b({0xc0, 0x40, 0x00, 1})), true), p),
              std::optional<bool>{false});
    // Type 0x7fff (odd) with a one-byte value, upper edge of the range.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(
                  build(subgroup(0x31, 7, {9}, 4, b({0xc0, 0x7f, 0xff, 1, 7})), true), p),
              std::optional<bool>{false});
    // 0x8000 is outside the Mandatory range.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(
                  build(subgroup(0x31, 7, {9}, 4, b({0xc0, 0x80, 0x00, 1})), true), p),
              std::optional<bool>{true});
    // Objects without a Properties field cannot carry one.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(subgroup(0x30, 7, {9}), true), p),
              std::optional<bool>{true});
    // An open stream with no complete judgement yet has no verdict.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(subgroup(0x30, 7, {9}), false), p), std::nullopt);
}

// -------------------------------------------------------- namespace discovery

TEST(Draft21GapAProbe, OriginalPublisherAnnouncesItsNamespaceToMatchingSubscriber) {
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-discover-original-publisher-namespaces", storage);
    const auto build = [&](Bytes response, bool fin) {
        auto t = start(p);
        accept(t, p, 0, 1);
        accept(t, p, 1, 5);
        data(t, 5, std::move(response), fin);
        finish(t);
        return t;
    };
    const auto matching = b({8, 0, 3, 1, 1, 'n'});
    const auto other = b({8, 0, 3, 1, 1, 'z'});
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(concat(request_ok(), matching), false), p),
              std::optional<bool>{true});
    // Other namespaces first, then ours.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(concat(concat(request_ok(), other), matching), false), p),
              std::optional<bool>{true});
    // REQUEST_OK then FIN without ever announcing the namespace.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(concat(request_ok(), other), true), p),
              std::optional<bool>{false});
    // Still waiting.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(request_ok(), false), p), std::nullopt);
    // A rejected discovery has no matching subscription.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(build(request_error(), true), p), std::nullopt);
    // A suffix that is not the whole namespace does not match.
    EXPECT_EQ(evaluate_draft21_gap_a_probe(
                  build(concat(request_ok(), b({8, 0, 1, 0})), true), p),
              std::optional<bool>{false});
}

// -------------------------------------------------------- control stream

TEST(Draft21GapAProbe, ControlStreamStaysOpenWhileTheSessionIsActive) {
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-control-stream-lifetime", storage);
    auto passing = start(p);
    accept(passing, p, 0, 1);
    data(passing, 1, subscribe_ok());
    finish(passing);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(passing, p), std::optional<bool>{true});

    auto fin = start(p);
    accept(fin, p, 0, 1);
    data(fin, 2, {}, true);
    data(fin, 1, subscribe_ok());
    finish(fin);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(fin, p), std::optional<bool>{false});

    auto reset = start(p);
    accept(reset, p, 0, 1);
    reset.events.push_back(transport::PeerResetEvent{2, 1});
    finish(reset);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(reset, p), std::optional<bool>{false});

    // No response yet: the control stream has not been tested.
    auto pending = start(p);
    accept(pending, p, 0, 1);
    finish(pending);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(pending, p), std::nullopt);
}

// --------------------------------------------------------------- datagrams

TEST(Draft21GapAProbe, QuicDatagramMustBeNegotiated) {
    for (const char* scenario : {"d21-native-quic-datagram-support", "d21-webtransport-h3-datagram-support"}) {
        SCOPED_TRACE(scenario);
        std::vector<Draft21GapProbe> storage;
        const auto& p = probe(scenario, storage);
        auto passing = start(p);
        accept(passing, p, 0, 1);
        data(passing, 1, subscribe_ok());
        finish(passing);
        EXPECT_EQ(evaluate_draft21_gap_a_probe(passing, p), std::optional<bool>{true});

        // The listener closes a peer without DATAGRAM before anything else happens.
        RawProbeTranscript refused;
        refused.scenario_id = p.definition.id;
        refused.timed_out = true;
        const std::string reason = "QUIC DATAGRAM not negotiated";
        Bytes reason_bytes;
        for (const char c : reason) reason_bytes.push_back(static_cast<std::byte>(c));
        refused.events = {transport::LocalCloseEvent{transport::CloseErrorSpace::Application, 3,
                                                     reason_bytes}};
        EXPECT_EQ(evaluate_draft21_gap_a_probe(refused, p), std::optional<bool>{false});

        // An unrelated local close or a silent timeout proves nothing.
        refused.events = {transport::LocalCloseEvent{transport::CloseErrorSpace::Application, 3, b({'x'})}};
        EXPECT_EQ(evaluate_draft21_gap_a_probe(refused, p), std::nullopt);
        refused.events.clear();
        EXPECT_EQ(evaluate_draft21_gap_a_probe(refused, p), std::nullopt);

        auto harness = passing;
        harness.harness_failed = true;
        EXPECT_EQ(evaluate_draft21_gap_a_probe(harness, p), std::nullopt);
    }
}

// ------------------------------------------------------------ Location filter

TEST(Draft21GapAProbe, SubscriptionObjectsStayInsideTheRequestedRange) {
    for (const char* scenario : {"d21-subscribe-bounded-location-range", "d21-update-subscription-location-range"}) {
        SCOPED_TRACE(scenario);
        std::vector<Draft21GapProbe> storage;
        const auto& p = probe(scenario, storage);
        const bool updated = std::string(scenario).find("update") != std::string::npos;
        const auto build = [&](std::initializer_list<std::pair<unsigned, Bytes>> streams, bool timed_out) {
            auto t = start(p);
            accept(t, p, 0, 1);
            data(t, 1, subscribe_ok());
            if (updated) {
                accept(t, p, 1, 1);
                data(t, 1, request_ok());
            }
            for (const auto& [id, payload] : streams) data(t, id, payload, true);
            t.complete = !timed_out;
            t.timed_out = timed_out;
            return t;
        };
        // The one Object in range, then the window ends.
        EXPECT_EQ(evaluate_draft21_gap_a_probe(build({{6, subgroup(0x30, 7, {9})}}, true), p),
                  std::optional<bool>{true});
        // An Object outside the range fails, even before the window ends.
        EXPECT_EQ(evaluate_draft21_gap_a_probe(build({{6, subgroup(0x30, 7, {9, 10})}}, true), p),
                  std::optional<bool>{false});
        EXPECT_EQ(evaluate_draft21_gap_a_probe(build({{6, subgroup(0x30, 7, {9})}, {10, subgroup(0x30, 8, {0})}}, false), p),
                  std::optional<bool>{false});
        EXPECT_EQ(evaluate_draft21_gap_a_probe(build({{6, subgroup(0x30, 7, {8})}}, true), p),
                  std::optional<bool>{false});
        // The inclusive boundary Object must actually be delivered.
        EXPECT_EQ(evaluate_draft21_gap_a_probe(build({}, true), p), std::nullopt);
        // A context that stopped before the window ended has no verdict.
        EXPECT_EQ(evaluate_draft21_gap_a_probe(build({{6, subgroup(0x30, 7, {9})}}, false), p), std::nullopt);
    }
}

TEST(Draft21GapAProbe, RangeUpdateMustBeAcknowledgedBeforeObjectsAreTrusted) {
    // With FORWARD=0 nothing is sent before the update, so any Object seen was
    // sent under the new filter; without a REQUEST_OK the update never applied.
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-update-subscription-location-range", storage);
    auto t = start(p);
    accept(t, p, 0, 1);
    data(t, 1, subscribe_ok());
    accept(t, p, 1, 1);
    data(t, 1, request_error());
    data(t, 6, subgroup(0x30, 7, {9}), true);
    t.timed_out = true;
    EXPECT_EQ(evaluate_draft21_gap_a_probe(t, p), std::nullopt);
}

// -------------------------------------------------------- stimulus integrity

TEST(Draft21GapAProbe, ForgedOrAlteredStimulusIsNeverEvaluated) {
    std::vector<Draft21GapProbe> storage;
    const auto& p = probe("d21-publisher-request-response-before-fin", storage);
    auto altered = start(p);
    accept(altered, p, 0, 1);
    altered.writes[0].write.bytes.back() = std::byte{10};
    data(altered, 1, subscribe_ok());
    finish(altered);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(altered, p), std::nullopt);

    auto other_scenario = start(p);
    accept(other_scenario, p, 0, 1);
    data(other_scenario, 1, subscribe_ok());
    finish(other_scenario);
    other_scenario.scenario_id = "d21-control-stream-lifetime";
    EXPECT_EQ(evaluate_draft21_gap_a_probe(other_scenario, p), std::nullopt);

    // A fixture the catalog cannot accept (empty name) is rejected.
    auto bad_fixture = start(p);
    accept(bad_fixture, p, 0, 1);
    bad_fixture.writes[0].write.bytes = b({3, 0, 9, 1, 1, 1, 'n', 0, 0});
    data(bad_fixture, 1, subscribe_ok());
    finish(bad_fixture);
    EXPECT_EQ(evaluate_draft21_gap_a_probe(bad_fixture, p), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios

namespace moq::interop::requirements {
namespace {

using scenarios::Draft21GapProbe;

const RequirementCatalog& catalog21() {
    static const auto catalog = [] {
        const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
        const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
        return RequirementCatalog::load(source, root / "requirements/draft21.json");
    }();
    return catalog;
}

OutcomeState state(const std::vector<Outcome>& outcomes, std::string_view id) {
    for (const auto& outcome : outcomes) if (outcome.requirement_id == id) return outcome.state;
    ADD_FAILURE() << "missing outcome " << id;
    return OutcomeState::NotRun;
}

TEST(Draft21GapARows, SingleTransportRowsPassOnOneTransportScenario) {
    std::vector<Draft21GapProbe> storage;
    const auto& native = scenarios::probe("d21-native-quic-datagram-support", storage);
    auto t = scenarios::start(native);
    scenarios::accept(t, native, 0, 1);
    scenarios::data(t, 1, scenarios::subscribe_ok());
    scenarios::finish(t);
    const std::vector transcripts{t};
    const auto outcomes = evaluate_draft21_raw_probes(catalog21(), transcripts);
    // The row names a native and a WebTransport scenario; each run executes the
    // one for its transport.
    EXPECT_EQ(state(outcomes, "D21-6-2-MUST-139"), OutcomeState::Pass);
    EXPECT_EQ(state(outcomes, "D21-6-4-2-2-MUST-157"), OutcomeState::NotRun);
}

TEST(Draft21GapARows, RowsWithTwoContextsNeedBothInOneRun) {
    std::vector<Draft21GapProbe> storage;
    const auto& bounded = scenarios::probe("d21-subscribe-bounded-location-range", storage);
    auto first = scenarios::start(bounded);
    scenarios::accept(first, bounded, 0, 1);
    scenarios::data(first, 1, scenarios::subscribe_ok());
    scenarios::data(first, 6, scenarios::subgroup(0x30, 7, {9}), true);
    first.timed_out = true;
    const auto& updated = scenarios::probe("d21-update-subscription-location-range", storage);
    auto second = scenarios::start(updated);
    scenarios::accept(second, updated, 0, 1);
    scenarios::data(second, 1, scenarios::subscribe_ok());
    scenarios::accept(second, updated, 1, 1);
    scenarios::data(second, 1, scenarios::request_ok());
    scenarios::data(second, 6, scenarios::subgroup(0x30, 7, {9}), true);
    second.timed_out = true;
    EXPECT_EQ(state(evaluate_draft21_raw_probes(catalog21(), std::vector{first}), "D21-3-3-1-MUST-NOT-057"),
              OutcomeState::NotRun);
    EXPECT_EQ(state(evaluate_draft21_raw_probes(catalog21(), std::vector{first, second}),
                    "D21-3-3-1-MUST-NOT-057"), OutcomeState::Pass);
    second.events.push_back(transport::StreamDataEvent{10, scenarios::subgroup(0x30, 7, {12}), true});
    EXPECT_EQ(state(evaluate_draft21_raw_probes(catalog21(), std::vector{first, second}),
                    "D21-3-3-1-MUST-NOT-057"), OutcomeState::Fail);
}

}  // namespace
}  // namespace moq::interop::requirements
