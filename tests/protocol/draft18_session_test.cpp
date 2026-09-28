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
using wire::draft18::Message;

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

void activate(PublisherSession& session) {
    establish_with_local_setup(session);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(128);
}

void activate_with_empty_transport_evidence(PublisherSession& session) {
    session.on_event(ConnectionEstablishedEvent{{}, {}, {}, 1200});
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(3, SetupMessage{}, false);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(128);
}

void activate_with_small_evidence_queue(PublisherSession& session) {
    session.on_event(established());
    session.take_evidence(16);
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(3, SetupMessage{}, false);
    session.take_evidence(16);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(16);
}

std::vector<std::byte> encode(const Message& message) {
    wire::ByteWriter output(65'546);
    EXPECT_TRUE(wire::draft18::encode_message(message, output).has_value());
    return {output.bytes().begin(), output.bytes().end()};
}

std::vector<Message> opening_requests(std::uint64_t first_id) {
    using namespace wire::draft18;
    return {
        SubscribeMessage{first_id, {}, {}, {}},
        PublishMessage{first_id + 2, {}, {}, 7, {}, {}},
        FetchMessage{first_id + 4,
                     StandaloneFetch{{}, {}, {0, 0}, {0, 0}}, {}},
        TrackStatusMessage{first_id + 6, {}, {}, {}},
        PublishNamespaceMessage{first_id + 8, {}, {}},
        SubscribeNamespaceMessage{first_id + 10, {}, {}},
        SubscribeTracksMessage{first_id + 12, {}, {}},
    };
}

TEST(Draft18SessionRequests, ObservesAllSevenPeerAndLocalOpeningRequests) {
    PublisherSession peer_session;
    activate(peer_session);
    const auto peer_messages = opening_requests(0);
    for (std::size_t index = 0; index < peer_messages.size(); ++index) {
        const auto frame = encode(peer_messages[index]);
        peer_session.on_event(StreamDataEvent{
            static_cast<transport::StreamId>(index * 4), frame, false});
    }
    const auto peer_evidence = peer_session.take_evidence(128);
    EXPECT_EQ(std::count_if(peer_evidence.begin(), peer_evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestObserved;
                            }),
              7);
    for (std::size_t index = 0; index < peer_messages.size(); ++index) {
        const auto& observed = std::get<RequestObservedEvidence>(
            peer_evidence[index * 2 + 1].data);
        EXPECT_EQ(observed.initiator, RequestInitiator::Peer);
        EXPECT_EQ(observed.request_id, index * 2);
        EXPECT_EQ(observed.stream_id, index * 4);
    }

    PublisherSession local_session;
    activate(local_session);
    const auto local_messages = opening_requests(1);
    for (std::size_t index = 0; index < local_messages.size(); ++index) {
        const auto stream_id = static_cast<transport::StreamId>(index * 4 + 1);
        local_session.observe_local_stream(stream_id,
                                           LocalStreamPurpose::Request);
        local_session.observe_local_message(stream_id, local_messages[index],
                                            false);
    }
    const auto local_evidence = local_session.take_evidence(128);
    EXPECT_EQ(std::count_if(local_evidence.begin(), local_evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestObserved;
                            }),
              7);
}

TEST(Draft18SessionRequests, EnforcesGlobalRequestIdParityReuseAndTracksGaps) {
    PublisherSession wrong_parity;
    activate(wrong_parity);
    const auto odd_peer = encode(opening_requests(1).front());
    const auto parity_transition = wrong_parity.on_event(
        StreamDataEvent{0, odd_peer, false});
    ASSERT_NE(close_action(parity_transition), nullptr);
    EXPECT_EQ(close_action(parity_transition)->application_error, 0x4u);

    PublisherSession reuse;
    activate(reuse);
    const auto first = encode(opening_requests(0).front());
    reuse.on_event(StreamDataEvent{0, first, false});
    const auto reuse_transition = reuse.on_event(StreamDataEvent{4, first, false});
    ASSERT_NE(close_action(reuse_transition), nullptr);
    EXPECT_EQ(close_action(reuse_transition)->application_error, 0x4u);

    PublisherSession gap;
    activate(gap);
    const auto gap_frame = encode(opening_requests(4).front());
    const auto gap_transition = gap.on_event(StreamDataEvent{0, gap_frame, false});
    EXPECT_EQ(close_action(gap_transition), nullptr);
    const auto gap_evidence = gap.take_evidence(32);
    const auto gap_event = std::find_if(
        gap_evidence.begin(), gap_evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::RequestIdSequenceViolation;
        });
    ASSERT_NE(gap_event, gap_evidence.end());
    EXPECT_EQ(std::get<RequestIdSequenceEvidence>(gap_event->data).expected, 0u);
    EXPECT_EQ(std::get<RequestIdSequenceEvidence>(gap_event->data).observed, 4u);

    PublisherSession maximum;
    activate(maximum);
    const auto maximum_id = (std::uint64_t{1} << 62) - 2;
    auto maximum_message = opening_requests(maximum_id).front();
    const auto maximum_transition = maximum.on_event(
        StreamDataEvent{0, encode(maximum_message), false});
    EXPECT_EQ(close_action(maximum_transition), nullptr);
}

TEST(Draft18SessionRequests, RejectsIllegalFirstMessageAndNeedMoreIsSemanticNoop) {
    PublisherSession illegal;
    activate(illegal);
    const auto response = encode(wire::draft18::RequestOkMessage{});
    const auto illegal_transition = illegal.on_event(
        StreamDataEvent{0, response, false});
    ASSERT_NE(close_action(illegal_transition), nullptr);
    EXPECT_EQ(close_action(illegal_transition)->application_error, 0x3u);

    const auto request = encode(opening_requests(0).front());
    for (std::size_t split = 0; split < request.size(); ++split) {
        PublisherSession session;
        activate(session);
        session.on_event(StreamDataEvent{
            0, {request.begin(), request.begin() +
                                     static_cast<std::ptrdiff_t>(split)}, false});
        const auto before = session.take_evidence(32);
        EXPECT_EQ(std::count_if(before.begin(), before.end(),
                                [](const EvidenceEvent& event) {
                                    return event.kind == EvidenceKind::RequestObserved;
                                }),
                  0) << split;
        session.on_event(StreamDataEvent{
            0, {request.begin() + static_cast<std::ptrdiff_t>(split),
                request.end()}, false});
        const auto after = session.take_evidence(32);
        EXPECT_EQ(std::count_if(after.begin(), after.end(),
                                [](const EvidenceEvent& event) {
                                    return event.kind == EvidenceKind::RequestObserved;
                                }),
                  1) << split;
    }
}

Message successful_response(RequestKind kind) {
    using namespace wire::draft18;
    if (kind == RequestKind::Subscribe) {
        return SubscribeOkMessage{7, {}, {}};
    }
    if (kind == RequestKind::Fetch) {
        return FetchOkMessage{0, {0, 0}, {}, {}};
    }
    return RequestOkMessage{};
}

TEST(Draft18SessionRequests, CorrelatesEveryInitialResponseFamily) {
    const auto requests = opening_requests(1);
    const std::array kinds{
        RequestKind::Subscribe, RequestKind::Publish, RequestKind::Fetch,
        RequestKind::TrackStatus, RequestKind::PublishNamespace,
        RequestKind::SubscribeNamespace, RequestKind::SubscribeTracks};
    for (std::size_t index = 0; index < requests.size(); ++index) {
        PublisherSession session;
        activate(session);
        const auto stream_id = static_cast<transport::StreamId>(index * 4 + 1);
        session.observe_local_stream(stream_id, LocalStreamPurpose::Request);
        session.observe_local_message(stream_id, requests[index], false);
        session.take_evidence(32);
        const auto response = encode(successful_response(kinds[index]));
        const auto transition = session.on_event(
            StreamDataEvent{stream_id, response, false});
        EXPECT_EQ(close_action(transition), nullptr) << index;
        const auto evidence = session.take_evidence(32);
        ASSERT_EQ(evidence.size(), 1u) << index;
        EXPECT_EQ(evidence.front().kind, EvidenceKind::InitialResponseObserved);
        const auto& observed =
            std::get<InitialResponseEvidence>(evidence.front().data);
        EXPECT_EQ(observed.original_request_id, index * 2 + 1);
        EXPECT_EQ(observed.request_kind, kinds[index]);
    }
}

TEST(Draft18SessionRequests, WrongOrDuplicateInitialResponseIsTypedEvidence) {
    PublisherSession wrong;
    activate(wrong);
    wrong.observe_local_stream(1, LocalStreamPurpose::Request);
    wrong.observe_local_message(1, opening_requests(1).front(), false);
    const auto wrong_transition = wrong.on_event(
        StreamDataEvent{1, encode(wire::draft18::RequestOkMessage{}), false});
    EXPECT_EQ(close_action(wrong_transition), nullptr);

    PublisherSession duplicate;
    activate(duplicate);
    duplicate.observe_local_stream(1, LocalStreamPurpose::Request);
    duplicate.observe_local_message(1, opening_requests(1).front(), false);
    const auto response = encode(wire::draft18::SubscribeOkMessage{7, {}, {}});
    duplicate.on_event(StreamDataEvent{1, response, false});
    const auto duplicate_transition = duplicate.on_event(
        StreamDataEvent{1, response, false});
    EXPECT_EQ(close_action(duplicate_transition), nullptr);
    const auto evidence = duplicate.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::ResponseViolation;
                            }),
              1);
}

TEST(Draft18SessionRequests, NamespaceWrongFirstResponseMandatesClose) {
    const auto requests = opening_requests(1);
    for (const auto index : {std::size_t{5}, std::size_t{6}}) {
        PublisherSession session;
        activate(session);
        session.observe_local_stream(1, LocalStreamPurpose::Request);
        session.observe_local_message(1, requests[index], false);
        const auto transition = session.on_event(StreamDataEvent{
            1,
            encode(wire::draft18::SubscribeOkMessage{7, {}, {}}),
            false});
        ASSERT_NE(close_action(transition), nullptr) << index;
        EXPECT_EQ(close_action(transition)->application_error, 0x3u)
            << index;
    }
}

TEST(Draft18SessionRequests, ExhaustionCannotSuppressNamespaceMandatoryClose) {
    PublisherSessionConfig config;
    config.maximum_evidence_count = 2;
    PublisherSession session(config);
    activate_with_small_evidence_queue(session);
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    session.observe_local_message(
        1, wire::draft18::SubscribeNamespaceMessage{1, {}, {}}, false);
    const auto transition = session.on_event(StreamDataEvent{
        1, encode(wire::draft18::SubscribeOkMessage{7, {}, {}}), false});
    ASSERT_NE(close_action(transition), nullptr);
    EXPECT_EQ(close_action(transition)->application_error, 0x3u);
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::ProtocolViolation;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::HarnessLimit;
                            }),
              0);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              1);
}

TEST(Draft18SessionRequests, CorrelatesUpdatesInOrderAndPreservesErrorAmbiguity) {
    PublisherSession fifo;
    activate(fifo);
    const auto request = encode(opening_requests(0).front());
    fifo.on_event(StreamDataEvent{0, request, false});
    fifo.observe_local_message(
        0, wire::draft18::SubscribeOkMessage{7, {}, {}}, false);
    std::vector<std::byte> updates =
        encode(wire::draft18::RequestUpdateMessage{2, {}});
    const auto second_update =
        encode(wire::draft18::RequestUpdateMessage{4, {}});
    updates.insert(updates.end(), second_update.begin(), second_update.end());
    fifo.on_event(StreamDataEvent{0, updates, false});
    fifo.observe_local_message(0, wire::draft18::RequestOkMessage{}, false);
    fifo.observe_local_message(0, wire::draft18::RequestOkMessage{}, false);
    const auto fifo_evidence = fifo.take_evidence(128);
    std::vector<std::uint64_t> resolved;
    for (const auto& event : fifo_evidence) {
        if (event.kind == EvidenceKind::UpdateResponseObserved) {
            resolved.push_back(std::get<UpdateResponseEvidence>(event.data)
                                   .update_request_id.value());
        }
    }
    EXPECT_EQ(resolved, (std::vector<std::uint64_t>{2, 4}));

    PublisherSession ambiguous;
    activate(ambiguous);
    ambiguous.on_event(StreamDataEvent{0, request, false});
    ambiguous.observe_local_message(
        0, wire::draft18::SubscribeOkMessage{7, {}, {}}, false);
    ambiguous.on_event(StreamDataEvent{0, updates, false});
    ambiguous.observe_local_message(
        0, wire::draft18::RequestErrorMessage{1, 0, {}, std::nullopt}, false);
    const auto ambiguous_evidence = ambiguous.take_evidence(128);
    const auto event = std::find_if(
        ambiguous_evidence.begin(), ambiguous_evidence.end(),
        [](const EvidenceEvent& value) {
            return value.kind == EvidenceKind::UpdateResponseObserved;
        });
    ASSERT_NE(event, ambiguous_evidence.end());
    const auto& response = std::get<UpdateResponseEvidence>(event->data);
    EXPECT_FALSE(response.update_request_id.has_value());
    EXPECT_EQ(response.candidate_update_ids,
              (std::vector<std::uint64_t>{2, 4}));
}

TEST(Draft18SessionRequests, UpdateErrorEntersFailedPhaseWithoutTerminalizing) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(SubscribeMessage{0, {}, {}, {}}), false});
    session.observe_local_message(0, SubscribeOkMessage{7, {}, {}}, false);
    session.on_event(
        StreamDataEvent{0, encode(RequestUpdateMessage{2, {}}), false});
    session.observe_local_message(
        0, RequestErrorMessage{1, 0, {}, std::nullopt}, false);
    session.on_event(
        StreamDataEvent{0, encode(RequestUpdateMessage{4, {}}), false});
    const auto evidence = session.take_evidence(128);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              0);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind ==
                                       EvidenceKind::RequestUpdateFailed;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind ==
                                       EvidenceKind::RequestStateViolation;
                            }),
              1);
}

TEST(Draft18SessionRequests, FailedPhaseStillOwnsValidUpdateIds) {
    using namespace wire::draft18;
    PublisherSession consumed;
    activate(consumed);
    consumed.on_event(StreamDataEvent{
        0, encode(SubscribeMessage{0, {}, {}, {}}), false});
    consumed.observe_local_message(0, SubscribeOkMessage{7, {}, {}}, false);
    consumed.on_event(
        StreamDataEvent{0, encode(RequestUpdateMessage{2, {}}), false});
    consumed.observe_local_message(
        0, RequestErrorMessage{1, 0, {}, std::nullopt}, false);
    consumed.on_event(
        StreamDataEvent{0, encode(RequestUpdateMessage{4, {}}), false});
    const auto reuse = consumed.on_event(StreamDataEvent{
        4, encode(SubscribeMessage{4, {}, {}, {}}), false});
    ASSERT_NE(close_action(reuse), nullptr);
    EXPECT_EQ(close_action(reuse)->application_error, 0x4u);

    PublisherSession parity;
    activate(parity);
    parity.on_event(StreamDataEvent{
        0, encode(SubscribeMessage{0, {}, {}, {}}), false});
    parity.observe_local_message(0, SubscribeOkMessage{7, {}, {}}, false);
    parity.on_event(
        StreamDataEvent{0, encode(RequestUpdateMessage{2, {}}), false});
    parity.observe_local_message(
        0, RequestErrorMessage{1, 0, {}, std::nullopt}, false);
    const auto invalid = parity.on_event(
        StreamDataEvent{0, encode(RequestUpdateMessage{3, {}}), false});
    ASSERT_NE(close_action(invalid), nullptr);
    EXPECT_EQ(close_action(invalid)->application_error, 0x4u);
}

TEST(Draft18SessionRequests, PrematureCrossPublishUpdateConsumesIdAsViolation) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {}, {}, 7, {}, {}}), false});
    session.observe_local_message(0, RequestUpdateMessage{1, {}}, false);
    session.observe_local_stream(5, LocalStreamPurpose::Request);
    session.observe_local_message(5, SubscribeMessage{1, {}, {}, {}}, false);
    const auto evidence = session.take_evidence(128);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind ==
                                       EvidenceKind::RequestStateViolation;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::UpdateObserved;
                            }),
              0);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind ==
                                       EvidenceKind::LocalObservationError;
                            }),
              1);
}

TEST(Draft18SessionRequests, PublishDoneRemainsLegalAfterFailedPublishUpdate) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {}, {}, 7, {}, {}}), false});
    session.observe_local_message(0, RequestOkMessage{}, false);
    session.observe_local_message(0, RequestUpdateMessage{1, {}}, false);
    session.on_event(StreamDataEvent{
        0, encode(RequestErrorMessage{1, 0, {}, std::nullopt}), false});
    const auto transition = session.on_event(StreamDataEvent{
        0, encode(PublishDoneMessage{5, 0, {}}), false});
    EXPECT_EQ(close_action(transition), nullptr);
    EXPECT_EQ(session.phase(), SessionPhase::Active);

    PublisherSession local;
    activate(local);
    local.observe_local_stream(1, LocalStreamPurpose::Request);
    local.observe_local_message(1, PublishMessage{1, {}, {}, 7, {}, {}},
                                false);
    local.on_event(StreamDataEvent{1, encode(RequestOkMessage{}), false});
    local.take_evidence(64);
    local.observe_local_message(1, PublishDoneMessage{0, 0, {}}, false);
    const auto local_evidence = local.take_evidence(64);
    ASSERT_EQ(local_evidence.size(), 1u);
    EXPECT_EQ(local_evidence.front().kind,
              EvidenceKind::RequestMessageObserved);
}

TEST(Draft18SessionRequests, ProtocolCloseCompactsEveryActiveRequest) {
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), false});
    session.on_event(StreamDataEvent{
        4, encode(opening_requests(2).front()), false});
    session.take_evidence(64);
    const auto transition = session.on_event(StreamDataEvent{
        0, encode(wire::draft18::SetupMessage{}), false});
    ASSERT_NE(close_action(transition), nullptr);
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              2);
    for (const auto& event : evidence) {
        if (event.kind == EvidenceKind::RequestTerminal) {
            EXPECT_EQ(std::get<RequestTerminalEvidence>(event.data).cause,
                      RequestTerminalCause::SessionClosed);
        }
    }
}

TEST(Draft18SessionRequests, ExhaustedEvidencePreservesProtocolTerminalKinds) {
    PublisherSessionConfig config;
    config.maximum_evidence_count = 4;
    PublisherSession session(config);
    activate_with_small_evidence_queue(session);
    session.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), false});
    session.on_event(StreamDataEvent{
        4, encode(opening_requests(2).front()), false});
    const auto transition = session.on_event(StreamDataEvent{
        0, encode(wire::draft18::SetupMessage{}), false});
    ASSERT_NE(close_action(transition), nullptr);
    EXPECT_EQ(close_action(transition)->application_error, 0x3u);
    const auto evidence = session.take_evidence(128);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              2);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::ProtocolViolation;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::HarnessLimit;
                            }),
              0);
}

TEST(Draft18SessionRequests, ExhaustedEvidencePreservesPeerCloseTerminalKinds) {
    PublisherSessionConfig config;
    config.maximum_evidence_count = 4;
    PublisherSession session(config);
    activate_with_small_evidence_queue(session);
    session.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), false});
    session.on_event(StreamDataEvent{
        4, encode(opening_requests(2).front()), false});
    session.on_event(transport::PeerCloseEvent{
        transport::CloseErrorSpace::Application, 9, {std::byte{0x61}}});
    const auto evidence = session.take_evidence(128);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              2);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::PeerClose;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::HarnessLimit;
                            }),
              0);
}

TEST(Draft18SessionRequests, ExhaustedEvidenceReservesFinTerminalBeforeHarnessClose) {
    PublisherSessionConfig config;
    config.maximum_evidence_count = 2;
    PublisherSession session(config);
    activate_with_small_evidence_queue(session);
    session.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), true});
    session.observe_local_fin(0);
    const auto evidence = session.take_evidence(128);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::HarnessLimit;
                            }),
              1);
}

TEST(Draft18SessionRequests, ReplaysRetainedPreSetupRequestAndRememberedFin) {
    PublisherSession session;
    establish_with_local_setup(session);
    session.take_evidence(32);
    session.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), true});
    EXPECT_EQ(session.phase(), SessionPhase::AwaitingSetup);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestObserved;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              0);
    session.observe_local_message(
        0, wire::draft18::SubscribeOkMessage{7, {}, {}}, true);
    evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              1);
}

TEST(Draft18SessionRequests, InvalidIdPrecedesRequestAndUpdateCapacity) {
    PublisherSessionConfig request_config;
    request_config.maximum_active_requests = 1;
    PublisherSession requests(request_config);
    activate(requests);
    requests.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), false});
    const auto request_transition = requests.on_event(StreamDataEvent{
        4, encode(opening_requests(1).front()), false});
    ASSERT_NE(close_action(request_transition), nullptr);
    EXPECT_EQ(close_action(request_transition)->application_error, 0x4u);

    PublisherSessionConfig update_config;
    update_config.maximum_outstanding_updates_per_request = 1;
    PublisherSession updates(update_config);
    activate(updates);
    updates.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), false});
    updates.on_event(StreamDataEvent{
        0, encode(wire::draft18::RequestUpdateMessage{2, {}}), false});
    const auto update_transition = updates.on_event(StreamDataEvent{
        0, encode(wire::draft18::RequestUpdateMessage{2, {}}), false});
    ASSERT_NE(close_action(update_transition), nullptr);
    EXPECT_EQ(close_action(update_transition)->application_error, 0x4u);
}

TEST(Draft18SessionRequests, ConfiguredWireLimitIsHarnessEvidence) {
    PublisherSessionConfig config;
    config.wire_limits.maximum_odd_value_length = 1;
    PublisherSession session(config);
    activate(session);
    wire::draft18::Token token;
    token.alias_type = wire::draft18::TokenAliasType::UseValue;
    token.token_type = 1;
    token.token_value.assign(32, std::byte{0x61});
    const auto transition = session.on_event(StreamDataEvent{
        0, encode(wire::draft18::SubscribeMessage{0, {}, {}, {{0x03, token}}}),
        false});
    ASSERT_NE(close_action(transition), nullptr);
    EXPECT_EQ(close_action(transition)->application_error, 0x1u);
    const auto evidence = session.take_evidence(64);
    const auto limit = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::HarnessLimit;
        });
    ASSERT_NE(limit, evidence.end());
    EXPECT_EQ(std::get<HarnessLimitEvidence>(limit->data).limit,
              HarnessLimitKind::PartialStreamBytes);
}

TEST(Draft18SessionRequests, EnforcesControlAndRequestGoawayContextAndCardinality) {
    using wire::draft18::GoawayMessage;
    PublisherSession control;
    activate(control);
    const auto control_goaway = encode(GoawayMessage{{}, 10, 1});
    const auto accepted = control.on_event(
        StreamDataEvent{2, control_goaway, false});
    EXPECT_EQ(close_action(accepted), nullptr);
    auto evidence = control.take_evidence(32);
    const auto observed = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::GoawayObserved;
        });
    ASSERT_NE(observed, evidence.end());
    EXPECT_EQ(std::get<GoawayEvidence>(observed->data).placement,
              GoawayPlacement::Control);
    const auto repeated = control.on_event(
        StreamDataEvent{2, control_goaway, false});
    ASSERT_NE(close_action(repeated), nullptr);
    EXPECT_EQ(close_action(repeated)->application_error, 0x3u);

    PublisherSession request;
    activate(request);
    request.on_event(StreamDataEvent{0, encode(opening_requests(0).front()),
                                     false});
    const auto request_goaway = encode(GoawayMessage{{}, 5, std::nullopt});
    EXPECT_EQ(close_action(request.on_event(
                  StreamDataEvent{0, request_goaway, false})),
              nullptr);
    const auto duplicate = request.on_event(
        StreamDataEvent{0, request_goaway, false});
    ASSERT_NE(close_action(duplicate), nullptr);
    EXPECT_EQ(close_action(duplicate)->application_error, 0x3u);

    PublisherSession wrong_cutoff;
    activate(wrong_cutoff);
    const auto parity = wrong_cutoff.on_event(StreamDataEvent{
        2, encode(GoawayMessage{{}, 0, 2}), false});
    ASSERT_NE(close_action(parity), nullptr);
    EXPECT_EQ(close_action(parity)->application_error, 0x4u);
}

TEST(Draft18SessionRequests, ReceivedControlGoawayCutsOffLocalRequests) {
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        2, encode(wire::draft18::GoawayMessage{{}, 0, 3}), false});
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    session.observe_local_message(1, opening_requests(1).front(), false);
    session.observe_local_stream(5, LocalStreamPurpose::Request);
    session.observe_local_message(5, opening_requests(3).front(), false);
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestObserved;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind ==
                                       EvidenceKind::LocalObservationError;
                            }),
              1);
}

TEST(Draft18SessionRequests, FinIsHalfCloseAndTerminalEvidenceIsExactOnce) {
    PublisherSession session;
    activate(session);
    const auto request = encode(opening_requests(0).front());
    session.on_event(StreamDataEvent{0, request, true});
    auto before = session.take_evidence(64);
    EXPECT_EQ(std::count_if(before.begin(), before.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              0);
    session.observe_local_message(
        0, wire::draft18::SubscribeOkMessage{7, {}, {}}, true);
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestTerminal;
                            }),
              1);
    const auto terminal = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::RequestTerminal;
        });
    ASSERT_NE(terminal, evidence.end());
    EXPECT_EQ(std::get<RequestTerminalEvidence>(terminal->data).cause,
              RequestTerminalCause::LocalFin);
}

TEST(Draft18SessionRequests, EitherSecondFinOrderingReleasesRequestStreamSlot) {
    PublisherSessionConfig config;
    config.maximum_active_streams = 2;

    PublisherSession peer_first(config);
    activate(peer_first);
    peer_first.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), true});
    peer_first.observe_local_fin(0);
    const auto peer_first_transition = peer_first.on_event(StreamDataEvent{
        4, encode(opening_requests(2).front()), false});
    EXPECT_EQ(close_action(peer_first_transition), nullptr);

    PublisherSession local_first(config);
    activate(local_first);
    local_first.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), false});
    local_first.observe_local_fin(0);
    local_first.on_event(StreamDataEvent{0, {}, true});
    const auto local_first_transition = local_first.on_event(StreamDataEvent{
        4, encode(opening_requests(2).front()), false});
    EXPECT_EQ(close_action(local_first_transition), nullptr);

    const auto peer_evidence = peer_first.take_evidence(128);
    const auto local_evidence = local_first.take_evidence(128);
    for (const auto* evidence : {&peer_evidence, &local_evidence}) {
        EXPECT_EQ(std::count_if(evidence->begin(), evidence->end(),
                                [](const EvidenceEvent& event) {
                                    return event.kind ==
                                           EvidenceKind::RequestTerminal;
                                }),
                  1);
        EXPECT_EQ(std::count_if(evidence->begin(), evidence->end(),
                                [](const EvidenceEvent& event) {
                                    return event.kind ==
                                           EvidenceKind::RequestObserved;
                                }),
                  2);
    }
}

TEST(Draft18SessionRequests, DeepOwnedEvidenceRespectsOneByteBudget) {
    using namespace wire::draft18;
    auto expect_limit = [](PublisherSession& session) {
        const auto evidence = session.take_evidence(128);
        const auto limit = std::find_if(
            evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
                return event.kind == EvidenceKind::HarnessLimit;
            });
        ASSERT_NE(limit, evidence.end());
        EXPECT_EQ(std::get<HarnessLimitEvidence>(limit->data).limit,
                  HarnessLimitKind::EvidenceBytes);
    };

    PublisherSessionConfig config;
    config.maximum_evidence_bytes = 1;

    PublisherSession request_session(config);
    activate_with_empty_transport_evidence(request_session);
    request_session.on_event(StreamDataEvent{
        0, encode(SubscribeMessage{0, {}, {{64, std::byte{0x61}}}, {}}),
        false});
    expect_limit(request_session);

    PublisherSession response_session(config);
    activate_with_empty_transport_evidence(response_session);
    response_session.observe_local_stream(1, LocalStreamPurpose::Request);
    response_session.observe_local_message(1, opening_requests(1).front(),
                                            false);
    response_session.take_evidence(32);
    response_session.on_event(StreamDataEvent{
        1, encode(SubscribeOkMessage{
               1, {}, {{{1, ByteValue{{64, std::byte{0x62}}}}}}}),
        false});
    expect_limit(response_session);

    PublisherSession update_session(config);
    activate_with_empty_transport_evidence(update_session);
    update_session.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), false});
    update_session.take_evidence(32);
    Token token;
    token.alias_type = TokenAliasType::UseValue;
    token.token_type = 1;
    token.token_value.assign(64, std::byte{0x63});
    update_session.on_event(StreamDataEvent{
        0, encode(RequestUpdateMessage{2, {{0x03, token}}}), false});
    expect_limit(update_session);

    PublisherSession goaway_session(config);
    activate_with_empty_transport_evidence(goaway_session);
    goaway_session.observe_local_message(
        3, GoawayMessage{{64, std::byte{0x64}}, 0, 0}, false);
    expect_limit(goaway_session);

    PublisherSession candidates_session(config);
    activate_with_empty_transport_evidence(candidates_session);
    candidates_session.on_event(StreamDataEvent{
        0, encode(SubscribeMessage{0, {}, {}, {}}), false});
    candidates_session.observe_local_message(
        0, SubscribeOkMessage{7, {}, {}}, false);
    candidates_session.on_event(
        StreamDataEvent{0, encode(RequestUpdateMessage{2, {}}), false});
    candidates_session.on_event(
        StreamDataEvent{0, encode(RequestUpdateMessage{4, {}}), false});
    candidates_session.observe_local_message(
        0, RequestErrorMessage{1, 0, {}, std::nullopt}, false);
    expect_limit(candidates_session);
}

TEST(Draft18SessionRequests, RequestHistoryHasAnIndependentBound) {
    PublisherSessionConfig config;
    config.maximum_request_history = 1;
    PublisherSession session(config);
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), true});
    session.observe_local_message(
        0, wire::draft18::SubscribeOkMessage{7, {}, {}}, true);
    session.on_event(StreamDataEvent{
        4, encode(opening_requests(2).front()), false});
    const auto evidence = session.take_evidence(128);
    const auto limit = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::HarnessLimit;
        });
    ASSERT_NE(limit, evidence.end());
    EXPECT_EQ(std::get<HarnessLimitEvidence>(limit->data).limit,
              HarnessLimitKind::RequestHistory);
}

TEST(Draft18SessionRequests, LocalStreamHistoryHasAnIndependentBound) {
    PublisherSessionConfig config;
    config.maximum_local_stream_history = 2;
    PublisherSession session(config);
    activate(session);
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    session.observe_local_stream(5, LocalStreamPurpose::Request);
    const auto evidence = session.take_evidence(64);
    const auto limit = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::HarnessLimit;
        });
    ASSERT_NE(limit, evidence.end());
    EXPECT_EQ(std::get<HarnessLimitEvidence>(limit->data).limit,
              HarnessLimitKind::LocalStreamHistory);
}

TEST(Draft18SessionRequests, RequestGoawayCardinalityIsPerDirection) {
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), false});
    const wire::draft18::GoawayMessage goaway{{}, 0, std::nullopt};
    EXPECT_EQ(close_action(session.on_event(
                  StreamDataEvent{0, encode(goaway), false})),
              nullptr);
    EXPECT_TRUE(session.observe_local_message(0, goaway, false).actions.empty());
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::GoawayObserved;
                            }),
              2);
    const auto repeated = session.on_event(
        StreamDataEvent{0, encode(goaway), false});
    ASSERT_NE(close_action(repeated), nullptr);
}

TEST(Draft18SessionRequests, PublishAllowsSubscriberInitiatedUpdate) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {}, {}, 7, {}, {}}), false});
    session.observe_local_message(0, RequestOkMessage{}, false);
    session.observe_local_message(0, RequestUpdateMessage{1, {}}, false);
    const auto response = session.on_event(
        StreamDataEvent{0, encode(RequestOkMessage{}), false});
    EXPECT_EQ(close_action(response), nullptr);
    const auto evidence = session.take_evidence(128);
    const auto update = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::UpdateObserved;
        });
    ASSERT_NE(update, evidence.end());
    EXPECT_EQ(std::get<UpdateObservedEvidence>(update->data).initiator,
              RequestInitiator::Local);
    const auto resolved = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::UpdateResponseObserved;
        });
    ASSERT_NE(resolved, evidence.end());
    EXPECT_EQ(std::get<UpdateResponseEvidence>(resolved->data).responder,
              RequestInitiator::Peer);
}

TEST(Draft18SessionRequests, LocalStopSendingIsDistinctAndExactOnce) {
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(opening_requests(0).front()), false});
    session.observe_local_stop_sending(0, 4);
    session.observe_local_stop_sending(0, 4);
    const auto evidence = session.take_evidence(64);
    const auto terminals = std::count_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::RequestTerminal;
        });
    EXPECT_EQ(terminals, 1);
    const auto terminal = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::RequestTerminal;
        });
    ASSERT_NE(terminal, evidence.end());
    EXPECT_EQ(std::get<RequestTerminalEvidence>(terminal->data).cause,
              RequestTerminalCause::LocalStopSending);
}

TEST(Draft18SessionRequests, CoalescedAndSplitUpdatesMutateOnlyOnCompleteFrames) {
    const auto request = encode(opening_requests(0).front());
    const auto update = encode(wire::draft18::RequestUpdateMessage{2, {}});
    for (std::size_t split = 0; split < update.size(); ++split) {
        PublisherSession session;
        activate(session);
        session.on_event(StreamDataEvent{0, request, false});
        session.take_evidence(32);
        session.on_event(StreamDataEvent{
            0, {update.begin(), update.begin() +
                                    static_cast<std::ptrdiff_t>(split)}, false});
        EXPECT_TRUE(session.take_evidence(32).empty()) << split;
        session.on_event(StreamDataEvent{
            0, {update.begin() + static_cast<std::ptrdiff_t>(split),
                update.end()}, false});
        const auto evidence = session.take_evidence(32);
        ASSERT_EQ(evidence.size(), 1u) << split;
        EXPECT_EQ(evidence.front().kind, EvidenceKind::UpdateObserved);
    }

    PublisherSession coalesced;
    activate(coalesced);
    std::vector<std::byte> frames = request;
    frames.insert(frames.end(), update.begin(), update.end());
    coalesced.on_event(StreamDataEvent{0, frames, false});
    const auto evidence = coalesced.take_evidence(32);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::RequestObserved ||
                                       event.kind == EvidenceKind::UpdateObserved;
                            }),
              2);
}

TEST(Draft18SessionRequests, EnforcesIndependentRequestAndUpdateBounds) {
    PublisherSessionConfig request_config;
    request_config.maximum_active_requests = 1;
    PublisherSession requests(request_config);
    activate(requests);
    requests.on_event(
        StreamDataEvent{0, encode(opening_requests(0).front()), false});
    requests.on_event(
        StreamDataEvent{4, encode(opening_requests(2).front()), false});
    auto evidence = requests.take_evidence(64);
    const auto request_limit = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::HarnessLimit;
        });
    ASSERT_NE(request_limit, evidence.end());
    EXPECT_EQ(std::get<HarnessLimitEvidence>(request_limit->data).limit,
              HarnessLimitKind::ActiveRequests);

    PublisherSessionConfig update_config;
    update_config.maximum_outstanding_updates_per_request = 1;
    PublisherSession updates(update_config);
    activate(updates);
    updates.on_event(
        StreamDataEvent{0, encode(opening_requests(0).front()), false});
    updates.observe_local_message(
        0, wire::draft18::SubscribeOkMessage{7, {}, {}}, false);
    updates.on_event(StreamDataEvent{
        0, encode(wire::draft18::RequestUpdateMessage{2, {}}), false});
    updates.on_event(StreamDataEvent{
        0, encode(wire::draft18::RequestUpdateMessage{4, {}}), false});
    evidence = updates.take_evidence(64);
    const auto update_limit = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::HarnessLimit;
        });
    ASSERT_NE(update_limit, evidence.end());
    EXPECT_EQ(std::get<HarnessLimitEvidence>(update_limit->data).limit,
              HarnessLimitKind::OutstandingUpdates);
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

TEST(Draft18Session, CoalescedPostSetupGoawayIsObserved) {
    PublisherSession session;
    establish_with_local_setup(session);
    std::vector<std::byte> bytes(kSetup.begin(), kSetup.end());
    const std::array goaway{std::byte{0x10}, std::byte{0x00}, std::byte{0x03},
                            std::byte{0x00}, std::byte{0x00}, std::byte{0x01}};
    bytes.insert(bytes.end(), goaway.begin(), goaway.end());
    const auto transition = session.on_event(StreamDataEvent{2, bytes, false});
    EXPECT_TRUE(transition.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
    const auto evidence = session.take_evidence(32);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& event) {
                               return event.kind == EvidenceKind::GoawayObserved;
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
