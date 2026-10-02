#include "moq/interop/scenarios/draft18_contribution.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

// Expectations are derived from draft-18 section 3.1.1, not from any publisher's behavior.
namespace moq::interop::scenarios {
namespace {
using namespace test;
using std::chrono::milliseconds;

std::optional<bool> run(const char* scenario, const char* requirement, const std::function<void(PeerView&)>& script,
                        std::optional<std::string> uri, bool webtransport, bool default_setup) {
    const auto all = draft18_contribution_probes(milliseconds{60}, {text("n")}, text("t"));
    const auto& p = probe(all, scenario, requirement);
    auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        if (default_setup) v.when("setup", true, [&] { v.data(2, setup_with({})); });
        script(v);
    });
    if (uri) transcript.connection_uri = *uri;
    return evaluate_draft18_contribution_probe(transcript, p, webtransport);
}

TEST(Draft18ContributionEmptyHost, IsRegisteredAsAnExecutableScenario) {
    const auto all = draft18_contribution_probes(milliseconds{60}, {text("n")}, text("t"));
    EXPECT_TRUE(draft18_contribution_scenario("connect-with-empty-host-moqt-uri"));
    EXPECT_EQ(probe(all, "connect-with-empty-host-moqt-uri", "D18-3-1-1-MUST-NOT-001").evaluator_id,
              "empty-host-uri-not-used-for-session");
}

// Section 3.1.1: no moqt URI with an empty host is used.
TEST(Draft18ContributionEmptyHost, UriIsNotUsedToConnect) {
    const auto row = [](const char* uri, bool webtransport = false) {
        return run("connect-with-empty-host-moqt-uri", "D18-3-1-1-MUST-NOT-001", [](PeerView&) {}, std::string(uri),
                   webtransport, false);
    };
    EXPECT_EQ(row("moqt://:4443/moq"), std::optional<bool>{false});
    EXPECT_EQ(row("moqt:///moq"), std::optional<bool>{false});
    EXPECT_EQ(row("moqt://user@:4443/moq"), std::optional<bool>{false});
    EXPECT_EQ(row("moqt://localhost:4443/moq"), std::nullopt);
    EXPECT_EQ(row("moqt://[::1]:4443/moq"), std::nullopt);
    EXPECT_EQ(row("moqt://:4443/moq", true), std::nullopt);
    EXPECT_TRUE(draft18_contribution_empty_host_scenario("connect-with-empty-host-moqt-uri"));
    EXPECT_FALSE(draft18_contribution_empty_host_scenario("publish-key-value-type-boundary"));
}

TEST(Draft18ContributionEmptyHost, NeverScoresWithoutAConnection) {
    const auto all = draft18_contribution_probes(milliseconds{60}, {text("n")}, text("t"));
    const auto& p = probe(all, "connect-with-empty-host-moqt-uri", "D18-3-1-1-MUST-NOT-001");
    RawProbeTranscript transcript;
    transcript.scenario_id = p.definition.id;
    transcript.connection_uri = "moqt://:4443/moq";
    EXPECT_EQ(evaluate_draft18_contribution_probe(transcript, p), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
