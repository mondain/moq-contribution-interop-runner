#include "moq/interop/scenarios/immutable_repeat.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> input) {
    Bytes out;
    for (const auto v : input) out.push_back(static_cast<std::byte>(v));
    return out;
}
std::vector<ImmutableRepeatProbe> all() {
    auto p = draft18_immutable_repeat_probes();
    const auto other = draft21_immutable_repeat_probes();
    p.insert(p.end(), other.begin(), other.end());
    return p;
}
Bytes properties(const Bytes& inner = b({0x40, 1})) {
    auto out = b({0xb, static_cast<unsigned>(inner.size())});
    out.insert(out.end(), inner.begin(), inner.end());
    return out;
}
Bytes ack(unsigned draft, const Bytes& props) {
    auto out = b({0x18, 0, static_cast<unsigned>(4 + props.size()), 0, 7,
                  draft == 18 ? 10u : 9u, 0});
    out.insert(out.end(), props.begin(), props.end());
    return out;
}
Bytes object(unsigned id, const Bytes& props) {
    auto out = b({5, id, 0x3c, 7, 9, 99, static_cast<unsigned>(props.size())});
    out.insert(out.end(), props.begin(), props.end());
    const auto payload = b({1, 42});
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}
RawProbeTranscript stimulus(const ImmutableRepeatProbe& p) {
    RawProbeTranscript t;
    t.scenario_id = p.definition.id;
    t.setup = {{RawProbeChannel::NewUni, b({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
    t.events = {transport::ConnectionEstablishedEvent{},
                transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false}};
    t.writes.push_back({p.definition.writes[0], 1, p.definition.writes[0].bytes.size(), true, 2});
    t.transport_established = t.peer_setup_received = true;
    return t;
}
void retrieve(RawProbeTranscript& t, const ImmutableRepeatProbe& p, unsigned index,
              const Bytes& track, const Bytes& obj, bool ack_first = true) {
    const transport::StreamDataEvent response{1 + 4 * index, ack(p.draft, track), true};
    const transport::StreamDataEvent data{6 + 4 * index, object(1 + 2 * index, obj), true};
    t.events.push_back(ack_first ? response : data);
    t.events.push_back(ack_first ? data : response);
}
void second_write(RawProbeTranscript& t, const ImmutableRepeatProbe& p) {
    t.writes.push_back({p.definition.writes[1], 5, p.definition.writes[1].bytes.size(), true, t.events.size()});
    t.delivery_event_count = t.events.size();
    t.complete = t.stimulus_delivered = true;
}
RawProbeTranscript complete(const ImmutableRepeatProbe& p, bool ack_first = true) {
    auto t = stimulus(p);
    retrieve(t, p, 0, properties(), properties(), ack_first);
    second_write(t, p);
    retrieve(t, p, 1, properties(), properties(), ack_first);
    return t;
}
TEST(ImmutableRepeat, SixRequiredProfilesOpenCanonicalRequestsWithPropertyResponsesEnabled) {
    const auto p = all();
    ASSERT_EQ(p.size(), 6u);
    for (const auto& profile : p) {
        ASSERT_EQ(profile.definition.writes.size(), 2u);
        for (const auto& write : profile.definition.writes) {
            EXPECT_EQ(write.channel, RawProbeChannel::NewBidi);
            EXPECT_TRUE(write.fin);
        }
        EXPECT_TRUE(profile.definition.writes[1].evidence_ready);
    }
}
TEST(ImmutableRepeat, GateRequiresFirstOrdinaryObjectItsFinAndTypedAckInEitherOrder) {
    ASSERT_EQ(all().size(), 6u);
    for (const auto& p : all()) {
        auto t = stimulus(p);
        ASSERT_TRUE(p.definition.writes[1].evidence_ready);
        const auto ready = [&] { return p.definition.writes[1].evidence_ready({t.writes, t.events}); };
        EXPECT_FALSE(ready());
        t.events.push_back(transport::StreamDataEvent{6, object(1, properties()), false});
        EXPECT_FALSE(ready());
        t.events.push_back(transport::StreamDataEvent{1, ack(p.draft, properties()), true});
        EXPECT_FALSE(ready());
        t.events.push_back(transport::StreamDataEvent{6, {}, true});
        EXPECT_TRUE(ready());
        t = stimulus(p);
        retrieve(t, p, 0, properties(), properties(), true);
        EXPECT_TRUE(ready());
    }
}
TEST(ImmutableRepeat, BothTrackAndObjectDomainsPassAfterActualRepeat) {
    ASSERT_EQ(all().size(), 6u);
    for (const auto& p : all()) for (const auto order : {false, true}) {
        auto t = complete(p, order);
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), true);
        t.complete = false;
        ASSERT_TRUE(p.definition.response_ready);
        EXPECT_TRUE(p.definition.response_ready(t));
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
    }
}
void replace(RawProbeTranscript& t, transport::StreamId id, const Bytes& value, bool fin = true) {
    for (auto& event : t.events)
        if (auto* data = std::get_if<transport::StreamDataEvent>(&event); data && data->stream_id == id) {
            data->data = value;
            data->fin = fin;
        }
}
TEST(ImmutableRepeat, ValueTypeAndListChangesFailOnlyContentAndSerialization) {
    for (const auto& p : all()) for (const auto track : {false, true}) {
        for (const auto& changed : {b({0x40, 2}), b({0x42, 1}), b({0x40, 1, 2, 1})}) {
            auto t = complete(p);
            replace(t, track ? 5 : 10, track ? ack(p.draft, properties(changed))
                                            : object(3, properties(changed)));
            EXPECT_EQ(evaluate_immutable_repeat_probe(t, p),
                      p.aspect == ImmutableRepeatAspect::Presence) << p.requirement_id;
        }
    }
}
TEST(ImmutableRepeat, NonminimalInnerVarintsPreserveContentButFailExactBytes) {
    for (const auto& p : all()) for (const auto track : {false, true}) {
        auto t = complete(p);
        const auto props = properties(b({0x80, 0x40, 0x80, 1}));
        replace(t, track ? 5 : 10, track ? ack(p.draft, props) : object(3, props));
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p),
                  p.aspect != ImmutableRepeatAspect::Serialization);
    }
}
TEST(ImmutableRepeat, OuterEncodingAndMutablePropertiesDoNotChangeInnerProof) {
    for (const auto& p : all()) {
        auto t = complete(p);
        // Both the wrapper type and wrapper length have a legal nonminimal
        // encoding; the mutable unknown property 0x42 follows the wrapper.
        const auto changed = b({0x80, 0xb, 0x80, 2, 0x40, 1, 0x37, 7});
        replace(t, 5, ack(p.draft, changed));
        replace(t, 10, object(3, changed));
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), true);
    }
}
TEST(ImmutableRepeat, RemovalRequiresCompletePropertyBearingSameIdentity) {
    for (const auto& p : all()) for (const auto track : {false, true}) {
        auto t = complete(p);
        replace(t, track ? 5 : 10, track ? ack(p.draft, {}) : object(3, {}));
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p),
            p.aspect == ImmutableRepeatAspect::Presence ? std::optional{false} : std::nullopt);
        if (!track) {
            // Ordinary data without the Properties-present flag cannot prove
            // removal from a property-bearing delivery.
            replace(t, 10, b({5, 3, 0x1c, 7, 9, 99, 1, 42}));
            EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
            // FETCH unavailable ranges have no Properties/Payload fields.
            // Both nonexistent and unknown 7/9 are legal unavailable evidence.
            for (const auto& range : {b({5, 3, 0x80, 0x8c, 7, 9}),
                                      b({5, 3, 0x81, 0x0c, 7, 9})}) {
                replace(t, 10, range);
                EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
            }
            if (p.draft == 21) {
                replace(t, 10, b({5, 3, 0x82, 0x0c, 7, 9}));
                EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
            }
        }
    }
}
TEST(ImmutableRepeat, BothDomainsNeedAnInitiallyObservedWrapper) {
    for (const auto& p : all()) for (const auto track : {false, true}) {
        auto t = complete(p);
        replace(t, track ? 1 : 6, track ? ack(p.draft, {}) : object(1, {}));
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
    }
}
TEST(ImmutableRepeat, EvidencedViolationDominatesMissingOtherDomain) {
    for (const auto& p : all()) for (const auto track : {false, true}) {
        auto t = complete(p);
        replace(t, track ? 6 : 1, track ? object(1, {}) : ack(p.draft, {}));
        const auto changed = p.aspect == ImmutableRepeatAspect::Presence ? Bytes{}
            : p.aspect == ImmutableRepeatAspect::Content ? properties(b({0x40, 2}))
                                                        : properties(b({0x80, 0x40, 1}));
        replace(t, track ? 5 : 10, track ? ack(p.draft, changed) : object(3, changed));
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), false);
        EXPECT_TRUE(p.definition.response_ready(t));
    }
}
TEST(ImmutableRepeat, MalformedNestedAndDuplicateWrappersAreNotRepeatViolations) {
    for (const auto& p : all()) for (const auto first : {false, true}) {
        for (const auto& invalid : {properties(b({0x40})), properties(b({0xb, 0})),
                                    b({0xb, 2, 0x40, 1, 0, 2, 0x40, 1})}) {
            for (const auto track : {false, true}) {
                auto t = complete(p);
                const unsigned index = first ? 0 : 1;
                replace(t, track ? 1 + 4 * index : 6 + 4 * index,
                    track ? ack(p.draft, invalid) : object(1 + 2 * index, invalid));
                EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
                if (first) {
                    t.writes.resize(1);
                    t.events.resize(4);
                    EXPECT_FALSE(p.definition.writes[1].evidence_ready({t.writes, t.events}));
                }
            }
        }
    }
}
TEST(ImmutableRepeat, DefinedTrackPropertiesOnObjectsCannotAnchorProof) {
    for (const auto& p : all()) {
        auto t = stimulus(p);
        retrieve(t, p, 0, properties(), properties(b({0x22, 1})));
        EXPECT_FALSE(p.definition.writes[1].evidence_ready({t.writes, t.events}));
        t = complete(p);
        replace(t, 10, object(3, properties(b({0x22, 1}))));
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
    }
}
TEST(ImmutableRepeat, GatesRejectFutureProofStaleFinCancellationAndAmbiguousStreams) {
    for (const auto& p : all()) for (unsigned mode = 0; mode < 10; ++mode) {
        auto t = complete(p);
        if (mode == 0) t.writes[1].delivery_event_count = 2;
        if (mode == 1) t.writes[0].delivery_event_count = 3;
        if (mode == 2) t.events.insert(t.events.begin() + 2, transport::PeerResetEvent{6, 0});
        if (mode == 3) t.events.insert(t.events.begin() + 2, transport::PeerStopSendingEvent{1, 0});
        if (mode == 4) t.events.insert(t.events.begin() + 2, transport::StreamDataEvent{6, {}, true});
        if (mode == 5) t.events.insert(t.events.begin() + 2,
            transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
        if (mode == 6) replace(t, 6, object(11, properties()));
        if (mode == 7) t.events.insert(t.events.begin() + 2,
            transport::StreamDataEvent{18, object(1, properties()), true});
        if (mode == 8) t.writes[1].stream_id = 1;
        if (mode == 9) {
            replace(t, 6, object(1, properties()), false);
            t.events.push_back(transport::StreamDataEvent{6, {}, true});
        }
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt) << mode;
    }
}
TEST(ImmutableRepeat, FragmentationAndSeparateFinUseEachAcceptedWriteMarker) {
    for (const auto& p : all()) {
        auto t = stimulus(p);
        for (unsigned index = 0; index < 2; ++index) {
            if (index) second_write(t, p);
            for (const auto byte : object(1 + 2 * index, properties()))
                t.events.push_back(transport::StreamDataEvent{6 + 4 * index, Bytes{byte}, false});
            for (const auto byte : ack(p.draft, properties()))
                t.events.push_back(transport::StreamDataEvent{1 + 4 * index, Bytes{byte}, false});
            t.events.push_back(transport::StreamDataEvent{6 + 4 * index, {}, true});
        }
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), true);
    }
}
TEST(ImmutableRepeat, ExactCompletePayloadIsRetainedPastDefault4096Limit) {
    for (const auto& p : all()) {
        auto t = complete(p);
        auto large = object(3, properties());
        large.resize(large.size() - 2);
        // VI64 width2, value5000 = 0x9388.
        large.push_back(std::byte{0x93});
        large.push_back(std::byte{0x88});
        large.insert(large.end(), 5000, std::byte{42});
        replace(t, 10, large);
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), true);
        large.pop_back();
        replace(t, 10, large);
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
    }
}
TEST(ImmutableRepeat, CanonicalActualFixtureAndBothRequestsMustMatch) {
    const std::vector<Bytes> ns{b({'i', 'n', 't', 'e', 'r', 'o', 'p'}), b({'p'})};
    for (const auto draft : {18u, 21u}) {
        const auto configured = draft == 18
            ? draft18_immutable_repeat_probes(std::chrono::milliseconds{1000}, ns, b({'v'}))
            : draft21_immutable_repeat_probes(std::chrono::milliseconds{1000}, ns, b({'v'}));
        const auto defaults = draft == 18 ? draft18_immutable_repeat_probes() : draft21_immutable_repeat_probes();
        for (std::size_t i = 0; i < configured.size(); ++i) {
            auto t = complete(configured[i]);
            // Evaluators recover the configured fixture from accepted bytes.
            EXPECT_EQ(evaluate_immutable_repeat_probe(t, defaults[i]), true);
            t.writes[1].write.bytes.back() ^= std::byte{1};
            EXPECT_EQ(evaluate_immutable_repeat_probe(t, defaults[i]), std::nullopt);
        }
    }
    EXPECT_THROW(draft18_immutable_repeat_probes(std::chrono::milliseconds{0}), std::invalid_argument);
    EXPECT_THROW(draft21_immutable_repeat_probes(std::chrono::milliseconds{1}, {b({'.'})}), std::invalid_argument);
}
TEST(ImmutableRepeat, MissingAcceptedMetadataAndBoundsCannotClaimResults) {
    for (const auto& p : all()) for (unsigned mode = 0; mode < 14; ++mode) {
        auto t = complete(p);
        if (mode == 0) t.writes[0].accepted--;
        if (mode == 1) t.writes[1].fin_accepted = false;
        if (mode == 2) t.writes[0].delivery_event_count.reset();
        if (mode == 3) t.delivery_event_count.reset();
        if (mode == 4) t.setup.accepted--;
        if (mode == 5) t.scenario_id = "claimed";
        if (mode == 6) t.harness_failed = true;
        if (mode == 7) t.timed_out = true;
        if (mode == 8) t.events.resize(4097);
        if (mode == 9) t.events.push_back(transport::DatagramEvent{Bytes(65547)});
        if (mode == 10) t.events.push_back(transport::ConnectionEstablishedEvent{});
        if (mode == 11) t.writes[1].delivery_event_count = 5;
        if (mode == 12) t.writes[0].write.fin = false;
        if (mode == 13) t.writes[1].write.evidence_ready = {};
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt) << mode;
    }
}
TEST(ImmutableRepeat, SharedScenarioFinishesAnyProvenViolationAndMissingWrappers) {
    for (const auto& p : all()) {
        auto t = complete(p);
        replace(t, 6, object(1, {}));
        replace(t, 5, ack(p.draft, properties(b({0x80, 0x40, 1}))));
        EXPECT_TRUE(p.definition.response_ready(t));
        t = complete(p);
        replace(t, 1, ack(p.draft, {}));
        replace(t, 6, object(1, {}));
        EXPECT_TRUE(p.definition.response_ready(t));
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
    }
}
TEST(ImmutableRepeat, TerminalPrefixPreservesCompletedEvidenceAndIgnoresLaterBytes) {
    for (const auto& p : all()) {
        auto t = complete(p);
        t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
        t.events.push_back(transport::StreamDataEvent{10, Bytes(65547), false});
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), true);
        t = complete(p);
        t.events.insert(t.events.begin() + 4,
            transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
        t = complete(p);
        t.events.push_back(transport::DatagramEvent{Bytes(32768)});
        t.events.push_back(transport::DatagramEvent{Bytes(32768)});
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
        t = complete(p);
        t.writes[1].write.bytes.resize(65547);
        EXPECT_EQ(evaluate_immutable_repeat_probe(t, p), std::nullopt);
    }
}
TEST(ImmutableRepeatCatalog, SixBindingsUseActualContextsAndRejectDuplicateRuns) {
    using namespace requirements;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    for (const auto draft : {18u, 21u}) {
        const auto source = load_draft_source(draft, root / "docs", root / "requirements/draft-digests.json");
        const auto catalog = RequirementCatalog::load(source,
            root / "requirements" / ("draft" + std::to_string(draft) + ".json"));
        const auto profiles = draft == 18 ? draft18_immutable_repeat_probes() : draft21_immutable_repeat_probes();
        const auto bindings = draft == 18 ? draft18_executable_bindings() : draft21_executable_bindings();
        ASSERT_EQ(profiles.size(), 3u);
        for (const auto& p : profiles)
            EXPECT_TRUE(std::ranges::any_of(bindings, [&](const auto& binding) {
                return binding.requirement_id == p.requirement_id && binding.scenario_id == p.definition.id &&
                       binding.evaluator_id == p.evaluator_id;
            })) << p.requirement_id;
        const auto outcomes = [&](const std::vector<RawProbeTranscript>& transcripts) {
            if (draft == 21) return evaluate_draft21_raw_probes(catalog, transcripts);
            std::vector<ScenarioContext> contexts;
            for (const auto& t : transcripts) {
                ScenarioContext c;
                c.scenario_id = t.scenario_id;
                c.complete = t.complete;
                c.stimulus_delivered = t.stimulus_delivered;
                c.raw_probe = t;
                contexts.push_back(std::move(c));
            }
            return evaluate_draft18(catalog, contexts);
        };
        const auto state = [&](const std::vector<RawProbeTranscript>& t, const ImmutableRepeatProbe& p) {
            const auto results = outcomes(t);
            const auto found = std::ranges::find_if(results, [&](const auto& o) {
                return o.requirement_id == p.requirement_id;
            });
            EXPECT_NE(found, results.end());
            return found == results.end() ? OutcomeState::NotRun : found->state;
        };
        std::vector<RawProbeTranscript> contexts{complete(profiles.front())};
        if (draft == 18) contexts.push_back(complete(profiles.back()));
        for (const auto& p : profiles) {
            EXPECT_EQ(state(contexts, p), OutcomeState::Pass) << p.requirement_id;
            EXPECT_EQ(state({}, p), OutcomeState::NotRun);
            auto duplicate = contexts;
            const auto selected = std::ranges::find_if(contexts, [&](const auto& t) {
                return t.scenario_id == p.definition.id;
            });
            ASSERT_NE(selected, contexts.end());
            duplicate.push_back(*selected);
            EXPECT_EQ(state(duplicate, p), OutcomeState::NotRun);
            auto changed = contexts;
            for (auto& t : changed) if (t.scenario_id == p.definition.id) {
                const auto second = p.aspect == ImmutableRepeatAspect::Presence ? Bytes{}
                    : properties(p.aspect == ImmutableRepeatAspect::Content ? b({0x40, 2})
                                                                           : b({0x80, 0x40, 1}));
                replace(t, 10, object(3, second));
            }
            EXPECT_EQ(state(changed, p), OutcomeState::Fail);
        }
    }
}
} // namespace
} // namespace moq::interop::scenarios
