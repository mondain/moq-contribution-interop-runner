#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/scenarios/draft18_contribution.h"
#include "moq/interop/scenarios/draft18_peer_close.h"
#include "moq/interop/scenarios/draft21_contribution.h"
#include "moq/interop/scenarios/draft21_peer_close.h"
#include "moq/interop/scenarios/raw_probe.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;

// Draft 21 PUBLISH_NAMESPACE (0x6): Request ID, one namespace field "n", no Parameters.
Bytes announce21(unsigned parameters = 0, unsigned field = 'n') {
    Bytes body = bytes_of({0, 1, 1, field, 0});
    if (parameters != 0) body = bytes_of({0, 1, 1, field, 1, 2, 1});  // one Parameter: type 2, value 1
    Bytes frame = bytes_of({0x6, 0, static_cast<unsigned>(body.size())});
    frame.insert(frame.end(), body.begin(), body.end());
    return frame;
}
Bytes announce18(std::uint64_t id) {
    return encode(d18::PublishNamespaceMessage{id, d18::TrackNamespace{{text("n")}}, {}});
}

RawProbeDefinition plain(const std::string& id) {
    return RawProbeDefinition{id, bytes_of({0xaf, 0, 0, 0}), {}, true,
        [](std::span<const std::byte> input) { return !input.empty(); }, std::chrono::milliseconds{50}};
}

// Feeds `streams` (id, bytes) to a controller and returns what it wrote.
struct Outcome {
    std::map<transport::StreamId, std::vector<std::byte>> output;
    RawProbeTranscript transcript;
};
Outcome feed(const RawProbeDefinition& definition,
             const std::vector<std::pair<transport::StreamId, Bytes>>& streams) {
    ScriptTransport transport;
    transport.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
    RawProbeController controller(transport, definition);
    transport.events.push_back(transport::StreamDataEvent{2, bytes_of({0xaf, 0, 0, 0}), false});
    for (const auto& [id, bytes] : streams) transport.events.push_back(transport::StreamDataEvent{id, bytes, false});
    const auto start = RawProbeClock::now();
    for (int step = 0; step < 6; ++step) {
        transport.events.push_back(transport::DatagramEvent{{std::byte{0}}});
        controller.poll(start + std::chrono::milliseconds(step));
    }
    // Only answers on peer-opened bidirectional streams count; the probe's own SETUP
    // goes out on a unidirectional stream.
    std::map<transport::StreamId, std::vector<std::byte>> answers;
    for (const auto& [id, bytes] : transport.output)
        if ((id & 3u) == 0u && !bytes.empty()) answers.emplace(id, bytes);
    return {answers, controller.transcript()};
}

TEST(DefaultNamespaceAnswer, AppliesTheDraftSpecificMechanism) {
    auto d18def = plain("a");
    EXPECT_EQ(apply_default_namespace_answer(d18def, 18), DefaultNamespaceAnswer::Applied);
    EXPECT_TRUE(d18def.acknowledge_publisher_namespace);
    EXPECT_FALSE(d18def.acknowledge_publisher_namespace_draft21);
    auto d21def = plain("a");
    EXPECT_EQ(apply_default_namespace_answer(d21def, 21), DefaultNamespaceAnswer::Applied);
    EXPECT_TRUE(d21def.acknowledge_publisher_namespace_draft21);
    EXPECT_FALSE(d21def.acknowledge_publisher_namespace);
    auto other = plain("a");
    EXPECT_EQ(apply_default_namespace_answer(other, 17), DefaultNamespaceAnswer::UnsupportedDraft);
}

TEST(DefaultNamespaceAnswer, Draft18AnswersWithRequestOkAndKeepsTheStimulusProofValid) {
    auto def = plain("d18");
    ASSERT_EQ(apply_default_namespace_answer(def, 18), DefaultNamespaceAnswer::Applied);
    const auto result = feed(def, {{0, announce18(0)}});
    ASSERT_EQ(result.transcript.acknowledgements.size(), 1u);
    EXPECT_EQ(result.output.at(0), ok());
    EXPECT_TRUE(result.transcript.auto_replies.empty());
    EXPECT_TRUE(result.transcript.writes.empty());
}

TEST(DefaultNamespaceAnswer, Draft21AnswersWithRequestOkBytes) {
    auto def = plain("d21");
    ASSERT_EQ(apply_default_namespace_answer(def, 21), DefaultNamespaceAnswer::Applied);
    const auto result = feed(def, {{0, announce21()}});
    ASSERT_EQ(result.transcript.acknowledgements.size(), 1u);
    EXPECT_EQ(result.output.at(0), bytes_of({7, 0, 1, 0}));  // draft 21 Section 9.3 Figure 7
    EXPECT_TRUE(result.transcript.auto_replies.empty());
}

TEST(DefaultNamespaceAnswer, Draft21LeavesParametersReservedNamespaceAndOtherRequestsUnanswered) {
    auto def = plain("d21");
    ASSERT_EQ(apply_default_namespace_answer(def, 21), DefaultNamespaceAnswer::Applied);
    const auto result = feed(def, {{0, announce21(1)}, {4, announce21(0, '.')},
        {8, bytes_of({0x3, 0, 2, 0, 0})},                    // another request type
        {12, bytes_of({0x6, 0, 5, 0, 1, 1, 'n'})}});         // truncated: not yet complete
    EXPECT_TRUE(result.transcript.acknowledgements.empty());
    EXPECT_TRUE(result.output.empty());
}

TEST(DefaultNamespaceAnswer, Draft18DoesNotAnswerParametersOrReservedNamespace) {
    auto def = plain("d18");
    ASSERT_EQ(apply_default_namespace_answer(def, 18), DefaultNamespaceAnswer::Applied);
    const auto reserved = encode(d18::PublishNamespaceMessage{2, d18::TrackNamespace{{text(".")}}, {}});
    const auto result = feed(def, {{4, reserved}});
    EXPECT_TRUE(result.transcript.acknowledgements.empty());
}

TEST(DefaultNamespaceAnswer, ExistingMechanismsAreLeftAlone) {
    for (const unsigned draft : {18u, 21u}) {
        auto own18 = plain("a");
        own18.acknowledge_publisher_namespace = true;
        EXPECT_EQ(apply_default_namespace_answer(own18, draft), DefaultNamespaceAnswer::OwnMechanism);
        auto own21 = plain("a");
        own21.acknowledge_publisher_namespace_draft21 = true;
        EXPECT_EQ(apply_default_namespace_answer(own21, draft), DefaultNamespaceAnswer::OwnMechanism);
        EXPECT_FALSE(own21.acknowledge_publisher_namespace);
        auto auto_accept = plain("a");
        auto_accept.auto_accept_ready = [](std::span<const std::byte>) { return true; };
        EXPECT_EQ(apply_default_namespace_answer(auto_accept, draft), DefaultNamespaceAnswer::OwnMechanism);
        EXPECT_FALSE(auto_accept.acknowledge_publisher_namespace || auto_accept.acknowledge_publisher_namespace_draft21);
    }
}

TEST(DefaultNamespaceAnswer, DefinitionsAddressingThePublishersRequestAreSkipped) {
    for (const unsigned draft : {18u, 21u}) {
        auto writes = plain("a");
        writes.peer_request_ready = [](std::span<const std::byte>) { return true; };
        writes.writes.push_back({RawProbeChannel::PeerBidi, bytes_of({1}), false});
        EXPECT_EQ(apply_default_namespace_answer(writes, draft), DefaultNamespaceAnswer::TargetsRequest);
        EXPECT_FALSE(writes.acknowledge_publisher_namespace || writes.acknowledge_publisher_namespace_draft21);
        auto ready_only = plain("a");
        ready_only.peer_request_ready = [](std::span<const std::byte>) { return true; };
        EXPECT_EQ(apply_default_namespace_answer(ready_only, draft), DefaultNamespaceAnswer::TargetsRequest);
    }
}

TEST(DefaultNamespaceAnswer, ConflictingCombinationsAreRejectedByTheController) {
    ScriptTransport transport;
    auto both = plain("a");
    both.acknowledge_publisher_namespace = both.acknowledge_publisher_namespace_draft21 = true;
    EXPECT_THROW(RawProbeController(transport, both), std::invalid_argument);
    for (const bool d21 : {false, true}) {
        auto peer = plain("a");
        (d21 ? peer.acknowledge_publisher_namespace_draft21 : peer.acknowledge_publisher_namespace) = true;
        peer.peer_request_ready = [](std::span<const std::byte>) { return true; };
        peer.writes.push_back({RawProbeChannel::PeerBidi, bytes_of({1}), false});
        EXPECT_THROW(RawProbeController(transport, peer), std::invalid_argument);
    }
}

TEST(DefaultNamespaceAnswer, ExplicitOptOutFlagIsRespected) {
    for (const unsigned draft : {18u, 21u}) {
        auto def = plain("a");
        def.no_default_namespace_answer = true;
        EXPECT_EQ(apply_default_namespace_answer(def, draft), DefaultNamespaceAnswer::OptedOut);
        EXPECT_FALSE(def.acknowledge_publisher_namespace || def.acknowledge_publisher_namespace_draft21);
        const auto result = feed(def, {{0, draft == 18 ? announce18(0) : announce21()}});
        EXPECT_TRUE(result.output.empty());
        EXPECT_TRUE(result.transcript.acknowledgements.empty());
    }
}

// The scenarios whose subject is an unanswered, rejected or otherwise specially
// answered announcement, or that hold the publisher's only request stream, with
// the reason each opts out of the default answer.
TEST(DefaultNamespaceAnswer, EnumeratedOptOutsAreRealScenariosWithReasons) {
    const auto table = namespace_answer_opt_outs();
    std::set<std::pair<unsigned, std::string_view>> seen;
    for (const auto& entry : table) {
        EXPECT_FALSE(entry.reason.empty()) << entry.scenario;
        EXPECT_TRUE(seen.insert({entry.draft, entry.scenario}).second) << "duplicate " << entry.scenario;
        EXPECT_TRUE(app::raw_probe_scenario(entry.draft, entry.scenario)) << entry.draft << " " << entry.scenario;
        auto def = plain(std::string(entry.scenario));
        EXPECT_EQ(apply_default_namespace_answer(def, entry.draft), DefaultNamespaceAnswer::OptedOut);
        EXPECT_FALSE(def.acknowledge_publisher_namespace || def.acknowledge_publisher_namespace_draft21);
        // The same id under the other draft is not affected unless it is listed too.
        auto other = plain(std::string(entry.scenario));
        const unsigned other_draft = entry.draft == 18 ? 21 : 18;
        if (!seen.contains({other_draft, entry.scenario}))
            EXPECT_NE(apply_default_namespace_answer(other, other_draft), DefaultNamespaceAnswer::OptedOut);
    }
}

// A probe whose own response goes to a publisher PUBLISH (or TRACK_STATUS) is answered
// beside it: the announcement is acknowledged, the targeted stream never is. A probe
// whose response goes to the PUBLISH_NAMESPACE itself (a rejection, a redirect, a
// malformed REQUEST_OK, an unknown error code) is never given the default answer.
TEST(DefaultNamespaceAnswer, AcknowledgesBesideAProbeTargetingAnotherRequest) {
    for (const unsigned draft : {18u, 21u}) {
        auto def = plain("beside");
        // Targets any request that is not a PUBLISH_NAMESPACE: here one starting with 0x1d/0x1.
        def.peer_request_ready = [draft](std::span<const std::byte> input) {
            return !input.empty() && input[0] == std::byte{draft == 21 ? 0x1d : 0x1};
        };
        def.writes.push_back({RawProbeChannel::PeerBidi, bytes_of({9}), false});
        ASSERT_EQ(apply_default_namespace_answer(def, draft), DefaultNamespaceAnswer::Applied);
        EXPECT_TRUE(def.acknowledge_skips_peer_target);
        const Bytes target = draft == 21 ? bytes_of({0x1d, 0, 1, 0}) : bytes_of({0x1, 0, 1, 0});
        const auto result = feed(def, {{0, draft == 21 ? announce21() : announce18(0)}, {4, target}});
        EXPECT_EQ(result.output.count(0), 1u);          // the announcement was answered
        EXPECT_EQ(result.output.count(4) ? result.output.at(4) : Bytes{}, result.output.count(4) ? bytes_of({9}) : Bytes{});
        ASSERT_EQ(result.transcript.acknowledgements.size(), 1u);
        EXPECT_EQ(result.transcript.acknowledgements.front().stream_id, 0u);
    }
}

TEST(DefaultNamespaceAnswer, ProbesThatAnswerThePublishersAnnouncementThemselvesAreNotAcknowledged) {
    std::size_t skipped = 0, beside = 0;
    const auto check = [&](RawProbeDefinition definition, unsigned draft) {
        const bool peer_writes = std::any_of(definition.writes.begin(), definition.writes.end(),
            [](const auto& write) { return write.channel == RawProbeChannel::PeerBidi; });
        if (!peer_writes) return;
        const auto decision = apply_default_namespace_answer(definition, draft);
        if (decision == DefaultNamespaceAnswer::Applied) {
            ++beside;
            EXPECT_TRUE(definition.acknowledge_skips_peer_target) << definition.id;
            EXPECT_EQ(definition.id.find("namespace"), std::string::npos) << definition.id;
        } else {
            ++skipped;
            EXPECT_FALSE(definition.acknowledge_publisher_namespace_draft21 && !definition.auto_accept_ready) << definition.id;
            if (decision == DefaultNamespaceAnswer::TargetsRequest)
                EXPECT_FALSE(definition.acknowledge_publisher_namespace) << definition.id;
        }
    };
    for (auto& probe : draft18_peer_close_probes()) check(std::move(probe.definition), 18);
    for (auto& probe : draft21_peer_close_probes()) check(std::move(probe.definition), 21);
    for (auto& probe : draft18_contribution_probes()) check(std::move(probe.definition), 18);
    for (auto& probe : draft21_contribution_probes()) check(std::move(probe.definition), 21);
    EXPECT_GT(skipped, 5u);
    EXPECT_GT(beside, 0u);
    // The namespace-targeting probes named in the audit are skipped, not answered.
    for (auto& probe : draft18_peer_close_probes())
        if (probe.definition.id.find("publish-namespace") != std::string::npos)
            EXPECT_EQ(apply_default_namespace_answer(probe.definition, 18), DefaultNamespaceAnswer::TargetsRequest);
    for (auto& probe : draft21_peer_close_probes())
        if (probe.definition.id.find("publish-namespace") != std::string::npos)
            EXPECT_EQ(apply_default_namespace_answer(probe.definition, 21), DefaultNamespaceAnswer::TargetsRequest);
    bool unknown_error = false;
    for (auto& probe : draft18_contribution_probes())
        if (probe.definition.id == "publisher-request-rejected-with-unknown-error") {
            unknown_error = true;
            EXPECT_EQ(apply_default_namespace_answer(probe.definition, 18), DefaultNamespaceAnswer::TargetsRequest);
        }
    EXPECT_TRUE(unknown_error);
}

}  // namespace
}  // namespace moq::interop::scenarios
