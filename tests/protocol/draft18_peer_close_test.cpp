#include "moq/interop/scenarios/draft18_peer_close.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/requirements/draft_source.h"
#include "moq/interop/wire/draft18/messages.h"

#include <gtest/gtest.h>
#include <algorithm>
#include <filesystem>
#include <string_view>

namespace moq::interop::scenarios {
namespace {
std::vector<std::byte> literal(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
struct Fixture {
    const char* requirement;
    const char* scenario;
    unsigned opener_type;
    std::vector<std::byte> response;
};
std::vector<Fixture> fixtures() {
    // Section 1.4.4: 1025 UTF-8 bytes, two-byte reason length, full 1029-byte payload.
    auto oversized = literal({5,4,5,0x20,0,0x84,1});
    oversized.insert(oversized.end(),1025,std::byte{'a'});
    return {
        {"D18-1-4-4-MUST-001","receive-reason-phrase-length-over-1024",0x1d,oversized},
        {"D18-10-5-MUST-001","receive-publish-request-ok-with-track-properties",0x1d,literal({7,0,3,0,0x22,1})},
        {"D18-10-5-MUST-004","receive-publish-namespace-ok-with-track-properties",6,literal({7,0,3,0,0x22,1})},
        {"D18-10-6-1-MUST-005","receive-publish-namespace-redirect-with-nonempty-track-name",6,
            literal({5,0,9,0x34,0,0,0,1,1,'n',1,'x'})},
        {"D18-12-5-MUST-001","publisher-recovery-track-status-reply-with-invalid-default-group-order",0xd,
            literal({7,0,3,0,0x22,3})},
        {"D18-12-6-MUST-001","publisher-recovery-track-status-reply-with-dynamic-groups-two",0xd,
            literal({7,0,3,0,0x30,2})},
        {"D18-12-7-MUST-004","publisher-recovery-track-status-reply-with-invalid-default-group-order",0xd,
            literal({7,0,3,0,0x22,3})},
        {"D18-12-7-MUST-005","publisher-recovery-track-status-reply-with-invalid-default-group-order-in-immutable-wrapper",0xd,
            literal({7,0,5,0,0xb,2,0x22,3})},
    };
}
std::vector<std::byte> opener(unsigned type) {
    if (type == 6) return literal({6,0,5,0,1,1,'n',0});
    if (type == 0x1d) return literal({0x1d,0,8,0,1,1,'n',1,'x',0,0});
    return literal({0xd,0,7,0,1,1,'n',1,'x',0});
}
const Draft18PeerCloseProbe* find_probe(const std::vector<Draft18PeerCloseProbe>& probes,
                                       const Fixture& fixture) {
    const auto found = std::find_if(probes.begin(),probes.end(),[&](const auto& probe) {
        return probe.requirement_id == fixture.requirement && probe.definition.id == fixture.scenario;
    });
    return found == probes.end() ? nullptr : &*found;
}

TEST(Draft18PeerCloseProfiles, ResponsesHaveIndependentLiteralFramesAndCatalogBindings) {
    const auto probes = draft18_peer_close_probes(std::chrono::milliseconds(37));
    const auto expected = fixtures();
    ASSERT_EQ(probes.size(),expected.size() + 1);
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = requirements::load_draft_source(18,root / "docs",root / "requirements/draft-digests.json");
    const auto catalog = requirements::RequirementCatalog::load(source,root / "requirements/draft18.json");
    for (const auto& fixture : expected) {
        SCOPED_TRACE(fixture.requirement);
        const auto* probe = find_probe(probes,fixture);
        ASSERT_NE(probe,nullptr);
        EXPECT_EQ(probe->expected_close,3u);
        EXPECT_EQ(probe->evaluator_id,"session-closed-protocol-violation");
        EXPECT_EQ(probe->definition.deadline,std::chrono::milliseconds(37));
        EXPECT_EQ(probe->definition.setup_bytes,literal({0xaf,0,0,0}));
        ASSERT_EQ(probe->definition.writes.size(),1u);
        EXPECT_EQ(probe->definition.writes.front().channel,RawProbeChannel::PeerBidi);
        EXPECT_EQ(probe->definition.writes.front().bytes,fixture.response);
        const auto row = std::find_if(catalog.requirements.begin(),catalog.requirements.end(),[&](const auto& item) {
            return item.id == fixture.requirement;
        });
        ASSERT_NE(row,catalog.requirements.end());
        EXPECT_EQ(row->applicability,requirements::Applicability::Applicable);
        EXPECT_EQ(row->testability,requirements::Testability::Testable);
        EXPECT_NE(std::find(row->scenarios.begin(),row->scenarios.end(),fixture.scenario),row->scenarios.end());
        EXPECT_NE(std::find(row->evaluators.begin(),row->evaluators.end(),probe->evaluator_id),row->evaluators.end());
    }
}

TEST(Draft18PeerCloseProfiles, OversizedReasonUsesDraftVi64AndOnlyReasonLengthIsInvalid) {
    const auto probes = draft18_peer_close_probes();
    const auto found = std::find_if(probes.begin(), probes.end(), [](const auto& profile) {
        return profile.requirement_id == "D18-1-4-4-MUST-001";
    });
    ASSERT_NE(found, probes.end());
    const auto& response = found->definition.writes.front().bytes;
    wire::Cursor payload(std::span<const std::byte>(response).subspan(3));
    EXPECT_EQ(std::get<std::uint64_t>(wire::read_vi64(payload)), 0x20u);
    EXPECT_EQ(std::get<std::uint64_t>(wire::read_vi64(payload)), 0u);
    EXPECT_EQ(std::get<std::uint64_t>(wire::read_vi64(payload)), 1025u);
    EXPECT_EQ(payload.remaining(), 1025u);
    wire::Cursor frame(response);
    const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Request, frame, {});
    const auto* failure = std::get_if<wire::DecodeError>(&decoded);
    ASSERT_NE(failure, nullptr);
    EXPECT_NE(failure->detail.find("reason phrase exceeds 1024 bytes"), std::string::npos);
}

TEST(Draft18PeerCloseProfiles, PeerPredicateRequiresOneCompleteMatchingOpeningRequest) {
    const auto probes = draft18_peer_close_probes();
    ASSERT_EQ(probes.size(),9u);
    for (const auto& fixture : fixtures()) {
        SCOPED_TRACE(fixture.requirement);
        const auto* probe = find_probe(probes,fixture);
        ASSERT_NE(probe,nullptr);
        ASSERT_TRUE(probe->definition.peer_request_ready);
        auto request = opener(fixture.opener_type);
        EXPECT_TRUE(probe->definition.peer_request_ready(request));
        auto another_request = request;
        another_request[3] = std::byte{2};
        another_request[6] = std::byte{'m'};
        EXPECT_TRUE(probe->definition.peer_request_ready(another_request));
        auto odd_request_id = request;
        odd_request_id[3] = std::byte{1};
        EXPECT_FALSE(probe->definition.peer_request_ready(odd_request_id));
        for (std::size_t length = 0; length < request.size(); ++length)
            EXPECT_FALSE(probe->definition.peer_request_ready(std::span(request).first(length)));
        for (auto other : {6u,0xdu,0x1du}) {
            if (other != fixture.opener_type)
                EXPECT_FALSE(probe->definition.peer_request_ready(opener(other)));
        }
        auto malformed = request;
        malformed[2] = std::byte{0};
        EXPECT_FALSE(probe->definition.peer_request_ready(malformed));
        auto trailing = request;
        trailing.push_back(std::byte{0});
        EXPECT_FALSE(probe->definition.peer_request_ready(trailing));
        auto invalid_namespace = request;
        invalid_namespace[4] = std::byte{33};
        EXPECT_FALSE(probe->definition.peer_request_ready(invalid_namespace));
        EXPECT_FALSE(probe->definition.peer_request_ready(literal({7,0,1,0})));
        EXPECT_FALSE(probe->definition.peer_request_ready(literal({0xaf,0,0,0})));
        ASSERT_TRUE(probe->definition.peer_setup_ready);
        EXPECT_TRUE(probe->definition.peer_setup_ready(literal({0xaf,0,0,0})));
        EXPECT_FALSE(probe->definition.peer_setup_ready(literal({0xaf,0,0})));
    }
}

TEST(Draft18PeerCloseProfiles, CloseProofRequiresTheActualCompletePeerRequestBeforeResponseDelivery) {
    const auto probes = draft18_peer_close_probes();
    ASSERT_EQ(probes.size(),9u);
    for (const auto& fixture : fixtures()) {
        SCOPED_TRACE(fixture.requirement);
        const auto* probe = find_probe(probes,fixture);
        ASSERT_NE(probe,nullptr);
        RawProbeTranscript transcript;
        transcript.scenario_id = fixture.scenario;
        transcript.setup = {{RawProbeChannel::NewUni,literal({0xaf,0,0,0}),false},3,4,false};
        transcript.writes = {{probe->definition.writes.front(),0,fixture.response.size(),false}};
        transcript.transport_established = transcript.peer_setup_received = true;
        transcript.stimulus_delivered = transcript.complete = true;
        transcript.delivery_event_count = 3;
        transcript.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2,literal({0xaf,0,0,0}),false},
            transport::StreamDataEvent{0,opener(fixture.opener_type),false},
            transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
        EXPECT_EQ(evaluate_raw_probe_close(transcript,probe->definition,probe->expected_close),true);
        auto wrong_code = transcript;
        std::get<transport::PeerCloseEvent>(wrong_code.events.back()).error_code = 99;
        EXPECT_EQ(evaluate_raw_probe_close(wrong_code,probe->definition,probe->expected_close),false);
        auto incomplete = transcript;
        --incomplete.writes.front().accepted;
        EXPECT_FALSE(evaluate_raw_probe_close(incomplete,probe->definition,probe->expected_close).has_value());
        auto absent = transcript;
        std::get<transport::StreamDataEvent>(absent.events[2]).data.clear();
        EXPECT_FALSE(evaluate_raw_probe_close(absent,probe->definition,probe->expected_close).has_value());
        auto unrelated = transcript;
        std::get<transport::StreamDataEvent>(unrelated.events[2]).stream_id = 4;
        EXPECT_FALSE(evaluate_raw_probe_close(unrelated,probe->definition,probe->expected_close).has_value());
        auto late = transcript;
        late.delivery_event_count = 2;
        EXPECT_FALSE(evaluate_raw_probe_close(late,probe->definition,probe->expected_close).has_value());
        auto malformed = transcript;
        std::get<transport::StreamDataEvent>(malformed.events[2]).data.pop_back();
        EXPECT_FALSE(evaluate_raw_probe_close(malformed,probe->definition,probe->expected_close).has_value());
    }
}

TEST(Draft18PeerCloseProfiles, TrackPropertiesAndRedirectFixturesHaveOnlyTheirTargetViolation) {
    // The wire decoder accepts these complete frames; request context and property
    // semantics provide the violations in Sections 10.5, 10.6.1, 12.5 and 12.6.
    for (const auto& fixture : fixtures()) {
        if (std::string_view(fixture.requirement) == "D18-1-4-4-MUST-001") continue;
        wire::Cursor cursor(fixture.response);
        const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<wire::draft18::Message>(&decoded);
        ASSERT_NE(message,nullptr);
        EXPECT_EQ(cursor.remaining(),0u);
        if (fixture.response.front() == std::byte{7}) {
            const auto* ok = std::get_if<wire::draft18::RequestOkMessage>(message);
            ASSERT_NE(ok,nullptr);
            EXPECT_TRUE(ok->parameters.empty());
            ASSERT_EQ(ok->track_properties.entries.size(),1u);
        } else {
            const auto* error = std::get_if<wire::draft18::RequestErrorMessage>(message);
            ASSERT_NE(error,nullptr);
            EXPECT_EQ(error->error_code,0x34u);
            ASSERT_TRUE(error->redirect);
            EXPECT_TRUE(error->redirect->connect_uri.empty());
            EXPECT_EQ(error->redirect->track_name.bytes,literal({'x'}));
        }
    }
}
}  // namespace
}  // namespace moq::interop::scenarios
