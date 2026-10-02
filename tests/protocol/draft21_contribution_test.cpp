#include "../support/contribution_transcript.h"

#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/wire/draft21/setup.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <set>
#include <tuple>

namespace moq::interop::scenarios {
namespace {
using test::Bytes;
using test::cbytes;
using test::cconcat;
using test::cframe;
using test::cvi;
using test::ContributionRun;
using test::find_probe;
using test::request_error;
using test::request_ok;
using test::subscribe_ok;

const std::vector<Draft21ContributionProbe>& probes() {
    static const auto value = draft21_contribution_probes();
    return value;
}

// Peer SETUP carrying one numeric option (types 4, 6 and 8 are even).
Bytes peer_setup(unsigned type, unsigned value) { return cbytes({0xaf, 0, 0, 2, type, value}); }

std::optional<bool> evaluate(const std::string& scenario, const ContributionRun& run,
                             const std::string& requirement = {}) {
    const auto& probe = find_probe(probes(), scenario, requirement);
    ContributionRun copy = run;
    return evaluate_draft21_contribution_probe(copy.finish(), probe);
}

TEST(Contribution, RejectsInvalidConfiguration) {
    EXPECT_THROW(draft21_contribution_probes(std::chrono::milliseconds{0}), std::invalid_argument);
    EXPECT_THROW(draft21_contribution_probes(std::chrono::milliseconds{1000}, {cbytes({'.'})}),
                 std::invalid_argument);
    EXPECT_NO_THROW(draft21_contribution_probes(std::chrono::milliseconds{1000},
                                                {cbytes({'n'}), cbytes({'m'})}, cbytes({'t'})));
}

TEST(Contribution, RegistryListsExactlyTheProfileScenarios) {
    std::set<std::string> from_profiles;
    for (const auto& probe : probes()) from_profiles.insert(probe.definition.id);
    std::set<std::string> from_registry;
    for (const auto scenario : app::kDraft21ContributionScenarios)
        EXPECT_TRUE(from_registry.insert(std::string(scenario)).second) << scenario;
    EXPECT_EQ(from_profiles, from_registry);
    for (const auto& scenario : from_registry) {
        EXPECT_TRUE(app::executable_scenario(21, scenario)) << scenario;
        EXPECT_TRUE(app::raw_probe_scenario(21, scenario)) << scenario;
        EXPECT_TRUE(app::scenario_requires_track(21, scenario)) << scenario;
        EXPECT_FALSE(app::executable_scenario(18, scenario)) << scenario;
    }
}

TEST(Contribution, EveryScenarioUsesCanonicalSetupAndIsUniquePerRow) {
    std::set<std::tuple<std::string, std::string, std::string>> seen;
    for (const auto& probe : probes()) {
        EXPECT_EQ(probe.draft, 21u);
        EXPECT_FALSE(probe.definition.id.empty());
        EXPECT_TRUE(static_cast<bool>(probe.definition.response_ready));
        EXPECT_TRUE(static_cast<bool>(probe.definition.peer_setup_ready));
        EXPECT_TRUE(seen.insert({probe.requirement_id, probe.definition.id, probe.evaluator_id}).second);
    }
}

// ---- Section 9.1.4: oversized SETUP registration ---------------------------
TEST(Contribution, OversizedRegistrationUsesAnnouncedLimitOrZeroDefault) {
    const auto& announced = find_probe(probes(), "d21-setup-register-exceeds-token-cache");
    const auto& defaulted = find_probe(probes(), "d21-setup-register-default-zero-cache");
    wire::Cursor cursor(announced.definition.setup_bytes);
    const auto decoded = wire::draft21::decode_setup(cursor);
    const auto* setup = std::get_if<wire::draft21::SetupMessage>(&decoded);
    ASSERT_NE(setup, nullptr);
    ASSERT_EQ(setup->options.size(), 1u);
    EXPECT_EQ(setup->options[0].type, 3u);
    EXPECT_GT(std::get<Bytes>(setup->options[0].value).size(), 40000u);

    EXPECT_FALSE(announced.definition.peer_setup_ready(cbytes({0xaf, 0, 0, 0})));
    EXPECT_FALSE(announced.definition.peer_setup_ready(peer_setup(4, 0)));
    EXPECT_TRUE(announced.definition.peer_setup_ready(peer_setup(4, 32)));
    EXPECT_FALSE(announced.definition.peer_setup_ready(cbytes({0xaf, 0, 0, 3, 4, 0xc0, 0xff})));
    EXPECT_TRUE(defaulted.definition.peer_setup_ready(cbytes({0xaf, 0, 0, 0})));
    EXPECT_TRUE(defaulted.definition.peer_setup_ready(peer_setup(4, 0)));
    EXPECT_FALSE(defaulted.definition.peer_setup_ready(peer_setup(4, 32)));
    // Partial SETUP bytes never satisfy the gate.
    EXPECT_FALSE(announced.definition.peer_setup_ready(cbytes({0xaf, 0})));
}

TEST(Contribution, OversizedRegistrationOnlyFailsOnCacheOverflowClose) {
    for (const auto& [scenario, setup] :
         std::vector<std::pair<std::string, Bytes>>{{"d21-setup-register-exceeds-token-cache", peer_setup(4, 32)},
                                                    {"d21-setup-register-default-zero-cache", cbytes({0xaf, 0, 0, 0})}}) {
        const auto& probe = find_probe(probes(), scenario);
        ContributionRun pending(probe, setup);
        pending.deliver(0);
        EXPECT_FALSE(probe.definition.response_ready(pending.partial())) << scenario;

        ContributionRun answered = pending;
        answered.reply(answered.stream_of(0), subscribe_ok());
        EXPECT_TRUE(probe.definition.response_ready(answered.partial())) << scenario;
        EXPECT_EQ(evaluate(scenario, answered), true) << scenario;

        ContributionRun rejected = pending;
        rejected.reply(rejected.stream_of(0), request_error(1));
        EXPECT_EQ(evaluate(scenario, rejected), true) << scenario;

        ContributionRun overflow = pending;
        overflow.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x13, {}});
        EXPECT_EQ(evaluate(scenario, overflow), false) << scenario;

        // Another code, or a transport close, is not this error.
        ContributionRun other = pending;
        other.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x2, {}});
        EXPECT_EQ(evaluate(scenario, other), std::nullopt) << scenario;
        ContributionRun transport_close = pending;
        transport_close.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Transport, 0x13, {}});
        EXPECT_EQ(evaluate(scenario, transport_close), std::nullopt) << scenario;

        // Wrong peer SETUP evidence, even with a response, does not prove the stimulus.
        ContributionRun mismatched(probe, peer_setup(4, 0x3f));
        mismatched.deliver(0);
        mismatched.reply(mismatched.stream_of(0), subscribe_ok());
        EXPECT_EQ(evaluate(scenario, mismatched), scenario.find("exceeds") != std::string::npos
                                                      ? std::optional<bool>{true} : std::nullopt) << scenario;
    }
}

// ---- Section 13: GREASE ----------------------------------------------------
TEST(Contribution, GreaseSetupOptionsAreOddEvenAndRepeated) {
    const auto& probe = find_probe(probes(), "d21-grease-setup-options", "D21-13-MUST-595");
    wire::Cursor cursor(probe.definition.setup_bytes);
    const auto decoded = wire::draft21::decode_setup(cursor);
    const auto* setup = std::get_if<wire::draft21::SetupMessage>(&decoded);
    ASSERT_NE(setup, nullptr);
    ASSERT_EQ(setup->options.size(), 4u);
    EXPECT_EQ(setup->options[0].type, 7u);
    EXPECT_EQ(setup->options[1].type, 0x9du);
    EXPECT_EQ(setup->options[2].type, 0x9du);
    EXPECT_EQ(setup->options[3].type, 0x11cu);
    EXPECT_TRUE(std::holds_alternative<std::uint64_t>(setup->options[3].value));
    EXPECT_EQ((0x11cu - 0x9du) % 0x7fu, 0u);
}

TEST(Contribution, GreaseSetupOptionsFeedBothSetupRows) {
    for (const char* row : {"D21-13-MUST-595", "D21-16-4-MUST-629"}) {
        const auto& probe = find_probe(probes(), "d21-grease-setup-options", row);
        EXPECT_EQ(probe.evaluator_id, "d21-grease-setup-options-ignored");
        ContributionRun run(probe);
        run.deliver(0);
        EXPECT_FALSE(probe.definition.response_ready(run.partial()));
        run.reply(run.stream_of(0), subscribe_ok());
        EXPECT_TRUE(probe.definition.response_ready(run.partial()));
        EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), true) << row;
    }
    // A close without the exchange is not scored against the publisher.
    const auto& probe = find_probe(probes(), "d21-grease-setup-options", "D21-13-MUST-595");
    ContributionRun closed(probe);
    closed.deliver(0);
    closed.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
    EXPECT_EQ(evaluate_draft21_contribution_probe(closed.finish(), probe), std::nullopt);
}

Bytes peer_publish() {
    // PUBLISH: request 0, one namespace field "n", track "t", alias 5, no parameters.
    return cframe(0x1d, cbytes({0, 1, 1, 'n', 1, 't', 5, 0}));
}

TEST(Contribution, UnknownRequestErrorCodeRejectsPublishThenUsesFreshRequest) {
    const auto& probe = find_probe(probes(), "d21-grease-request-error");
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    EXPECT_EQ(probe.definition.writes[0].channel, RawProbeChannel::PeerBidi);
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({5, 0, 4, 0x80, 0x9d, 0, 0}));
    EXPECT_TRUE(probe.definition.writes[0].fin);
    EXPECT_FALSE(probe.definition.peer_request_ready(cbytes({0x1d, 0, 8})));
    EXPECT_TRUE(probe.definition.peer_request_ready(peer_publish()));

    ContributionRun run(probe);
    run.reply(0, peer_publish());
    run.deliver(0);
    run.deliver(1);
    EXPECT_FALSE(probe.definition.response_ready(run.partial()));
    run.reply(run.stream_of(1), request_error(0x10));
    EXPECT_TRUE(probe.definition.response_ready(run.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), true);

    ContributionRun closed(probe);
    closed.reply(0, peer_publish());
    closed.deliver(0);
    closed.deliver(1);
    closed.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
    EXPECT_EQ(evaluate_draft21_contribution_probe(closed.finish(), probe), std::nullopt);
}

// ---- Sections 9.5 and 9.5.1: REQUEST_UPDATE responses ------------------------
ContributionRun established_updates(const std::string& scenario, const Bytes& setup = cbytes({0xaf, 0, 0, 0})) {
    const auto& probe = find_probe(probes(), scenario);
    ContributionRun run(probe, setup);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok());
    run.deliver(1);
    run.deliver(2);
    return run;
}

TEST(Contribution, SingleUpdateNeedsExactlyOneResponseBeforeTheFenceCompletes) {
    const std::string scenario = "d21-single-request-update-response";
    const auto& probe = find_probe(probes(), scenario);
    ASSERT_EQ(probe.definition.writes.size(), 3u);
    EXPECT_EQ(probe.definition.writes[1].bytes, cbytes({2, 0, 4, 3, 1, 0x20, 100}));
    EXPECT_EQ(probe.definition.writes[2].bytes.at(3), std::byte{5});
    EXPECT_TRUE(probe.definition.writes[1].reuse_write_stream.has_value());
    EXPECT_TRUE(static_cast<bool>(probe.definition.writes[1].peer_response_ready));
    EXPECT_TRUE(probe.definition.writes[2].fin);

    auto run = established_updates(scenario);
    EXPECT_FALSE(probe.definition.response_ready(run.partial()));
    run.reply(run.stream_of(0), request_ok());
    EXPECT_FALSE(probe.definition.response_ready(run.partial()));  // fence not yet answered
    run.reply(run.stream_of(2), request_ok());
    EXPECT_TRUE(probe.definition.response_ready(run.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), true);

    // A second response to the one update is a duplicate regardless of the fence.
    auto duplicate = established_updates(scenario);
    duplicate.reply(duplicate.stream_of(0), cconcat({request_ok(), request_error(0x10)}));
    EXPECT_TRUE(probe.definition.response_ready(duplicate.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(duplicate.finish(), probe), false);

    // A refused update is still exactly one response, but not a successful one.
    auto refused = established_updates(scenario);
    refused.reply(refused.stream_of(0), request_error(0x10));
    refused.reply(refused.stream_of(2), request_ok());
    EXPECT_EQ(evaluate_draft21_contribution_probe(refused.finish(), probe), std::nullopt);
}

TEST(Contribution, CoalescedSuccessfulUpdatesRequireAnOkEach) {
    const std::string scenario = "d21-coalesced-successful-update-responses";
    const auto& exclusive = find_probe(probes(), scenario, "D21-9-5-MUST-345");
    const auto& each = find_probe(probes(), scenario, "D21-9-5-1-MUST-352");
    // Three pipelined updates need an unlimited or large-enough stream credit.
    EXPECT_TRUE(exclusive.definition.peer_setup_ready(cbytes({0xaf, 0, 0, 0})));
    EXPECT_TRUE(exclusive.definition.peer_setup_ready(peer_setup(8, 0)));
    EXPECT_TRUE(exclusive.definition.peer_setup_ready(peer_setup(8, 3)));
    EXPECT_FALSE(exclusive.definition.peer_setup_ready(peer_setup(8, 2)));
    // Request IDs 3, 5 and 7 carry distinct priorities in one write.
    EXPECT_EQ(exclusive.definition.writes[1].bytes,
              cconcat({cbytes({2, 0, 4, 3, 1, 0x20, 100}), cbytes({2, 0, 4, 5, 1, 0x20, 101}),
                       cbytes({2, 0, 4, 7, 1, 0x20, 102})}));
    EXPECT_EQ(exclusive.definition.writes[2].bytes.at(3), std::byte{9});

    for (const auto* row : {&exclusive, &each}) {
        auto run = established_updates(scenario);
        run.reply(run.stream_of(0), cconcat({request_ok(), request_ok(), request_ok()}));
        EXPECT_FALSE(row->definition.response_ready(run.partial()));
        run.reply(run.stream_of(2), request_ok());
        EXPECT_TRUE(row->definition.response_ready(run.partial()));
        EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), *row), true);

        auto extra = established_updates(scenario);
        extra.reply(extra.stream_of(0), cconcat({request_ok(), request_ok(), request_ok(), request_ok()}));
        EXPECT_EQ(evaluate_draft21_contribution_probe(extra.finish(), *row), false);

        // A collapsed acknowledgement is unproven: the fence can overtake the data.
        auto collapsed = established_updates(scenario);
        collapsed.reply(collapsed.stream_of(0), request_ok());
        collapsed.reply(collapsed.stream_of(2), request_ok());
        EXPECT_FALSE(row->definition.response_ready(collapsed.partial()));

        auto mixed = established_updates(scenario);
        mixed.reply(mixed.stream_of(0), cconcat({request_ok(), request_error(0x3), request_ok()}));
        mixed.reply(mixed.stream_of(2), request_ok());
        EXPECT_EQ(evaluate_draft21_contribution_probe(mixed.finish(), *row), std::nullopt);
    }
}

TEST(Contribution, CoalescedFailedUpdatesMayShareOneError) {
    const std::string scenario = "d21-coalesced-failed-update-response";
    const auto& probe = find_probe(probes(), scenario);
    // USE_ALIAS for an alias this fresh sender never registered.
    EXPECT_EQ(probe.definition.writes[1].bytes.size(), 3 * cbytes({2, 0, 6, 3, 1, 3, 2, 2, 0}).size());
    EXPECT_EQ(std::vector<std::byte>(probe.definition.writes[1].bytes.begin(),
                                     probe.definition.writes[1].bytes.begin() + 9),
              cbytes({2, 0, 6, 3, 1, 3, 2, 2, 0}));

    for (const std::size_t errors : {1u, 2u, 3u}) {
        auto run = established_updates(scenario);
        Bytes responses;
        for (std::size_t index = 0; index < errors; ++index)
            responses = cconcat({responses, request_error(0x17)});
        run.reply(run.stream_of(0), responses);
        EXPECT_FALSE(probe.definition.response_ready(run.partial()));
        run.reply(run.stream_of(2), request_error(0x17));
        EXPECT_TRUE(probe.definition.response_ready(run.partial()));
        EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), true) << errors;
    }
    auto excess = established_updates(scenario);
    excess.reply(excess.stream_of(0), cconcat({request_error(0x17), request_error(0x17),
                                               request_error(0x17), request_error(0x17)}));
    EXPECT_EQ(evaluate_draft21_contribution_probe(excess.finish(), probe), false);

    // A failed update acknowledged as successful is not conclusive here.
    auto acknowledged = established_updates(scenario);
    acknowledged.reply(acknowledged.stream_of(0), request_ok());
    acknowledged.reply(acknowledged.stream_of(2), request_ok());
    EXPECT_EQ(evaluate_draft21_contribution_probe(acknowledged.finish(), probe), std::nullopt);

    // Responses and PUBLISH_DONE for the terminated subscription are not duplicates.
    auto done = established_updates(scenario);
    done.reply(done.stream_of(0), cconcat({request_error(0x17), cbytes({0xb, 0, 3, 8, 0, 0})}));
    done.reply(done.stream_of(2), request_ok());
    EXPECT_EQ(evaluate_draft21_contribution_probe(done.finish(), probe), true);

    auto session_error = established_updates(scenario);
    session_error.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x17, {}});
    EXPECT_EQ(evaluate_draft21_contribution_probe(session_error.finish(), probe), std::nullopt);
}

// ---- Section 9.1.7: outstanding update limit ---------------------------------
TEST(Contribution, UpdateOverrunSendsOneMoreThanAnnouncedLimit) {
    const std::string scenario = "d21-request-update-overrun";
    const auto& probe = find_probe(probes(), scenario);
    EXPECT_FALSE(probe.definition.peer_setup_ready(cbytes({0xaf, 0, 0, 0})));
    EXPECT_FALSE(probe.definition.peer_setup_ready(peer_setup(8, 0)));
    EXPECT_TRUE(probe.definition.peer_setup_ready(peer_setup(8, 1)));
    EXPECT_FALSE(probe.definition.peer_setup_ready(cbytes({0xaf, 0, 0, 3, 8, 0x80, 65})));

    const std::vector<transport::TransportEvent> events{transport::StreamDataEvent{2, peer_setup(8, 2), false}};
    const auto burst = probe.definition.writes[1].prepare_bytes({{}, events});
    ASSERT_TRUE(burst.has_value());
    EXPECT_EQ(*burst, cconcat({cbytes({2, 0, 4, 3, 1, 0x20, 100}), cbytes({2, 0, 4, 5, 1, 0x20, 101}),
                               cbytes({2, 0, 4, 7, 1, 0x20, 102})}));
    EXPECT_FALSE(probe.definition.writes[1].prepare_bytes({{}, {}}).has_value());

    ContributionRun run(probe, peer_setup(8, 2));
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok());
    run.deliver(1);
    EXPECT_FALSE(probe.definition.response_ready(run.partial()));
    auto closed = run;
    closed.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x1b, {}});
    EXPECT_TRUE(probe.definition.response_ready(closed.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(closed.finish(), probe), true);

    // An immediate responder never observes the overrun: not a failure, not a pass.
    auto immediate = run;
    immediate.reply(immediate.stream_of(0), cconcat({request_ok(), request_ok(), request_ok()}));
    EXPECT_TRUE(probe.definition.response_ready(immediate.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(immediate.finish(), probe), std::nullopt);

    auto other = run;
    other.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
    EXPECT_EQ(evaluate_draft21_contribution_probe(other.finish(), probe), std::nullopt);
}

TEST(Contribution, UpdateLimitIsPerRequestStream) {
    const std::string scenario = "d21-request-update-independent-streams";
    const auto& probe = find_probe(probes(), scenario);
    ASSERT_EQ(probe.definition.writes.size(), 4u);
    EXPECT_FALSE(probe.definition.peer_setup_ready(cbytes({0xaf, 0, 0, 0})));
    EXPECT_TRUE(probe.definition.peer_setup_ready(peer_setup(8, 2)));
    EXPECT_FALSE(probe.definition.peer_setup_ready(peer_setup(8, 17)));

    ContributionRun run(probe, peer_setup(8, 2));
    run.deliver(0);
    run.deliver(1);
    run.reply(run.stream_of(0), subscribe_ok());
    run.reply(run.stream_of(1), subscribe_ok(1));
    run.deliver(2);
    run.deliver(3);
    const auto& first = run.snapshot().writes[2].write.bytes;
    const auto& second = run.snapshot().writes[3].write.bytes;
    EXPECT_EQ(first, cconcat({cbytes({2, 0, 4, 5, 1, 0x20, 100}), cbytes({2, 0, 4, 7, 1, 0x20, 101})}));
    EXPECT_EQ(second, cconcat({cbytes({2, 0, 4, 9, 1, 0x20, 100}), cbytes({2, 0, 4, 11, 1, 0x20, 101})}));
    EXPECT_NE(run.stream_of(2), run.stream_of(3));

    auto partial = run;
    partial.reply(partial.stream_of(0), cconcat({request_ok(), request_ok()}));
    EXPECT_FALSE(probe.definition.response_ready(partial.partial()));
    auto answered = run;
    answered.reply(answered.stream_of(0), cconcat({request_ok(), request_ok()}));
    answered.reply(answered.stream_of(1), cconcat({request_ok(), request_ok()}));
    EXPECT_TRUE(probe.definition.response_ready(answered.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(answered.finish(), probe), true);

    // Closing is wrong even for an immediate responder: each stream stayed in budget.
    auto overflow = run;
    overflow.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x1b, {}});
    EXPECT_EQ(evaluate_draft21_contribution_probe(overflow.finish(), probe), false);
}

TEST(Contribution, UnlimitedUpdatesRequireAnOmittedOrZeroLimit) {
    const std::string scenario = "d21-request-update-unlimited";
    const auto& probe = find_probe(probes(), scenario);
    EXPECT_TRUE(probe.definition.peer_setup_ready(cbytes({0xaf, 0, 0, 0})));
    EXPECT_TRUE(probe.definition.peer_setup_ready(peer_setup(8, 0)));
    EXPECT_FALSE(probe.definition.peer_setup_ready(peer_setup(8, 1)));

    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok());
    run.deliver(1);
    Bytes responses;
    for (int index = 0; index < 8; ++index) responses = cconcat({responses, request_ok()});
    auto answered = run;
    answered.reply(answered.stream_of(0), responses);
    EXPECT_TRUE(probe.definition.response_ready(answered.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(answered.finish(), probe), true);

    auto closed = run;
    closed.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x1b, {}});
    EXPECT_EQ(evaluate_draft21_contribution_probe(closed.finish(), probe), false);
    EXPECT_FALSE(probe.definition.response_ready(run.partial()));
}

// ---- Sections 9.6/9.7: SUBSCRIBE_OK ------------------------------------------
TEST(Contribution, SuccessfulSubscribeMustUseSubscribeOk) {
    const std::string scenario = "d21-successful-subscribe-response";
    const auto& probe = find_probe(probes(), scenario);
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({3, 0, 5, 1, 0, 1, 'x', 0}));

    ContributionRun ok(probe);
    ok.deliver(0);
    EXPECT_FALSE(probe.definition.response_ready(ok.partial()));
    ok.reply(ok.stream_of(0), subscribe_ok(7));
    EXPECT_TRUE(probe.definition.response_ready(ok.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(ok.finish(), probe), true);

    ContributionRun generic(probe);
    generic.deliver(0);
    generic.reply(generic.stream_of(0), request_ok());
    EXPECT_EQ(evaluate_draft21_contribution_probe(generic.finish(), probe), false);

    ContributionRun rejected(probe);
    rejected.deliver(0);
    rejected.reply(rejected.stream_of(0), request_error(0x10), true);
    EXPECT_EQ(evaluate_draft21_contribution_probe(rejected.finish(), probe), std::nullopt);

    // SUBSCRIBE_OK with a parameter outside its message scope is malformed evidence.
    ContributionRun malformed(probe);
    malformed.deliver(0);
    malformed.reply(malformed.stream_of(0), cframe(4, cbytes({0, 1, 0x10, 1})));
    EXPECT_EQ(evaluate_draft21_contribution_probe(malformed.finish(), probe), std::nullopt);
}

// ---- Section 9.11: FETCH INVALID_RANGE ----------------------------------------
TEST(Contribution, FetchBeyondLargestObjectMustBeInvalidRange) {
    const std::string scenario = "d21-fetch-start-beyond-largest-object";
    const auto& probe = find_probe(probes(), scenario);
    const auto& bytes = probe.definition.writes[0].bytes;
    EXPECT_EQ(bytes[0], std::byte{0x16});
    EXPECT_TRUE(probe.definition.writes[0].fin);

    for (const auto& [reply, expected] : std::vector<std::pair<Bytes, std::optional<bool>>>{
             {request_error(0x11), true},
             {request_error(0x10), std::nullopt},  // DOES_NOT_EXIST: fixture is absent
             {request_error(0x36), std::nullopt},
             {cframe(0x18, cbytes({0, 0, 0, 0})), false}}) {
        ContributionRun run(probe);
        run.deliver(0);
        EXPECT_FALSE(probe.definition.response_ready(run.partial()));
        run.reply(run.stream_of(0), reply);
        EXPECT_TRUE(probe.definition.response_ready(run.partial()));
        EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), expected);
    }
}

TEST(Contribution, EmptyTrackFetchOnlyPassesOnInvalidRange) {
    const std::string scenario = "d21-fetch-track-with-no-published-objects";
    const auto& probe = find_probe(probes(), scenario);
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({0x16, 0, 5, 1, 0, 1, 'x', 0}));
    for (const auto& [reply, expected] : std::vector<std::pair<Bytes, std::optional<bool>>>{
             {request_error(0x11), true},
             {request_error(0x10), std::nullopt},
             // The fixture's emptiness is not observable, so FETCH_OK is not proof of failure.
             {cframe(0x18, cbytes({0, 0, 0, 0})), std::nullopt}}) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), reply);
        EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), expected);
    }
}

// ---- Section 9.15: NAMESPACE_DONE ordering -----------------------------------
TEST(Contribution, NamespaceDoneMustFollowItsNamespace) {
    const std::string scenario = "d21-namespace-discovery-withdrawal-order";
    const auto probes_for_namespace = draft21_contribution_probes(std::chrono::milliseconds{1000},
                                                                  {cbytes({'p'})}, cbytes({'t'}));
    const auto& probe = find_probe(probes_for_namespace, scenario);
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({0x50, 0, 5, 1, 1, 1, 'p', 0}));
    const auto namespace_frame = [](unsigned type, std::initializer_list<unsigned> suffix) {
        return cframe(type, cbytes(suffix));
    };
    const auto run_with = [&](const Bytes& stream) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), stream);
        return run;
    };
    auto good = run_with(cconcat({request_ok(), namespace_frame(8, {1, 1, 'a'}), namespace_frame(0xe, {1, 1, 'a'})}));
    EXPECT_TRUE(probe.definition.response_ready(good.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(good.finish(), probe), true);

    auto early = run_with(cconcat({request_ok(), namespace_frame(0xe, {1, 1, 'a'})}));
    EXPECT_TRUE(probe.definition.response_ready(early.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(early.finish(), probe), false);

    auto wrong_suffix = run_with(cconcat({request_ok(), namespace_frame(8, {1, 1, 'a'}), namespace_frame(0xe, {1, 1, 'b'})}));
    EXPECT_EQ(evaluate_draft21_contribution_probe(wrong_suffix.finish(), probe), false);

    auto repeated = run_with(cconcat({request_ok(), namespace_frame(8, {1, 1, 'a'}), namespace_frame(0xe, {1, 1, 'a'}),
                                      namespace_frame(0xe, {1, 1, 'a'})}));
    EXPECT_EQ(evaluate_draft21_contribution_probe(repeated.finish(), probe), false);

    auto reannounced = run_with(cconcat({request_ok(), namespace_frame(8, {1, 1, 'a'}), namespace_frame(0xe, {1, 1, 'a'}),
                                         namespace_frame(8, {1, 1, 'a'}), namespace_frame(0xe, {1, 1, 'a'})}));
    EXPECT_EQ(evaluate_draft21_contribution_probe(reannounced.finish(), probe), true);

    // Without a withdrawal the ordering rule is not exercised.
    auto only_announced = run_with(cconcat({request_ok(), namespace_frame(8, {1, 1, 'a'})}));
    EXPECT_FALSE(probe.definition.response_ready(only_announced.partial()));
    auto rejected = run_with(request_error(0x10));
    EXPECT_EQ(evaluate_draft21_contribution_probe(rejected.finish(), probe), std::nullopt);
}

// ---- Section 9.20: parameter serialization -----------------------------------
Bytes subscribe_ok_with(const Bytes& parameters) { return subscribe_ok(0, parameters); }

TEST(Contribution, ParameterOrderingDetectsOnlyDeltaViolations) {
    const std::string scenario = "d21-publisher-parameter-serialization";
    const auto& probe = find_probe(probes(), scenario);
    const auto run_with = [&](const Bytes& reply) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), reply);
        return run;
    };
    // EXPIRES(8)=500 then LARGEST_OBJECT(9)={3,4}: deltas 8 then 1.
    auto good = run_with(subscribe_ok_with(cbytes({2, 8, 0x81, 0xf4, 1, 3, 4})));
    EXPECT_TRUE(probe.definition.response_ready(good.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(good.finish(), probe), true);

    // One parameter cannot distinguish absolute from delta encoding.
    auto single = run_with(subscribe_ok_with(cbytes({1, 9, 3, 4})));
    EXPECT_EQ(evaluate_draft21_contribution_probe(single.finish(), probe), std::nullopt);

    // A wrapped subtraction produces a delta that overflows 2^64-1.
    auto overflow = run_with(cframe(4, cconcat({cbytes({0, 2, 9, 3, 4}), cvi(0xffffffffffffffffull), cbytes({1})})));
    EXPECT_EQ(evaluate_draft21_contribution_probe(overflow.finish(), probe), false);

    // Absolute types after the first parameter decode as an undefined type: unproven here.
    auto absolute = run_with(subscribe_ok_with(cbytes({2, 9, 3, 4, 8, 1})));
    EXPECT_EQ(evaluate_draft21_contribution_probe(absolute.finish(), probe), std::nullopt);

    auto rejected = run_with(request_error(0x10));
    EXPECT_EQ(evaluate_draft21_contribution_probe(rejected.finish(), probe), std::nullopt);
}

TEST(Contribution, PublisherPublishParametersAreChecked) {
    const std::string scenario = "d21-publisher-parameter-serialization";
    const auto& probe = find_probe(probes(), scenario);
    ContributionRun run(probe);
    // A peer PUBLISH carrying FORWARD(0x10)=1 and GROUP_ORDER(0x22)=1: deltas 0x10 then 0x12.
    run.reply(0, cframe(0x1d, cbytes({0, 1, 1, 'n', 1, 't', 5, 2, 0x10, 1, 0x12, 1})));
    run.deliver(0);
    run.reply(run.stream_of(0), request_error(0x10));
    EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), true);
}

TEST(Contribution, UndefinedParametersAreNotNegotiated) {
    const std::string scenario = "d21-publisher-parameter-negotiation";
    const auto& probe = find_probe(probes(), scenario);
    const auto evaluate_reply = [&](const Bytes& reply) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), reply);
        return evaluate_draft21_contribution_probe(run.finish(), probe);
    };
    EXPECT_EQ(evaluate_reply(subscribe_ok()), true);
    EXPECT_EQ(evaluate_reply(subscribe_ok_with(cbytes({1, 9, 3, 4}))), true);
    // 0x7e is not in the registry and no extension was offered in SETUP.
    EXPECT_EQ(evaluate_reply(subscribe_ok_with(cbytes({1, 0x7e, 0}))), false);
    // A known type with an impossible encoding is a framing problem, not this row.
    EXPECT_EQ(evaluate_reply(subscribe_ok_with(cbytes({1, 9, 3}))), std::nullopt);
    EXPECT_EQ(evaluate_reply(request_error(0x10)), std::nullopt);
}

TEST(Contribution, RepeatedParametersOnlyWhereDefinitionsAllowThem) {
    const std::string scenario = "d21-publisher-parameter-multiplicity";
    const auto& probe = find_probe(probes(), scenario);
    const auto evaluate_reply = [&](const Bytes& reply) {
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), reply);
        return evaluate_draft21_contribution_probe(run.finish(), probe);
    };
    EXPECT_EQ(evaluate_reply(subscribe_ok()), true);
    // LARGEST_OBJECT twice: delta 0 repeats a type that allows one instance.
    EXPECT_EQ(evaluate_reply(subscribe_ok_with(cbytes({2, 9, 3, 4, 0, 5, 6}))), false);
    // AUTHORIZATION_TOKEN and range filters allow multiple distinct instances.
    EXPECT_EQ(evaluate_reply(subscribe_ok_with(cbytes({2, 3, 2, 3, 0, 0, 1, 4}))), true);
    EXPECT_EQ(evaluate_reply(subscribe_ok_with(cbytes({2, 9, 3, 4, 0x7e, 0}))), std::nullopt);
}

// ---- Section 11.5: padding received by the publisher ---------------------------
TEST(Contribution, InboundPaddingStreamIsZeroFilledAndFollowedByARequest) {
    const std::string scenario = "d21-inbound-padding-stream";
    const auto& probe = find_probe(probes(), scenario);
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    const auto& padding = probe.definition.writes[0];
    EXPECT_EQ(padding.channel, RawProbeChannel::NewUni);
    EXPECT_TRUE(padding.fin);
    EXPECT_GT(padding.bytes.size(), 65536u);
    ASSERT_GT(padding.bytes.size(), 5u);
    // 0x132b3e28 uses a five-byte vi64; every following byte is zero.
    EXPECT_EQ(std::vector<std::byte>(padding.bytes.begin(), padding.bytes.begin() + 5),
              cbytes({0xf0, 0x13, 0x2b, 0x3e, 0x28}));
    EXPECT_TRUE(std::all_of(padding.bytes.begin() + 5, padding.bytes.end(),
                            [](std::byte value) { return value == std::byte{0}; }));

    ContributionRun run(probe);
    run.deliver(0);
    run.deliver(1);
    EXPECT_FALSE(probe.definition.response_ready(run.partial()));
    run.reply(run.stream_of(1), subscribe_ok());
    EXPECT_TRUE(probe.definition.response_ready(run.partial()));
    EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), true);

    ContributionRun closed(probe);
    closed.deliver(0);
    closed.deliver(1);
    closed.event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
    EXPECT_EQ(evaluate_draft21_contribution_probe(closed.finish(), probe), std::nullopt);
}

TEST(Contribution, InboundPaddingDatagramIsZeroFilledAndFollowedByARequest) {
    const std::string scenario = "d21-inbound-padding-datagram";
    const auto& probe = find_probe(probes(), scenario);
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    const auto& padding = probe.definition.writes[0];
    EXPECT_EQ(padding.channel, RawProbeChannel::Datagram);
    EXPECT_EQ(padding.bytes.size(), 64u);
    EXPECT_EQ(std::vector<std::byte>(padding.bytes.begin(), padding.bytes.begin() + 5),
              cbytes({0xf0, 0x13, 0x2b, 0x3e, 0x29}));
    EXPECT_TRUE(std::all_of(padding.bytes.begin() + 5, padding.bytes.end(),
                            [](std::byte value) { return value == std::byte{0}; }));
    ContributionRun run(probe);
    run.deliver(0);
    run.deliver(1);
    run.reply(run.stream_of(1), request_error(0x10));
    EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), true);
}

// ---- Evaluator integrity --------------------------------------------------
TEST(Contribution, StimulusMustMatchTheCanonicalProbe) {
    const auto& probe = find_probe(probes(), "d21-successful-subscribe-response");
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok());
    auto transcript = run.finish();
    ASSERT_EQ(evaluate_draft21_contribution_probe(transcript, probe), true);

    auto altered = transcript;
    altered.writes[0].write.bytes.back() = std::byte{1};
    EXPECT_EQ(evaluate_draft21_contribution_probe(altered, probe), std::nullopt);

    auto other_scenario = transcript;
    other_scenario.scenario_id = "d21-grease-setup-options";
    EXPECT_EQ(evaluate_draft21_contribution_probe(other_scenario, probe), std::nullopt);

    auto incomplete = transcript;
    incomplete.complete = false;
    EXPECT_EQ(evaluate_draft21_contribution_probe(incomplete, probe), std::nullopt);

    auto timed_out = transcript;
    timed_out.timed_out = true;
    EXPECT_EQ(evaluate_draft21_contribution_probe(timed_out, probe), std::nullopt);

    auto wrong_row = probe;
    wrong_row.requirement_id = "D21-9-6-MUST-355";
    EXPECT_EQ(evaluate_draft21_contribution_probe(transcript, wrong_row), std::nullopt);

    auto no_stimulus = transcript;
    no_stimulus.writes.clear();
    EXPECT_EQ(evaluate_draft21_contribution_probe(no_stimulus, probe), std::nullopt);
}

TEST(Contribution, ConfiguredFixtureIsRecoveredFromTheActualRequest) {
    const auto configured = draft21_contribution_probes(std::chrono::milliseconds{1000},
                                                        {cbytes({'a'}), cbytes({'b'})}, cbytes({'v'}));
    const auto& probe = find_probe(configured, "d21-successful-subscribe-response");
    EXPECT_EQ(probe.definition.writes[0].bytes, cbytes({3, 0, 9, 1, 2, 1, 'a', 1, 'b', 1, 'v', 0}));
    ContributionRun run(probe);
    run.deliver(0);
    run.reply(run.stream_of(0), subscribe_ok());
    EXPECT_EQ(evaluate_draft21_contribution_probe(run.finish(), probe), true);
}

TEST(Contribution, CatalogRowsNeedEveryNamedContextAndEvaluator) {
    using namespace requirements;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, root / "requirements/draft21.json");
    const auto state = [&](const std::vector<RawProbeTranscript>& transcripts, const char* row) {
        const auto outcomes = evaluate_draft21_raw_probes(catalog, transcripts);
        const auto found = std::find_if(outcomes.begin(), outcomes.end(),
                                        [&](const auto& outcome) { return outcome.requirement_id == row; });
        return found == outcomes.end() ? OutcomeState::NotRun : found->state;
    };
    const auto transcript_for = [&](const std::string& scenario, const Bytes& reply, int stream_write = 0) {
        const auto& probe = find_probe(probes(), scenario);
        ContributionRun run(probe);
        for (std::size_t index = 0; index < probe.definition.writes.size(); ++index) {
            run.deliver(index);
            if (index == 0 && probe.definition.writes.size() > 1 && probe.definition.writes[1].peer_response_ready)
                run.reply(run.stream_of(0), subscribe_ok());
        }
        run.reply(run.stream_of(static_cast<std::size_t>(stream_write)), reply);
        return run.finish();
    };
    // D21-9-6-MUST-354 is covered by one context and one evaluator.
    EXPECT_EQ(state({transcript_for("d21-successful-subscribe-response", subscribe_ok())}, "D21-9-6-MUST-354"),
              OutcomeState::Pass);
    EXPECT_EQ(state({transcript_for("d21-successful-subscribe-response", request_ok())}, "D21-9-6-MUST-354"),
              OutcomeState::Fail);
    EXPECT_EQ(state({}, "D21-9-6-MUST-354"), OutcomeState::NotRun);

    // Padding, namespace and FETCH rows are likewise single-context rows.
    EXPECT_EQ(state({transcript_for("d21-fetch-start-beyond-largest-object", request_error(0x11))},
                    "D21-9-11-MUST-376"), OutcomeState::Pass);
    EXPECT_EQ(state({transcript_for("d21-fetch-start-beyond-largest-object", cframe(0x18, cbytes({0, 0, 0, 0})))},
                    "D21-9-11-MUST-376"), OutcomeState::Fail);

    // REQUEST_UPDATE exclusivity needs all three named contexts.
    const auto single = [&] {
        const auto& probe = find_probe(probes(), "d21-single-request-update-response");
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok());
        run.deliver(1);
        run.deliver(2);
        run.reply(run.stream_of(0), request_ok());
        run.reply(run.stream_of(2), request_ok());
        return run.finish();
    }();
    const auto coalesced = [&](const std::string& scenario, const Bytes& answers) {
        const auto& probe = find_probe(probes(), scenario);
        ContributionRun run(probe);
        run.deliver(0);
        run.reply(run.stream_of(0), subscribe_ok());
        run.deliver(1);
        run.deliver(2);
        run.reply(run.stream_of(0), answers);
        run.reply(run.stream_of(2), request_ok());
        return run.finish();
    };
    const auto successes = coalesced("d21-coalesced-successful-update-responses",
                                     cconcat({request_ok(), request_ok(), request_ok()}));
    const auto failures = coalesced("d21-coalesced-failed-update-response", request_error(0x17));
    EXPECT_EQ(state({single}, "D21-9-5-MUST-345"), OutcomeState::NotRun);
    EXPECT_EQ(state({single, successes, failures}, "D21-9-5-MUST-345"), OutcomeState::Pass);
    EXPECT_EQ(state({single}, "D21-9-5-1-MUST-352"), OutcomeState::NotRun);
    EXPECT_EQ(state({successes}, "D21-9-5-1-MUST-352"), OutcomeState::Pass);
    const auto too_many = coalesced("d21-coalesced-successful-update-responses",
                                    cconcat({request_ok(), request_ok(), request_ok(), request_ok()}));
    EXPECT_EQ(state({single, too_many, failures}, "D21-9-5-MUST-345"), OutcomeState::Fail);
    EXPECT_EQ(state({single, successes, failures, successes}, "D21-9-5-MUST-345"), OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop::scenarios
