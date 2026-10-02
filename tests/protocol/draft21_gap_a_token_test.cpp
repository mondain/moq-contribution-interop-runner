// Authorization Token Alias probes (Section 8.9, 9.1.4) for completeness-gap
// slice A. Each case builds the transcript an actual publisher would produce.
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/scenarios/draft21_gap_a_token.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <optional>
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

std::vector<Draft21TokenProbe> storage_;

const Draft21TokenProbe& probe(const char* scenario) {
    storage_ = draft21_gap_a_token_probes(std::chrono::milliseconds(1000), {b({'n'})}, b({'t'}));
    const auto found = std::find_if(storage_.begin(), storage_.end(),
                                    [&](const auto& candidate) { return candidate.definition.id == scenario; });
    EXPECT_NE(found, storage_.end()) << scenario;
    return *found;
}

// MAX_AUTH_TOKEN_CACHE_SIZE (option 4) of 100 bytes: room for a small token.
Bytes peer_setup_with_cache() { return b({0xaf, 0, 0, 2, 4, 100}); }
Bytes peer_setup_default_cache() { return b({0xaf, 0, 0, 0}); }

RawProbeTranscript begin(const Draft21TokenProbe& p, Bytes peer_setup = peer_setup_with_cache()) {
    RawProbeTranscript t;
    t.scenario_id = p.definition.id;
    t.setup = {{RawProbeChannel::NewUni, p.definition.setup_bytes, false}, 3,
               p.definition.setup_bytes.size(), false, 1};
    t.events = {transport::ConnectionEstablishedEvent{{}, {}, {}, 1200},
                transport::StreamDataEvent{2, std::move(peer_setup), false}};
    t.transport_established = t.peer_setup_received = true;
    for (const auto& write : p.definition.writes) t.writes.push_back({write, {}, 0, false});
    return t;
}

transport::StreamId stream_of(std::size_t index) { return 1 + 4 * index; }

void send(RawProbeTranscript& t, std::size_t index) {
    auto& write = t.writes[index];
    write.stream_id = stream_of(index);
    write.accepted = write.write.bytes.size();
    write.fin_accepted = write.write.fin;
    write.delivery_event_count = t.events.size();
    t.delivery_event_count = t.events.size();
    t.stimulus_delivered = index + 1 == t.writes.size();
}

Bytes ok() { return b({7, 0, 1, 0}); }
Bytes error(unsigned code) { return b({5, 0, 3, code, 0, 0}); }

void respond(RawProbeTranscript& t, std::size_t index, Bytes response) {
    t.events.push_back(transport::StreamDataEvent{stream_of(index), std::move(response), true});
}

// Sends request `index` and delivers its response.
void exchange(RawProbeTranscript& t, std::size_t index, Bytes response) {
    send(t, index);
    respond(t, index, std::move(response));
}

void close_with(RawProbeTranscript& t, std::uint64_t code) {
    t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, code, {}});
}

std::optional<bool> verdict(const Draft21TokenProbe& p, RawProbeTranscript& t, bool complete = true) {
    t.complete = complete;
    t.timed_out = !complete;
    return evaluate_draft21_gap_a_token_probe(t, p);
}

TEST(Draft21GapAToken, SixProfilesWithCanonicalGatedRequests) {
    const auto probes = draft21_gap_a_token_probes(std::chrono::milliseconds(1000), {b({'n'})}, b({'t'}));
    ASSERT_EQ(probes.size(), 6u);
    const auto& p = probe("d21-token-register-alias-lifetime");
    ASSERT_EQ(p.definition.writes.size(), 4u);
    // TRACK_STATUS id 1 with AUTHORIZATION TOKEN REGISTER alias 1, type 0, value "k".
    EXPECT_EQ(p.definition.writes[0].bytes,
              b({0x0d, 0, 13, 1, 1, 1, 'n', 1, 't', 1, 3, 4, 1, 1, 0, 'k'}));
    // USE_VALUE (type 0, value "k") on id 5, USE_ALIAS control alias 2 on id 7.
    EXPECT_EQ(p.definition.writes[2].bytes, b({0x0d, 0, 12, 5, 1, 1, 'n', 1, 't', 1, 3, 3, 3, 0, 'k'}));
    EXPECT_EQ(p.definition.writes[3].bytes, b({0x0d, 0, 11, 7, 1, 1, 'n', 1, 't', 1, 3, 2, 2, 2}));
    EXPECT_FALSE(p.definition.writes[0].evidence_ready);
    for (std::size_t index = 1; index < p.definition.writes.size(); ++index)
        EXPECT_TRUE(p.definition.writes[index].evidence_ready) << index;
    for (const auto& write : p.definition.writes) EXPECT_TRUE(write.fin);
    // The failing registration names a track that does not exist.
    EXPECT_EQ(probe("d21-register-token-on-other-request-error").definition.writes[0].bytes,
              b({0x0d, 0, 14, 1, 1, 1, 'n', 2, 't', 0xff, 1, 3, 4, 1, 1, 0, 'k'}));
}

TEST(Draft21GapAToken, SequencedRequestsWaitForTheirPredecessorsResponse) {
    const auto& p = probe("d21-token-register-alias-lifetime");
    auto t = begin(p);
    send(t, 0);
    const auto& gate = p.definition.writes[1].evidence_ready;
    ASSERT_TRUE(gate);
    const auto input = [&] {
        return RawProbeGateInput{std::span<const RawProbeAcceptedWrite>(t.writes).first(1), t.events};
    };
    EXPECT_FALSE(gate(input()));
    // Half a response is not a response.
    t.events.push_back(transport::StreamDataEvent{stream_of(0), b({7, 0}), false});
    EXPECT_FALSE(gate(input()));
    t.events.push_back(transport::StreamDataEvent{stream_of(0), b({1, 0}), true});
    EXPECT_TRUE(gate(input()));
}

TEST(Draft21GapAToken, RegisteredAliasResolvesLikeItsTokenValue) {
    const auto& p = probe("d21-token-register-alias-lifetime");
    const auto run = [&](Bytes use_alias, Bytes use_value, Bytes control) {
        auto t = begin(p);
        exchange(t, 0, ok());
        exchange(t, 1, std::move(use_alias));
        exchange(t, 2, std::move(use_value));
        exchange(t, 3, std::move(control));
        return t;
    };
    auto pass = run(error(1), error(1), error(0x30));
    EXPECT_EQ(verdict(p, pass), std::optional<bool>{true});
    // The Alias is treated as unknown while its value is not.
    auto fail = run(error(0x30), error(1), error(0x30));
    EXPECT_EQ(verdict(p, fail), std::optional<bool>{false});
    // Nothing distinguishes the Alias from an unknown one.
    auto same = run(error(1), error(1), error(1));
    EXPECT_EQ(verdict(p, same), std::nullopt);
    // With a compatibility mapping the unknown code is exact.
    auto mapped = run(error(0x30), error(0x30), error(0x30));
    mapped.unknown_auth_token_alias_compatibility_code = 0x30;
    EXPECT_EQ(verdict(p, mapped), std::optional<bool>{false});
    // A missing control response leaves the comparison undecided.
    auto partial = begin(p);
    exchange(partial, 0, ok());
    exchange(partial, 1, error(1));
    exchange(partial, 2, error(1));
    send(partial, 3);
    EXPECT_EQ(verdict(p, partial), std::nullopt);
}

TEST(Draft21GapAToken, DeleteRetiresTheAliasAndAllowsRegistrationAgain) {
    const auto& p = probe("d21-token-delete-and-reuse");
    const auto run = [&](Bytes pre, Bytes control, Bytes post) {
        auto t = begin(p);
        exchange(t, 0, ok());
        exchange(t, 1, std::move(pre));
        exchange(t, 2, std::move(control));
        exchange(t, 3, ok());
        exchange(t, 4, std::move(post));
        exchange(t, 5, ok());
        return t;
    };
    auto pass = run(error(1), error(0x30), error(0x30));
    EXPECT_EQ(verdict(p, pass), std::optional<bool>{true});
    // The Alias still resolves after DELETE.
    auto still_registered = run(error(1), error(0x30), error(1));
    EXPECT_EQ(verdict(p, still_registered), std::optional<bool>{false});
    // Before deletion the Alias cannot be told from an unknown one.
    auto indistinct = run(error(0x30), error(0x30), error(0x30));
    EXPECT_EQ(verdict(p, indistinct), std::nullopt);
    // Registering the retired Alias again must not be a duplicate.
    auto duplicate = begin(p);
    exchange(duplicate, 0, ok());
    exchange(duplicate, 1, error(1));
    exchange(duplicate, 2, error(0x30));
    exchange(duplicate, 3, ok());
    exchange(duplicate, 4, error(0x30));
    send(duplicate, 5);
    close_with(duplicate, 0x14);
    EXPECT_EQ(verdict(p, duplicate), std::optional<bool>{false});
    // The last response is still outstanding.
    auto waiting = begin(p);
    exchange(waiting, 0, ok());
    exchange(waiting, 1, error(1));
    exchange(waiting, 2, error(0x30));
    exchange(waiting, 3, ok());
    exchange(waiting, 4, error(0x30));
    send(waiting, 5);
    EXPECT_EQ(verdict(p, waiting), std::nullopt);
}

TEST(Draft21GapAToken, DeletedAliasRejectionNeedsTheCompatibilityMapping) {
    const auto& p = probe("d21-request-deleted-token-alias");
    const auto run = [&](Bytes response, std::optional<std::uint64_t> mapping) {
        auto t = begin(p);
        exchange(t, 0, ok());
        exchange(t, 1, ok());
        exchange(t, 2, std::move(response));
        t.unknown_auth_token_alias_compatibility_code = mapping;
        return t;
    };
    auto unmapped = run(error(0x17), std::nullopt);
    EXPECT_EQ(verdict(p, unmapped), std::nullopt);
    auto pass = run(error(0x17), 0x17);
    EXPECT_EQ(verdict(p, pass), std::optional<bool>{true});
    auto other_code = run(error(1), 0x17);
    EXPECT_EQ(verdict(p, other_code), std::optional<bool>{false});
    auto accepted = run(ok(), 0x17);
    EXPECT_EQ(verdict(p, accepted), std::optional<bool>{false});
}

TEST(Draft21GapAToken, RegistrationSurvivesAFailedMessage) {
    for (const char* scenario : {"d21-register-token-on-unauthorized-request",
                                 "d21-register-token-on-other-request-error"}) {
        SCOPED_TRACE(scenario);
        const auto& p = probe(scenario);
        const bool unauthorized = std::string(scenario).find("unauthorized") != std::string::npos;
        const auto first_error = unauthorized ? error(1) : error(0x10);
        const auto run = [&](Bytes registration, Bytes use_alias, Bytes control) {
            auto t = begin(p);
            exchange(t, 0, std::move(registration));
            exchange(t, 1, std::move(use_alias));
            exchange(t, 2, std::move(control));
            return t;
        };
        auto pass = run(first_error, error(1), error(0x30));
        EXPECT_EQ(verdict(p, pass), std::optional<bool>{true});
        auto same = run(first_error, error(0x30), error(0x30));
        EXPECT_EQ(verdict(p, same), std::nullopt);
        auto mapped = run(first_error, error(0x30), error(0x30));
        mapped.unknown_auth_token_alias_compatibility_code = 0x30;
        EXPECT_EQ(verdict(p, mapped), std::optional<bool>{false});
        // The registration request must really fail.
        auto accepted = run(ok(), error(1), error(0x30));
        EXPECT_EQ(verdict(p, accepted), std::nullopt);
        // Closing the session with UNKNOWN_AUTH_TOKEN_ALIAS means the Alias was not kept.
        auto closed = begin(p);
        exchange(closed, 0, first_error);
        send(closed, 1);
        close_with(closed, 0x17);
        EXPECT_EQ(verdict(p, closed, false), std::optional<bool>{false});
    }
    // UNAUTHORIZED is what the first scenario promises; another error is not.
    const auto& unauthorized = probe("d21-register-token-on-unauthorized-request");
    auto other = begin(unauthorized);
    exchange(other, 0, error(0x10));
    exchange(other, 1, error(1));
    exchange(other, 2, error(0x30));
    EXPECT_EQ(verdict(unauthorized, other), std::nullopt);
}

TEST(Draft21GapAToken, OversizedSetupRegistrationIsTreatedAsUseValue) {
    const auto& p = probe("d21-setup-register-use-value-fallback");
    // SETUP carries one AUTHORIZATION TOKEN option: REGISTER alias 1, type 0, 8192 bytes.
    EXPECT_GT(p.definition.setup_bytes.size(), 8192u);
    const auto run = [&](Bytes first, Bytes second) {
        auto t = begin(p, peer_setup_default_cache());
        exchange(t, 0, std::move(first));
        exchange(t, 1, std::move(second));
        return t;
    };
    auto pass = run(error(0x30), error(0x30));
    EXPECT_EQ(verdict(p, pass), std::optional<bool>{true});
    // The Alias was registered despite exceeding the cache.
    auto registered = run(error(1), error(0x30));
    EXPECT_EQ(verdict(p, registered), std::optional<bool>{false});
    // AUTH_TOKEN_CACHE_OVERFLOW is exactly what the draft forbids; the peer may
    // close before any request is accepted.
    auto overflow = begin(p, peer_setup_default_cache());
    close_with(overflow, 0x13);
    EXPECT_EQ(verdict(p, overflow, false), std::optional<bool>{false});
    auto unrelated_close = begin(p, peer_setup_default_cache());
    close_with(unrelated_close, 0x3);
    EXPECT_EQ(verdict(p, unrelated_close, false), std::nullopt);
    // A publisher whose cache can hold the token never exercised the fallback.
    auto roomy = begin(p, b({0xaf, 0, 0, 3, 4, 0xc0, 0}));
    roomy.events[1] = transport::StreamDataEvent{2, b({0xaf, 0, 0, 3, 4, 0xa7, 0x10}), false};
    exchange(roomy, 0, error(0x30));
    exchange(roomy, 1, error(0x30));
    EXPECT_EQ(verdict(p, roomy), std::nullopt);
}

TEST(Draft21GapAToken, ContextsNeedAPublisherThatAdvertisesTokenCacheRoom) {
    const auto& p = probe("d21-token-register-alias-lifetime");
    // Default cache size 0 prohibits Aliases: the stimulus is never valid.
    auto t = begin(p, peer_setup_default_cache());
    exchange(t, 0, ok());
    exchange(t, 1, error(1));
    exchange(t, 2, error(1));
    exchange(t, 3, error(0x30));
    EXPECT_EQ(verdict(p, t), std::nullopt);
}

TEST(Draft21GapAToken, AlteredStimulusIsNeverEvaluated) {
    const auto& p = probe("d21-token-register-alias-lifetime");
    auto t = begin(p);
    exchange(t, 0, ok());
    exchange(t, 1, error(1));
    exchange(t, 2, error(1));
    exchange(t, 3, error(0x30));
    t.writes[1].write.bytes.back() = std::byte{9};
    EXPECT_EQ(verdict(p, t), std::nullopt);
    auto wrong_scenario = begin(p);
    wrong_scenario.scenario_id = "d21-token-delete-and-reuse";
    EXPECT_EQ(verdict(p, wrong_scenario), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios

namespace moq::interop::requirements {
namespace {

TEST(Draft21GapATokenRows, CatalogRowsScoreFromTheirTranscripts) {
    const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
    const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, root / "requirements/draft21.json");
    const auto state = [&](const std::vector<scenarios::RawProbeTranscript>& transcripts, std::string_view id) {
        for (const auto& outcome : evaluate_draft21_raw_probes(catalog, transcripts))
            if (outcome.requirement_id == id) return outcome.state;
        ADD_FAILURE() << id;
        return OutcomeState::NotRun;
    };
    const auto& associates = scenarios::probe("d21-token-register-alias-lifetime");
    auto t = scenarios::begin(associates);
    scenarios::exchange(t, 0, scenarios::ok());
    scenarios::exchange(t, 1, scenarios::error(1));
    scenarios::exchange(t, 2, scenarios::error(1));
    scenarios::exchange(t, 3, scenarios::error(0x30));
    t.complete = true;
    EXPECT_EQ(state({t}, "D21-8-9-MUST-265"), OutcomeState::Pass);
    EXPECT_EQ(state({t}, "D21-8-9-MUST-264"), OutcomeState::NotRun);
    // A row that needs two contexts stays incomplete with one.
    const auto& unauthorized = scenarios::probe("d21-register-token-on-unauthorized-request");
    auto first = scenarios::begin(unauthorized);
    scenarios::exchange(first, 0, scenarios::error(1));
    scenarios::exchange(first, 1, scenarios::error(1));
    scenarios::exchange(first, 2, scenarios::error(0x30));
    first.complete = true;
    EXPECT_EQ(state({first}, "D21-8-9-MUST-271"), OutcomeState::NotRun);
    const auto& other = scenarios::probe("d21-register-token-on-other-request-error");
    auto second = scenarios::begin(other);
    scenarios::exchange(second, 0, scenarios::error(0x10));
    scenarios::exchange(second, 1, scenarios::error(1));
    scenarios::exchange(second, 2, scenarios::error(0x30));
    second.complete = true;
    EXPECT_EQ(state({first, second}, "D21-8-9-MUST-271"), OutcomeState::Pass);
    second.writes[2].write.bytes.clear();
    EXPECT_EQ(state({first, second}, "D21-8-9-MUST-271"), OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop::requirements
