#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/scenarios/draft18_contribution.h"
#include "moq/interop/wire/draft18/messages.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>

namespace moq::interop::scenarios {
namespace {
using namespace test;
TEST(Draft18ContributionRegistry, NamesEveryScenarioOfTheFirstBatch) {
    const std::set<std::string> expected{
        "observe-publisher-setup-options", "observe-webtransport-publisher-setup",
        "receive-setup-with-unknown-option", "receive-setup-with-duplicate-unknown-options",
        "setup-unknown-grease-options-and-duplicates",
        "receive-setup-token-register-exceeding-cache-limit",
        "receive-oversize-setup-register-then-use-its-alias"};
    std::set<std::string> actual;
    for (const auto& p : draft18_contribution_probes()) actual.insert(p.definition.id);
    for (const auto& id : expected) {
        EXPECT_TRUE(actual.contains(id)) << id;
        EXPECT_TRUE(draft18_contribution_scenario(id)) << id;
    }
    EXPECT_FALSE(draft18_contribution_scenario("subscribe-to-publisher-track"));
}

// Section 10.3: the sender of SETUP must not repeat a Setup Option Type
// unless its definition permits multiple instances; only AUTHORIZATION TOKEN does.
TEST(Draft18ContributionSetup, ObservesPublisherSetupOptionRepetition) {
    const auto probes = draft18_contribution_probes();
    const auto& p = probe(probes, "observe-publisher-setup-options", "D18-10-3-MUST-NOT-001");
    EXPECT_EQ(p.evaluator_id, "setup-option-types-unique-except-defined-repeatable-options");
    const auto run = [&](const Bytes& setup) {
        const auto t = drive_probe(p.definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup); });
        });
        return evaluate_draft18_contribution_probe(t, p);
    };
    // Option types 1 (PATH) then delta 0 again: a repeated known option.
    EXPECT_EQ(run(raw_setup(bytes_of({1, 1, 'a', 0, 1, 'b'}))), std::optional<bool>{false});
    // Type 7 (MOQT_IMPLEMENTATION) repeated.
    EXPECT_EQ(run(raw_setup(bytes_of({7, 1, 'a', 0, 1, 'b'}))), std::optional<bool>{false});
    // AUTHORIZATION TOKEN (3) may repeat.
    EXPECT_EQ(run(raw_setup(bytes_of({3, 2, 3, 0x41, 0, 2, 3, 0x42}))), std::optional<bool>{true});
    EXPECT_EQ(run(setup_with({})), std::optional<bool>{true});
    // A repeated unknown option may be defined to repeat: no verdict.
    EXPECT_EQ(run(raw_setup(bytes_of({0x80, 0x9d, 1, 'a', 0, 1, 'b'}))), std::nullopt);
}

TEST(Draft18ContributionSetup, WebTransportPublisherSetupOmitsAuthorityAndPath) {
    const auto probes = draft18_contribution_probes();
    const auto& authority = probe(probes, "observe-webtransport-publisher-setup", "D18-10-3-1-1-MUST-NOT-002");
    const auto& path = probe(probes, "observe-webtransport-publisher-setup", "D18-10-3-1-2-MUST-NOT-002");
    EXPECT_EQ(authority.evaluator_id, "setup-omits-authority");
    EXPECT_EQ(path.evaluator_id, "setup-omits-path");
    const auto run = [&](const Draft18ContributionProbe& which, const Bytes& setup, bool webtransport) {
        const auto t = drive_probe(which.definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup); });
        });
        return evaluate_draft18_contribution_probe(t, which, webtransport);
    };
    const auto with_authority = raw_setup(bytes_of({5, 3, 'a', ':', '1'}));
    const auto with_path = raw_setup(bytes_of({1, 2, '/', 'm'}));
    EXPECT_EQ(run(authority, with_authority, true), std::optional<bool>{false});
    EXPECT_EQ(run(authority, with_path, true), std::optional<bool>{true});
    EXPECT_EQ(run(path, with_path, true), std::optional<bool>{false});
    EXPECT_EQ(run(path, with_authority, true), std::optional<bool>{true});
    // Native QUIC publishers may send both; nothing is established there.
    EXPECT_EQ(run(authority, with_authority, false), std::nullopt);
    EXPECT_EQ(run(path, with_path, false), std::nullopt);
}

// Section 10.3: receivers MUST ignore unrecognized Setup Options and MUST
// allow duplicates of unknown options. The publisher must keep processing.
TEST(Draft18ContributionSetup, UnknownSetupOptionsAreSentThenASessionSurvivalRequest) {
    const auto probes = draft18_contribution_probes();
    struct Row { const char* scenario; const char* requirement; };
    const std::vector<Row> rows{
        {"receive-setup-with-unknown-option", "D18-10-3-MUST-002"},
        {"receive-setup-with-duplicate-unknown-options", "D18-10-3-MUST-003"},
        {"setup-unknown-grease-options-and-duplicates", "D18-14-MUST-001"},
        {"setup-unknown-grease-options-and-duplicates", "D18-14-MUST-008"},
        {"setup-unknown-grease-options-and-duplicates", "D18-15-4-MUST-001"},
    };
    for (const auto& row : rows) {
        SCOPED_TRACE(row.requirement);
        const auto& p = probe(probes, row.scenario, row.requirement);
        EXPECT_EQ(p.evaluator_id, "setup-continues-with-unknown-options-ignored");
        EXPECT_FALSE(p.requires_track);
        // SETUP: a GREASE odd option (0x9D) is present and no known option is.
        wire::Cursor cursor(p.definition.setup_bytes);
        const auto decoded = d18::decode_message(d18::StreamRole::Control, cursor, {});
        const auto* message = std::get_if<d18::Message>(&decoded);
        ASSERT_NE(message, nullptr);
        const auto& options = std::get<d18::SetupMessage>(*message).options;
        ASSERT_FALSE(options.empty());
        for (const auto& option : options) EXPECT_TRUE(option.type == 0x9d || option.type == 0x11c);
        EXPECT_EQ(options.front().type, 0x9du);
        if (std::string_view(row.scenario) != "receive-setup-with-unknown-option")
            EXPECT_EQ(options[0].type, options[1].type);  // an unknown duplicate
        if (std::string_view(row.scenario) == "setup-unknown-grease-options-and-duplicates")
            EXPECT_EQ(options.back().type, 0x11cu);  // even, integer valued
        // One valid discovery request follows the peer's SETUP.
        ASSERT_EQ(p.definition.writes.size(), 1u);
        EXPECT_TRUE(p.definition.start_after_peer_setup);
        Bytes request = p.definition.writes.front().bytes;
        wire::Cursor request_cursor(request);
        const auto parsed = d18::decode_message(d18::StreamRole::Request, request_cursor, {});
        const auto* frame = std::get_if<d18::Message>(&parsed);
        ASSERT_NE(frame, nullptr);
        EXPECT_TRUE(std::holds_alternative<d18::SubscribeNamespaceMessage>(*frame));

        const auto run = [&](const std::function<void(PeerView&)>& peer, bool shorten = false) {
            auto definition = p.definition;
            if (shorten) definition.deadline = std::chrono::milliseconds{20};
            const auto t = drive_probe(definition, [&](PeerView& v) {
                v.when("setup", true, [&] { v.data(2, setup_with({})); });
                peer(v);
            });
            return evaluate_draft18_contribution_probe(t, p);
        };
        EXPECT_EQ(run([](PeerView& v) {
                      v.when("ok", v.sent(1), [&] { v.data(1, ok()); });
                  }), std::optional<bool>{true});
        EXPECT_EQ(run([](PeerView& v) {
                      v.when("err", v.sent(1), [&] { v.data(1, error(0x3)); });
                  }), std::optional<bool>{true});
        EXPECT_EQ(run([](PeerView& v) {
                      v.when("close", v.sent(1), [&] {
                          v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
                      });
                  }), std::optional<bool>{false});
        // A transport-level close or silence cannot be blamed on the option.
        EXPECT_EQ(run([](PeerView& v) {
                      v.when("close", v.sent(1), [&] {
                          v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Transport, 0, {}});
                      });
                  }), std::nullopt);
        EXPECT_EQ(run([](PeerView&) {}, true), std::nullopt);
        // A reply that is neither REQUEST_OK nor REQUEST_ERROR proves nothing.
        EXPECT_EQ(run([](PeerView& v) {
                      v.when("odd", v.sent(1), [&] { v.data(1, encode(d18::SubscribeOkMessage{1, {}, {}})); });
                  }), std::nullopt);
    }
}

// Section 10.3.1.4: a SETUP REGISTER that exceeds MAX_AUTH_TOKEN_CACHE_SIZE
// must not fail the session with AUTH_TOKEN_CACHE_OVERFLOW (0x13).
TEST(Draft18ContributionSetup, OversizeSetupTokenRegistrationIsNotASessionError) {
    const auto probes = draft18_contribution_probes();
    const auto& p = probe(probes, "receive-setup-token-register-exceeding-cache-limit", "D18-10-3-1-4-MUST-NOT-001");
    EXPECT_EQ(p.evaluator_id, "no-auth-token-cache-overflow-session-error");
    wire::Cursor cursor(p.definition.setup_bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Control, cursor, {});
    const auto& options = std::get<d18::SetupMessage>(std::get<d18::Message>(decoded)).options;
    ASSERT_EQ(options.size(), 1u);
    EXPECT_EQ(options.front().type, 3u);
    const auto& value = std::get<d18::ByteValue>(options.front().value).bytes;
    wire::Cursor token_cursor(value);
    const auto token = d18::decode_token(token_cursor, value.size());
    const auto* parsed = std::get_if<d18::Token>(&token);
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(parsed->alias_type, d18::TokenAliasType::Register);
    EXPECT_EQ(parsed->alias, 1u);
    EXPECT_EQ(parsed->token_value.size(), 64u);
    const auto run = [&](Bytes peer_setup, const std::function<void(PeerView&)>& peer, bool shorten = false) {
        auto definition = p.definition;
        if (shorten) definition.deadline = std::chrono::milliseconds{20};
        const auto t = drive_probe(definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, peer_setup); });
            peer(v);
        });
        return evaluate_draft18_contribution_probe(t, p);
    };
    const auto answer = [](PeerView& v) { v.when("ok", v.sent(1), [&] { v.data(1, ok()); }); };
    EXPECT_EQ(run(setup_with({}), answer), std::optional<bool>{true});
    // The default cache limit is zero; 0x04 = 79 is still smaller than 16 + 64.
    EXPECT_EQ(run(setup_with({{4, d18::VarIntValue{79, {}}}}), answer), std::optional<bool>{true});
    // A cache big enough for the token means the rule is not exercised.
    EXPECT_EQ(run(setup_with({{4, d18::VarIntValue{80, {}}}}), answer), std::nullopt);
    EXPECT_EQ(run(setup_with({}), [](PeerView& v) {
                  v.when("close", v.sent(1), [&] {
                      v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x13, {}});
                  });
              }), std::optional<bool>{false});
    // Another close code does not establish the overflow behaviour.
    EXPECT_EQ(run(setup_with({}), [](PeerView& v) {
                  v.when("close", v.sent(1), [&] {
                      v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
                  });
              }), std::nullopt);
}

// Sections 10.3.1.4 and 10.2.2: the oversize REGISTER acts as USE_VALUE, so a
// later USE_ALIAS for it names an unregistered alias and must be rejected.
TEST(Draft18ContributionSetup, AliasOfOversizeSetupRegisterIsNotRegistered) {
    const auto probes = draft18_contribution_probes();
    const auto& p = probe(probes, "receive-oversize-setup-register-then-use-its-alias", "D18-10-3-1-4-MUST-001");
    EXPECT_EQ(p.evaluator_id, "setup-token-processed-as-value-without-alias-registration");
    wire::Cursor cursor(p.definition.writes.front().bytes);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto& request = std::get<d18::SubscribeNamespaceMessage>(std::get<d18::Message>(decoded));
    ASSERT_EQ(request.parameters.size(), 1u);
    const auto& token = std::get<d18::Token>(request.parameters.front().value);
    EXPECT_EQ(token.alias_type, d18::TokenAliasType::UseAlias);
    EXPECT_EQ(token.alias, 1u);
    const auto run = [&](const std::function<void(PeerView&)>& peer,
                         std::optional<std::uint64_t> compatibility, bool shorten = false) {
        auto definition = p.definition;
        if (shorten) definition.deadline = std::chrono::milliseconds{20};
        auto t = drive_probe(definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup_with({})); });
            peer(v);
        });
        t.unknown_auth_token_alias_compatibility_code = compatibility;
        return evaluate_draft18_contribution_probe(t, p);
    };
    const auto reject = [](PeerView& v) { v.when("err", v.sent(1), [&] { v.data(1, error(0x17)); }); };
    // An accepted request means the alias resolved: it was wrongly registered.
    EXPECT_EQ(run([](PeerView& v) { v.when("ok", v.sent(1), [&] { v.data(1, ok()); }); }, 0x17),
              std::optional<bool>{false});
    EXPECT_EQ(run([](PeerView& v) { v.when("ok", v.sent(1), [&] { v.data(1, ok()); }); }, std::nullopt),
              std::optional<bool>{false});
    // Draft 18 assigns no REQUEST_ERROR code for the rejection; compare only a configured one.
    EXPECT_EQ(run(reject, 0x17), std::optional<bool>{true});
    EXPECT_EQ(run(reject, 0x18), std::optional<bool>{false});
    EXPECT_EQ(run(reject, std::nullopt), std::nullopt);
    EXPECT_EQ(run([](PeerView&) {}, 0x17, true), std::nullopt);
}

// The registry, healthz listing and evaluator all key on the probe list.
TEST(Draft18ContributionRegistry, EveryProbeIsAnExecutableRawScenario) {
    std::set<std::string> scenarios_seen;
    for (const auto& p : draft18_contribution_probes()) {
        scenarios_seen.insert(p.definition.id);
        EXPECT_TRUE(app::executable_scenario(18, p.definition.id)) << p.definition.id;
        EXPECT_TRUE(app::raw_probe_scenario(18, p.definition.id)) << p.definition.id;
        EXPECT_EQ(app::scenario_requires_track(18, p.definition.id), p.requires_track) << p.definition.id;
        EXPECT_FALSE(p.requirement_id.empty());
        EXPECT_FALSE(p.evaluator_id.empty());
    }
    for (const auto id : app::executable_scenarios(18))
        if (draft18_contribution_scenario(id)) EXPECT_TRUE(scenarios_seen.contains(std::string(id)));
}

// Scoring through the catalog: rows pass only on complete, valid contexts.
TEST(Draft18ContributionEvaluators, ScoresRowsFromRawContexts) {
    using requirements::OutcomeState;
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(
        18, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = requirements::RequirementCatalog::load(
        source, root / "requirements/draft18.json");
    const auto outcome = [&](const std::vector<requirements::ScenarioContext>& contexts,
                             std::string_view row) {
        for (const auto& item : requirements::evaluate_draft18(catalog, contexts))
            if (item.requirement_id == row) return item.state;
        ADD_FAILURE() << row;
        return OutcomeState::NotRun;
    };
    const auto probes = draft18_contribution_probes();
    const auto context_for = [&](std::string_view scenario, std::string_view requirement,
                                 const Bytes& peer_setup, const std::function<void(PeerView&)>& peer,
                                 bool webtransport = false) {
        const auto& p = probe(probes, scenario, requirement);
        requirements::ScenarioContext context;
        context.scenario_id = std::string(scenario);
        context.webtransport = webtransport;
        context.raw_probe = drive_probe(p.definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, peer_setup); });
            peer(v);
        });
        context.complete = context.raw_probe->complete;
        context.stimulus_delivered = context.raw_probe->stimulus_delivered;
        return context;
    };
    const auto answer = [](PeerView& v) { v.when("ok", v.sent(1), [&] { v.data(1, ok()); }); };
    const auto none = [](PeerView&) {};
    EXPECT_EQ(outcome({context_for("receive-setup-with-unknown-option", "D18-10-3-MUST-002",
                                   setup_with({}), answer)}, "D18-10-3-MUST-002"),
              OutcomeState::Pass);
    EXPECT_EQ(outcome({context_for("receive-setup-with-unknown-option", "D18-10-3-MUST-002",
                                   setup_with({}), [](PeerView& v) {
                                       v.when("close", v.sent(1), [&] {
                                           v.push(transport::PeerCloseEvent{
                                               transport::CloseErrorSpace::Application, 3, {}});
                                       });
                                   })}, "D18-10-3-MUST-002"),
              OutcomeState::Fail);
    // D18-14-MUST-NOT-001 is not bound yet and so cannot be passed by one scenario.
    EXPECT_EQ(outcome({context_for("setup-unknown-grease-options-and-duplicates", "D18-14-MUST-001",
                                   setup_with({}), answer)}, "D18-14-MUST-NOT-001"),
              OutcomeState::NotRun);
    // The SETUP rows also named by a typed scenario accept either kind of context.
    EXPECT_EQ(outcome({context_for("observe-publisher-setup-options", "D18-10-3-MUST-NOT-001",
                                   setup_with({}), none)}, "D18-10-3-MUST-NOT-001"),
              OutcomeState::Pass);
    EXPECT_EQ(outcome({context_for("observe-publisher-setup-options", "D18-10-3-MUST-NOT-001",
                                   raw_setup(bytes_of({7, 1, 'a', 0, 1, 'b'})), none)},
                      "D18-10-3-MUST-NOT-001"),
              OutcomeState::Fail);
    EXPECT_EQ(outcome({context_for("observe-webtransport-publisher-setup", "D18-10-3-1-1-MUST-NOT-002",
                                   setup_with({}), none, true)}, "D18-10-3-1-1-MUST-NOT-002"),
              OutcomeState::Pass);
    EXPECT_EQ(outcome({context_for("observe-webtransport-publisher-setup", "D18-10-3-1-1-MUST-NOT-002",
                                   setup_with({}), none, false)}, "D18-10-3-1-1-MUST-NOT-002"),
              OutcomeState::NotRun);
}

}  // namespace
}  // namespace moq::interop::scenarios
