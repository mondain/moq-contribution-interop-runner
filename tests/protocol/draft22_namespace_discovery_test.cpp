// Draft 22 own scenario for D22-4-2-MUST-110 (Section 4.2): each case builds the transcript a publisher
// exchange would produce and checks the evaluator's verdict.
#include "moq/interop/scenarios/draft22_namespace_discovery.h"
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

// Request streams of the runner (server-initiated bidirectional streams 1, 5, 9, 13).
constexpr transport::StreamId kSubscribeStream = 1;
constexpr transport::StreamId kEmptyStream = 5;
constexpr transport::StreamId kMatchingStream = 9;
constexpr transport::StreamId kNonmatchingStream = 13;

class Draft22NamespaceDiscovery : public ::testing::Test {
protected:
    ScopedWireDraft wire{22};
};

// Track (moq)(n)/t: matching prefix (moq), nonmatching prefix (mo).
RawProbeDefinition probe() {
    return draft22_namespace_discovery_probe(1000ms, {b({'m', 'o', 'q'}), b({'n'})}, b({'t'}));
}

const RawProbeClock::time_point kStart{};

RawProbeTranscript start(const RawProbeDefinition& p) {
    RawProbeTranscript t;
    t.scenario_id = p.id;
    t.setup = {{RawProbeChannel::NewUni, b({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
    t.setup.accepted_at = kStart;
    t.events = {transport::ConnectionEstablishedEvent{{}, {}, {}, 1200},
                transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false}};
    t.transport_established = t.peer_setup_received = true;
    t.max_datagram_payload = 1200;
    for (const auto& write : p.writes) t.writes.push_back({write, {}, 0, false});
    return t;
}

// Write `index` accepted now, `at` after the start.
void accept(RawProbeTranscript& t, std::size_t index, transport::StreamId stream,
            std::chrono::milliseconds at = 0ms) {
    auto& write = t.writes[index];
    write.stream_id = stream;
    write.delivery_event_count = t.events.size();
    write.accepted_at = kStart + at;
    if (write.write.operation == RawProbeOperation::StopSending) {
        write.operation_accepted = true;
    } else {
        write.accepted = write.write.bytes.size();
        write.fin_accepted = write.write.fin;
    }
    if (index + 1 == t.writes.size()) {
        t.delivery_event_count = t.events.size();
        t.stimulus_delivered = true;
    }
}

void data(RawProbeTranscript& t, transport::StreamId stream, Bytes payload, bool fin = false) {
    t.events.push_back(transport::StreamDataEvent{stream, std::move(payload), fin});
}
void answer(RawProbeTranscript& t, transport::StreamId stream, const std::vector<Bytes>& messages, bool fin) {
    for (std::size_t index = 0; index < messages.size(); ++index)
        data(t, stream, messages[index], fin && index + 1 == messages.size());
}

Bytes subscribe_ok() { return b({4, 0, 2, 1, 0}); }
Bytes request_ok() { return b({7, 0, 1, 0}); }
Bytes request_error(unsigned code = 0x10) { return b({5, 0, 3, code, 0, 0}); }
// NAMESPACE (0x8) with suffix (n), (moq)(n), (moq), or no field.
Bytes namespace_n() { return b({8, 0, 3, 1, 1, 'n'}); }
Bytes namespace_full() { return b({8, 0, 7, 2, 3, 'm', 'o', 'q', 1, 'n'}); }
Bytes namespace_moq() { return b({8, 0, 5, 1, 3, 'm', 'o', 'q'}); }
Bytes namespace_none() { return b({8, 0, 1, 0}); }

struct Answers {
    std::vector<Bytes> subscribe{subscribe_ok()};
    std::vector<Bytes> empty{request_ok(), namespace_full()};
    bool empty_fin{false};
    std::vector<Bytes> matching{request_ok(), namespace_n()};
    bool matching_fin{false};
    std::vector<Bytes> nonmatching{request_error()};
    // Whether the cancellation, and so the matching and nonmatching prefixes, went out: a FIN on the empty
    // prefix, or no settled answer on it, keeps the runner from sending them, and the context times out.
    bool cancelled{true};
};

// The exchange: SUBSCRIBE and the empty prefix, their answers, the cancellation (the publisher resets the
// stream), then 250 ms later the matching and nonmatching prefixes and their answers.
RawProbeTranscript exchange(const Answers& answers) {
    const auto p = probe();
    auto t = start(p);
    accept(t, 0, kSubscribeStream);
    accept(t, 1, kEmptyStream);
    answer(t, kSubscribeStream, answers.subscribe, false);
    answer(t, kEmptyStream, answers.empty, answers.empty_fin);
    if (!answers.cancelled) {
        t.timed_out = true;
        return t;
    }
    accept(t, 2, kEmptyStream, 10ms);
    t.events.push_back(transport::PeerResetEvent{kEmptyStream, 1});
    accept(t, 3, kMatchingStream, 260ms);
    accept(t, 4, kNonmatchingStream, 260ms);
    answer(t, kMatchingStream, answers.matching, answers.matching_fin);
    answer(t, kNonmatchingStream, answers.nonmatching, false);
    t.complete = true;
    return t;
}

std::optional<bool> verdict(const RawProbeTranscript& t) { return evaluate_draft22_namespace_discovery(t); }

// ------------------------------------------------------------------ wiring

TEST_F(Draft22NamespaceDiscovery, NonmatchingPrefixFieldIsABytePrefixOrAnExtension) {
    EXPECT_EQ(draft22_nonmatching_prefix_field(b({'m', 'o', 'q'})), b({'m', 'o'}));
    EXPECT_EQ(draft22_nonmatching_prefix_field(b({'n'})), b({'n', '-'}));
}

TEST_F(Draft22NamespaceDiscovery, ProbeAsksWithEmptyThenMatchingAndNonmatchingPrefixes) {
    const auto p = probe();
    EXPECT_EQ(p.id, kDraft22DiscoverNamespaces);
    ASSERT_EQ(p.writes.size(), 5u);
    // SUBSCRIBE (0x3), Request ID 1, (moq)(n)/t, one parameter: FORWARD=0.
    EXPECT_EQ(p.writes[0].bytes, b({3, 0, 13, 1, 2, 3, 'm', 'o', 'q', 1, 'n', 1, 't', 1, 0x10, 0}));
    // SUBSCRIBE_NAMESPACE (0x50), Request IDs 3, 5, 7: no field, (moq), (mo); no parameters.
    EXPECT_EQ(p.writes[1].bytes, b({0x50, 0, 3, 3, 0, 0}));
    EXPECT_EQ(p.writes[2].operation, RawProbeOperation::StopSending);
    EXPECT_EQ(p.writes[2].reuse_write_stream, std::optional<std::size_t>(1));
    EXPECT_EQ(p.writes[2].application_error, 1u) << "CANCELLED (Section 12.5)";
    EXPECT_TRUE(static_cast<bool>(p.writes[2].evidence_ready));
    EXPECT_EQ(p.writes[3].bytes, b({0x50, 0, 7, 5, 1, 3, 'm', 'o', 'q', 0}));
    EXPECT_EQ(p.writes[3].delay_after_previous, 250ms);
    EXPECT_EQ(p.writes[4].bytes, b({0x50, 0, 6, 7, 1, 2, 'm', 'o', 0}));
    for (const auto& write : p.writes) EXPECT_FALSE(write.fin);
}

TEST(Draft22NamespaceDiscoveryWire, ProbeIsBuiltOnTheDraft22WireOnly) {
    const ScopedWireDraft wire(21);
    EXPECT_THROW(probe(), std::logic_error);
}

// -------------------------------------------------------------------- verdicts

TEST_F(Draft22NamespaceDiscovery, EmptyAndMatchingPrefixesAnnouncedPass) {
    EXPECT_EQ(verdict(exchange({})), std::optional<bool>{true});
    Answers accepted_nonmatching;
    accepted_nonmatching.nonmatching = {request_ok()};
    EXPECT_EQ(verdict(exchange(accepted_nonmatching)), std::optional<bool>{true})
        << "an accepted nonmatching prefix with no NAMESPACE is correct";
    Answers done_too;
    done_too.matching = {request_ok(), namespace_n(), b({0x0e, 0, 3, 1, 1, 'n'})};
    done_too.matching_fin = true;
    EXPECT_EQ(verdict(exchange(done_too)), std::optional<bool>{true}) << "NAMESPACE_DONE and FIN after it";
}

TEST_F(Draft22NamespaceDiscovery, AcceptedPrefixEndedWithoutTheNamespaceFails) {
    Answers matching_fin;
    matching_fin.matching = {request_ok()};
    matching_fin.matching_fin = true;
    EXPECT_EQ(verdict(exchange(matching_fin)), std::optional<bool>{false});
    // A prefix-ignoring publisher: under (moq) its suffix is the whole namespace, i.e. (moq)(moq)(n).
    Answers ignoring = matching_fin;
    ignoring.matching = {request_ok(), namespace_full()};
    EXPECT_EQ(verdict(exchange(ignoring)), std::optional<bool>{false});
    // The empty prefix's FIN keeps its cancellation from being sent; the delivered writes still prove it.
    Answers empty_fin;
    empty_fin.empty = {request_ok()};
    empty_fin.empty_fin = true;
    empty_fin.cancelled = false;
    EXPECT_EQ(verdict(exchange(empty_fin)), std::optional<bool>{false});
}

TEST_F(Draft22NamespaceDiscovery, NoFailureWithoutProofThePublisherServesTheTrack) {
    Answers refused;
    refused.subscribe = {request_error()};
    refused.matching = {request_ok()};
    refused.matching_fin = true;
    EXPECT_EQ(verdict(exchange(refused)), std::nullopt);
    Answers unanswered = refused;
    unanswered.subscribe = {};
    EXPECT_EQ(verdict(exchange(unanswered)), std::nullopt);
}

TEST_F(Draft22NamespaceDiscovery, AnUnreadableNamespaceNeverLetsAFinFail) {
    // A NAMESPACE that cannot be read (a byte after the suffix; a zero-length field, which Section 8.7
    // forbids) may be the one owed: the FIN after it is not proof that none was sent.
    for (const auto& unreadable : {b({8, 0, 4, 1, 1, 'n', 0}), b({8, 0, 2, 1, 0})}) {
        SCOPED_TRACE(::testing::PrintToString(unreadable));
        Answers matching;
        matching.matching = {request_ok(), unreadable};
        matching.matching_fin = true;
        EXPECT_EQ(verdict(exchange(matching)), std::nullopt);
        Answers empty;
        empty.empty = {request_ok(), unreadable};
        empty.empty_fin = true;
        empty.cancelled = false;
        EXPECT_EQ(verdict(exchange(empty)), std::nullopt);
        Answers nonmatching;
        nonmatching.nonmatching = {request_ok(), unreadable};
        EXPECT_EQ(verdict(exchange(nonmatching)), std::nullopt) << "it could carry an ignored prefix";
    }
    // A readable response that omits the track's namespace and ends still fails.
    Answers omits;
    omits.matching = {request_ok(), b({8, 0, 3, 1, 1, 'z'})};
    omits.matching_fin = true;
    EXPECT_EQ(verdict(exchange(omits)), std::optional<bool>{false});
}

TEST_F(Draft22NamespaceDiscovery, AProbeStoppedShortNeverPasses) {
    // The empty prefix is answered correctly and then ended, so its cancellation (and the other prefixes)
    // never go out: the delivered writes can show a failure only.
    Answers exact_then_fin;
    exact_then_fin.empty_fin = true;
    exact_then_fin.cancelled = false;
    EXPECT_EQ(verdict(exchange(exact_then_fin)), std::nullopt);
}

TEST_F(Draft22NamespaceDiscovery, TamperedDeliveredWritesGiveNoVerdict) {
    Answers empty_fin;
    empty_fin.empty = {request_ok()};
    empty_fin.empty_fin = true;
    empty_fin.cancelled = false;
    const auto t = exchange(empty_fin);
    ASSERT_EQ(verdict(t), std::optional<bool>{false});
    auto prefix = t;
    prefix.writes[1].write.bytes.back() = std::byte{1};  // the empty prefix's parameter count
    EXPECT_EQ(verdict(prefix), std::nullopt);
    auto forward = t;
    forward.writes[0].write.bytes.back() = std::byte{1};  // the SUBSCRIBE's FORWARD value
    EXPECT_EQ(verdict(forward), std::nullopt);
}

TEST_F(Draft22NamespaceDiscovery, OnlyACoveringNamespaceNeitherPassesNorFails) {
    // NAMESPACE (moq): a shorter namespace that the track's lies in.
    Answers covering;
    covering.empty = {request_ok(), namespace_moq()};
    covering.empty_fin = true;
    covering.cancelled = false;
    EXPECT_EQ(verdict(exchange(covering)), std::nullopt);
    Answers matching_covering;
    matching_covering.matching = {request_ok(), namespace_none()};
    matching_covering.matching_fin = true;
    EXPECT_EQ(verdict(exchange(matching_covering)), std::nullopt);
}

TEST_F(Draft22NamespaceDiscovery, IgnoredOrBytePrefixMatchingOnTheNonmatchingPrefixBlocksAPass) {
    for (const auto& suffix : {namespace_full(), namespace_n()}) {
        Answers nonmatching;
        nonmatching.nonmatching = {request_ok(), suffix};
        EXPECT_EQ(verdict(exchange(nonmatching)), std::nullopt);
    }
    Answers other;
    other.nonmatching = {request_ok(), b({8, 0, 3, 1, 1, 'z'})};
    EXPECT_EQ(verdict(exchange(other)), std::optional<bool>{true}) << "(mo)(z) may be a namespace it knows";
}

TEST_F(Draft22NamespaceDiscovery, RefusedOrUnansweredPrefixesGiveNoVerdict) {
    Answers empty_refused;
    empty_refused.empty = {request_error()};
    EXPECT_EQ(verdict(exchange(empty_refused)), std::nullopt);
    Answers overlap;
    overlap.matching = {request_error(0x7)};
    EXPECT_EQ(verdict(exchange(overlap)), std::nullopt) << "PREFIX_OVERLAP: the cancellation was not yet seen";
    Answers silent;
    silent.matching = {request_ok()};
    auto t = exchange(silent);
    t.complete = false;
    t.timed_out = true;
    EXPECT_EQ(verdict(t), std::nullopt) << "no NAMESPACE yet and no FIN: the duty has no deadline";
    Answers never;
    never.empty = {request_ok()};
    never.cancelled = false;
    EXPECT_EQ(verdict(exchange(never)), std::nullopt);
    Answers nonmatching_silent;
    nonmatching_silent.nonmatching = {};
    t = exchange(nonmatching_silent);
    t.complete = false;
    t.timed_out = true;
    EXPECT_EQ(verdict(t), std::nullopt);
}

TEST_F(Draft22NamespaceDiscovery, UnprovenEvidenceGivesNoVerdict) {
    const auto t = exchange({});
    auto other = t;
    other.scenario_id = "d22-subscribe-bounded-location-range";
    EXPECT_EQ(verdict(other), std::nullopt);
    auto altered = t;
    altered.writes[4].write.bytes.back() = std::byte{1};
    EXPECT_EQ(verdict(altered), std::nullopt);
    auto early = t;
    early.writes[3].accepted_at = kStart + 100ms;
    EXPECT_EQ(verdict(early), std::nullopt) << "the matching prefix went out too soon after the cancellation";
    auto ungated = exchange({});
    ungated.writes[2].delivery_event_count = 3;  // cancelled before the empty prefix was answered
    EXPECT_EQ(verdict(ungated), std::nullopt);
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
