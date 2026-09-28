#include "moq/interop/session/publisher_session.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <variant>
#include <vector>

namespace moq::interop::session {
namespace {

using transport::ConnectionEstablishedEvent;
using transport::StreamDataEvent;
using wire::draft18::SetupMessage;

ConnectionEstablishedEvent established() {
    return {{std::byte{'m'}, std::byte{'o'}, std::byte{'q'}, std::byte{'t'},
             std::byte{'-'}, std::byte{'1'}, std::byte{'8'}},
            {std::byte{0}, std::byte{1}},
            {std::byte{2}, std::byte{0}}, 1200};
}

const std::array kSetup{std::byte{0xaf}, std::byte{0x00}, std::byte{0x00},
                        std::byte{0x00}};

void establish_with_local_setup(PublisherSession& session) {
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(3, SetupMessage{}, false);
}

const CloseSessionAction* close_action(const SessionTransition& transition) {
    if (transition.actions.size() != 1) return nullptr;
    return std::get_if<CloseSessionAction>(&transition.actions.front());
}

TEST(Draft18Session, BecomesActiveOnlyAfterBothSetupMessages) {
    PublisherSession session;
    EXPECT_EQ(session.phase(), SessionPhase::AwaitingTransport);

    session.on_event(established());
    EXPECT_EQ(session.phase(), SessionPhase::AwaitingSetup);

    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(3, SetupMessage{}, false);
    EXPECT_EQ(session.phase(), SessionPhase::AwaitingSetup);

    const std::array peer_setup{std::byte{0xaf}, std::byte{0x00},
                                std::byte{0x00}, std::byte{0x00}};
    session.on_event(StreamDataEvent{2, {peer_setup.begin(), peer_setup.end()},
                                     false});
    EXPECT_EQ(session.phase(), SessionPhase::Active);

    const auto evidence = session.take_evidence(32);
    ASSERT_GE(evidence.size(), 5u);
    for (std::size_t index = 0; index < evidence.size(); ++index) {
        EXPECT_EQ(evidence[index].sequence, index);
    }
}

TEST(Draft18Session, ControlSetupIsIncrementalAndNeedMoreHasNoSetupEvidence) {
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(3, SetupMessage{}, false);
    session.take_evidence(32);

    session.on_event(StreamDataEvent{2, {std::byte{0xaf}}, false});
    EXPECT_EQ(session.phase(), SessionPhase::AwaitingSetup);
    EXPECT_TRUE(session.take_evidence(32).empty());

    session.on_event(StreamDataEvent{
        2, {std::byte{0x00}, std::byte{0}, std::byte{0}},
        false});
    EXPECT_EQ(session.phase(), SessionPhase::Active);
    const auto evidence = session.take_evidence(32);
    ASSERT_EQ(evidence.size(), 2u);
    EXPECT_EQ(evidence[0].kind, EvidenceKind::PeerStreamClassified);
    EXPECT_EQ(evidence[1].kind, EvidenceKind::PeerSetupReceived);
}

TEST(Draft18Session, SecondControlStreamClosesWithProtocolViolation) {
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(3, SetupMessage{}, false);
    const std::array setup{std::byte{0xc0}, std::byte{0x2f}, std::byte{0x00},
                           std::byte{0x00}, std::byte{0x00}};
    session.on_event(StreamDataEvent{2, {setup.begin(), setup.end()}, false});

    const auto transition = session.on_event(
        StreamDataEvent{6, {setup.begin(), setup.end()}, false});
    ASSERT_EQ(transition.actions.size(), 1u);
    const auto* close = std::get_if<CloseSessionAction>(&transition.actions[0]);
    ASSERT_NE(close, nullptr);
    EXPECT_EQ(close->application_error, 0x3u);
    EXPECT_EQ(session.phase(), SessionPhase::Closing);
}

TEST(Draft18Session, SetupMayBeSplitAtEveryByteBoundaryWithoutMutation) {
    for (std::size_t split = 0; split <= kSetup.size(); ++split) {
        PublisherSession session;
        establish_with_local_setup(session);
        session.take_evidence(32);

        session.on_event(StreamDataEvent{
            2, {kSetup.begin(), kSetup.begin() +
                                    static_cast<std::ptrdiff_t>(split)},
            false});
        if (split != kSetup.size()) {
            EXPECT_EQ(session.phase(), SessionPhase::AwaitingSetup) << split;
            const auto partial_evidence = session.take_evidence(32);
            EXPECT_LE(partial_evidence.size(), 1u) << split;
        }
        session.on_event(StreamDataEvent{
            2, {kSetup.begin() + static_cast<std::ptrdiff_t>(split),
                kSetup.end()},
            false});
        EXPECT_EQ(session.phase(), SessionPhase::Active) << split;
    }
}

TEST(Draft18Session, NonMinimalStreamAndMessageTypesAreAccepted) {
    PublisherSession session;
    establish_with_local_setup(session);
    const std::vector<std::byte> bytes{
        std::byte{0xc0}, std::byte{0x2f}, std::byte{0x00},
        std::byte{0x00}, std::byte{0x00}};
    session.on_event(StreamDataEvent{2, bytes, false});
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18Session, CoalescedSecondSetupClosesWithProtocolViolation) {
    PublisherSession session;
    establish_with_local_setup(session);
    std::vector<std::byte> bytes(kSetup.begin(), kSetup.end());
    bytes.insert(bytes.end(), kSetup.begin(), kSetup.end());
    const auto transition = session.on_event(StreamDataEvent{2, bytes, false});
    ASSERT_NE(close_action(transition), nullptr);
    EXPECT_EQ(close_action(transition)->application_error, 0x3u);
    EXPECT_EQ(session.phase(), SessionPhase::Closing);
}

TEST(Draft18Session, UnknownUnidirectionalStreamTypeClosesSession) {
    PublisherSession session;
    establish_with_local_setup(session);
    const auto transition =
        session.on_event(StreamDataEvent{2, {std::byte{0x01}}, false});
    ASSERT_NE(close_action(transition), nullptr);
    EXPECT_EQ(close_action(transition)->application_error, 0x3u);
}

TEST(Draft18Session, ExactSubgroupPatternExcludesHighBitTypes) {
    PublisherSession valid;
    establish_with_local_setup(valid);
    valid.on_event(StreamDataEvent{2, {std::byte{0x10}}, false});
    ASSERT_NE(valid.phase(), SessionPhase::Closing);
    const auto evidence = valid.take_evidence(32);
    ASSERT_FALSE(evidence.empty());
    const auto classified_event = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::PeerStreamClassified;
        });
    ASSERT_NE(classified_event, evidence.end());
    const auto* classified =
        std::get_if<StreamEvidence>(&classified_event->data);
    ASSERT_NE(classified, nullptr);
    EXPECT_EQ(classified->stream_kind, PeerStreamKind::Subgroup);

    PublisherSession invalid;
    establish_with_local_setup(invalid);
    wire::ByteWriter encoded_0x90(9);
    ASSERT_TRUE(wire::write_vi64(0x90, encoded_0x90));
    const auto transition = invalid.on_event(StreamDataEvent{
        2, {encoded_0x90.bytes().begin(), encoded_0x90.bytes().end()}, false});
    ASSERT_NE(close_action(transition), nullptr);

    PublisherSession above_seven_bits;
    establish_with_local_setup(above_seven_bits);
    wire::ByteWriter encoded_0x110(9);
    ASSERT_TRUE(wire::write_vi64(0x110, encoded_0x110));
    const auto above_seven_bits_transition = above_seven_bits.on_event(
        StreamDataEvent{2,
                        {encoded_0x110.bytes().begin(),
                         encoded_0x110.bytes().end()},
                        false});
    ASSERT_NE(close_action(above_seven_bits_transition), nullptr);
}

TEST(Draft18Session, IncomingLocalStreamIdsAreObservationErrors) {
    for (const auto stream_id : {1u, 3u}) {
        PublisherSession session;
        session.on_event(established());
        session.take_evidence(32);
        const auto transition = session.on_event(
            StreamDataEvent{stream_id, {std::byte{0x10}}, false});
        EXPECT_TRUE(transition.actions.empty());
        const auto evidence = session.take_evidence(32);
        ASSERT_EQ(evidence.size(), 1u);
        EXPECT_EQ(evidence.front().kind,
                  EvidenceKind::LocalObservationError);
    }
}

TEST(Draft18Session, ControlFinAndResetCloseWithProtocolViolation) {
    {
        PublisherSession session;
        establish_with_local_setup(session);
        const auto transition = session.on_event(
            StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, true});
        ASSERT_NE(close_action(transition), nullptr);
        EXPECT_EQ(close_action(transition)->application_error, 0x3u);
    }
    {
        PublisherSession session;
        establish_with_local_setup(session);
        session.on_event(
            StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
        const auto transition = session.on_event(transport::PeerResetEvent{2, 9});
        ASSERT_NE(close_action(transition), nullptr);
        EXPECT_EQ(close_action(transition)->application_error, 0x3u);
    }
}

TEST(Draft18Session, SetupOptionsAreOwnedAndKnownDuplicatesAreEvidence) {
    PublisherSession session;
    establish_with_local_setup(session);
    session.take_evidence(32);
    const std::vector<std::byte> setup{
        std::byte{0xaf}, std::byte{0x00}, std::byte{0x00}, std::byte{0x0a},
        std::byte{0x04}, std::byte{0x01},
        std::byte{0x00}, std::byte{0x02},
        std::byte{0x05}, std::byte{0x01}, std::byte{0xaa},
        std::byte{0x00}, std::byte{0x01}, std::byte{0xbb}};
    const auto transition = session.on_event(StreamDataEvent{2, setup, false});
    EXPECT_TRUE(transition.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
    const auto evidence = session.take_evidence(32);
    const auto setup_event = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::PeerSetupReceived;
        });
    ASSERT_NE(setup_event, evidence.end());
    const auto& owned = std::get<SetupEvidence>(setup_event->data).setup.options;
    ASSERT_EQ(owned.size(), 4u);
    EXPECT_EQ(owned[2].type, 9u);
    EXPECT_EQ(owned[3].type, 9u);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind ==
                                       EvidenceKind::SetupOptionDuplicate;
                            }),
              1);
}

TEST(Draft18Session, CoalescedPostSetupControlFrameIsDeferred) {
    PublisherSession session;
    establish_with_local_setup(session);
    std::vector<std::byte> bytes(kSetup.begin(), kSetup.end());
    const std::array goaway{std::byte{0x10}, std::byte{0x00}, std::byte{0x03},
                            std::byte{0x00}, std::byte{0x00}, std::byte{0x02}};
    bytes.insert(bytes.end(), goaway.begin(), goaway.end());
    const auto transition = session.on_event(StreamDataEvent{2, bytes, false});
    EXPECT_TRUE(transition.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
    const auto evidence = session.take_evidence(32);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& event) {
                               return event.kind ==
                                      EvidenceKind::DeferredStreamBytes;
                           }),
              evidence.end());
}

TEST(Draft18Session, LocalStreamPurposesRequireExactPhysicalRoles) {
    PublisherSession session;
    session.on_event(established());
    session.take_evidence(32);
    session.observe_local_stream(1, LocalStreamPurpose::Control);
    session.observe_local_stream(3, LocalStreamPurpose::Request);
    session.observe_local_stream(1, LocalStreamPurpose::Data);
    const auto evidence = session.take_evidence(32);
    ASSERT_EQ(evidence.size(), 3u);
    for (const auto& event : evidence) {
        EXPECT_EQ(event.kind, EvidenceKind::LocalObservationError);
    }
}

TEST(Draft18Session, EarlyStreamCountAndByteLimitsAreIndependent) {
    {
        PublisherSessionConfig config;
        config.maximum_early_streams = 1;
        PublisherSession session(config);
        session.on_event(established());
        session.on_event(StreamDataEvent{0, {}, false});
        const auto transition = session.on_event(StreamDataEvent{4, {}, false});
        ASSERT_NE(close_action(transition), nullptr);
        const auto evidence = session.take_evidence(32);
        EXPECT_EQ(evidence.back().kind, EvidenceKind::HarnessLimit);
        EXPECT_EQ(std::get<HarnessLimitEvidence>(evidence.back().data).limit,
                  HarnessLimitKind::EarlyStreams);
    }
    {
        PublisherSessionConfig config;
        config.maximum_early_bytes = 1;
        PublisherSession session(config);
        session.on_event(established());
        session.on_event(StreamDataEvent{2, {std::byte{0x10}}, false});
        const auto transition =
            session.on_event(StreamDataEvent{6, {std::byte{0x10}}, false});
        ASSERT_NE(close_action(transition), nullptr);
        const auto evidence = session.take_evidence(32);
        EXPECT_EQ(std::get<HarnessLimitEvidence>(evidence.back().data).limit,
                  HarnessLimitKind::EarlyBytes);
    }
}

TEST(Draft18Session, PartialBufferLimitsAreIndependent) {
    {
        PublisherSessionConfig config;
        config.maximum_partial_bytes_per_stream = 1;
        PublisherSession session(config);
        session.on_event(established());
        const auto transition = session.on_event(
            StreamDataEvent{0, {std::byte{0x03}, std::byte{0x00}}, false});
        ASSERT_NE(close_action(transition), nullptr);
        const auto evidence = session.take_evidence(32);
        EXPECT_EQ(std::get<HarnessLimitEvidence>(evidence.back().data).limit,
                  HarnessLimitKind::PartialStreamBytes);
    }
    {
        PublisherSessionConfig config;
        config.maximum_partial_bytes_per_session = 1;
        PublisherSession session(config);
        session.on_event(established());
        session.on_event(StreamDataEvent{0, {std::byte{0x03}}, false});
        const auto transition =
            session.on_event(StreamDataEvent{4, {std::byte{0x03}}, false});
        ASSERT_NE(close_action(transition), nullptr);
        const auto evidence = session.take_evidence(32);
        EXPECT_EQ(std::get<HarnessLimitEvidence>(evidence.back().data).limit,
                  HarnessLimitKind::PartialSessionBytes);
    }
}

TEST(Draft18Session, SetupOnBidirectionalRequestStreamClosesSession) {
    PublisherSession session;
    establish_with_local_setup(session);
    const auto transition = session.on_event(
        StreamDataEvent{0, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_NE(close_action(transition), nullptr);
    EXPECT_EQ(close_action(transition)->application_error, 0x3u);
}

TEST(Draft18Session, EstablishmentOrderingErrorsAreLocalEvidence) {
    PublisherSession session;
    session.on_event(StreamDataEvent{0, {}, false});
    session.on_event(established());
    session.on_event(established());
    const auto evidence = session.take_evidence(32);
    ASSERT_EQ(evidence.size(), 3u);
    EXPECT_EQ(evidence[0].kind, EvidenceKind::LocalObservationError);
    EXPECT_EQ(evidence[1].kind, EvidenceKind::TransportEstablished);
    EXPECT_EQ(evidence[2].kind, EvidenceKind::LocalObservationError);
    EXPECT_EQ(session.phase(), SessionPhase::AwaitingSetup);
}

TEST(Draft18Session, PeerStopAndNonControlResetRemainDistinctAndReleaseState) {
    PublisherSessionConfig config;
    config.maximum_partial_bytes_per_session = 1;
    PublisherSession session(config);
    session.on_event(established());
    session.on_event(StreamDataEvent{0, {std::byte{0x03}}, false});
    session.on_event(transport::PeerStopSendingEvent{0, 7});
    session.on_event(transport::PeerResetEvent{0, 8});
    const auto transition =
        session.on_event(StreamDataEvent{4, {std::byte{0x03}}, false});
    EXPECT_TRUE(transition.actions.empty());
    EXPECT_NE(session.phase(), SessionPhase::Closing);
    const auto evidence = session.take_evidence(32);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& event) {
                               return event.kind == EvidenceKind::PeerStopSending;
                           }),
              evidence.end());
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& event) {
                               return event.kind == EvidenceKind::PeerReset;
                           }),
              evidence.end());
}

TEST(Draft18Session, EvidenceCountLimitHasReservedTerminalSlotAndDrainsSafely) {
    PublisherSessionConfig config;
    config.maximum_evidence_count = 1;
    PublisherSession session(config);
    session.on_event(established());
    const auto transition =
        session.observe_local_stream(3, LocalStreamPurpose::Control);
    ASSERT_NE(close_action(transition), nullptr);
    auto first = session.take_evidence(1);
    auto second = session.take_evidence(1);
    ASSERT_EQ(first.size(), 1u);
    ASSERT_EQ(second.size(), 1u);
    EXPECT_EQ(first.front().kind, EvidenceKind::TransportEstablished);
    EXPECT_EQ(second.front().kind, EvidenceKind::HarnessLimit);
    EXPECT_TRUE(session.take_evidence(1).empty());
}

TEST(Draft18Session, EvidenceByteLimitIsHarnessFailureNotPeerFailure) {
    PublisherSessionConfig config;
    config.maximum_evidence_bytes = 1;
    PublisherSession session(config);
    const auto transition = session.on_event(established());
    ASSERT_NE(close_action(transition), nullptr);
    EXPECT_EQ(close_action(transition)->application_error, 0x1u);
    const auto evidence = session.take_evidence(32);
    ASSERT_EQ(evidence.size(), 1u);
    EXPECT_EQ(evidence.front().kind, EvidenceKind::HarnessLimit);
    EXPECT_EQ(std::get<HarnessLimitEvidence>(evidence.front().data).limit,
              HarnessLimitKind::EvidenceBytes);
}

TEST(Draft18Session, TerminalCloseReasonIsBoundedByEvidenceByteLimit) {
    PublisherSessionConfig config;
    config.maximum_evidence_bytes = 4;
    PublisherSession session(config);
    std::vector<std::byte> reason(32);
    for (std::size_t index = 0; index < reason.size(); ++index) {
        reason[index] = static_cast<std::byte>(index);
    }
    session.on_event(transport::PeerCloseEvent{
        transport::CloseErrorSpace::Application, 77, reason});
    const auto evidence = session.take_evidence(32);
    ASSERT_EQ(evidence.size(), 1u);
    EXPECT_EQ(evidence.front().kind, EvidenceKind::PeerClose);
    const auto& close = std::get<CloseEvidence>(evidence.front().data);
    EXPECT_EQ(close.error_space, transport::CloseErrorSpace::Application);
    EXPECT_EQ(close.error_code, 77u);
    EXPECT_EQ(close.reason.size(), config.maximum_evidence_bytes);
    EXPECT_TRUE(std::equal(close.reason.begin(), close.reason.end(),
                           reason.begin()));
    EXPECT_EQ(session.phase(), SessionPhase::Closed);
}

TEST(Draft18Session, TransportTerminalKindsAreDistinctAndAbsorbing) {
    const std::vector<std::pair<transport::TransportEvent, EvidenceKind>> cases{
        {transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 4,
                                   {std::byte{'x'}}},
         EvidenceKind::PeerClose},
        {transport::LocalCloseEvent{transport::CloseErrorSpace::Transport, 5,
                                    {std::byte{'y'}}},
         EvidenceKind::LocalClose},
        {transport::IdleTimeoutEvent{}, EvidenceKind::IdleTimeout},
        {transport::TransportErrorEvent{
             transport::TransportError::ProtocolFailure},
         EvidenceKind::TransportError},
        {transport::EventQueueOverflowEvent{},
         EvidenceKind::TransportEventOverflow},
    };
    for (const auto& [terminal_event, kind] : cases) {
        PublisherSession session;
        session.on_event(terminal_event);
        EXPECT_EQ(session.phase(), SessionPhase::Closed);
        EXPECT_TRUE(session.on_event(transport::IdleTimeoutEvent{}).actions.empty());
        const auto evidence = session.take_evidence(32);
        ASSERT_EQ(evidence.size(), 1u);
        EXPECT_EQ(evidence.front().kind, kind);
    }
}

TEST(Draft18Session, ProtocolTerminalEvidenceAndActionAreExactOnce) {
    PublisherSession session;
    establish_with_local_setup(session);
    session.take_evidence(32);
    const auto first =
        session.on_event(StreamDataEvent{2, {std::byte{0x01}}, false});
    ASSERT_NE(close_action(first), nullptr);
    EXPECT_TRUE(session.on_event(transport::IdleTimeoutEvent{}).actions.empty());
    const auto evidence = session.take_evidence(32);
    ASSERT_EQ(evidence.size(), 1u);
    EXPECT_EQ(evidence.front().kind, EvidenceKind::ProtocolViolation);
    EXPECT_TRUE(session.take_evidence(32).empty());
}

TEST(Draft18Session, PeerPhysicalStreamsClassifyToAllDraftKinds) {
    const std::vector<std::pair<std::uint64_t, PeerStreamKind>> types{
        {0x10, PeerStreamKind::Subgroup},
        {0x05, PeerStreamKind::Fetch},
        {0x132b3e28, PeerStreamKind::Padding},
    };
    for (const auto& [type, expected] : types) {
        PublisherSession session;
        session.on_event(established());
        wire::ByteWriter encoded(16);
        ASSERT_TRUE(wire::write_vi64(type, encoded));
        session.on_event(StreamDataEvent{
            2, {encoded.bytes().begin(), encoded.bytes().end()}, false});
        const auto evidence = session.take_evidence(32);
        const auto classified = std::find_if(
            evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
                return event.kind == EvidenceKind::PeerStreamClassified;
            });
        ASSERT_NE(classified, evidence.end());
        EXPECT_EQ(std::get<StreamEvidence>(classified->data).stream_kind,
                  expected);
    }

    PublisherSession request;
    request.on_event(established());
    request.on_event(StreamDataEvent{0, {}, false});
    const auto request_evidence = request.take_evidence(32);
    EXPECT_NE(std::find_if(request_evidence.begin(), request_evidence.end(),
                           [](const EvidenceEvent& event) {
                               const auto* stream =
                                   std::get_if<StreamEvidence>(&event.data);
                               return stream != nullptr &&
                                      stream->stream_kind ==
                                          PeerStreamKind::Request;
                           }),
              request_evidence.end());
}

TEST(Draft18Session, ControlSetupDoesNotConsumeEarlyStreamOrByteQuota) {
    PublisherSessionConfig config;
    config.maximum_early_streams = 1;
    config.maximum_early_bytes = 1;
    PublisherSession session(config);
    establish_with_local_setup(session);
    const auto transition = session.on_event(
        StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    EXPECT_TRUE(transition.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18Session, ActiveStreamLimitIsCheckedBeforeNewStreamState) {
    PublisherSessionConfig config;
    config.maximum_active_streams = 1;
    config.maximum_early_streams = 2;
    PublisherSession session(config);
    session.on_event(established());
    session.on_event(StreamDataEvent{0, {}, false});
    const auto transition = session.on_event(StreamDataEvent{4, {}, false});
    ASSERT_NE(close_action(transition), nullptr);
    const auto evidence = session.take_evidence(32);
    EXPECT_EQ(std::get<HarnessLimitEvidence>(evidence.back().data).limit,
              HarnessLimitKind::ActiveStreams);
}

TEST(Draft18Session, EvidenceSequencesRemainContiguousAcrossBoundedDrains) {
    PublisherSession session;
    session.on_event(established());
    auto first = session.take_evidence(1);
    ASSERT_EQ(first.size(), 1u);
    EXPECT_EQ(first.front().sequence, 0u);
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(3, SetupMessage{}, false);
    auto second = session.take_evidence(1);
    auto third = session.take_evidence(1);
    ASSERT_EQ(second.size(), 1u);
    ASSERT_EQ(third.size(), 1u);
    EXPECT_EQ(second.front().sequence, 1u);
    EXPECT_EQ(third.front().sequence, 2u);
}

}  // namespace
}  // namespace moq::interop::session
