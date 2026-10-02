#include "moq/interop/scenarios/draft18_gap_a.h"
#include "support/scripted_publisher.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace moq::interop::scenarios {
namespace {
using namespace test;
namespace d18 = wire::draft18;

Bytes setup(d18::KeyValuePairs options = {}) { return encode_draft18(d18::SetupMessage{std::move(options)}); }
Bytes ok() { return encode_draft18(d18::RequestOkMessage{{}, {}}); }
Bytes error(std::uint64_t code) {
    return encode_draft18(d18::RequestErrorMessage{code, 0, {}, std::nullopt});
}
Bytes subscribe_ok(std::uint64_t alias = 4, d18::Parameters parameters = {}) {
    return encode_draft18(d18::SubscribeOkMessage{alias, std::move(parameters), {}});
}

Draft18GapAProbe profile(const std::string& requirement) {
    auto profiles = draft18_gap_a_probes(std::chrono::milliseconds(200), {bytes_of("n")}, bytes_of("t"));
    const auto found = std::find_if(profiles.begin(), profiles.end(),
        [&](const auto& candidate) { return candidate.requirement_id == requirement; });
    if (found == profiles.end()) throw std::runtime_error("missing profile " + requirement);
    return *found;
}

std::optional<bool> score(const Draft18GapAProbe& probe, ScriptedPublisher& publisher, bool webtransport = false,
                          std::optional<std::uint64_t> compatibility = std::nullopt) {
    auto transcript = run_probe(publisher, probe.definition);
    transcript.unknown_auth_token_alias_compatibility_code = compatibility;
    return evaluate_draft18_gap_a_probe(transcript, probe, webtransport);
}

TEST(Draft18GapASetup, UnknownSetupOptionIsSentAndSessionContinues) {
    const auto probe = profile("D18-10-3-MUST-001");
    EXPECT_EQ(probe.definition.id, "receive-setup-with-unknown-option");
    // Section 14 GREASE option 0x9d with a one-byte value, ignored by receivers.
    EXPECT_EQ(probe.definition.setup_bytes, (Bytes{std::byte{0xaf}, std::byte{0}, std::byte{0}, std::byte{4},
        std::byte{0x80}, std::byte{0x9d}, std::byte{1}, std::byte{0}}));
    ScriptedPublisher publisher(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("a")) { peer.mark("a"); peer.data(1, subscribe_ok()); }
    });
    EXPECT_EQ(score(probe, publisher), std::optional<bool>{true});
    ScriptedPublisher rejecting(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("a")) { peer.mark("a"); peer.data(1, error(0x10), true); }
    });
    EXPECT_EQ(score(probe, rejecting), std::optional<bool>{true});
}

TEST(Draft18GapASetup, UnknownOptionSessionCloseIsAProtocolViolationFailure) {
    const auto probe = profile("D18-10-3-MUST-001");
    ScriptedPublisher closing(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("a")) { peer.mark("a"); peer.close_session(3); }
    });
    EXPECT_EQ(score(probe, closing), std::optional<bool>{false});
    ScriptedPublisher other_close(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("a")) { peer.mark("a"); peer.close_session(1); }
    });
    EXPECT_EQ(score(probe, other_close), std::nullopt);
    ScriptedPublisher silent(setup());
    EXPECT_EQ(score(probe, silent), std::nullopt);
}

TEST(Draft18GapASetup, ControlStreamStaysOpenWhileRequestCompletes) {
    const auto probe = profile("D18-3-3-MUST-NOT-002");
    ScriptedPublisher publisher(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("a")) { peer.mark("a"); peer.data(1, ok(), true); }
    });
    EXPECT_EQ(score(probe, publisher), std::optional<bool>{true});
    ScriptedPublisher closes_control(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("a")) { peer.mark("a"); peer.data(1, ok(), true); peer.fin(2); }
    });
    EXPECT_EQ(score(probe, closes_control), std::optional<bool>{false});
    ScriptedPublisher resets_control(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("a")) { peer.mark("a"); peer.peer_reset(2); }
    });
    EXPECT_EQ(score(probe, resets_control), std::optional<bool>{false});
    ScriptedPublisher never_completes(setup());
    EXPECT_EQ(score(probe, never_completes), std::nullopt);
}

TEST(Draft18GapASetup, DatagramNegotiationNeedsEstablishedSessionWithCapacity) {
    const auto probe = profile("D18-3-1-MUST-001");
    ScriptedPublisher capable(setup());
    EXPECT_EQ(score(probe, capable), std::optional<bool>{true});
    ScriptedPublisher without(setup());
    without.datagram_payload = 0;
    EXPECT_EQ(score(probe, without), std::optional<bool>{false});
}

TEST(Draft18GapASetup, NativeClientSetupMustCarryAuthorityAndPath) {
    const auto probe = profile("D18-3-2-MUST-001");
    EXPECT_TRUE(probe.native_only);
    ScriptedPublisher complete(setup({{1, d18::ByteValue{bytes_of("/live")}}, {5, d18::ByteValue{bytes_of("relay.example:4443")}}}));
    EXPECT_EQ(score(probe, complete), std::optional<bool>{true});
    ScriptedPublisher empty_path(setup({{1, d18::ByteValue{{}}}, {5, d18::ByteValue{bytes_of("relay.example")}}}));
    EXPECT_EQ(score(probe, empty_path), std::optional<bool>{true});
    ScriptedPublisher no_path(setup({{5, d18::ByteValue{bytes_of("relay.example")}}}));
    EXPECT_EQ(score(probe, no_path), std::optional<bool>{false});
    ScriptedPublisher no_authority(setup({{1, d18::ByteValue{bytes_of("/live")}}}));
    EXPECT_EQ(score(probe, no_authority), std::optional<bool>{false});
    ScriptedPublisher empty_authority(setup({{1, d18::ByteValue{bytes_of("/")}}, {5, d18::ByteValue{{}}}}));
    EXPECT_EQ(score(probe, empty_authority), std::optional<bool>{false});
    ScriptedPublisher webtransport(setup());
    EXPECT_EQ(score(probe, webtransport, true), std::nullopt);
}

// Token probes drive TRACK_STATUS requests 1, 3, 5 on local streams 1, 5, 9.
Bytes cache_setup(std::uint64_t size) { return setup({{4, d18::VarIntValue{size, {}}}}); }
void token_reaction(ScriptedPublisher& peer, std::vector<Bytes> answers, std::vector<bool> fins) {
    const std::uint64_t streams[] = {1, 5, 9};
    for (std::size_t i = 0; i < answers.size(); ++i) {
        const auto key = "s" + std::to_string(i);
        if (peer.sent(streams[i]) && !peer.answered(key)) {
            peer.mark(key);
            peer.data(streams[i], answers[i], fins[i]);
        }
    }
}
d18::Token token_of(const Bytes& frame) {
    wire::Cursor cursor(frame);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto& message = std::get<d18::Message>(decoded);
    const auto& status = std::get<d18::TrackStatusMessage>(message);
    return std::get<d18::Token>(status.parameters.at(0).value);
}

TEST(Draft18GapASetup, TokenRegisterIsWithheldUntilPublisherAdvertisesCacheSpace) {
    const auto probe = profile("D18-10-2-2-MUST-002");
    ScriptedPublisher no_cache(setup());
    EXPECT_EQ(score(probe, no_cache), std::nullopt);
    EXPECT_EQ(no_cache.sent(1), nullptr);
    ScriptedPublisher tiny(cache_setup(19));
    EXPECT_EQ(score(probe, tiny), std::nullopt);
    EXPECT_EQ(tiny.sent(1), nullptr);
    ScriptedPublisher enough(cache_setup(20), [](ScriptedPublisher& peer) {
        token_reaction(peer, {ok(), ok()}, {true, true});
    });
    EXPECT_EQ(score(probe, enough), std::optional<bool>{true});
    const auto token = token_of(enough.sent(1)->bytes);
    EXPECT_EQ(token.alias_type, d18::TokenAliasType::Register);
    EXPECT_EQ(token.token_type, std::optional<std::uint64_t>{0});
    EXPECT_EQ(token_of(enough.sent(5)->bytes).alias_type, d18::TokenAliasType::UseAlias);
    EXPECT_EQ(token_of(enough.sent(5)->bytes).alias, token.alias);
}

TEST(Draft18GapASetup, AliasUseMustMatchTheRegisteringRequestOutcome) {
    const auto probe = profile("D18-10-2-2-MUST-002");
    ScriptedPublisher same_error(cache_setup(64), [](ScriptedPublisher& peer) {
        token_reaction(peer, {error(1), error(1)}, {true, true});
    });
    EXPECT_EQ(score(probe, same_error), std::optional<bool>{true});
    ScriptedPublisher unknown(cache_setup(64), [](ScriptedPublisher& peer) {
        token_reaction(peer, {error(1), error(0x17)}, {true, true});
    });
    EXPECT_EQ(score(probe, unknown, false, 0x17), std::optional<bool>{false});
    ScriptedPublisher lost_after_ok(cache_setup(64), [](ScriptedPublisher& peer) {
        token_reaction(peer, {ok(), error(0x17)}, {true, true});
    });
    EXPECT_EQ(score(probe, lost_after_ok), std::optional<bool>{false});
    ScriptedPublisher ambiguous(cache_setup(64), [](ScriptedPublisher& peer) {
        token_reaction(peer, {error(1), error(9)}, {true, true});
    });
    EXPECT_EQ(score(probe, ambiguous), std::nullopt);
}

TEST(Draft18GapASetup, DeletedAliasNeedsConfiguredUnknownAliasMapping) {
    const auto probe = profile("D18-10-2-2-MUST-001");
    const auto reaction = [](std::uint64_t code) {
        return [code](ScriptedPublisher& peer) {
            token_reaction(peer, {ok(), ok(), error(code)}, {true, true, true});
        };
    };
    ScriptedPublisher rejects(cache_setup(64), reaction(0x17));
    EXPECT_EQ(score(probe, rejects, false, 0x17), std::optional<bool>{true});
    ScriptedPublisher wrong_code(cache_setup(64), reaction(0x10));
    EXPECT_EQ(score(probe, wrong_code, false, 0x17), std::optional<bool>{false});
    ScriptedPublisher unmapped(cache_setup(64), reaction(0x17));
    EXPECT_EQ(score(probe, unmapped), std::nullopt);
    EXPECT_EQ(token_of(rejects.sent(5)->bytes).alias_type, d18::TokenAliasType::Delete);
    EXPECT_EQ(token_of(rejects.sent(9)->bytes).alias_type, d18::TokenAliasType::UseAlias);
    ScriptedPublisher still_resolves(cache_setup(64), [](ScriptedPublisher& peer) {
        token_reaction(peer, {ok(), ok(), ok()}, {true, true, true});
    });
    EXPECT_EQ(score(probe, still_resolves, false, 0x17), std::optional<bool>{false});
}

TEST(Draft18GapASetup, AliasRegisteredByRejectedRequestStillResolves) {
    const auto probe = profile("D18-10-2-2-MUST-009");
    ScriptedPublisher resolves(cache_setup(64), [](ScriptedPublisher& peer) {
        token_reaction(peer, {error(0x10), error(0x10)}, {true, true});
    });
    EXPECT_EQ(score(probe, resolves), std::optional<bool>{true});
    // The probe targets a track the publisher cannot have, never the fixture.
    const auto target = token_of(resolves.sent(1)->bytes);
    EXPECT_EQ(target.alias_type, d18::TokenAliasType::Register);
    ScriptedPublisher forgotten(cache_setup(64), [](ScriptedPublisher& peer) {
        token_reaction(peer, {error(0x10), error(0x17)}, {true, true});
    });
    EXPECT_EQ(score(probe, forgotten, false, 0x17), std::optional<bool>{false});
    ScriptedPublisher not_rejected(cache_setup(64), [](ScriptedPublisher& peer) {
        token_reaction(peer, {ok(), ok()}, {true, true});
    });
    EXPECT_EQ(score(probe, not_rejected), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
