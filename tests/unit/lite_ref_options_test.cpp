#include "lite_ref_options.h"

#include <gtest/gtest.h>

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::lite_ref {
namespace {

using test::lite::LiteDefect;

std::vector<std::string_view> args(std::vector<std::string_view> values) { return values; }

TEST(LiteRefOptions, EveryDefectRoundTripsThroughItsName) {
    std::set<std::string_view> seen;
    for (const auto name : all_defect_names()) {
        const auto defect = defect_from_name(name);
        ASSERT_TRUE(defect.has_value()) << name;
        EXPECT_EQ(defect_name(*defect), name);
        EXPECT_NE(*defect, LiteDefect::None);
        EXPECT_TRUE(seen.insert(name).second) << name;
    }
    EXPECT_FALSE(defect_from_name("no-such-defect").has_value());
    EXPECT_EQ(defect_from_name("none"), LiteDefect::None);
    // One entry per enumerator: a new LiteDefect must get a name here (DatagramOnly is the last enumerator).
    EXPECT_EQ(all_defect_names().size(), static_cast<std::size_t>(LiteDefect::DatagramOnly));
}

TEST(LiteRefOptions, AnUnknownDefectIsAUsageError) {
    const auto r = parse_options(args({"--connect", "moql://127.0.0.1:1/moq", "--defect", "bogus"}));
    EXPECT_FALSE(r.options.has_value());
    EXPECT_NE(r.error.find("bogus"), std::string::npos);
}

TEST(LiteRefOptions, ConnectIsRequiredAndMustBeMoqlOrHttps) {
    EXPECT_FALSE(parse_options(args({})).options.has_value());
    EXPECT_FALSE(parse_options(args({"--connect", "ftp://x/moq"})).options.has_value());
    EXPECT_FALSE(parse_options(args({"--connect", "moql://127.0.0.1:1"})).options.has_value());  // no path
    EXPECT_FALSE(parse_options(args({"--connect"})).options.has_value());
    EXPECT_TRUE(parse_options(args({"--connect", "https://127.0.0.1:1/moq"})).options.has_value());
    EXPECT_TRUE(parse_options(args({"--help"})).options->help);
}

TEST(LiteRefOptions, AnUnknownFlagIsAUsageError) {
    const auto r = parse_options(args({"--connect", "moql://h:1/moq", "--frobnicate"}));
    EXPECT_FALSE(r.options.has_value());
    EXPECT_NE(r.error.find("--frobnicate"), std::string::npos);
}

TEST(LiteRefOptions, FlagsMapOntoThePublisherConfig) {
    const auto r = parse_options(args({"--connect", "moql://h:1/moq?token=l1d", "--datagrams", "--probe-level", "none",
                                       "--frames-per-group", "1", "--defect", "datagram-oversize"}));
    ASSERT_TRUE(r.options.has_value()) << r.error;
    const auto& config = r.options->publisher;
    EXPECT_TRUE(config.datagrams);
    EXPECT_EQ(config.frames_per_group, 1u);
    EXPECT_EQ(config.defect, LiteDefect::DatagramOversize);
    EXPECT_EQ(config.binding, scenarios::LiteBinding::NativeQuic);
    EXPECT_EQ(config.session_url_path, "/moq");
    EXPECT_EQ(config.session_url_query, "token=l1d");
    EXPECT_EQ(config.broadcast, "interop.hang");
    EXPECT_EQ(config.track, "0.m4s");
    for (const auto& parameter : config.setup_parameters)
        EXPECT_NE(parameter.id, wire::moqlite06::kParamProbe);  // level none: no Probe parameter
    EXPECT_EQ(r.options->defect, "datagram-oversize");
}

TEST(LiteRefOptions, GroupCountAndPacingMapOntoThePublisherConfig) {
    const auto r = parse_options(args({"--connect", "moql://h:1/moq", "--groups", "12", "--group-interval-polls", "300"}));
    ASSERT_TRUE(r.options.has_value()) << r.error;
    EXPECT_EQ(r.options->publisher.groups_per_subscription, 12u);
    EXPECT_EQ(r.options->publisher.group_period_polls, 300u);
    EXPECT_FALSE(parse_options(args({"--connect", "moql://h:1/moq", "--groups", "0"})).options.has_value());
}

TEST(LiteRefOptions, WebTransportEndpointsSelectTheWebTransportBinding) {
    const auto r = parse_options(args({"--connect", "https://127.0.0.1:4443/moq?token=l1d"}));
    ASSERT_TRUE(r.options.has_value());
    EXPECT_EQ(r.options->publisher.binding, scenarios::LiteBinding::WebTransport);
    EXPECT_EQ(r.options->publisher.defect, LiteDefect::None);
}

TEST(LiteRefOptions, ProbeLevelDefaultsToReportAndAnUnknownLevelIsRejected) {
    const auto r = parse_options(args({"--connect", "moql://h:1/moq"}));
    ASSERT_TRUE(r.options.has_value());
    bool report = false;
    for (const auto& parameter : r.options->publisher.setup_parameters)
        if (parameter.id == wire::moqlite06::kParamProbe) report = parameter.value == std::vector<std::byte>{std::byte{1}};
    EXPECT_TRUE(report);
    EXPECT_FALSE(parse_options(args({"--connect", "moql://h:1/moq", "--probe-level", "loud"})).options.has_value());
}

}  // namespace
}  // namespace moq::interop::lite_ref
