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

}  // namespace
}  // namespace moq::interop::requirements
