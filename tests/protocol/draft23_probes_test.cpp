#include "moq/interop/session/draft23_probes.h"

#include <gtest/gtest.h>

#include <set>
#include <string>

namespace {
using namespace moq::interop::session;

std::vector<unsigned> raw(const std::vector<std::byte>& bytes) {
    std::vector<unsigned> result;
    for (const auto byte : bytes) result.push_back(std::to_integer<unsigned>(byte));
    return result;
}

TEST(Draft23Probes, EveryProbeHasAUniqueScenarioIdAndRow) {
    std::set<std::string> scenarios, rows;
    for (const auto& probe : draft23_probes()) {
        EXPECT_TRUE(scenarios.insert(std::string(probe.scenario_id)).second) << probe.scenario_id;
        EXPECT_TRUE(rows.insert(std::string(probe.requirement_id)).second) << probe.requirement_id;
        EXPECT_TRUE(probe.scenario_id.starts_with("d23-probe-")) << probe.scenario_id;
        EXPECT_TRUE(probe.requirement_id.starts_with("D23-")) << probe.requirement_id;
    }
    EXPECT_EQ(scenarios.size(), 7u);
}
TEST(Draft23Probes, FindsAProbeByScenarioId) {
    ASSERT_NE(find_draft23_probe("d23-probe-unknown-control-message"), nullptr);
    EXPECT_EQ(find_draft23_probe("d23-probe-unknown-control-message")->requirement_id, "D23-9-MUST-257");
    EXPECT_EQ(find_draft23_probe("d23-publisher-smoke"), nullptr);
    EXPECT_EQ(find_draft23_probe(""), nullptr);
}
TEST(Draft23Probes, ExpectedSessionCloseCodesComeFromTheDraft) {
    const auto code = [](std::string_view id) { return find_draft23_probe(id)->expected_code; };
    EXPECT_EQ(code("d23-probe-unknown-control-message"), 0x303u);   // UNEXPECTED_MESSAGE
    EXPECT_EQ(code("d23-probe-bad-first-bidi-message"), 0x303u);
    EXPECT_EQ(code("d23-probe-length-mismatch"), 0x300u);           // MALFORMED_MESSAGE
    EXPECT_EQ(code("d23-probe-oversize-control-message"), 0x401u);  // FIELD_LENGTH_EXCEEDED
    EXPECT_EQ(code("d23-probe-server-authority"), 0x19u);           // INVALID_AUTHORITY
    EXPECT_EQ(code("d23-probe-server-path"), 0x8u);                 // INVALID_PATH
    EXPECT_EQ(code("d23-probe-unknown-uni-stream"), 0x200u);        // INVALID_STREAM_HEADER
}
TEST(Draft23Probes, InjectedBytesAreExactlyTheMalformedInput) {
    const auto bytes = [](std::string_view id, std::uint64_t limit = 65535) {
        return raw(draft23_probe_bytes(*find_draft23_probe(id), limit));
    };
    EXPECT_EQ(bytes("d23-probe-unknown-control-message"), (std::vector<unsigned>{0x3f, 0}));
    EXPECT_EQ(bytes("d23-probe-bad-first-bidi-message"), (std::vector<unsigned>{4, 2, 7, 0}));
    EXPECT_EQ(bytes("d23-probe-length-mismatch"), (std::vector<unsigned>{6, 1, 0}));
    EXPECT_EQ(bytes("d23-probe-unknown-uni-stream"), (std::vector<unsigned>{0x0a}));
}
TEST(Draft23Probes, OversizeLengthIsOneByteBeyondThePeersAdvertisedLimit) {
    const auto probe = find_draft23_probe("d23-probe-oversize-control-message");
    // PUBLISH_NAMESPACE (6) with vi64 length 65536 = c1 00 00, and no body.
    EXPECT_EQ(raw(draft23_probe_bytes(*probe, 65535)), (std::vector<unsigned>{6, 0xc1, 0, 0}));
    // A larger advertised limit moves the length up with it: 100000 + 1 = 0x0186a1 -> c1 86 a1.
    EXPECT_EQ(raw(draft23_probe_bytes(*probe, 100000)), (std::vector<unsigned>{6, 0xc1, 0x86, 0xa1}));
}
TEST(Draft23Probes, ServerSetupProbesAddTheForbiddenOptionToTheRunnersSetup) {
    moq::interop::wire::draft21::KeyValues options;
    draft23_probe_setup_options(*find_draft23_probe("d23-probe-server-authority"), options);
    ASSERT_EQ(options.size(), 1u);
    EXPECT_EQ(options[0].type, 5u);
    options.clear();
    draft23_probe_setup_options(*find_draft23_probe("d23-probe-server-path"), options);
    ASSERT_EQ(options.size(), 1u);
    EXPECT_EQ(options[0].type, 1u);
    options.clear();
    draft23_probe_setup_options(*find_draft23_probe("d23-probe-length-mismatch"), options);
    EXPECT_TRUE(options.empty());
}
TEST(Draft23Probes, ChannelsSayWhereTheBytesGo) {
    EXPECT_EQ(find_draft23_probe("d23-probe-unknown-control-message")->channel, Draft23ProbeChannel::Bidi);
    EXPECT_EQ(find_draft23_probe("d23-probe-unknown-uni-stream")->channel, Draft23ProbeChannel::Uni);
    EXPECT_EQ(find_draft23_probe("d23-probe-server-authority")->channel, Draft23ProbeChannel::SetupOption);
    EXPECT_EQ(find_draft23_probe("d23-probe-server-path")->channel, Draft23ProbeChannel::SetupOption);
}
}  // namespace
