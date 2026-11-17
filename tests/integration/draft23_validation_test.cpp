#include "moq/interop/app/draft23_run.h"
#include "moq/interop/app/scenario_registry.h"
#include <gtest/gtest.h>
#include <chrono>

namespace moq::interop::app {
namespace {
using namespace std::chrono_literals;
RunConfig smoke() {
    return {DraftVersion::Draft23, TransportKind::NativeQuic, RunMode::Observed,
            {"d23-publisher-smoke"}, 100ms, TrackFixture{{"test"}, "video"}};
}
TEST(Draft23Validation, AcceptsBothSmokeModesOnBothTransportsWithinTimeoutBounds) {
    for (const auto* id : {"d23-publisher-smoke", "d23-direct-subscribe-smoke"})
        for (const auto transport : {TransportKind::NativeQuic, TransportKind::WebTransport})
            for (const auto mode : {RunMode::Observed, RunMode::Driven}) {
                auto config = smoke();
                config.scenario_ids = {id}; config.transport = transport; config.mode = mode;
                EXPECT_TRUE(valid_draft23_run(config));
                config.timeout = 1h;
                EXPECT_TRUE(valid_draft23_run(config));
                config.timeout = 99ms;
                EXPECT_FALSE(valid_draft23_run(config));
                config.timeout = 1h + 1ms;
                EXPECT_FALSE(valid_draft23_run(config));
            }
}
TEST(Draft23Validation, RefusesOtherDraftsUnknownIdsAndMultipleContexts) {
    auto config = smoke();
    config.draft = DraftVersion::Draft22;
    EXPECT_FALSE(valid_draft23_run(config));
    config = smoke(); config.scenario_ids = {"d22-setup-unknown-options"};
    EXPECT_FALSE(valid_draft23_run(config));
    config.scenario_ids = {"d23-publisher-smoke", "d23-direct-subscribe-smoke"};
    EXPECT_FALSE(valid_draft23_run(config));
    config.scenario_ids.clear();
    EXPECT_FALSE(valid_draft23_run(config));
}
TEST(Draft23Validation, EnforcesFixtureFieldAndByteLimits) {
    auto config = smoke();
    config.track_fixture.reset();
    EXPECT_FALSE(valid_draft23_run(config));
    config = smoke(); config.track_fixture->namespace_fields.clear();
    EXPECT_FALSE(valid_draft23_run(config));
    config = smoke(); config.track_fixture->namespace_fields = {""};
    EXPECT_FALSE(valid_draft23_run(config));
    config = smoke(); config.track_fixture->track_name.clear();
    EXPECT_FALSE(valid_draft23_run(config));
    config = smoke(); config.track_fixture->namespace_fields.assign(32, "x");
    EXPECT_TRUE(valid_draft23_run(config));
    config.track_fixture->namespace_fields.emplace_back("x");
    EXPECT_FALSE(valid_draft23_run(config));
    config = smoke(); config.track_fixture->namespace_fields = {"x"};
    config.track_fixture->track_name.assign(4095, 'x');
    EXPECT_TRUE(valid_draft23_run(config));
    config.track_fixture->track_name.push_back('x');
    EXPECT_FALSE(valid_draft23_run(config));
}
TEST(Draft23Validation, DoesNotRequireFetchForPublisherSmoke) {
    auto config = smoke(); config.publisher_capabilities.fetch = false;
    EXPECT_TRUE(valid_draft23_run(config));
    EXPECT_FALSE(scenario_requires_fetch(23, "d23-publisher-smoke"));
}
}
}
