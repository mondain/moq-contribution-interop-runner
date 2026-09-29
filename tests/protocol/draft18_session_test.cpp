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
        const auto event = std::find_if(
            peer_evidence.begin(), peer_evidence.end(),
            [index](const EvidenceEvent& value) {
                const auto* observed =
                    std::get_if<RequestObservedEvidence>(&value.data);
                return observed && observed->stream_id == index * 4;
            });
        ASSERT_NE(event, peer_evidence.end());
        const auto& observed = std::get<RequestObservedEvidence>(event->data);
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
        const auto event = std::find_if(
            evidence.begin(), evidence.end(), [](const EvidenceEvent& value) {
                return value.kind == EvidenceKind::InitialResponseObserved;
            });
        ASSERT_NE(event, evidence.end()) << index;
        const auto& observed =
            std::get<InitialResponseEvidence>(event->data);
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

TEST(Draft18SessionSubscriptions, PeerRequestsCreateExactLocalRolesAndPhases) {
    using namespace wire::draft18;
    const TrackNamespace name_space{{{std::byte{'a'}},
                                     {std::byte{'b'}, std::byte{0}}}};
    const TrackName name{{std::byte{'t'}}};
    const TrackKey expected{{name_space.fields}, name.bytes};

    PublisherSession subscribe;
    activate(subscribe);
    subscribe.on_event(StreamDataEvent{
        0, encode(SubscribeMessage{0, name_space, name, {}}), false});
    subscribe.observe_local_message(0, SubscribeOkMessage{7, {}, {}}, false);
    auto evidence = subscribe.take_evidence(64);
    auto created = std::find_if(evidence.begin(), evidence.end(),
                                [](const EvidenceEvent& event) {
                                    return event.kind ==
                                           EvidenceKind::SubscriptionCreated;
                                });
    ASSERT_NE(created, evidence.end());
    const auto& subscription =
        std::get<SubscriptionCreatedEvidence>(created->data);
    EXPECT_EQ(subscription.track, expected);
    EXPECT_EQ(subscription.local_role, LocalSubscriptionRole::Publisher);
    EXPECT_EQ(subscription.phase, SubscriptionPhase::Pending);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& event) {
                               const auto* phase =
                                   std::get_if<SubscriptionPhaseEvidence>(
                                       &event.data);
                               return phase != nullptr &&
                                      phase->new_phase ==
                                          SubscriptionPhase::Established;
                           }),
              evidence.end());

    PublisherSession publish;
    activate(publish);
    publish.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, name, 9, {}, {}}), false});
    publish.observe_local_message(0, RequestOkMessage{}, false);
    evidence = publish.take_evidence(64);
    created = std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& event) {
                               return event.kind ==
                                      EvidenceKind::SubscriptionCreated;
                           });
    ASSERT_NE(created, evidence.end());
    EXPECT_EQ(std::get<SubscriptionCreatedEvidence>(created->data).local_role,
              LocalSubscriptionRole::Subscriber);
}

TEST(Draft18SessionSubscriptions, LocalRequestsMapToSenderRole) {
    using namespace wire::draft18;
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'x'}}};

    PublisherSession session;
    activate(session);
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    session.observe_local_message(
        1, SubscribeMessage{1, name_space, name, {}}, false);
    session.observe_local_stream(5, LocalStreamPurpose::Request);
    session.observe_local_message(
        5, PublishMessage{3, name_space, name, 4, {}, {}}, false);
    const auto evidence = session.take_evidence(64);
    std::vector<LocalSubscriptionRole> roles;
    for (const auto& event : evidence) {
        if (const auto* created =
                std::get_if<SubscriptionCreatedEvidence>(&event.data)) {
            roles.push_back(created->local_role);
        }
    }
    EXPECT_EQ(roles, (std::vector<LocalSubscriptionRole>{
                         LocalSubscriptionRole::Subscriber,
                         LocalSubscriptionRole::Publisher}));
}

TEST(Draft18SessionSubscriptions, DuplicatePeerPublicationReturnsRequestError) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'t'}}};
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, name, 7, {}, {}}), false});
    const auto duplicate = session.on_event(StreamDataEvent{
        4, encode(PublishMessage{2, name_space, name, 9, {}, {}}), false});
    ASSERT_EQ(duplicate.actions.size(), 1u);
    const auto* send = std::get_if<SendMessageAction>(&duplicate.actions[0]);
    ASSERT_NE(send, nullptr);
    EXPECT_EQ(send->stream_id, 4u);
    const auto* error = std::get_if<RequestErrorMessage>(&send->message);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->error_code, 0x19u);
    EXPECT_EQ(session.phase(), SessionPhase::Active);
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind == EvidenceKind::DuplicateSubscription;
                            }), 1);
}

TEST(Draft18SessionSubscriptions, DotAndSessionNamespaceReturnDoesNotExist) {
    using namespace wire::draft18;
    const std::vector<TrackNamespace> namespaces{
        {{{std::byte{'.'}}}},
        {{{std::byte{'.'}, std::byte{'s'}, std::byte{'e'}, std::byte{'s'},
           std::byte{'s'}, std::byte{'i'}, std::byte{'o'}, std::byte{'n'}}}}};
    for (const auto& name_space : namespaces) {
        PublisherSession session;
        activate(session);
        const auto result = session.on_event(StreamDataEvent{
            0, encode(PublishMessage{0, name_space, {{std::byte{'t'}}}, 1, {}, {}}),
            false});
        ASSERT_EQ(result.actions.size(), 1u);
        const auto* send = std::get_if<SendMessageAction>(&result.actions[0]);
        ASSERT_NE(send, nullptr);
        const auto* error = std::get_if<RequestErrorMessage>(&send->message);
        ASSERT_NE(error, nullptr);
        EXPECT_EQ(error->error_code, 0x10u);
        EXPECT_EQ(session.phase(), SessionPhase::Active);
    }
}

TEST(Draft18SessionSubscriptions, RejectedSubscriptionReleasesTrackRole) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'t'}}};
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, name, 7, {}, {}}), false});
    session.observe_local_message(
        0, RequestErrorMessage{0x20, 0, {}, std::nullopt}, true);
    const auto later = session.on_event(StreamDataEvent{
        4, encode(PublishMessage{2, name_space, name, 9, {}, {}}), false});
    EXPECT_TRUE(later.actions.empty());
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind == EvidenceKind::SubscriptionCreated;
                            }), 2);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind == EvidenceKind::DuplicateSubscription;
                            }), 0);
}

TEST(Draft18SessionSubscriptions, PeerPublishReplacesPendingLocalSubscribe) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'t'}}};
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    session.observe_local_message(
        1, SubscribeMessage{1, name_space, name, {}}, false);
    const auto incoming = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, name, 7, {}, {}}), false});
    ASSERT_EQ(incoming.actions.size(), 1u);
    const auto* stop = std::get_if<StopSendingAction>(&incoming.actions[0]);
    ASSERT_NE(stop, nullptr);
    EXPECT_EQ(stop->stream_id, 1u);
    EXPECT_EQ(stop->application_error, 0x1u);
    const auto evidence = session.take_evidence(64);
    const auto replacement = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& value) {
            return value.kind == EvidenceKind::PendingSubscriptionReplaced;
        });
    ASSERT_NE(replacement, evidence.end());
    const auto created = std::find_if(
        replacement, evidence.end(), [](const EvidenceEvent& value) {
            return value.kind == EvidenceKind::SubscriptionCreated;
        });
    ASSERT_NE(created, evidence.end());
    EXPECT_EQ(std::get<SubscriptionCreatedEvidence>(created->data).stream_id, 0u);
    const auto old_terminal = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& value) {
            const auto* terminal =
                std::get_if<RequestTerminalEvidence>(&value.data);
            return terminal && terminal->stream_id == 1;
        });
    ASSERT_NE(old_terminal, evidence.end());
    EXPECT_LT(old_terminal->sequence, replacement->sequence);
    EXPECT_LT(replacement->sequence, created->sequence);
}

TEST(Draft18SessionSubscriptions, KeyByteLimitCountsBothRetainedCopies) {
    using namespace wire::draft18;
    PublisherSessionConfig config;
    config.maximum_subscription_key_bytes = 3;
    PublisherSession session(config);
    activate(session);
    const auto result = session.on_event(StreamDataEvent{
        0,
        encode(PublishMessage{0, {{{std::byte{'n'}}}},
                              {{std::byte{'t'}}}, 1, {}, {}}),
        false});
    ASSERT_NE(close_action(result), nullptr);
    const auto evidence = session.take_evidence(64);
    const auto limit = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& value) {
            const auto* exhausted =
                std::get_if<HarnessLimitEvidence>(&value.data);
            return exhausted &&
                   exhausted->limit == HarnessLimitKind::SubscriptionKeyBytes;
        });
    EXPECT_NE(limit, evidence.end());
}

TEST(Draft18SessionSubscriptions, BinaryTrackIdentityPreservesFieldBoundaries) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const std::vector<TrackNamespace> namespaces{
        {{{std::byte{'a'}}, {std::byte{'b'}}}},
        {{{std::byte{'a'}, std::byte{'b'}}}},
        {{{std::byte{'a'}}, {std::byte{'b'}, std::byte{0}}}},
        {{{std::byte{'a'}, std::byte{0}}}}};
    // Empty fields remain distinct in the key type, although draft-18
    // forbids transmitting them (Section 2.4.1).
    EXPECT_NE((TrackKey{{{{}}}, {}}), (TrackKey{{}, {}}));
    for (std::size_t index = 0; index < namespaces.size(); ++index) {
        const auto stream = static_cast<transport::StreamId>(index * 4);
        const auto result = session.on_event(StreamDataEvent{
            stream,
            encode(PublishMessage{index * 2, namespaces[index],
                                  {}, 1, {}, {}}),
            false});
        EXPECT_TRUE(result.actions.empty()) << index;
    }
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind == EvidenceKind::SubscriptionCreated;
                            }),
              namespaces.size());
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind == EvidenceKind::DuplicateSubscription;
                            }),
              0);
}

TEST(Draft18SessionSubscriptions, OppositeLocalRolesCoexistForExactTrack) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'t'}}};
    const auto published = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, name, 1, {}, {}}), false});
    ASSERT_TRUE(published.actions.empty());
    const auto subscribed = session.on_event(StreamDataEvent{
        4, encode(SubscribeMessage{2, name_space, name, {}}), false});
    EXPECT_TRUE(subscribed.actions.empty());
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind == EvidenceKind::OppositeRoleCoexistence;
                            }),
              1);
}

TEST(Draft18SessionSubscriptions, SameRoleDuplicateForBothOperationsAndSenders) {
    using namespace wire::draft18;
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'t'}}};
    for (const bool peer : {false, true}) {
        for (const bool publish : {false, true}) {
            PublisherSession session;
            activate(session);
            const auto request = [&](std::uint64_t id) -> Message {
                if (publish) {
                    return PublishMessage{id, name_space, name, 1, {}, {}};
                }
                return SubscribeMessage{id, name_space, name, {}};
            };
            SessionTransition duplicate;
            if (peer) {
                session.on_event(StreamDataEvent{0, encode(request(0)), false});
                duplicate = session.on_event(
                    StreamDataEvent{4, encode(request(2)), false});
                ASSERT_EQ(duplicate.actions.size(), 1u);
                const auto* send =
                    std::get_if<SendMessageAction>(&duplicate.actions.front());
                ASSERT_NE(send, nullptr);
                const auto* error =
                    std::get_if<RequestErrorMessage>(&send->message);
                ASSERT_NE(error, nullptr);
                EXPECT_EQ(error->error_code, 0x19u);
            } else {
                session.observe_local_stream(1, LocalStreamPurpose::Request);
                session.observe_local_message(1, request(1), false);
                session.observe_local_stream(5, LocalStreamPurpose::Request);
                duplicate = session.observe_local_message(5, request(3), false);
                EXPECT_TRUE(duplicate.actions.empty());
            }
            const auto evidence = session.take_evidence(64);
            EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                                    [](const EvidenceEvent& value) {
                                        return value.kind ==
                                               EvidenceKind::DuplicateSubscription;
                                    }),
                      1);
        }
    }
}

TEST(Draft18SessionSubscriptions, ReservedCategoriesAreByteExact) {
    using namespace wire::draft18;
    const std::vector<std::pair<TrackNamespace, TrackName>> cases{
        {{{{std::byte{'.'}}}}, {{}}},
        {{{{std::byte{'.'}, std::byte{'s'}, std::byte{'e'},
            std::byte{'s'}, std::byte{'s'}, std::byte{'i'},
            std::byte{'o'}, std::byte{'n'}}}}, {{}}},
        {{{{std::byte{'.'}, std::byte{'s'}, std::byte{'e'},
            std::byte{'s'}, std::byte{'s'}, std::byte{'i'},
            std::byte{'o'}, std::byte{'n'}}}}, {{std::byte{'x'}}}}};
    const std::array categories{
        ReservedNamespaceCategory::Dot,
        ReservedNamespaceCategory::SessionEmptyTrack,
        ReservedNamespaceCategory::SessionUnrecognized};
    for (std::size_t index = 0; index < cases.size(); ++index) {
        PublisherSession session;
        activate(session);
        const auto result = session.on_event(StreamDataEvent{
            0, encode(PublishMessage{0, cases[index].first,
                                     cases[index].second, 1, {}, {}}),
            false});
        ASSERT_EQ(result.actions.size(), 1u);
        const auto evidence = session.take_evidence(64);
        const auto found = std::find_if(
            evidence.begin(), evidence.end(), [](const EvidenceEvent& value) {
                return value.kind == EvidenceKind::ReservedNamespaceRejected;
            });
        ASSERT_NE(found, evidence.end());
        EXPECT_EQ(std::get<ReservedNamespaceEvidence>(found->data).category,
                  categories[index]);
    }
    for (const TrackNamespace& name_space : {
             TrackNamespace{{{std::byte{'.'}, std::byte{'x'}}}},
             TrackNamespace{}}) {
        PublisherSession session;
        activate(session);
        const auto result = session.on_event(StreamDataEvent{
            0, encode(PublishMessage{0, name_space, {}, 1, {}, {}}), false});
        EXPECT_TRUE(result.actions.empty());
    }
}

TEST(Draft18SessionSubscriptions, ActiveAndHistoryLimitsRemainIndependent) {
    using namespace wire::draft18;
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const auto publish = [&](std::uint64_t id, char name) {
        return PublishMessage{id, name_space,
                              {{static_cast<std::byte>(name)}}, 1, {}, {}};
    };
    PublisherSessionConfig active_config;
    active_config.maximum_active_subscriptions = 1;
    PublisherSession active(active_config);
    activate(active);
    active.on_event(StreamDataEvent{0, encode(publish(0, 'a')), false});
    const auto active_limit = active.on_event(
        StreamDataEvent{4, encode(publish(2, 'b')), false});
    ASSERT_NE(close_action(active_limit), nullptr);
    auto evidence = active.take_evidence(64);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& value) {
                               const auto* limit =
                                   std::get_if<HarnessLimitEvidence>(&value.data);
                               return limit && limit->limit ==
                                   HarnessLimitKind::ActiveSubscriptions;
                           }),
              evidence.end());

    PublisherSessionConfig history_config;
    history_config.maximum_subscription_history = 1;
    PublisherSession history(history_config);
    activate(history);
    history.on_event(StreamDataEvent{0, encode(publish(0, 'a')), false});
    history.observe_local_message(
        0, RequestErrorMessage{0x20, 0, {}, std::nullopt}, true);
    const auto history_limit = history.on_event(
        StreamDataEvent{4, encode(publish(2, 'b')), false});
    ASSERT_NE(close_action(history_limit), nullptr);
    evidence = history.take_evidence(64);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& value) {
                               const auto* limit =
                                   std::get_if<HarnessLimitEvidence>(&value.data);
                               return limit && limit->limit ==
                                   HarnessLimitKind::SubscriptionHistory;
                           }),
              evidence.end());
}

TEST(Draft18SessionSubscriptions, SessionCloseRecordsSubscriptionTermination) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {{{std::byte{'n'}}}},
                                 {{std::byte{'t'}}}, 1, {}, {}}), false});
    session.take_evidence(64);
    session.on_event(transport::PeerCloseEvent{
        transport::CloseErrorSpace::Application, 9, {}});
    const auto evidence = session.take_evidence(64);
    const auto phase = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& value) {
            const auto* changed =
                std::get_if<SubscriptionPhaseEvidence>(&value.data);
            return changed &&
                   changed->new_phase == SubscriptionPhase::Terminated;
        });
    ASSERT_NE(phase, evidence.end());
    const auto terminal = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& value) {
            return value.kind == EvidenceKind::RequestTerminal;
        });
    ASSERT_NE(terminal, evidence.end());
    EXPECT_LT(phase->sequence, terminal->sequence);
}

TEST(Draft18SessionSubscriptions, TerminalCausesReleaseSameRoleSlot) {
    using namespace wire::draft18;
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'t'}}};
    const auto first = encode(PublishMessage{0, name_space, name, 1, {}, {}});
    const auto second = encode(PublishMessage{2, name_space, name, 1, {}, {}});
    for (int cause = 0; cause < 5; ++cause) {
        PublisherSession session;
        activate(session);
        session.on_event(StreamDataEvent{0, first, false});
        switch (cause) {
            case 0:
                session.observe_local_message(
                    0, RequestErrorMessage{0x20, 0, {}, std::nullopt}, true);
                break;
            case 1:
                session.on_event(transport::PeerResetEvent{0, 9});
                break;
            case 2:
                session.on_event(transport::PeerStopSendingEvent{0, 9});
                break;
            case 3:
                session.on_event(StreamDataEvent{0, {}, true});
                session.observe_local_fin(0);
                break;
            case 4:
                session.observe_local_fin(0);
                session.on_event(StreamDataEvent{0, {}, true});
                break;
        }
        const auto result = session.on_event(StreamDataEvent{4, second, false});
        EXPECT_TRUE(result.actions.empty()) << cause;
        const auto evidence = session.take_evidence(64);
        EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                                [](const EvidenceEvent& value) {
                                    return value.kind ==
                                           EvidenceKind::DuplicateSubscription;
                                }),
                  0) << cause;
    }
}

TEST(Draft18SessionSubscriptions, PeerRejectionHasOneTerminalAfterLocalSend) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'t'}}};
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, name, 1, {}, {}}), false});
    const auto duplicate = session.on_event(StreamDataEvent{
        4, encode(PublishMessage{2, name_space, name, 1, {}, {}}), false});
    ASSERT_EQ(duplicate.actions.size(), 1u);
    session.observe_local_message(
        4, RequestErrorMessage{0x19, 0, {}, std::nullopt}, true);
    session.observe_local_message(
        4, RequestErrorMessage{0x19, 0, {}, std::nullopt}, true);
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                const auto* terminal =
                                    std::get_if<RequestTerminalEvidence>(&value.data);
                                return terminal && terminal->stream_id == 4;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind ==
                                       EvidenceKind::SubscriptionPhaseChanged;
                            }),
              0);
}

TEST(Draft18SessionSubscriptions, LocalReservedPublicationIsNotPeerFault) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    const auto result = session.observe_local_message(
        1, PublishMessage{1, {{{std::byte{'.'}}}}, {}, 1, {}, {}}, false);
    EXPECT_TRUE(result.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
    const auto evidence = session.take_evidence(64);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& value) {
                               return value.kind ==
                                      EvidenceKind::LocalObservationError;
                           }),
              evidence.end());
}

TEST(Draft18SessionSubscriptions, AllRoleOriginsReachEstablishedAndTerminated) {
    using namespace wire::draft18;
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'t'}}};
    for (const bool peer : {false, true}) {
        for (const bool publish : {false, true}) {
            PublisherSession session;
            activate(session);
            const auto stream = peer ? 0u : 1u;
            const auto request_id = peer ? 0u : 1u;
            const Message request = publish
                ? Message{PublishMessage{request_id, name_space, name,
                                         1, {}, {}}}
                : Message{SubscribeMessage{request_id, name_space, name, {}}};
            if (peer) {
                session.on_event(StreamDataEvent{stream, encode(request), false});
                session.observe_local_message(
                    stream,
                    publish ? Message{RequestOkMessage{}}
                            : Message{SubscribeOkMessage{7, {}, {}}},
                    false);
            } else {
                session.observe_local_stream(stream, LocalStreamPurpose::Request);
                session.observe_local_message(stream, request, false);
                session.on_event(StreamDataEvent{
                    stream,
                    encode(publish ? Message{RequestOkMessage{}}
                                   : Message{SubscribeOkMessage{7, {}, {}}}),
                    false});
            }
            session.observe_local_stop_sending(stream, 1);
            const auto evidence = session.take_evidence(64);
            std::vector<SubscriptionPhase> phases;
            for (const auto& value : evidence) {
                if (const auto* changed =
                        std::get_if<SubscriptionPhaseEvidence>(&value.data)) {
                    phases.push_back(changed->new_phase);
                }
            }
            EXPECT_EQ(phases, (std::vector<SubscriptionPhase>{
                                  SubscriptionPhase::Established,
                                  SubscriptionPhase::Terminated}))
                << peer << publish;
        }
    }
}

TEST(Draft18SessionSubscriptions, RequiredRejectionsSurviveNoOptionalEvidenceSpace) {
    using namespace wire::draft18;
    PublisherSessionConfig config;
    config.maximum_evidence_count = 4;
    PublisherSession duplicate(config);
    activate_with_small_evidence_queue(duplicate);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const TrackName name{{std::byte{'t'}}};
    duplicate.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, name, 1, {}, {}}), false});
    duplicate.take_evidence(64);
    const auto rejected = duplicate.on_event(StreamDataEvent{
        4, encode(PublishMessage{2, name_space, name, 1, {}, {}}), false});
    ASSERT_EQ(rejected.actions.size(), 1u);
    const auto* duplicate_action =
        std::get_if<SendMessageAction>(&rejected.actions.front());
    ASSERT_NE(duplicate_action, nullptr);
    ASSERT_NE(std::get_if<RequestErrorMessage>(&duplicate_action->message),
              nullptr);
    EXPECT_EQ(std::get<RequestErrorMessage>(duplicate_action->message).error_code,
              0x19u);

    PublisherSession reserved(config);
    activate_with_small_evidence_queue(reserved);
    const auto reserved_result = reserved.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {{{std::byte{'.'}}}}, name, 1, {}, {}}),
        false});
    ASSERT_EQ(reserved_result.actions.size(), 1u);
    const auto* reserved_action =
        std::get_if<SendMessageAction>(&reserved_result.actions.front());
    ASSERT_NE(reserved_action, nullptr);
    EXPECT_EQ(std::get<RequestErrorMessage>(reserved_action->message).error_code,
              0x10u);
}

TEST(Draft18SessionAliases, PeerPublishCannotReuseEstablishedAliasForDifferentTrack) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, {{std::byte{'a'}}}, 7, {}, {}}),
        false});
    session.observe_local_message(0, RequestOkMessage{}, false);
    const auto collision = session.on_event(StreamDataEvent{
        4, encode(PublishMessage{2, name_space, {{std::byte{'b'}}}, 7, {}, {}}),
        false});
    ASSERT_NE(close_action(collision), nullptr);
    EXPECT_EQ(close_action(collision)->application_error, 0x5u);
}

TEST(Draft18SessionAliases, PeerSubscribeOkCannotReuseEstablishedAlias) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, {{std::byte{'a'}}}, 7, {}, {}}),
        false});
    session.observe_local_message(0, RequestOkMessage{}, false);
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    session.observe_local_message(
        1, SubscribeMessage{1, name_space, {{std::byte{'b'}}}, {}}, false);
    const auto collision = session.on_event(StreamDataEvent{
        1, encode(SubscribeOkMessage{7, {}, {}}), false});
    ASSERT_NE(close_action(collision), nullptr);
    EXPECT_EQ(close_action(collision)->application_error, 0x5u);
}

TEST(Draft18SessionAliases, TerminationReleasesAliasForAnotherTrack) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, {{std::byte{'a'}}}, 7, {}, {}}),
        false});
    session.observe_local_message(0, RequestOkMessage{}, false);
    session.observe_local_stop_sending(0, 1);
    const auto reuse = session.on_event(StreamDataEvent{
        4, encode(PublishMessage{2, name_space, {{std::byte{'b'}}}, 7, {}, {}}),
        false});
    EXPECT_TRUE(reuse.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18SessionAliases, SameNumericAliasIsLegalForOppositePublishers) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, name_space, {{std::byte{'a'}}}, 7, {}, {}}),
        false});
    session.observe_local_message(0, RequestOkMessage{}, false);
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    session.observe_local_message(
        1, PublishMessage{1, name_space, {{std::byte{'b'}}}, 7, {}, {}},
        false);
    const auto accepted = session.on_event(StreamDataEvent{
        1, encode(RequestOkMessage{}), false});
    EXPECT_TRUE(accepted.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18SessionAliases, ReservedNamespaceRejectionPrecedesAliasCollision) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {{{std::byte{'n'}}}},
                                 {{std::byte{'a'}}}, 7, {}, {}}), false});
    session.observe_local_message(0, RequestOkMessage{}, false);
    const auto reserved = session.on_event(StreamDataEvent{
        4, encode(PublishMessage{2, {{{std::byte{'.'}}}},
                                 {{std::byte{'b'}}}, 7, {}, {}}), false});
    ASSERT_EQ(reserved.actions.size(), 1u);
    const auto* send = std::get_if<SendMessageAction>(&reserved.actions.front());
    ASSERT_NE(send, nullptr);
    EXPECT_EQ(std::get<RequestErrorMessage>(send->message).error_code, 0x10u);
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18SessionTokens, RegisterExceedsDefaultZeroCacheLimit) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const Token token{TokenAliasType::Register, 1, 0, {std::byte{'x'}}};
    const auto result = session.on_event(StreamDataEvent{
        0,
        encode(PublishMessage{0, {{{std::byte{'n'}}}},
                              {{std::byte{'t'}}}, 1,
                              {{0x03, token}}, {}}),
        false});
    ASSERT_NE(close_action(result), nullptr);
    EXPECT_EQ(close_action(result)->application_error, 0x13u);
}

TEST(Draft18SessionTokens, DuplicateRegisteredAliasClosesSession) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(
        3, SetupMessage{{{0x04, VarIntValue{17, {}}}}}, false);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(64);
    const Token token{TokenAliasType::Register, 1, 0, {std::byte{'x'}}};
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {{{std::byte{'n'}}}},
                                 {{std::byte{'a'}}}, 1,
                                 {{0x03, token}}, {}}), false});
    const auto duplicate = session.on_event(StreamDataEvent{
        4, encode(PublishMessage{2, {{{std::byte{'n'}}}},
                                 {{std::byte{'b'}}}, 2,
                                 {{0x03, token}}, {}}), false});
    ASSERT_NE(close_action(duplicate), nullptr);
    EXPECT_EQ(close_action(duplicate)->application_error, 0x14u);
}

TEST(Draft18SessionTokens, UnknownAliasUsesSessionErrorCode) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const Token token{TokenAliasType::UseAlias, 9, std::nullopt, {}};
    const auto result = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {{{std::byte{'n'}}}},
                                 {{std::byte{'a'}}}, 1,
                                 {{0x03, token}}, {}}), false});
    ASSERT_NE(close_action(result), nullptr);
    EXPECT_EQ(close_action(result)->application_error, 0x17u);
}

TEST(Draft18SessionTokens, DeleteReleasesCacheCapacityAndAlias) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(
        3, SetupMessage{{{0x04, VarIntValue{17, {}}}}}, false);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(64);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    const auto publish_with = [&](std::uint64_t id, char name,
                                  Token token) {
        return PublishMessage{id, name_space,
                              {{static_cast<std::byte>(name)}}, id,
                              {{0x03, std::move(token)}}, {}};
    };
    const auto registered = session.on_event(StreamDataEvent{
        0, encode(publish_with(0, 'a',
            Token{TokenAliasType::Register, 1, 0, {std::byte{'x'}}})), false});
    EXPECT_TRUE(registered.actions.empty());
    const auto removed = session.on_event(StreamDataEvent{
        4, encode(publish_with(2, 'b',
            Token{TokenAliasType::Delete, 1, std::nullopt, {}})), false});
    EXPECT_TRUE(removed.actions.empty());
    const auto reused = session.on_event(StreamDataEvent{
        8, encode(publish_with(4, 'c',
            Token{TokenAliasType::Register, 1, 0, {std::byte{'y'}}})), false});
    EXPECT_TRUE(reused.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18SessionTokens, CacheSizeIncludesSixteenByteAliasOverhead) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(
        3, SetupMessage{{{0x04, VarIntValue{32, {}}}}}, false);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(64);
    const auto first = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {{{std::byte{'n'}}}},
                                 {{std::byte{'a'}}}, 1,
                                 {{0x03, Token{TokenAliasType::Register,
                                               1, 0, {std::byte{'x'}}}}}, {}}),
        false});
    EXPECT_TRUE(first.actions.empty());
    const auto overflow = session.on_event(StreamDataEvent{
        4, encode(PublishMessage{2, {{{std::byte{'n'}}}},
                                 {{std::byte{'b'}}}, 2,
                                 {{0x03, Token{TokenAliasType::Register,
                                               2, 0, {std::byte{'y'}}}}}, {}}),
        false});
    ASSERT_NE(close_action(overflow), nullptr);
    EXPECT_EQ(close_action(overflow)->application_error, 0x13u);
}

TEST(Draft18SessionTokens, RepeatedResolvedTokenValueIsMalformed) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(
        3, SetupMessage{{{0x04, VarIntValue{17, {}}}}}, false);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(64);
    const auto result = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{
               0, {{{std::byte{'n'}}}}, {{std::byte{'a'}}}, 1,
               {{0x03, Token{TokenAliasType::Register, 1, 0,
                             {std::byte{'x'}}}},
                {0x03, Token{TokenAliasType::UseValue, std::nullopt, 0,
                             {std::byte{'x'}}}}},
               {}}),
        false});
    ASSERT_EQ(result.actions.size(), 1u);
    const auto* send = std::get_if<SendMessageAction>(&result.actions.front());
    ASSERT_NE(send, nullptr);
    EXPECT_EQ(std::get<RequestErrorMessage>(send->message).error_code, 0x4u);
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18SessionTokens, SetupRegisterOverDefaultZeroIsUseValue) {
    using namespace wire::draft18;
    PublisherSession session;
    establish_with_local_setup(session);
    wire::ByteWriter encoded_token(64);
    ASSERT_TRUE(encode_token(Token{TokenAliasType::Register, 1, 0,
                                   {std::byte{'x'}}},
                             encoded_token).has_value());
    const auto peer_setup = encode(SetupMessage{{
        {0x03, ByteValue{{encoded_token.bytes().begin(),
                          encoded_token.bytes().end()}}}}});
    const auto setup_result = session.on_event(
        StreamDataEvent{2, peer_setup, false});
    EXPECT_TRUE(setup_result.actions.empty());
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    const auto use = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {{{std::byte{'n'}}}},
                                 {{std::byte{'a'}}}, 1,
                                 {{0x03, Token{TokenAliasType::UseAlias,
                                               1, std::nullopt, {}}}}, {}}),
        false});
    ASSERT_NE(close_action(use), nullptr);
    EXPECT_EQ(close_action(use)->application_error, 0x17u);
}

TEST(Draft18SessionTokens, SetupRegisterWithinAdvertisedLimitCanBeUsed) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(
        3, SetupMessage{{{0x04, VarIntValue{17, {}}}}}, false);
    wire::ByteWriter encoded_token(64);
    ASSERT_TRUE(encode_token(Token{TokenAliasType::Register, 1, 0,
                                   {std::byte{'x'}}},
                             encoded_token).has_value());
    const auto peer_setup = encode(SetupMessage{{
        {0x03, ByteValue{{encoded_token.bytes().begin(),
                          encoded_token.bytes().end()}}}}});
    const auto setup_result = session.on_event(
        StreamDataEvent{2, peer_setup, false});
    EXPECT_TRUE(setup_result.actions.empty());
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    const auto use = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {{{std::byte{'n'}}}},
                                 {{std::byte{'a'}}}, 1,
                                 {{0x03, Token{TokenAliasType::UseAlias,
                                               1, std::nullopt, {}}}}, {}}),
        false});
    EXPECT_TRUE(use.actions.empty());
}

TEST(Draft18SessionTokens, UnknownAliasInRequestUpdateClosesSession) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const TrackNamespace name_space{{{std::byte{'n'}}}};
    session.on_event(StreamDataEvent{
        0, encode(SubscribeMessage{0, name_space,
                                   {{std::byte{'a'}}}, {}}), false});
    session.observe_local_message(0, SubscribeOkMessage{7, {}, {}}, false);
    const auto update = session.on_event(StreamDataEvent{
        0, encode(RequestUpdateMessage{
               2, {{0x03, Token{TokenAliasType::UseAlias,
                                  9, std::nullopt, {}}}}}), false});
    ASSERT_NE(close_action(update), nullptr);
    EXPECT_EQ(close_action(update)->application_error, 0x17u);
}

TEST(Draft18SessionTokens, RegisterPersistsWhenTrackRequestIsRejected) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(
        3, SetupMessage{{{0x04, VarIntValue{17, {}}}}}, false);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(64);
    const auto rejected = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{
               0, {{{std::byte{'.'}}}}, {{std::byte{'a'}}}, 1,
               {{0x03, Token{TokenAliasType::Register, 1, 0,
                             {std::byte{'x'}}}}}, {}}), false});
    ASSERT_EQ(rejected.actions.size(), 1u);
    EXPECT_EQ(std::get<RequestErrorMessage>(
                  std::get<SendMessageAction>(rejected.actions.front()).message)
                  .error_code,
              0x10u);
    const auto use = session.on_event(StreamDataEvent{
        4, encode(PublishMessage{
               2, {{{std::byte{'n'}}}}, {{std::byte{'b'}}}, 2,
               {{0x03, Token{TokenAliasType::UseAlias, 1,
                             std::nullopt, {}}}}, {}}), false});
    EXPECT_TRUE(use.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18SessionTokens, UnknownAliasCloseAbsorbsLaterFin) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    const auto rejected = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{
               0, {{{std::byte{'n'}}}}, {{std::byte{'a'}}}, 1,
               {{0x03, Token{TokenAliasType::UseAlias, 9,
                             std::nullopt, {}}}}, {}}), false});
    ASSERT_NE(close_action(rejected), nullptr);
    const auto fin = session.on_event(StreamDataEvent{0, {}, true});
    EXPECT_TRUE(fin.actions.empty());
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind == EvidenceKind::RequestObserved;
                            }),
              1);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind == EvidenceKind::ResponseViolation;
                            }),
              0);
}

TEST(Draft18SessionTokens, SetupCannotDeleteOrUseAlias) {
    using namespace wire::draft18;
    for (const auto alias_type : {TokenAliasType::Delete,
                                  TokenAliasType::UseAlias}) {
        PublisherSession session;
        establish_with_local_setup(session);
        wire::ByteWriter encoded_token(64);
        ASSERT_TRUE(encode_token(Token{alias_type, 1, std::nullopt, {}},
                                 encoded_token).has_value());
        const auto result = session.on_event(StreamDataEvent{
            2, encode(SetupMessage{{
                   {0x03, ByteValue{{encoded_token.bytes().begin(),
                                     encoded_token.bytes().end()}}}}}),
            false});
        ASSERT_NE(close_action(result), nullptr);
        EXPECT_EQ(close_action(result)->application_error, 0x3u);
    }
}

TEST(Draft18SessionTokens, AliasSpacesAreIndependentBySender) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    session.observe_local_message(
        3, SetupMessage{{{0x04, VarIntValue{17, {}}}}}, false);
    session.on_event(StreamDataEvent{
        2, encode(SetupMessage{{{0x04, VarIntValue{17, {}}}}}), false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(64);
    const auto peer = session.on_event(StreamDataEvent{
        0, encode(PublishMessage{
               0, {{{std::byte{'n'}}}}, {{std::byte{'a'}}}, 1,
               {{0x03, Token{TokenAliasType::Register, 1, 0,
                             {std::byte{'x'}}}}}, {}}), false});
    EXPECT_TRUE(peer.actions.empty());
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    const auto local = session.observe_local_message(
        1, PublishMessage{
               1, {{{std::byte{'n'}}}}, {{std::byte{'b'}}}, 2,
               {{0x03, Token{TokenAliasType::Register, 1, 0,
                             {std::byte{'y'}}}}}, {}}, false);
    EXPECT_TRUE(local.actions.empty());
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18SessionTokens, LocalSetupAliasIsAvailableAfterPeerLimitArrives) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    wire::ByteWriter encoded_token(64);
    ASSERT_TRUE(encode_token(Token{TokenAliasType::Register, 1, 0,
                                   {std::byte{'x'}}},
                             encoded_token).has_value());
    session.observe_local_message(
        3, SetupMessage{{
               {0x03, ByteValue{{encoded_token.bytes().begin(),
                                 encoded_token.bytes().end()}}}}},
        false);
    session.on_event(StreamDataEvent{
        2, encode(SetupMessage{{{0x04, VarIntValue{17, {}}}}}), false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(64);
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    const auto use = session.observe_local_message(
        1, PublishMessage{
               1, {{{std::byte{'n'}}}}, {{std::byte{'a'}}}, 1,
               {{0x03, Token{TokenAliasType::UseAlias, 1,
                             std::nullopt, {}}}}, {}}, false);
    const auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& value) {
                                return value.kind ==
                                       EvidenceKind::LocalObservationError;
                            }),
              0);
    EXPECT_TRUE(use.actions.empty());
}

TEST(Draft18SessionTokens, LocalSetupCannotReferenceUnregisteredAlias) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    wire::ByteWriter encoded_token(64);
    ASSERT_TRUE(encode_token(Token{TokenAliasType::UseAlias, 1,
                                   std::nullopt, {}},
                             encoded_token).has_value());
    const auto invalid = session.observe_local_message(
        3, SetupMessage{{
               {0x03, ByteValue{{encoded_token.bytes().begin(),
                                 encoded_token.bytes().end()}}}}},
        false);
    EXPECT_EQ(session.phase(), SessionPhase::AwaitingSetup);
    const auto evidence = session.take_evidence(32);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& value) {
                               return value.kind ==
                                      EvidenceKind::LocalObservationError;
                           }),
              evidence.end());
    EXPECT_TRUE(invalid.actions.empty());
    session.observe_local_message(3, SetupMessage{}, false);
    session.on_event(StreamDataEvent{2, {kSetup.begin(), kSetup.end()}, false});
    EXPECT_EQ(session.phase(), SessionPhase::Active);
}

TEST(Draft18SessionTokens, InvalidLocalSetupRegistrationDoesNotPartiallyCache) {
    using namespace wire::draft18;
    PublisherSession session;
    session.on_event(established());
    session.observe_local_stream(3, LocalStreamPurpose::Control);
    wire::ByteWriter first_token(64);
    wire::ByteWriter second_token(64);
    ASSERT_TRUE(encode_token(Token{TokenAliasType::Register, 1, 0,
                                   {std::byte{'x'}}},
                             first_token).has_value());
    ASSERT_TRUE(encode_token(Token{TokenAliasType::Register, 1, 0,
                                   {std::byte{'y'}}},
                             second_token).has_value());
    session.observe_local_message(
        3, SetupMessage{{
               {0x03, ByteValue{{first_token.bytes().begin(),
                                 first_token.bytes().end()}}},
               {0x03, ByteValue{{second_token.bytes().begin(),
                                 second_token.bytes().end()}}}}}, false);
    session.on_event(StreamDataEvent{
        2, encode(SetupMessage{{{0x04, VarIntValue{34, {}}}}}), false});
    ASSERT_EQ(session.phase(), SessionPhase::Active);
    session.take_evidence(64);
    session.observe_local_stream(1, LocalStreamPurpose::Request);
    session.observe_local_message(
        1, PublishMessage{
               1, {{{std::byte{'n'}}}}, {{std::byte{'a'}}}, 1,
               {{0x03, Token{TokenAliasType::UseAlias, 1,
                             std::nullopt, {}}}}, {}}, false);
    const auto evidence = session.take_evidence(64);
    EXPECT_NE(std::find_if(evidence.begin(), evidence.end(),
                           [](const EvidenceEvent& value) {
                               return value.kind ==
                                      EvidenceKind::LocalObservationError;
                           }),
              evidence.end());
}

TEST(Draft18SessionForward, PublishForwardZeroChangesOnPublishOk) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{
               0, {{{std::byte{'n'}}}}, {{std::byte{'a'}}}, 1,
               {{0x10, Uint8ParameterValue{0}}}, {}}), false});
    session.observe_local_message(
        0, RequestOkMessage{{{0x10, Uint8ParameterValue{1}}}, {}}, false);
    const auto evidence = session.take_evidence(64);
    const auto created = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& value) {
            return value.kind == EvidenceKind::SubscriptionCreated;
        });
    ASSERT_NE(created, evidence.end());
    EXPECT_FALSE(std::get<SubscriptionCreatedEvidence>(created->data)
                     .forward_state);
    const auto changed = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& value) {
            return value.kind == EvidenceKind::ForwardStateChanged;
        });
    ASSERT_NE(changed, evidence.end());
    EXPECT_TRUE(std::get<ForwardStateEvidence>(changed->data).new_state);
}

TEST(Draft18SessionForward, SubscribeUpdateChangesStateOnlyWhenSpecified) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(SubscribeMessage{0, {{{std::byte{'n'}}}},
                                   {{std::byte{'a'}}},
                                   {{0x10, Uint8ParameterValue{0}}}}), false});
    session.observe_local_message(0, SubscribeOkMessage{7, {}, {}}, false);
    session.take_evidence(64);
    session.on_event(StreamDataEvent{
        0, encode(RequestUpdateMessage{2, {}}), false});
    auto evidence = session.take_evidence(64);
    EXPECT_EQ(std::count_if(evidence.begin(), evidence.end(),
                            [](const EvidenceEvent& event) {
                                return event.kind == EvidenceKind::ForwardStateChanged;
                            }), 0);
    session.on_event(StreamDataEvent{
        0, encode(RequestUpdateMessage{4, {{0x10, Uint8ParameterValue{1}}}}), false});
    evidence = session.take_evidence(64);
    const auto changed = std::find_if(evidence.begin(), evidence.end(),
                                      [](const EvidenceEvent& event) {
                                          return event.kind == EvidenceKind::ForwardStateChanged;
                                      });
    ASSERT_NE(changed, evidence.end());
    const auto& state = std::get<ForwardStateEvidence>(changed->data);
    EXPECT_FALSE(state.old_state);
    EXPECT_TRUE(state.new_state);
    EXPECT_EQ(state.actor, RequestInitiator::Peer);
}

TEST(Draft18SessionForward, PublishResumingForwardSavesJoiningLocation) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{
               0, {{{std::byte{'n'}}}}, {{std::byte{'a'}}}, 1,
               {{0x09, Location{3, 7}}, {0x10, Uint8ParameterValue{0}}},
               {}}), false});
    session.observe_local_message(
        0, RequestOkMessage{{{0x10, Uint8ParameterValue{1}}}, {}}, false);
    const auto evidence = session.take_evidence(64);
    const auto changed = std::find_if(evidence.begin(), evidence.end(),
                                      [](const EvidenceEvent& event) {
                                          return event.kind == EvidenceKind::ForwardStateChanged;
                                      });
    ASSERT_NE(changed, evidence.end());
    EXPECT_EQ(std::get<ForwardStateEvidence>(changed->data).joining_location,
              (Location{3, 7}));
}

TEST(Draft18SessionForward, UpdateOkSavesFreshJoiningLocation) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{
               0, {{{std::byte{'n'}}}}, {{std::byte{'a'}}}, 1,
               {{0x10, Uint8ParameterValue{0}}}, {}}), false});
    session.observe_local_message(
        0, RequestOkMessage{{{0x10, Uint8ParameterValue{0}}}, {}}, false);
    session.take_evidence(64);
    session.observe_local_message(
        0, RequestUpdateMessage{1, {{0x10, Uint8ParameterValue{1}}}}, false);
    session.take_evidence(64);
    session.on_event(StreamDataEvent{
        0, encode(RequestOkMessage{{{0x09, Location{5, 2}}}, {}}), false});
    const auto evidence = session.take_evidence(64);
    const auto response = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::UpdateResponseObserved;
        });
    ASSERT_NE(response, evidence.end());
    EXPECT_EQ(std::get<UpdateResponseEvidence>(response->data).joining_location,
              (Location{5, 2}));
}

TEST(Draft18SessionObjects, DatagramAssociatesAliasAndForwardState) {
    using namespace wire::draft18;
    PublisherSession session;
    activate(session);
    session.on_event(StreamDataEvent{
        0, encode(PublishMessage{0, {{{std::byte{'n'}}}},
                                 {{std::byte{'a'}}}, 1,
                                 {{0x10, Uint8ParameterValue{0}}}, {}}), false});
    session.observe_local_message(
        0, RequestOkMessage{{{0x10, Uint8ParameterValue{0}}}, {}}, false);
    session.take_evidence(64);
    session.on_event(transport::DatagramEvent{
        {std::byte{0x00}, std::byte{0x01}, std::byte{0x02},
         std::byte{0x03}, std::byte{0x04}, std::byte{0xaa}}});
    const auto evidence = session.take_evidence(64);
    const auto observed = std::find_if(
        evidence.begin(), evidence.end(), [](const EvidenceEvent& event) {
            return event.kind == EvidenceKind::ObjectObserved;
        });
    ASSERT_NE(observed, evidence.end());
    const auto& object = std::get<ObjectObservedEvidence>(observed->data);
    EXPECT_EQ(object.request_id, 0u);
    EXPECT_EQ(object.forward_state, false);
    EXPECT_EQ(object.object.group_id, 2u);
    EXPECT_EQ(object.object.object_id, 3u);
}

}  // namespace
}  // namespace moq::interop::session
