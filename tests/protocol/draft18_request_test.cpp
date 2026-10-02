#include "moq/interop/scenarios/draft18_request.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <map>

namespace moq::interop::scenarios {
namespace {
std::vector<std::byte> literal(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
// A publisher that announces its namespace first waits for the acknowledgement (section 10.15)
// before it reads other requests, so the stimulus is never answered without it.
TEST(Draft18RequestProfiles, TheRunnerAcknowledgesThePublishersNamespaceAnnouncement) {
    for (const auto& profile : draft18_request_profiles(std::chrono::milliseconds(37)))
        EXPECT_TRUE(profile.definition.acknowledge_publisher_namespace) << profile.definition.id;
}
TEST(Draft18RequestProfiles, RequiredReceiverErrorsHaveIndependentRequestBytes) {
    const auto profiles = draft18_request_profiles(std::chrono::milliseconds(37));
    const std::map<std::string,std::vector<std::byte>> requests{
        {"request-track-in-single-period-namespace",literal({3,0,7,1,1,1,'.',1,'x',0})},
        {"request-empty-track-name-in-session-namespace",
            literal({3,0,13,1,1,8,'.','s','e','s','s','i','o','n',0,0})},
        {"request-unrecognized-session-level-name",
            literal({3,0,49,1,1,8,'.','s','e','s','s','i','o','n',36,
                'i','n','t','e','r','o','p','-','u','n','k','n','o','w','n','-',
                '7','8','9','3','7','a','e','9','d','2','a','b','c','4','e','0','b','6','c','1',0})},
        {"receive-use-alias-for-unregistered-token",literal({3,0,11,1,1,1,'n',1,'x',1,3,2,2,7})},
        {"receive-delete-for-unregistered-token",literal({3,0,11,1,1,1,'n',1,'x',1,3,2,0,7})},
        {"receive-joining-fetch-with-unrelated-or-wrong-state-request-id",literal({0x16,0,5,1,2,3,0,0})},
    };
    for (const auto& [id,bytes] : requests) {
        SCOPED_TRACE(id);
        const auto profile = std::find_if(profiles.begin(),profiles.end(),[&](const auto& item) {
            return item.definition.id == id;
        });
        EXPECT_NE(profile,profiles.end());
        if (profile == profiles.end()) continue;
        EXPECT_EQ(profile->draft,18u);
        EXPECT_EQ(profile->definition.deadline,std::chrono::milliseconds(37));
        EXPECT_FALSE(profile->namespace_scoped);
        EXPECT_EQ(profile->definition.setup_bytes,literal({0xaf,0,0,0}));
        ASSERT_TRUE(profile->definition.start_after_peer_setup);
        ASSERT_TRUE(profile->definition.peer_setup_ready);
        EXPECT_TRUE(profile->definition.peer_setup_ready(literal({0xaf,0,0,0})));
        EXPECT_FALSE(profile->definition.peer_setup_ready(literal({0xaf,0,0})));
        ASSERT_TRUE(profile->definition.response_ready);
        ASSERT_EQ(profile->definition.writes.size(),1u);
        EXPECT_EQ(profile->definition.writes.front().channel,RawProbeChannel::NewBidi);
        EXPECT_EQ(profile->definition.writes.front().bytes,bytes);
        EXPECT_FALSE(profile->definition.writes.front().fin);
        if (id == "request-track-in-single-period-namespace" ||
            id == "request-empty-track-name-in-session-namespace") {
            EXPECT_EQ(profile->requirement_id,id == "request-track-in-single-period-namespace"
                ? "D18-3-2-1-MUST-002" : "D18-3-2-2-MUST-001");
            EXPECT_EQ(profile->evaluator_id,"request-rejected-does-not-exist");
            EXPECT_EQ(profile->expected_error,0x10u);
        } else if (id == "request-unrecognized-session-level-name") {
            EXPECT_EQ(profile->requirement_id,"D18-3-2-2-MUST-002");
            EXPECT_EQ(profile->evaluator_id,"request-error-does-not-exist-for-session-name");
            EXPECT_EQ(profile->expected_error,0x10u);
        } else if (id == "receive-joining-fetch-with-unrelated-or-wrong-state-request-id") {
            EXPECT_EQ(profile->requirement_id,"D18-10-12-2-MUST-004");
            EXPECT_EQ(profile->evaluator_id,"request-error-invalid-joining-request-id");
            EXPECT_EQ(profile->expected_error,0x32u);
        } else {
            EXPECT_EQ(profile->requirement_id,"D18-10-2-2-MUST-007");
            EXPECT_EQ(profile->evaluator_id,"request-error-unknown-auth-token-alias");
            EXPECT_EQ(profile->expected_error,0x17u);
        }
    }
}
TEST(Draft18RequestProfiles, ErrorProofRequiresExactResponseAndDeliveredRequest) {
    const std::map<std::string,std::vector<std::byte>> responses{
        {"request-track-in-single-period-namespace",literal({5,0,3,0x10,0,0})},
        {"request-empty-track-name-in-session-namespace",literal({5,0,3,0x10,0,0})},
        {"request-unrecognized-session-level-name",literal({5,0,3,0x10,0,0})},
        {"receive-use-alias-for-unregistered-token",literal({5,0,3,0x17,0,0})},
        {"receive-delete-for-unregistered-token",literal({5,0,3,0x17,0,0})},
        {"receive-joining-fetch-with-unrelated-or-wrong-state-request-id",literal({5,0,3,0x32,0,0})},
    };
    for (const auto& profile : draft18_request_profiles()) {
        SCOPED_TRACE(profile.definition.id);
        RawProbeTranscript observed;
        observed.scenario_id = profile.definition.id;
        observed.setup = {{RawProbeChannel::NewUni,profile.definition.setup_bytes,false},3,
            profile.definition.setup_bytes.size(),false};
        observed.writes = {{profile.definition.writes.front(),1,
            profile.definition.writes.front().bytes.size(),false}};
        observed.transport_established = observed.peer_setup_received = true;
        observed.stimulus_delivered = observed.complete = true;
        observed.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2,literal({0xaf,0,0,0}),false},
            transport::StreamDataEvent{1,responses.at(profile.definition.id),true}};
        observed.delivery_event_count = 2;
        EXPECT_TRUE(profile.definition.response_ready(observed));
        if (profile.compatibility_error) observed.unknown_auth_token_alias_compatibility_code = profile.expected_error;
        EXPECT_EQ(evaluate_raw_probe_request_error(observed,profile),true);
        if (profile.compatibility_error) observed.unknown_auth_token_alias_compatibility_code = profile.expected_error;
        auto wrong_code = observed;
        std::get<transport::StreamDataEvent>(wrong_code.events.back()).data[3] = std::byte{1};
        EXPECT_EQ(evaluate_raw_probe_request_error(wrong_code,profile),false);
        auto partial = observed;
        --partial.writes.front().accepted;
        EXPECT_FALSE(evaluate_raw_probe_request_error(partial,profile).has_value());
        auto wrong_stream = observed;
        std::get<transport::StreamDataEvent>(wrong_stream.events.back()).stream_id = 5;
        EXPECT_FALSE(profile.definition.response_ready(wrong_stream));
        EXPECT_FALSE(evaluate_raw_probe_request_error(wrong_stream,profile).has_value());
        auto missing_setup = observed;
        std::get<transport::StreamDataEvent>(missing_setup.events[1]).data.clear();
        EXPECT_FALSE(evaluate_raw_probe_request_error(missing_setup,profile).has_value());
        auto session_close = observed;
        session_close.events.back() = transport::PeerCloseEvent{transport::CloseErrorSpace::Application,
            profile.expected_error,{}};
        EXPECT_EQ(evaluate_raw_probe_request_error(session_close,profile),false);
    }
}
}  // namespace
}  // namespace moq::interop::scenarios
