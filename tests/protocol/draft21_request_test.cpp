#include "moq/interop/scenarios/draft21_request.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace moq::interop::scenarios {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

const RequestProbeProfile* find(const std::vector<RequestProbeProfile>& profiles,
                                const char* scenario) {
    const auto found = std::find_if(profiles.begin(), profiles.end(),
        [&](const auto& profile) { return profile.definition.id == scenario; });
    return found == profiles.end() ? nullptr : &*found;
}

TEST(Draft21RequestProbes, ReservedNamesHaveLiteralDraft21RequestFrames) {
    // Draft 21 sections 2.4.2, 6.5, 9.6 and 9.15. All server requests
    // use odd Request ID 1. '.session' tests use no application Tracks.
    struct Fixture {
        const char* scenario;
        const char* requirement;
        const char* evaluator;
        bool namespace_scoped;
        std::vector<std::byte> frame;
    };
    const Fixture fixtures[]{
        {"d21-request-single-period-namespace", "D21-2-4-2-MUST-031",
         "d21-single-period-request-does-not-exist", false,
         bytes({3, 0, 7, 1, 1, 1, '.', 1, 'x', 0})},
        {"d21-session-namespace-empty-track-request", "D21-6-5-MUST-170",
         "d21-session-empty-track-does-not-exist", false,
         bytes({3, 0, 13, 1, 1, 8, '.', 's', 'e', 's', 's', 'i', 'o', 'n', 0, 0})},
        {"d21-session-namespace-unknown-track-request", "D21-6-5-MUST-171",
         "d21-session-unknown-track-does-not-exist", false,
         bytes({3, 0, 14, 1, 1, 8, '.', 's', 'e', 's', 's', 'i', 'o', 'n', 1, 'x', 0})},
        {"d21-session-namespace-unknown-namespace-request", "D21-6-5-MUST-172",
         "d21-session-unknown-namespace-does-not-exist", true,
         bytes({0x50, 0, 14, 1, 2, 8, '.', 's', 'e', 's', 's', 'i', 'o', 'n', 1, 'x', 0})},
    };
    const auto profiles = draft21_request_profiles();
    for (const auto& fixture : fixtures) {
        SCOPED_TRACE(fixture.scenario);
        const auto* profile = find(profiles, fixture.scenario);
        ASSERT_NE(profile, nullptr);
        EXPECT_EQ(profile->draft, 21u);
        EXPECT_EQ(profile->requirement_id, fixture.requirement);
        EXPECT_EQ(profile->evaluator_id, fixture.evaluator);
        EXPECT_EQ(profile->expected_error, 0x10u);
        EXPECT_EQ(profile->namespace_scoped, fixture.namespace_scoped);
        ASSERT_EQ(profile->definition.writes.size(), 1u);
        EXPECT_EQ(profile->definition.writes[0].channel, RawProbeChannel::NewBidi);
        EXPECT_FALSE(profile->definition.writes[0].fin);
        EXPECT_EQ(profile->definition.writes[0].bytes, fixture.frame);
        EXPECT_TRUE(profile->definition.response_ready);
    }
}

TEST(Draft21RequestProbes, InvalidFiltersHaveIndependentLiteralFrames) {
    // Draft 21 sections 3.3.2, 8.6, 9.20.13, 9.20.14, 9.20.15.
    struct Fixture {
        const char* scenario;
        const char* requirement;
        const char* evaluator;
        bool namespace_scoped;
        std::vector<std::byte> frame;
    };
    const Fixture fixtures[]{
        {"d21-range-filter-start-delta-overflow", "D21-8-6-MUST-249",
         "d21-range-delta-overflow-invalid-filter", false,
         bytes({3, 0, 19, 1, 0, 1, 'x', 1, 0x26, 12, 0,
                0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 1})},
        {"d21-range-filter-end-delta-overflow", "D21-8-6-MUST-249",
         "d21-range-delta-overflow-invalid-filter", false,
         bytes({3, 0, 18, 1, 0, 1, 'x', 1, 0x26, 11, 0,
                0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 1})},
        {"d21-duplicate-range-filter-key-in-request", "D21-3-3-2-MUST-064",
         "d21-duplicate-range-filter-invalid-filter", false,
         bytes({3, 0, 13, 1, 0, 1, 'x', 2, 0x26, 2, 0, 0, 0, 2, 0, 0})},
        {"d21-priority-filter-start-above-255", "D21-9-20-13-MUST-433",
         "d21-priority-filter-invalid-filter", false,
         bytes({3, 0, 10, 1, 0, 1, 'x', 1, 0x27, 3, 0, 0x81, 0})},
        {"d21-priority-filter-end-above-255", "D21-9-20-13-MUST-433",
         "d21-priority-filter-invalid-filter", false,
         bytes({3, 0, 11, 1, 0, 1, 'x', 1, 0x27, 4, 0, 0x80, 0xff, 1})},
        {"d21-object-property-filter-odd-property-type", "D21-9-20-14-MUST-435",
         "d21-object-property-filter-invalid-filter", false,
         bytes({3, 0, 10, 1, 0, 1, 'x', 1, 0x28, 3, 0, 1, 0})},
        {"d21-track-property-filter-odd-property-type", "D21-9-20-15-MUST-437",
         "d21-track-property-filter-invalid-filter", true,
         bytes({0x51, 0, 8, 1, 0, 1, 0x29, 3, 0, 1, 0})},
    };
    const auto profiles = draft21_request_profiles();
    for (const auto& fixture : fixtures) {
        SCOPED_TRACE(fixture.scenario);
        const auto* profile = find(profiles, fixture.scenario);
        ASSERT_NE(profile, nullptr);
        EXPECT_EQ(profile->requirement_id, fixture.requirement);
        EXPECT_EQ(profile->evaluator_id, fixture.evaluator);
        EXPECT_EQ(profile->expected_error, 0x36u);
        EXPECT_EQ(profile->namespace_scoped, fixture.namespace_scoped);
        ASSERT_EQ(profile->definition.writes.size(), 1u);
        EXPECT_FALSE(profile->definition.writes[0].fin);
        EXPECT_EQ(profile->definition.writes[0].bytes, fixture.frame);
    }
}

TEST(Draft21RequestProbes, UnknownTokenAliasUsesNoPriorRegistration) {
    // Draft 21 sections 8.9 Figure 3 and 9.20.3: USE_ALIAS(2), Alias 0.
    const auto profiles = draft21_request_profiles();
    const auto* profile = find(profiles, "d21-request-unknown-token-alias");
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(profile->requirement_id, "D21-8-9-MUST-269");
    EXPECT_EQ(profile->evaluator_id, "d21-unknown-token-alias-message-error");
    EXPECT_EQ(profile->expected_error, 0x17u);
    EXPECT_EQ(profile->definition.setup_bytes, bytes({0xaf, 0, 0, 0}));
    ASSERT_EQ(profile->definition.writes.size(), 1u);
    EXPECT_FALSE(profile->definition.writes[0].fin);
    EXPECT_EQ(profile->definition.writes[0].bytes,
              bytes({3, 0, 9, 1, 0, 1, 'x', 1, 3, 2, 2, 0}));
}

TEST(Draft21RequestProbes, InvalidFiltersRequireTheirAdvertisedRangeCapacity) {
    // Draft 21 sections 3.3.2 and 9.1.6: unsupported optional filters
    // must not prove specific malformed-filter rejection obligations.
    const auto profiles = draft21_request_profiles();
    const auto* single = find(profiles, "d21-priority-filter-start-above-255");
    const auto* multiple = find(profiles, "d21-duplicate-range-filter-key-in-request");
    ASSERT_NE(single, nullptr);
    ASSERT_NE(multiple, nullptr);
    EXPECT_FALSE(single->definition.peer_setup_ready(bytes({0xaf})));
    EXPECT_FALSE(single->definition.peer_setup_ready(bytes({0xaf, 0, 0, 0})));
    EXPECT_FALSE(single->definition.peer_setup_ready(bytes({0xaf, 0, 0, 2, 6, 0})));
    EXPECT_TRUE(single->definition.peer_setup_ready(bytes({0xaf, 0, 0, 2, 6, 1})));
    EXPECT_FALSE(multiple->definition.peer_setup_ready(bytes({0xaf, 0, 0, 2, 6, 1})));
    EXPECT_TRUE(multiple->definition.peer_setup_ready(bytes({0xaf, 0, 0, 2, 6, 2})));
}

TEST(Draft21RequestProbes, FilterErrorProofRequiresActualPeerSetupCapacity) {
    // Draft 21 sections 3.3.2 and 9.1.6. Replaying an INVALID_FILTER
    // response cannot prove a malformed-filter rule when support is absent.
    struct Fixture {
        const char* scenario;
        unsigned ranges;
    };
    const Fixture fixtures[]{
        {"d21-range-filter-start-delta-overflow", 2},
        {"d21-range-filter-end-delta-overflow", 1},
        {"d21-duplicate-range-filter-key-in-request", 2},
        {"d21-priority-filter-start-above-255", 1},
        {"d21-priority-filter-end-above-255", 1},
        {"d21-object-property-filter-odd-property-type", 1},
        {"d21-track-property-filter-odd-property-type", 1},
    };
    const auto profiles = draft21_request_profiles(std::chrono::milliseconds{25});
    for (const auto& fixture : fixtures) {
        SCOPED_TRACE(fixture.scenario);
        const auto* profile = find(profiles, fixture.scenario);
        ASSERT_NE(profile, nullptr);
        EXPECT_EQ(profile->definition.deadline, std::chrono::milliseconds{25});
        RawProbeTranscript observed;
        observed.scenario_id = profile->definition.id;
        observed.setup = {{RawProbeChannel::NewUni, profile->definition.setup_bytes, false},
                          3, profile->definition.setup_bytes.size(), false};
        observed.writes = {{profile->definition.writes.front(), 1,
                           profile->definition.writes.front().bytes.size(), false}};
        observed.transport_established = observed.peer_setup_received = true;
        observed.stimulus_delivered = observed.complete = true;
        observed.delivery_event_count = 2;
        observed.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 0}), false},
            transport::StreamDataEvent{1, bytes({5, 0, 3, 0x36, 0, 0}), true}};
        EXPECT_FALSE(evaluate_raw_probe_request_error(observed, *profile).has_value());
        auto& peer_setup = std::get<transport::StreamDataEvent>(observed.events[1]);
        peer_setup.data = bytes({0xaf, 0, 0, 2, 6, 0});
        EXPECT_FALSE(evaluate_raw_probe_request_error(observed, *profile).has_value());
        peer_setup.data = bytes({0xaf, 0, 0, 2, 6, fixture.ranges});
        EXPECT_EQ(evaluate_raw_probe_request_error(observed, *profile), true);
        std::get<transport::StreamDataEvent>(observed.events.back()).data[3] = std::byte{0x10};
        EXPECT_EQ(evaluate_raw_probe_request_error(observed, *profile), false);
    }
}

}  // namespace
}  // namespace moq::interop::scenarios
