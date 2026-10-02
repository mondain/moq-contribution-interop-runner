#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/draft21_evaluators.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::requirements {
namespace {

const RequirementCatalog& catalog21() {
    static const auto catalog = [] {
        const std::filesystem::path root = MOQ_INTEROP_PROJECT_SOURCE_DIR;
        const auto source = load_draft_source(
            21, root / "docs", root / "requirements/draft-digests.json");
        return RequirementCatalog::load(source, root / "requirements/draft21.json");
    }();
    return catalog;
}

// Rows closed by slice A. Each must have every named scenario registered and
// every named evaluator bound with usable evidence.
const std::vector<std::string_view> kClaimedRows{
    "D21-6-3-MUST-NOT-141", "D21-9-1-MUST-NOT-289", "D21-9-1-1-MUST-NOT-292",
    "D21-9-1-2-MUST-NOT-299", "D21-9-1-1-MUST-294", "D21-9-1-2-MUST-301",
    "D21-9-1-1-MUST-296", "D21-9-1-2-MUST-303", "D21-9-1-2-MUST-304",
    "D21-6-3-2-MUST-150",
    "D21-2-4-2-MUST-NOT-026", "D21-2-4-2-MUST-NOT-028", "D21-2-4-2-MUST-NOT-029",
    "D21-2-4-2-MUST-NOT-030", "D21-6-5-MUST-NOT-166", "D21-6-5-MUST-NOT-167",
    "D21-8-3-MUST-NOT-230", "D21-8-7-MUST-250", "D21-7-5-MUST-206",
    "D21-6-4-2-2-MUST-157", "D21-6-4-2-2-MUST-158", "D21-6-4-2-2-MUST-NOT-156",
    "D21-2-2-MUST-020", "D21-3-6-MUST-070", "D21-4-2-MUST-089", "D21-6-3-MUST-NOT-146",
    "D21-6-2-MUST-139", "D21-3-3-1-MUST-NOT-057",
    "D21-2-2-MUST-NOT-018", "D21-8-9-MUST-264", "D21-8-9-MUST-265", "D21-8-9-MUST-269", "D21-8-9-MUST-271",
    "D21-9-1-4-MUST-308",
};

using scenarios::Draft21AnnouncementContext;
using scenarios::Draft21AnnouncementEventKind;
using scenarios::Draft21SetupOptionValue;

std::vector<std::byte> to_bytes(std::string_view text) {
    std::vector<std::byte> result;
    for (const char c : text) result.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return result;
}

Draft21SetupOptionValue text_option(std::uint64_t type, std::string_view text) {
    return {type, true, 0, to_bytes(text)};
}

// A completed publisher PUBLISH exchange after the publisher's SETUP.
Draft21AnnouncementContext exchange(std::string scenario) {
    Draft21AnnouncementContext context;
    context.complete = context.target_publish_seen = context.response_delivered = true;
    context.scenario_id = std::move(scenario);
    context.evidence = {
        {Draft21AnnouncementEventKind::TransportEstablished, std::nullopt, std::nullopt},
        {Draft21AnnouncementEventKind::PeerSetupReceived, 2, std::nullopt},
        {Draft21AnnouncementEventKind::PublishObserved, 0, 0},
        {Draft21AnnouncementEventKind::ResponseDelivered, 0, 0}};
    return context;
}

OutcomeState state(const Draft21AnnouncementContext& context, std::string_view id) {
    for (const auto& outcome : evaluate_draft21_announcement(catalog21(), context))
        if (outcome.requirement_id == id) return outcome.state;
    ADD_FAILURE() << "missing outcome " << id;
    return OutcomeState::NotRun;
}

TEST(Draft21GapA, ClaimedRowsHaveCompleteExecutableBindings) {
    const auto report = audit_completeness(
        catalog21(), draft21_executable_bindings(), app::executable_scenarios(21));
    for (const auto row : kClaimedRows) {
        for (const auto& finding : report.findings) {
            EXPECT_NE(finding.requirement_id, row) << finding.code << ' ' << finding.detail;
        }
    }
}

TEST(Draft21GapA, AnnouncementScenariosRouteToTheAnnouncementController) {
    for (const auto id : app::kDraft21GapAnnouncementScenarios) {
        EXPECT_TRUE(app::executable_scenario(21, id)) << id;
        EXPECT_TRUE(app::announcement_gap_scenario(21, id)) << id;
        EXPECT_FALSE(app::raw_probe_scenario(21, id)) << id;
        EXPECT_TRUE(app::scenario_requires_track(21, id)) << id;
        EXPECT_FALSE(app::executable_scenario(18, id)) << id;
    }
}

TEST(Draft21GapA, RawScenariosRouteToRawProbesAndTransportRules) {
    for (const auto id : app::kDraft21GapRawScenarios) {
        EXPECT_TRUE(app::executable_scenario(21, id)) << id;
        EXPECT_TRUE(app::gap_raw_scenario(21, id)) << id;
        EXPECT_TRUE(app::raw_probe_scenario(21, id)) << id;
        EXPECT_FALSE(app::announcement_gap_scenario(21, id)) << id;
        EXPECT_TRUE(app::scenario_requires_track(21, id)) << id;
        EXPECT_FALSE(app::executable_scenario(18, id)) << id;
    }
    EXPECT_TRUE(app::gap_native_only_scenario("d21-native-quic-datagram-support"));
    EXPECT_TRUE(app::gap_webtransport_only_scenario("d21-webtransport-h3-datagram-support"));
    EXPECT_TRUE(app::gap_native_only_scenario("d21-native-quic-without-datagram-negotiation"));
    EXPECT_TRUE(app::gap_webtransport_only_scenario("d21-webtransport-h3-without-datagram-negotiation"));
    EXPECT_FALSE(app::gap_native_only_scenario("d21-control-stream-lifetime"));
    EXPECT_FALSE(app::gap_webtransport_only_scenario("d21-control-stream-lifetime"));
}

TEST(Draft21GapA, BindingsNeverNameUnregisteredScenarios) {
    const auto report = audit_completeness(
        catalog21(), draft21_executable_bindings(), app::executable_scenarios(21));
    for (const auto& finding : report.findings) {
        EXPECT_NE(finding.code, "nonexecutable_scenario") << finding.requirement_id;
        EXPECT_NE(finding.code, "mismatched_binding") << finding.requirement_id;
        EXPECT_NE(finding.code, "duplicate_binding") << finding.requirement_id;
        EXPECT_NE(finding.code, "orphan_binding") << finding.requirement_id;
        EXPECT_NE(finding.code, "missing_evidence_schema") << finding.requirement_id;
    }
}

TEST(Draft21GapA, NativeUriOptionsMustEchoTheDrivenConnectionUri) {
    auto context = exchange("d21-native-publisher-uri-options");
    context.expected_uri = scenarios::Draft21ExpectedConnectionUri{"127.0.0.1:4433", "/moq"};
    // Observed mode never learns the URI, so nothing is scored.
    auto observed = context;
    observed.expected_uri.reset();
    observed.peer_setup_options = {text_option(5, "127.0.0.1:4433"), text_option(1, "/moq")};
    EXPECT_EQ(state(observed, "D21-9-1-1-MUST-296"), OutcomeState::NotRun);

    context.peer_setup_options = {text_option(5, "127.0.0.1:4433"), text_option(1, "/moq")};
    EXPECT_EQ(state(context, "D21-9-1-1-MUST-296"), OutcomeState::Pass);
    EXPECT_EQ(state(context, "D21-9-1-2-MUST-303"), OutcomeState::Pass);

    auto wrong_port = context;
    wrong_port.peer_setup_options = {text_option(5, "127.0.0.1:4434"), text_option(1, "/moq")};
    EXPECT_EQ(state(wrong_port, "D21-9-1-1-MUST-296"), OutcomeState::Fail);
    EXPECT_EQ(state(wrong_port, "D21-9-1-2-MUST-303"), OutcomeState::Pass);

    auto wrong_path = context;
    wrong_path.peer_setup_options = {text_option(5, "127.0.0.1:4433"), text_option(1, "/other")};
    EXPECT_EQ(state(wrong_path, "D21-9-1-1-MUST-296"), OutcomeState::Pass);
    EXPECT_EQ(state(wrong_path, "D21-9-1-2-MUST-303"), OutcomeState::Fail);

    auto absent = context;
    absent.peer_setup_options.clear();
    EXPECT_EQ(state(absent, "D21-9-1-1-MUST-296"), OutcomeState::Fail);
    EXPECT_EQ(state(absent, "D21-9-1-2-MUST-303"), OutcomeState::Fail);

    // A correct SETUP without a completed exchange is not a pass.
    auto incomplete = context;
    incomplete.complete = false;
    EXPECT_EQ(state(incomplete, "D21-9-1-1-MUST-296"), OutcomeState::NotRun);

    // Over WebTransport the options are forbidden, not echoed.
    auto webtransport = context;
    webtransport.webtransport = true;
    EXPECT_EQ(state(webtransport, "D21-9-1-1-MUST-296"), OutcomeState::NotRun);

    // The row is bound to this scenario only.
    auto other = context;
    other.scenario_id = "d21-native-publisher-uri-query";
    EXPECT_EQ(state(other, "D21-9-1-2-MUST-303"), OutcomeState::NotRun);
}

TEST(Draft21GapA, PathConcatenatesTheLiteralQuestionMarkAndQuery) {
    for (const auto& [scenario, path] : {std::pair<const char*, const char*>{
             "d21-native-publisher-uri-query", "/moq?run=1"},
         {"d21-native-publisher-empty-query", "/moq?"}}) {
        SCOPED_TRACE(scenario);
        auto context = exchange(scenario);
        context.expected_uri = scenarios::Draft21ExpectedConnectionUri{"h:1", path};
        context.peer_setup_options = {text_option(5, "h:1"), text_option(1, path)};
        EXPECT_EQ(state(context, "D21-9-1-2-MUST-304"), OutcomeState::Pass);
        // Dropping the query, or the separator of an empty query, fails.
        context.peer_setup_options = {text_option(5, "h:1"), text_option(1, "/moq")};
        EXPECT_EQ(state(context, "D21-9-1-2-MUST-304"), OutcomeState::Fail);
        // Sending the query without the literal "?" also fails.
        context.peer_setup_options = {text_option(5, "h:1"), text_option(1, "/moqrun=1")};
        EXPECT_EQ(state(context, "D21-9-1-2-MUST-304"), OutcomeState::Fail);
    }
    // A URI without a query cannot exercise concatenation.
    auto context = exchange("d21-native-publisher-uri-query");
    context.expected_uri = scenarios::Draft21ExpectedConnectionUri{"h:1", "/moq"};
    context.peer_setup_options = {text_option(5, "h:1"), text_option(1, "/moq")};
    EXPECT_EQ(state(context, "D21-9-1-2-MUST-304"), OutcomeState::NotRun);
}

TEST(Draft21GapA, RequiredSetupOptionsDependOnTransport) {
    auto native = exchange("d21-native-quic-required-setup-options");
    native.expected_uri = scenarios::Draft21ExpectedConnectionUri{"h:1", "/moq"};
    native.peer_setup_options = {text_option(5, "h:1"), text_option(1, "/moq")};
    EXPECT_EQ(state(native, "D21-6-3-2-MUST-150"), OutcomeState::Pass);
    native.peer_setup_options = {text_option(5, "h:1")};
    EXPECT_EQ(state(native, "D21-6-3-2-MUST-150"), OutcomeState::Fail);
    native.peer_setup_options = {text_option(1, "/moq")};
    EXPECT_EQ(state(native, "D21-6-3-2-MUST-150"), OutcomeState::Fail);
    native.expected_uri.reset();
    EXPECT_EQ(state(native, "D21-6-3-2-MUST-150"), OutcomeState::NotRun);

    auto webtransport = exchange("d21-webtransport-required-setup-options");
    webtransport.webtransport = true;
    EXPECT_EQ(state(webtransport, "D21-6-3-2-MUST-150"), OutcomeState::Pass);
    // A forbidden option is left to the AUTHORITY and PATH rows.
    webtransport.peer_setup_options = {text_option(5, "h:1")};
    EXPECT_EQ(state(webtransport, "D21-6-3-2-MUST-150"), OutcomeState::NotRun);
    webtransport.peer_setup_options.clear();
    webtransport.webtransport = false;
    EXPECT_EQ(state(webtransport, "D21-6-3-2-MUST-150"), OutcomeState::NotRun);
}

scenarios::Draft21AnnouncementEvent publication(Draft21AnnouncementEventKind kind,
                                                std::string_view first_field) {
    scenarios::Draft21AnnouncementEvent event{kind, 0, 0};
    event.track_namespace = {to_bytes(first_field), to_bytes("x")};
    return event;
}

// A fully set up session that stayed alive through the observation window.
Draft21AnnouncementContext quiet_window(std::string scenario) {
    Draft21AnnouncementContext context;
    context.scenario_id = std::move(scenario);
    context.window_elapsed = true;
    context.evidence = {
        {Draft21AnnouncementEventKind::TransportEstablished, std::nullopt, std::nullopt},
        {Draft21AnnouncementEventKind::LocalSetupSent, 3, std::nullopt},
        {Draft21AnnouncementEventKind::PeerSetupReceived, 2, std::nullopt}};
    return context;
}

struct AttemptCase {
    const char* row;
    const char* scenario;
    const char* forbidden_first_field;
    Draft21AnnouncementEventKind forbidden_kind;
    bool other_kind_is_violation;
};

TEST(Draft21GapA, ReservedNamespaceAttemptsPassOnlyAfterAQuietLiveWindow) {
    using Kind = Draft21AnnouncementEventKind;
    const std::vector<AttemptCase> cases{
        {"D21-2-4-2-MUST-NOT-026", "d21-attempt-unregistered-period-namespace-publication", ".custom",
         Kind::PublishObserved, true},
        {"D21-2-4-2-MUST-NOT-028", "d21-attempt-single-period-namespace-use", ".",
         Kind::PublishObserved, true},
        {"D21-2-4-2-MUST-NOT-029", "d21-attempt-single-period-track-publication", ".",
         Kind::PublishObserved, false},
        {"D21-2-4-2-MUST-NOT-030", "d21-attempt-single-period-namespace-publication", ".",
         Kind::NamespaceObserved, false},
        {"D21-6-5-MUST-NOT-166", "d21-application-track-publication-under-session", ".session",
         Kind::PublishObserved, false},
        {"D21-6-5-MUST-NOT-167", "d21-application-namespace-publication-under-session", ".session",
         Kind::NamespaceObserved, false},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.row);
        auto context = quiet_window(test.scenario);
        EXPECT_EQ(state(context, test.row), OutcomeState::Pass);

        auto early = context;
        early.window_elapsed = false;
        EXPECT_EQ(state(early, test.row), OutcomeState::NotRun);

        auto never_set_up = context;
        never_set_up.evidence.pop_back();
        EXPECT_EQ(state(never_set_up, test.row), OutcomeState::NotRun);

        auto wrong_scenario = context;
        wrong_scenario.scenario_id = "d21-publisher-request-stream-placement";
        EXPECT_EQ(state(wrong_scenario, test.row), OutcomeState::NotRun);

        auto violated = context;
        violated.evidence.push_back(publication(test.forbidden_kind, test.forbidden_first_field));
        EXPECT_EQ(state(violated, test.row), OutcomeState::Fail);
        // A violation is a failure even when the window did not elapse.
        violated.window_elapsed = false;
        EXPECT_EQ(state(violated, test.row), OutcomeState::Fail);

        // An ordinary namespace is never a violation.
        auto ordinary = context;
        ordinary.evidence.push_back(publication(Kind::PublishObserved, "media"));
        ordinary.evidence.push_back(publication(Kind::NamespaceObserved, "media"));
        EXPECT_EQ(state(ordinary, test.row), OutcomeState::Pass);

        // The other publication kind violates only the broad rows.
        const auto other_kind = test.forbidden_kind == Kind::PublishObserved
            ? Kind::NamespaceObserved : Kind::PublishObserved;
        auto other = context;
        other.evidence.push_back(publication(other_kind, test.forbidden_first_field));
        EXPECT_EQ(state(other, test.row),
                  test.other_kind_is_violation ? OutcomeState::Fail : OutcomeState::Pass);
    }
}

TEST(Draft21GapA, SessionNamespaceIsRegisteredButOtherPeriodNamespacesAreNot) {
    auto context = quiet_window("d21-attempt-unregistered-period-namespace-publication");
    context.evidence.push_back(publication(Draft21AnnouncementEventKind::PublishObserved, ".session"));
    EXPECT_EQ(state(context, "D21-2-4-2-MUST-NOT-026"), OutcomeState::Pass);
    context.evidence.push_back(publication(Draft21AnnouncementEventKind::NamespaceObserved, ".x"));
    EXPECT_EQ(state(context, "D21-2-4-2-MUST-NOT-026"), OutcomeState::Fail);
    // The single period begins with a period and has no registration either.
    auto single = quiet_window("d21-attempt-unregistered-period-namespace-publication");
    single.evidence.push_back(publication(Draft21AnnouncementEventKind::PublishObserved, "."));
    EXPECT_EQ(state(single, "D21-2-4-2-MUST-NOT-026"), OutcomeState::Fail);
    // ".sessions" is not ".session".
    auto longer = quiet_window("d21-application-track-publication-under-session");
    longer.evidence.push_back(publication(Draft21AnnouncementEventKind::PublishObserved, ".sessions"));
    EXPECT_EQ(state(longer, "D21-6-5-MUST-NOT-166"), OutcomeState::Pass);
}

TEST(Draft21GapA, EmittedKeyValueTypeDeltasFailOnlyOnObservedOverflow) {
    auto context = exchange("d21-publisher-key-value-type-deltas");
    EXPECT_EQ(state(context, "D21-8-3-MUST-NOT-230"), OutcomeState::Pass);
    auto incomplete = context;
    incomplete.complete = false;
    EXPECT_EQ(state(incomplete, "D21-8-3-MUST-NOT-230"), OutcomeState::NotRun);
    for (const auto* detail : {"draft-21 key-value type overflow", "SETUP option type overflow",
                               "draft-21 parameter type overflow", "Track Property type overflow"}) {
        SCOPED_TRACE(detail);
        auto overflow = incomplete;
        overflow.evidence.push_back(
            {Draft21AnnouncementEventKind::MalformedPublisherMessage, 0, std::nullopt});
        overflow.evidence.back().detail = detail;
        EXPECT_EQ(state(overflow, "D21-8-3-MUST-NOT-230"), OutcomeState::Fail);
    }
    // Some other malformed message is not a type-delta violation.
    auto other = incomplete;
    other.evidence.push_back(
        {Draft21AnnouncementEventKind::MalformedPublisherMessage, 0, std::nullopt});
    other.evidence.back().detail = "empty draft-21 namespace field";
    EXPECT_EQ(state(other, "D21-8-3-MUST-NOT-230"), OutcomeState::NotRun);
}

TEST(Draft21GapA, EmittedNamespaceFieldsMustBeNonEmpty) {
    auto context = exchange("d21-publisher-emitted-namespace-fields");
    EXPECT_EQ(state(context, "D21-8-7-MUST-250"), OutcomeState::NotRun);  // no decoded field yet
    context.evidence[2].track_namespace = {to_bytes("media")};
    EXPECT_EQ(state(context, "D21-8-7-MUST-250"), OutcomeState::Pass);
    // A zero-field namespace is legal but proves nothing about field contents.
    context.evidence[2].track_namespace.clear();
    EXPECT_EQ(state(context, "D21-8-7-MUST-250"), OutcomeState::NotRun);
    context.evidence.push_back(
        {Draft21AnnouncementEventKind::MalformedPublisherMessage, 0, std::nullopt});
    context.evidence.back().detail = "empty draft-21 namespace field";
    EXPECT_EQ(state(context, "D21-8-7-MUST-250"), OutcomeState::Fail);
}

TEST(Draft21GapA, RoutingNeedsAnExplicitNamespacePublication) {
    auto context = exchange("d21-publisher-namespace-routing-announcement");
    // A PUBLISH alone never satisfies the duty (and is not scored as a failure).
    EXPECT_EQ(state(context, "D21-7-5-MUST-206"), OutcomeState::NotRun);
    context.namespace_announced = true;
    EXPECT_EQ(state(context, "D21-7-5-MUST-206"), OutcomeState::Pass);
    context.scenario_id = "d21-publisher-request-stream-placement";
    EXPECT_EQ(state(context, "D21-7-5-MUST-206"), OutcomeState::NotRun);
}

TEST(Draft21GapA, ReservedNamespaceScenariosRequireMatchingFixtures) {
    using app::gap_fixture_valid;
    EXPECT_TRUE(gap_fixture_valid("d21-attempt-single-period-track-publication", {".", "x"}));
    EXPECT_FALSE(gap_fixture_valid("d21-attempt-single-period-track-publication", {".x"}));
    EXPECT_FALSE(gap_fixture_valid("d21-attempt-single-period-track-publication", {}));
    EXPECT_TRUE(gap_fixture_valid("d21-attempt-unregistered-period-namespace-publication", {".x"}));
    EXPECT_FALSE(gap_fixture_valid("d21-attempt-unregistered-period-namespace-publication", {"."}));
    EXPECT_FALSE(gap_fixture_valid("d21-attempt-unregistered-period-namespace-publication", {".session"}));
    EXPECT_FALSE(gap_fixture_valid("d21-attempt-unregistered-period-namespace-publication", {"media"}));
    EXPECT_TRUE(gap_fixture_valid("d21-application-namespace-publication-under-session", {".session", "a"}));
    EXPECT_FALSE(gap_fixture_valid("d21-application-namespace-publication-under-session", {".sessions"}));
    EXPECT_TRUE(gap_fixture_valid("d21-publisher-emitted-namespace-fields", {"anything"}));
}

}  // namespace
}  // namespace moq::interop::requirements
