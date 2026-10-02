#include "moq/interop/scenarios/draft18_contribution.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;

std::vector<Draft18ContributionProbe> probes() { return draft18_contribution_probes(std::chrono::milliseconds{60}); }

// Evaluates a publisher SETUP against the URI the runner named for it.
std::optional<bool> run(const Draft18ContributionProbe& p, std::optional<std::string> uri,
                        const Bytes& publisher_setup, bool webtransport = false) {
    auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, publisher_setup); });
    });
    transcript.connection_uri = std::move(uri);
    return evaluate_draft18_contribution_probe(transcript, p, webtransport);
}

TEST(Draft18ContributionUri, QueryIsOnlyAddedForTheQueryScenario) {
    EXPECT_EQ(draft18_contribution_connection_query("connect-publisher-to-native-uri-with-query"), "interop=1");
    EXPECT_TRUE(draft18_contribution_connection_query("connect-publisher-to-native-uri-with-path").empty());
    EXPECT_TRUE(draft18_contribution_connection_query("connect-publisher-to-native-uri-with-authority").empty());
    EXPECT_TRUE(draft18_contribution_connection_query("").empty());
}

// Section 10.3.1.1: the client sets AUTHORITY to the URI's authority.
TEST(Draft18ContributionUri, AuthorityMatchesTheConnectionUri) {
    const auto all = probes();
    const auto& p = probe(all, "connect-publisher-to-native-uri-with-authority", "D18-10-3-1-1-MUST-004");
    EXPECT_EQ(p.evaluator_id, "setup-authority-matches-connection-uri");
    EXPECT_FALSE(p.requires_track);
    EXPECT_TRUE(p.definition.writes.empty());
    const auto authority = [](std::string_view value) { return setup_with({odd_option(5, text(value))}); };
    EXPECT_EQ(run(p, "moqt://127.0.0.1:4443/moq", authority("127.0.0.1:4443")), std::optional<bool>{true});
    EXPECT_EQ(run(p, "moqt://[::1]:4443/moq", authority("[::1]:4443")), std::optional<bool>{true});
    EXPECT_EQ(run(p, "moqt://example.test:4443/moq?x=1", authority("example.test:4443")), std::optional<bool>{true});
    EXPECT_EQ(run(p, "moqt://127.0.0.1:4443/moq", authority("127.0.0.1")), std::optional<bool>{false});
    EXPECT_EQ(run(p, "moqt://127.0.0.1:4443/moq", authority("other:4443")), std::optional<bool>{false});
    // A missing AUTHORITY, or a repeated one, does not set it once.
    EXPECT_EQ(run(p, "moqt://127.0.0.1:4443/moq", setup_with({})), std::optional<bool>{false});
    auto repeated = bytes_of({5, 14});
    repeated = concat(repeated, text("127.0.0.1:4443"));
    repeated = concat(repeated, bytes_of({0, 14}));
    repeated = concat(repeated, text("127.0.0.1:4443"));
    EXPECT_EQ(run(p, "moqt://127.0.0.1:4443/moq", raw_setup(repeated)), std::optional<bool>{false});
    // WebTransport supplies the authority itself, and an unnamed URI proves nothing.
    EXPECT_EQ(run(p, "https://127.0.0.1:4443/moq", authority("127.0.0.1:4443"), true), std::nullopt);
    EXPECT_EQ(run(p, std::nullopt, authority("127.0.0.1:4443")), std::nullopt);
}

// Section 10.3.1.2: PATH is the path-abempty, followed by ? and the query when present.
TEST(Draft18ContributionUri, PathAndQueryMatchTheConnectionUri) {
    const auto all = probes();
    const auto& path = probe(all, "connect-publisher-to-native-uri-with-path", "D18-10-3-1-2-MUST-004");
    const auto& query = probe(all, "connect-publisher-to-native-uri-with-query", "D18-10-3-1-2-MUST-005");
    EXPECT_EQ(path.evaluator_id, "setup-path-matches-uri-path-component");
    EXPECT_EQ(query.evaluator_id, "setup-path-includes-question-mark-and-query");
    const auto with_path = [](std::string_view value) { return setup_with({odd_option(1, text(value))}); };
    EXPECT_EQ(run(path, "moqt://h:1/moq", with_path("/moq")), std::optional<bool>{true});
    EXPECT_EQ(run(path, "moqt://h:1/moq", with_path("/other")), std::optional<bool>{false});
    EXPECT_EQ(run(path, "moqt://h:1/moq", setup_with({})), std::optional<bool>{false});
    // The path row says nothing about a URI that carries a query.
    EXPECT_EQ(run(path, "moqt://h:1/moq?interop=1", with_path("/moq?interop=1")), std::nullopt);
    EXPECT_EQ(run(query, "moqt://h:1/moq?interop=1", with_path("/moq?interop=1")), std::optional<bool>{true});
    EXPECT_EQ(run(query, "moqt://h:1/moq?interop=1", with_path("/moq")), std::optional<bool>{false});
    EXPECT_EQ(run(query, "moqt://h:1/moq?interop=1", with_path("/moq?other")), std::optional<bool>{false});
    EXPECT_EQ(run(query, "moqt://h:1/moq?interop=1", with_path("/moq?")), std::optional<bool>{false});
    // A present but empty query still needs its question mark.
    EXPECT_EQ(run(query, "moqt://h:1/moq?", with_path("/moq?")), std::optional<bool>{true});
    EXPECT_EQ(run(query, "moqt://h:1/moq?", with_path("/moq")), std::optional<bool>{false});
    // No query in the URI: the query row cannot be checked.
    EXPECT_EQ(run(query, "moqt://h:1/moq", with_path("/moq")), std::nullopt);
    EXPECT_EQ(run(path, "https://h:1/moq", with_path("/moq"), true), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
