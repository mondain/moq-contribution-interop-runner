#include "moq/interop/session/draft23_session.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace {
using namespace moq::interop;
std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
class Transport final : public transport::SessionTransport {
public:
    struct Write { std::uint64_t id; std::vector<std::byte> data; bool fin; };
    std::vector<Write> writes;
    std::optional<std::uint64_t> closed;
    std::size_t allowance{65535};
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::Success, 1}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, 3}; }
    transport::OperationResult write(std::uint64_t id, std::span<const std::byte> data, bool fin) override {
        const auto count = std::min(data.size(), allowance);
        if (!count) return {transport::TransportStatus::WouldBlock, 0, {}};
        writes.push_back({id, {data.begin(), data.begin() + static_cast<std::ptrdiff_t>(count)}, fin && count == data.size()});
        return {count == data.size() ? transport::TransportStatus::Success : transport::TransportStatus::Partial, count, {}};
    }
    transport::OperationResult reset(std::uint64_t, std::uint64_t) override { return {}; }
    transport::OperationResult stop_sending(std::uint64_t, std::uint64_t) override { return {}; }
    transport::OperationResult send_datagram(std::span<const std::byte>) override { return {}; }
    transport::OperationResult close(std::uint64_t error, std::span<const std::byte>) override {
        closed = error; return {transport::TransportStatus::Success, 0, {}};
    }
    std::vector<transport::TransportEvent> poll(std::size_t) override { return {}; }
};
session::Draft23SessionConfig config() {
    session::Draft23SessionConfig result;
    result.track_namespace = {bytes({'m','e','d','i','a'})};
    result.track_name = bytes({'v','i','d','e'});
    return result;
}
void connect(session::Draft23Session& session) {
    session.on_event(transport::ConnectionEstablishedEvent{bytes({'m','o','q','t','-','2','3'}), {}, {}, 1200});
}
void setup(session::Draft23Session& session) {
    session.on_event(transport::StreamDataEvent{2, bytes({0xaf,0,0}), false});
}
void announce(session::Draft23Session& session) {
    session.on_event(transport::StreamDataEvent{0, bytes({6,9,0,1,5,'m','e','d','i','a',0}), false});
}
void subscribed(session::Draft23Session& session) {
    session.on_event(transport::StreamDataEvent{1, bytes({4,2,7,0}), false});
}
TEST(Draft23Session, AdvertisesSetupWithoutExtraStreamPrefix) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session);
    ASSERT_FALSE(transport.writes.empty());
    EXPECT_EQ(transport.writes[0].id, 3u);
    EXPECT_EQ(transport.writes[0].data[0], std::byte{0xaf});
    EXPECT_FALSE(transport.writes[0].fin);
}
TEST(Draft23Session, AnnounceSubscribeAndProgressingMedia) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session); subscribed(session);
    session.on_event(transport::StreamDataEvent{6, bytes({0x30,7,0,0,1,'a',0,1,'b'}), true});
    EXPECT_FALSE(session.error());
    EXPECT_TRUE(session.stats().setup_received);
    EXPECT_TRUE(session.stats().namespace_announced);
    EXPECT_TRUE(session.stats().subscription_established);
    EXPECT_EQ(session.stats().objects, 2u);
    EXPECT_EQ(session.stats().payload_bytes, 2u);
    EXPECT_TRUE(session.stats().progressing());
}
TEST(Draft23Session, BuffersEarlyRequestsAndOneByteFragments) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); announce(session);
    for (const auto byte : bytes({0xaf,0,0})) session.on_event(transport::StreamDataEvent{2,{byte},false});
    EXPECT_FALSE(session.error());
    EXPECT_TRUE(session.stats().namespace_announced);
    subscribed(session);
    for (const auto byte : bytes({0x30,7,0,0,2,'a','b',0,1,'c'}))
        session.on_event(transport::StreamDataEvent{6,{byte},false});
    EXPECT_TRUE(session.stats().progressing());
    EXPECT_EQ(session.stats().payload_bytes, 3u);
}
TEST(Draft23Session, AcceptsPublishFirstAndBuffersCrossStreamData) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{6,bytes({0x30,7,0,0,1,'a',0,1,'b'}),true});
    EXPECT_EQ(session.stats().objects, 0u);
    session.on_event(transport::StreamDataEvent{0,bytes({0x1d,15,0,1,5,'m','e','d','i','a',4,'v','i','d','e',7,0}),false});
    EXPECT_FALSE(session.error());
    EXPECT_TRUE(session.stats().subscription_established);
    EXPECT_TRUE(session.stats().progressing());
}
TEST(Draft23Session, RejectsWrongAlpnAndClosedControlStream) {
    Transport transport;
    session::Draft23Session session(transport, config());
    session.on_event(transport::ConnectionEstablishedEvent{bytes({'m','o','q','t','-','2','2'}),{},{},0});
    EXPECT_TRUE(session.error());
    Transport second;
    session::Draft23Session another(second, config());
    connect(another); setup(another);
    another.on_event(transport::StreamDataEvent{2,{},true});
    EXPECT_EQ(another.application_error(), 0x306u);
}
TEST(Draft23Session, BoundsEarlyDataAndReportsTruncatedObjects) {
    Transport transport;
    auto limited = config(); limited.maximum_stream_bytes = 8;
    session::Draft23Session session(transport, limited);
    connect(session);
    session.on_event(transport::StreamDataEvent{0,std::vector<std::byte>(9),false});
    ASSERT_TRUE(session.error());
    EXPECT_NE(session.error()->find("harness limit"),std::string::npos);
    Transport second;
    session::Draft23Session another(second, config());
    connect(another); setup(another); announce(another); subscribed(another);
    another.on_event(transport::StreamDataEvent{6,bytes({0x30,7,0,0,2,'a'}),true});
    EXPECT_EQ(another.application_error(), 0x203u);
}
TEST(Draft23Session, RetriesPartialAndBlockedWritesWithoutDuplicatingBytes) {
    Transport transport; transport.allowance = 1;
    session::Draft23Session session(transport, config());
    connect(session);
    for (unsigned i=0;i<100;++i) session.flush();
    std::vector<std::byte> combined;
    for (const auto& write : transport.writes) combined.insert(combined.end(),write.data.begin(),write.data.end());
    ASSERT_GE(combined.size(),3u);
    EXPECT_EQ(combined[0],std::byte{0xaf});
    EXPECT_EQ(combined[1],std::byte{0});
    EXPECT_FALSE(session.error());
}
TEST(Draft23Session, AllowsDuplicateUnknownSetupOptions) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session);
    session.on_event(transport::StreamDataEvent{2,bytes({0xaf,0,4,2,0,0,0}),false});
    EXPECT_FALSE(session.error());
    EXPECT_TRUE(session.stats().setup_received);
}
TEST(Draft23Session, InvalidObjectStatusAndDeltaOverflowHaveDraft23Codes) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session); subscribed(session);
    session.on_event(transport::StreamDataEvent{6,bytes({0x30,7,0,0,0,2}),true});
    EXPECT_EQ(session.application_error(),0x202u);
    Transport second;
    session::Draft23Session another(second, config());
    connect(another); setup(another); announce(another); subscribed(another);
    another.on_event(transport::StreamDataEvent{6,bytes({0x30,7,0,255,255,255,255,255,255,255,255,255,1,'a',0,1,'b'}),true});
    EXPECT_EQ(another.application_error(),0x400u);
}
TEST(Draft23Session, FinitePublisherDoneAndNormalClosePreserveDeliveredMedia) {
    Transport transport;
    session::Draft23Session session(transport,config());
    connect(session); setup(session); announce(session); subscribed(session);
    session.on_event(transport::StreamDataEvent{1,bytes({0xb,3,0,1,0}),true});
    session.on_event(transport::StreamDataEvent{6,bytes({0x30,7,0,0,1,'a',0,1,'b'}),true});
    session.on_event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,0,{}});
    EXPECT_FALSE(session.error());
    EXPECT_TRUE(session.stats().progressing());
}
TEST(Draft23Session, DataStreamResetAllowsLaterMediaStreams) {
    Transport transport;
    session::Draft23Session session(transport,config());
    connect(session); setup(session); announce(session); subscribed(session);
    session.on_event(transport::StreamDataEvent{6,bytes({0x30,7,0,0,1,'a'}),false});
    session.on_event(transport::PeerResetEvent{6,0});
    session.on_event(transport::StreamDataEvent{10,bytes({0x30,7,1,0,1,'b'}),true});
    EXPECT_FALSE(session.error());
    EXPECT_TRUE(session.stats().progressing());
}
TEST(Draft23Session, PublishDoneIsNotAllowedOnNamespaceRequestStream) {
    Transport transport;
    session::Draft23Session session(transport,config());
    connect(session); setup(session); announce(session); subscribed(session);
    session.on_event(transport::StreamDataEvent{0,bytes({0xb,3,0,0,0}),true});
    EXPECT_EQ(session.application_error(),0x303u);
}
TEST(Draft23Session, DirectSubscribeUsesFixtureBeforeAnyPublisherRequest) {
    Transport transport;
    auto direct=config(); direct.subscribe_immediately=true;
    session::Draft23Session session(transport,direct);
    connect(session); setup(session);
    ASSERT_EQ(transport.writes.size(),2u);
    EXPECT_EQ(transport.writes.back().id,1u);
    EXPECT_EQ(transport.writes.back().data,bytes({3,14,1,1,5,'m','e','d','i','a',4,'v','i','d','e',0}));
    EXPECT_FALSE(session.stats().namespace_announced);
}
TEST(Draft23Session, UnrelatedNamespaceDoesNotStartFixtureSubscription) {
    Transport transport;
    session::Draft23Session session(transport,config());
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{0,bytes({6,9,0,1,5,'o','t','h','e','r',0}),false});
    ASSERT_EQ(transport.writes.size(),2u); // SETUP, then namespace acknowledgement only.
    EXPECT_EQ(transport.writes.back().id,0u);
    EXPECT_FALSE(session.stats().namespace_announced);
    session.on_event(transport::StreamDataEvent{4,bytes({6,9,2,1,5,'m','e','d','i','a',0}),false});
    ASSERT_EQ(transport.writes.size(),4u);
    EXPECT_EQ(transport.writes.back().id,1u);
    EXPECT_EQ(transport.writes.back().data,bytes({3,14,1,1,5,'m','e','d','i','a',4,'v','i','d','e',0}));
}
TEST(Draft23Session, RequestParserRetainsSpecificDraft23SessionErrorCodes) {
    Transport transport;
    session::Draft23Session session(transport,config());
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{0,bytes({0x33,0}),false});
    EXPECT_EQ(session.application_error(),0x303u);
    Transport second;
    session::Draft23Session invalid_parameter(second,config());
    connect(invalid_parameter); setup(invalid_parameter);
    invalid_parameter.on_event(transport::StreamDataEvent{0,
        bytes({0x1d,17,0,1,5,'m','e','d','i','a',4,'v','i','d','e',7,1,0x10,1}),false});
    EXPECT_EQ(invalid_parameter.application_error(),0x305u);
}
TEST(Draft23Session, UnrelatedPublishIsRejectedWithFinAndItsObjectsDoNotCount) {
    Transport transport;
    session::Draft23Session session(transport,config());
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{0,
        bytes({0x1d,15,0,1,5,'m','e','d','i','a',4,'s','o','u','n',8,0}),false});
    ASSERT_FALSE(transport.writes.empty());
    EXPECT_EQ(transport.writes.back().id,0u);
    EXPECT_EQ(transport.writes.back().data,bytes({5,3,0x20,0,0}));
    EXPECT_TRUE(transport.writes.back().fin);
    session.on_event(transport::StreamDataEvent{6,bytes({0x30,8,0,0,1,'a',0,1,'b'}),true});
    EXPECT_EQ(session.stats().objects,0u);
    session.on_event(transport::StreamDataEvent{4,
        bytes({0x1d,15,2,1,5,'m','e','d','i','a',4,'v','i','d','e',7,0}),false});
    session.on_event(transport::StreamDataEvent{10,bytes({0x30,7,0,0,1,'a',0,1,'b'}),true});
    EXPECT_FALSE(session.error());
    EXPECT_EQ(session.stats().objects,2u);
    EXPECT_TRUE(session.stats().progressing());
}
TEST(Draft23Session, KnownUnsupportedRequestReportsHarnessLimitWithoutClosingPeer) {
    Transport transport;
    session::Draft23Session session(transport,config());
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{0,bytes({0x50,0}),false});
    ASSERT_TRUE(session.error());
    EXPECT_FALSE(session.application_error());
    EXPECT_FALSE(transport.closed);
}
TEST(Draft23Session, InvalidMaximumIsInvalidFieldAndMalformedOptionIsMalformedMessage) {
    Transport transport;
    session::Draft23Session invalid(transport,config());
    connect(invalid);
    invalid.on_event(transport::StreamDataEvent{2,bytes({0xaf,0,3,9,1,0}),false});
    EXPECT_EQ(invalid.application_error(),0x301u);
    Transport second;
    session::Draft23Session malformed(second,config());
    connect(malformed);
    malformed.on_event(transport::StreamDataEvent{2,bytes({0xaf,0,1,7}),false});
    EXPECT_EQ(malformed.application_error(),0x300u);
}
} // namespace
namespace {
TEST(Draft23Session, RecordsPeerSetupOptionsAndLength) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session);
    // PATH "/moq" (0x01) then AUTHORITY "h:1" (0x05, delta 4).
    session.on_event(transport::StreamDataEvent{2, bytes({0xaf,0,11, 1,4,'/','m','o','q', 4,3,'h',':','1'}), false});
    ASSERT_TRUE(session.stats().peer_setup.has_value());
    const auto& observed = *session.stats().peer_setup;
    EXPECT_EQ(observed.frame_length, 11u);
    ASSERT_EQ(observed.options.size(), 2u);
    EXPECT_EQ(observed.options[0].type, 1u);
    EXPECT_EQ(observed.options[1].type, 5u);
}
TEST(Draft23Session, KeepsPeerSetupEvidenceWhenItClosesForDuplicateOption) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session);
    session.on_event(transport::StreamDataEvent{2, bytes({0xaf,0,6, 1,1,'a', 0,1,'b'}), false});
    EXPECT_TRUE(session.error());
    ASSERT_TRUE(session.stats().peer_setup.has_value());
    EXPECT_EQ(session.stats().peer_setup->options.size(), 2u);
}
TEST(Draft23Session, NoPeerSetupEvidenceBeforeSetup) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session);
    EXPECT_FALSE(session.stats().peer_setup.has_value());
}
TEST(Draft23Session, TracesEachSubgroupStreamHeaderAndObjects) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session); subscribed(session);
    // 0x30: bit 4 + default priority, subgroup ID mode 0 (subgroup 0). Alias 7, group 3, objects 0 and 1.
    session.on_event(transport::StreamDataEvent{6, bytes({0x30,7,3,0,1,'a',0,1,'b'}), true});
    ASSERT_EQ(session.stats().data_streams.size(), 1u);
    const auto& stream = session.stats().data_streams[0];
    EXPECT_EQ(stream.stream_id, 6u);
    EXPECT_EQ(stream.type_flags, 0x30u);
    EXPECT_TRUE(stream.valid_header);
    EXPECT_EQ(stream.track_alias, 7u);
    EXPECT_EQ(stream.group_id, 3u);
    EXPECT_EQ(stream.subgroup_id, std::optional<std::uint64_t>{0});
    EXPECT_EQ(stream.first_object_id, std::optional<std::uint64_t>{0});
    EXPECT_EQ(stream.objects, 2u);
    EXPECT_TRUE(stream.fin);
    EXPECT_FALSE(stream.reset);
}
TEST(Draft23Session, SubgroupIdModesComeFromTheHeaderOrTheFirstObject) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session); subscribed(session);
    // Mode 1 (0x32): the Subgroup ID is the first Object ID, here delta 5. Mode 2 (0x34): explicit ID 9.
    session.on_event(transport::StreamDataEvent{6, bytes({0x32,7,1,5,1,'a'}), true});
    session.on_event(transport::StreamDataEvent{10, bytes({0x34,7,1,9,0,1,'a'}), true});
    ASSERT_EQ(session.stats().data_streams.size(), 2u);
    EXPECT_EQ(session.stats().data_streams[0].subgroup_id, std::optional<std::uint64_t>{5});
    EXPECT_EQ(session.stats().data_streams[1].subgroup_id, std::optional<std::uint64_t>{9});
}
TEST(Draft23Session, KeepsTheRecordOfAStreamThatFinishedOrWasReset) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session); subscribed(session);
    session.on_event(transport::StreamDataEvent{6, bytes({0x30,7,0,0,1,'a'}), false});
    session.on_event(transport::PeerResetEvent{6, std::nullopt});
    ASSERT_EQ(session.stats().data_streams.size(), 1u);
    EXPECT_TRUE(session.stats().data_streams[0].reset);
    EXPECT_FALSE(session.stats().data_streams[0].fin);
}
TEST(Draft23Session, RecordsAHeaderWithBitFourClearBeforeClosing) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{6, bytes({0x20,7,0,0}), false});
    EXPECT_TRUE(session.error());
    ASSERT_EQ(session.stats().data_streams.size(), 1u);
    EXPECT_EQ(session.stats().data_streams[0].type_flags, 0x20u);
    EXPECT_FALSE(session.stats().data_streams[0].valid_header);
}
TEST(Draft23Session, RecordsRequestMessagesWithTypeAndBodyLength) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session);
    ASSERT_FALSE(session.stats().requests.empty());
    const auto& request = session.stats().requests[0];
    EXPECT_EQ(request.stream_id, 0u);
    EXPECT_FALSE(request.local);
    ASSERT_EQ(request.messages.size(), 1u);
    EXPECT_EQ(request.messages[0].type, 6u);
    EXPECT_EQ(request.messages[0].body_length, 9u);
    EXPECT_EQ(request.request_id, std::optional<std::uint64_t>{0});
}
TEST(Draft23Session, RecordsSubscribeResponsePublishDoneAndFinOrder) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session); subscribed(session);
    session.on_event(transport::StreamDataEvent{1, bytes({0x0b,3,0,1,0}), false});
    session.on_event(transport::StreamDataEvent{1, {}, true});
    const session::Draft23RequestStream* subscribe = nullptr;
    for (const auto& request : session.stats().requests) if (request.stream_id == 1) subscribe = &request;
    ASSERT_NE(subscribe, nullptr);
    EXPECT_TRUE(subscribe->local);
    ASSERT_EQ(subscribe->messages.size(), 2u);
    EXPECT_EQ(subscribe->messages[0].type, 4u);
    EXPECT_EQ(subscribe->messages[1].type, 0x0bu);
    ASSERT_TRUE(subscribe->publish_done.has_value());
    EXPECT_EQ(subscribe->publish_done->stream_count, 1u);
    EXPECT_TRUE(subscribe->fin);
    EXPECT_TRUE(subscribe->fin_after_publish_done);
    EXPECT_TRUE(subscribe->established);
    EXPECT_EQ(session.stats().subscription_alias, std::optional<std::uint64_t>{7});
}
TEST(Draft23Session, AnnouncementStreamIsNotAnEstablishedSubscription) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session);
    ASSERT_FALSE(session.stats().requests.empty());
    EXPECT_FALSE(session.stats().requests[0].established);
    EXPECT_FALSE(session.stats().subscription_alias.has_value());
}
TEST(Draft23Session, FinBeforePublishDoneIsRecordedAsSuch) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session); subscribed(session);
    session.on_event(transport::StreamDataEvent{1, {}, true});
    for (const auto& request : session.stats().requests) {
        if (request.stream_id != 1) continue;
        EXPECT_TRUE(request.fin);
        EXPECT_FALSE(request.fin_after_publish_done);
        EXPECT_FALSE(request.publish_done.has_value());
    }
}
TEST(Draft23Session, FlagsAReusedRequestId) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session);
    session.on_event(transport::StreamDataEvent{4, bytes({6,9,0,1,5,'m','e','d','i','a',0}), false});
    EXPECT_TRUE(session.error());
    EXPECT_TRUE(session.stats().duplicate_request_id);
}
TEST(Draft23Session, FlagsAControlMessageLongerThanTheAdvertisedLimit) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session);
    session.on_event(transport::StreamDataEvent{2, bytes({0xaf,0,0xc1,0,0}), false});
    EXPECT_TRUE(session.error());
    EXPECT_TRUE(session.stats().oversize_control_message);
}
TEST(Draft23Session, PublishDoneAndFinInOneEventIsRecordedInOrder) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session); subscribed(session);
    session.on_event(transport::StreamDataEvent{1, bytes({0x0b,3,2,2,0}), true});
    const session::Draft23RequestStream* subscribe = nullptr;
    for (const auto& request : session.stats().requests) if (request.stream_id == 1) subscribe = &request;
    ASSERT_NE(subscribe, nullptr);
    ASSERT_TRUE(subscribe->publish_done.has_value());
    EXPECT_EQ(subscribe->publish_done->stream_count, 2u);
    EXPECT_TRUE(subscribe->fin);
    EXPECT_TRUE(subscribe->fin_after_publish_done);
    EXPECT_FALSE(session.error());
}
}
