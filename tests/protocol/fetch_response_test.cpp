#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/scenarios/fetch_response.h"
#include <filesystem>
#include <gtest/gtest.h>
namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes bytes(std::initializer_list<unsigned> v) {
    Bytes r;
    for (auto x : v)
        r.push_back(static_cast<std::byte>(x));
    return r;
}
RawProbeTranscript stimulus(const FetchResponseProbe& p) {
    RawProbeTranscript t;
    t.scenario_id = p.definition.id;
    t.setup = {{RawProbeChannel::NewUni, bytes({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
    t.events = {transport::ConnectionEstablishedEvent{},
                transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 0}), false}};
    auto w = p.definition.writes.front();
    t.writes.push_back({w, 1, w.bytes.size(), true, 2});
    t.complete = t.stimulus_delivered = t.transport_established = t.peer_setup_received = true;
    t.delivery_event_count = 2;
    return t;
}
TEST(FetchResponse, SingletonRequiresActualFinAndMatchingObservedBranch) {
    auto ps = draft21_fetch_response_probes();
    ASSERT_EQ(ps.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        auto t = stimulus(ps[i]);
        auto reply = i == 0 ? bytes({0x18, 0, 4, 0, 0, 1, 0}) : bytes({5, 0, 3, 1, 0, 0});
        t.events.push_back(transport::StreamDataEvent{1, reply, false});
        EXPECT_FALSE(ps[i].definition.response_ready(t));
        EXPECT_EQ(evaluate_fetch_response_probe(t, ps[i]), std::nullopt);
        t.events.push_back(transport::StreamDataEvent{1, {}, true});
        EXPECT_TRUE(ps[i].definition.response_ready(t));
        EXPECT_EQ(evaluate_fetch_response_probe(t, ps[i]), true);
        auto other = stimulus(ps[1 - i]);
        other.events.insert(other.events.end(), t.events.begin() + 2, t.events.end());
        EXPECT_EQ(evaluate_fetch_response_probe(other, ps[1 - i]), std::nullopt);
    }
}
TEST(FetchResponse, DuplicateTypedRepliesFailBothContextsWithoutWaitingForFin) {
    auto ps = draft21_fetch_response_probes();
    for (const auto& p : ps)
        for (const auto& first : {bytes({0x18, 0, 4, 0, 0, 1, 0}), bytes({5, 0, 3, 1, 0, 0})}) {
            auto t = stimulus(p);
            t.events.push_back(transport::StreamDataEvent{1, first, false});
            t.events.push_back(transport::StreamDataEvent{1, bytes({5, 0, 3, 1, 0, 0}), false});
            EXPECT_TRUE(p.definition.response_ready(t));
            EXPECT_EQ(evaluate_fetch_response_probe(t, p), false);
        }
}
TEST(FetchResponse, EmptyFinFailsAndResetOrCloseOrMissingFinIsUnscored) {
    for (const auto& p : draft21_fetch_response_probes()) {
        auto t = stimulus(p);
        t.events.push_back(transport::StreamDataEvent{1, {}, true});
        EXPECT_EQ(evaluate_fetch_response_probe(t, p), false);
        for (unsigned v = 0; v < 3; ++v) {
            t = stimulus(p);
            t.events.push_back(
                transport::StreamDataEvent{1, bytes({0x18, 0, 4, 0, 0, 1, 0}), false});
            if (v == 0)
                t.events.push_back(transport::PeerResetEvent{1, 1});
            if (v == 1)
                t.events.push_back(
                    transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
            EXPECT_EQ(evaluate_fetch_response_probe(t, p), std::nullopt);
        }
    }
}
TEST(FetchResponse, FragmentationUnrelatedStreamsAndMalformedScopes) {
    auto p = draft21_fetch_response_probes().front();
    auto t = stimulus(p);
    t.events.push_back(transport::StreamDataEvent{5, bytes({5, 0, 3, 1, 0, 0}), true});
    t.events.push_back(transport::StreamDataEvent{1, bytes({0x18, 0, 4}), false});
    t.events.push_back(transport::StreamDataEvent{1, bytes({0, 0, 1, 0}), true});
    EXPECT_EQ(evaluate_fetch_response_probe(t, p), true);
    for (const auto& bad : {bytes({0x18, 0, 4, 2, 0, 1, 0}), bytes({0x18, 0, 6, 0, 0, 1, 1, 8, 0}),
                            bytes({5, 0, 4, 1, 0, 1, 0x80}), bytes({0x18, 0, 4, 0})}) {
        t = stimulus(p);
        t.events.push_back(transport::StreamDataEvent{1, bad, true});
        EXPECT_EQ(evaluate_fetch_response_probe(t, p), std::nullopt);
    }
    t = stimulus(p);
    t.events.insert(t.events.begin() + 1,
                    transport::StreamDataEvent{1, bytes({0x18, 0, 4, 0, 0, 1, 0}), false});
    t.writes.front().delivery_event_count = t.delivery_event_count = 3;
    t.events.push_back(transport::StreamDataEvent{1, {}, true});
    EXPECT_EQ(evaluate_fetch_response_probe(t, p), std::nullopt);
}
TEST(FetchResponse, CanonicalFixtureAndBounds) {
    auto p = draft21_fetch_response_probes().front();
    EXPECT_EQ(p.definition.writes.front().bytes,
              bytes({0x16, 0,   18,  1,   0,   1,   'x', 1,   0x21, 11, 0,
                     0,    255, 255, 255, 255, 255, 255, 255, 255,  255}));
    EXPECT_THROW(draft21_fetch_response_probes(std::chrono::milliseconds{0}),
                 std::invalid_argument);
    EXPECT_THROW(draft21_fetch_response_probes(std::chrono::milliseconds{1}, {bytes({'.'})}),
                 std::invalid_argument);
    EXPECT_THROW(draft21_fetch_response_probes(std::chrono::milliseconds{1},
                                               {bytes({'.', 's', 'e', 's', 's', 'i', 'o', 'n'})},
                                               {}),
                 std::invalid_argument);
    EXPECT_NO_THROW(draft21_fetch_response_probes(std::chrono::milliseconds{1}, {}, {}));
    for (unsigned v = 0; v < 6; ++v) {
        auto t = stimulus(p);
        t.events.push_back(transport::StreamDataEvent{1, bytes({0x18, 0, 4, 0, 0, 1, 0}), true});
        if (v == 0)
            t.writes.front().write.bytes[3] = std::byte{3};
        if (v == 1)
            t.writes.front().write.bytes.back() = std::byte{0};
        if (v == 2)
            t.writes.front().fin_accepted = false;
        if (v == 3)
            t.writes.front().stream_id = 5;
        if (v == 4)
            t.events.resize(4097);
        if (v == 5)
            t.events.push_back(transport::StreamDataEvent{1, bytes({5, 0, 3, 1, 0, 0}), false});
        EXPECT_EQ(evaluate_fetch_response_probe(t, p), std::nullopt);
    }
}
TEST(FetchResponse, ConfiguredFixtureAndInvalidHarnessEvidence) {
    const auto p =
        draft21_fetch_response_probes(std::chrono::milliseconds{1000}, {bytes({'n'})}, bytes({'t'}))
            .front();
    EXPECT_EQ(p.definition.writes.front().bytes,
              bytes({0x16, 0, 20,  1,   1,   1,   'n', 1,   't', 1,   0x21, 11,
                     0,    0, 255, 255, 255, 255, 255, 255, 255, 255, 255}));
    auto t = stimulus(p);
    t.events.push_back(transport::StreamDataEvent{1, bytes({0x18, 0, 4, 0, 0, 1, 0}), true});
    EXPECT_EQ(evaluate_fetch_response_probe(t, p), true);
    for (unsigned v = 0; v < 4; ++v) {
        auto bad = t;
        if (v == 0)
            bad.harness_failed = true;
        if (v == 1)
            bad.timed_out = true;
        if (v == 2)
            bad.stimulus_delivered = false;
        if (v == 3)
            bad.complete = false;
        EXPECT_EQ(evaluate_fetch_response_probe(bad, p), std::nullopt);
    }
}
TEST(FetchResponse, CatalogRequiresBothContextsAndPropagatesDuplicateFailure) {
    using namespace requirements;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source =
        load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, root / "requirements/draft21.json");
    auto ps = draft21_fetch_response_probes();
    std::vector<RawProbeTranscript> ts;
    for (std::size_t i = 0; i < 2; ++i) {
        auto t = stimulus(ps[i]);
        t.events.push_back(transport::StreamDataEvent{
            1, i == 0 ? bytes({0x18, 0, 4, 0, 0, 1, 0}) : bytes({5, 0, 3, 1, 0, 0}), true});
        ts.push_back(std::move(t));
    }
    const auto state = [&](const auto& input) {
        auto outcomes = evaluate_draft21_raw_probes(catalog, input);
        for (const auto& o : outcomes)
            if (o.requirement_id == "D21-3-2-1-MUST-052")
                return o.state;
        return OutcomeState::NotRun;
    };
    EXPECT_EQ(state(ts), OutcomeState::Pass);
    EXPECT_EQ(state(std::vector<RawProbeTranscript>{ts.front()}), OutcomeState::NotRun);
    EXPECT_EQ(state(std::vector<RawProbeTranscript>{ts.front(), ts.front(), ts.back()}),
              OutcomeState::NotRun);
    auto& reply = std::get<transport::StreamDataEvent>(ts.front().events.back()).data;
    const auto duplicate = reply;
    reply.insert(reply.end(), duplicate.begin(), duplicate.end());
    EXPECT_EQ(state(ts), OutcomeState::Fail);
}
} // namespace
} // namespace moq::interop::scenarios
