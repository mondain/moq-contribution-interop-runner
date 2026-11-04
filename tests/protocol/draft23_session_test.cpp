#include "moq/interop/session/draft23_session.h"
#include "moq/interop/session/draft23_probes.h"
#include "moq/interop/wire/draft23/setup.h"
#include "moq/interop/wire/draft23/messages.h"

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
    std::vector<std::vector<std::byte>> datagrams;
    std::optional<std::uint64_t> closed;
    std::size_t allowance{65535};
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::Success, 1}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, 3}; }
    transport::OperationResult write(std::uint64_t id, std::span<const std::byte> data, bool fin) override {
        if (data.empty() && fin) { writes.push_back({id, {}, true}); return {transport::TransportStatus::Success, 0, {}}; }
        const auto count = std::min(data.size(), allowance);
        if (!count) return {transport::TransportStatus::WouldBlock, 0, {}};
        writes.push_back({id, {data.begin(), data.begin() + static_cast<std::ptrdiff_t>(count)}, fin && count == data.size()});
        return {count == data.size() ? transport::TransportStatus::Success : transport::TransportStatus::Partial, count, {}};
    }
    transport::OperationResult reset(std::uint64_t, std::uint64_t) override { return {}; }
    transport::OperationResult stop_sending(std::uint64_t, std::uint64_t) override { return {}; }
    transport::OperationResult send_datagram(std::span<const std::byte> data) override {
        datagrams.emplace_back(data.begin(), data.end());
        return {transport::TransportStatus::Success, data.size(), {}};
    }
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
session::Draft23SessionConfig probe_config(std::string_view scenario) {
    auto result = config();
    result.probe = session::find_draft23_probe(scenario);
    return result;
}
TEST(Draft23Session, OrdinarySessionsCarryNoProbeResult) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    EXPECT_FALSE(session.stats().probe.has_value());
}
TEST(Draft23Session, BidiProbeIsWrittenOnANewStreamOnlyAfterThePublishersSetup) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-unknown-control-message"));
    connect(session);
    ASSERT_TRUE(session.stats().probe.has_value());
    EXPECT_FALSE(session.stats().probe->injected);
    const auto before = transport.writes.size();
    setup(session);
    EXPECT_TRUE(session.stats().probe->injected);
    ASSERT_EQ(transport.writes.size(), before + 1);
    EXPECT_EQ(transport.writes.back().id, 1u);
    EXPECT_EQ(transport.writes.back().data, bytes({0x3f, 0}));
    EXPECT_FALSE(transport.writes.back().fin);
}
TEST(Draft23Session, UniProbeUsesAUnidirectionalStream) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-unknown-uni-stream"));
    connect(session); setup(session);
    EXPECT_TRUE(session.stats().probe->injected);
    EXPECT_EQ(transport.writes.back().data, bytes({0x0a}));
}
TEST(Draft23Session, OversizeProbeFollowsThePublishersAdvertisedLimit) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-oversize-control-message"));
    connect(session);
    // SETUP carrying MAX_CONTROL_MESSAGE_SIZE (option 9) = 70000 (vi64 c1 11 70).
    session.on_event(transport::StreamDataEvent{2, bytes({0xaf,0,5, 9,3,0xc1,0x11,0x70}), false});
    ASSERT_TRUE(session.stats().probe->injected);
    // 70001 = 0x11171 -> c1 11 71
    EXPECT_EQ(transport.writes.back().data, bytes({6, 0xc1, 0x11, 0x71}));
}
TEST(Draft23Session, ServerOptionProbesChangeTheRunnersOwnSetupAndInjectAtConnect) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-server-authority"));
    connect(session);
    ASSERT_FALSE(transport.writes.empty());
    wire::Cursor cursor(transport.writes[0].data);
    const auto decoded = wire::draft23::decode_setup(cursor);
    ASSERT_TRUE(std::holds_alternative<wire::draft23::SetupMessage>(decoded));
    bool authority = false;
    for (const auto& option : std::get<wire::draft23::SetupMessage>(decoded).options) authority = authority || option.type == 5;
    EXPECT_TRUE(authority);
    EXPECT_TRUE(session.stats().probe->injected);
}
TEST(Draft23Session, RecordsHowThePublisherClosedTheSession) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-unknown-control-message"));
    connect(session); setup(session);
    session.on_event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x303, {}});
    ASSERT_TRUE(session.stats().probe->close.has_value());
    EXPECT_TRUE(session.stats().probe->close->application);
    EXPECT_EQ(session.stats().probe->close->code, 0x303u);
    EXPECT_FALSE(transport.closed.has_value());  // the runner does not answer with a close of its own
}
TEST(Draft23Session, KeepsTheFirstCloseAndTellsTransportClosesApart) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-unknown-control-message"));
    connect(session); setup(session);
    session.on_event(transport::PeerCloseEvent{transport::CloseErrorSpace::Transport, 0x0a, {}});
    session.on_event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0x303, {}});
    EXPECT_FALSE(session.stats().probe->close->application);
    EXPECT_EQ(session.stats().probe->close->code, 0x0au);
}
TEST(Draft23Session, RecordsWhatThePublisherSendsBackOnTheProbeStream) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-unknown-control-message"));
    connect(session); setup(session);
    ASSERT_TRUE(session.stats().probe->injected);
    EXPECT_EQ(session.stats().probe->reply_bytes, 0u);
    EXPECT_FALSE(session.stats().probe->reply_fin);
    session.on_event(transport::StreamDataEvent{1, bytes({5, 3, 0x20, 0, 0}), false});
    session.on_event(transport::StreamDataEvent{1, {}, true});
    EXPECT_EQ(session.stats().probe->reply_bytes, 5u);
    EXPECT_TRUE(session.stats().probe->reply_fin);
}
TEST(Draft23Session, DataOnOtherStreamsIsNotCountedAsAProbeReply) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-unknown-control-message"));
    connect(session); setup(session); announce(session);
    EXPECT_EQ(session.stats().probe->reply_bytes, 0u);
}
std::vector<std::byte> text_bytes(std::string_view value) {
    const auto view = std::as_bytes(std::span(value));
    return {view.begin(), view.end()};
}
std::vector<std::byte> encoded(const wire::draft23::Message& message) {
    wire::ByteWriter writer(4096);
    EXPECT_TRUE(wire::draft23::encode_message(message, writer));
    const auto out = writer.bytes();
    return {out.begin(), out.end()};
}
TEST(Draft23Session, RecordsEachPublishedTrackWithItsNamespaceNameAndAlias) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    wire::draft23::PublishMessage publish{2, {text_bytes("media")}, text_bytes("vide"), 7, {}, {}};
    session.on_event(transport::StreamDataEvent{4, encoded(publish), false});
    ASSERT_EQ(session.stats().published_tracks.size(), 1u);
    EXPECT_EQ(session.stats().published_tracks[0].namespace_first, "media");
    EXPECT_EQ(session.stats().published_tracks[0].name, "vide");
    EXPECT_EQ(session.stats().published_tracks[0].alias, 7u);
}
TEST(Draft23Session, RecordsTheFirstFieldOfEveryAnnouncedNamespace) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    wire::draft23::PublishNamespaceMessage dot{0, {text_bytes("."), text_bytes("x")}, {}};
    session.on_event(transport::StreamDataEvent{0, encoded(dot), false});
    ASSERT_EQ(session.stats().announced_namespaces.size(), 1u);
    EXPECT_EQ(session.stats().announced_namespaces[0], ".");
}
TEST(Draft23Session, FlagsAnEmptyNamespaceField) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{0, bytes({6,4,0,1,0,0}), false});
    EXPECT_TRUE(session.error());
    EXPECT_TRUE(session.stats().empty_namespace_field);
}
TEST(Draft23Session, FlagsADuplicateMessageParameter) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session);
    // SUBSCRIBE_OK, alias 7, two EXPIRES parameters (type 8 then delta 0), no properties.
    session.on_event(transport::StreamDataEvent{1, bytes({4,7,7,2,8,5,0,6,0}), false});
    EXPECT_TRUE(session.error());
    EXPECT_TRUE(session.stats().duplicate_parameter);
}
TEST(Draft23Session, RecordsTheTypeOfAFirstMessageEvenWhenItCannotBeDecoded) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{0, bytes({0x3f,0}), false});
    EXPECT_TRUE(session.error());
    ASSERT_FALSE(session.stats().requests.empty());
    ASSERT_FALSE(session.stats().requests[0].messages.empty());
    EXPECT_EQ(session.stats().requests[0].messages[0].type, 0x3fu);
    EXPECT_EQ(session.stats().requests[0].messages.size(), 1u);
}
TEST(Draft23Session, DoesNotRecordAPartialMessageTwice) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{0, bytes({6,9,0}), false});
    session.on_event(transport::StreamDataEvent{0, bytes({1,5,'m','e','d','i','a',0}), false});
    ASSERT_FALSE(session.stats().requests.empty());
    EXPECT_EQ(session.stats().requests[0].messages.size(), 1u);
}
TEST(Draft23Session, FlagsAClosedControlStream) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    EXPECT_FALSE(session.stats().control_stream_closed);
    session.on_event(transport::StreamDataEvent{2, {}, true});
    EXPECT_TRUE(session.stats().control_stream_closed);
}
TEST(Draft23Session, KeepsTheNegotiatedDatagramCapacity) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session);
    EXPECT_EQ(session.stats().max_datagram_payload, 1200u);
}
TEST(Draft23Session, CountsPaddingStreamsAndSpotsNonZeroPadding) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session);
    wire::ByteWriter type(8);
    ASSERT_TRUE(wire::write_vi64(0x132b3e28, type));
    std::vector<std::byte> zeros(type.bytes().begin(), type.bytes().end());
    zeros.insert(zeros.end(), 16, std::byte{0});
    session.on_event(transport::StreamDataEvent{6, zeros, true});
    EXPECT_EQ(session.stats().padding_streams, 1u);
    EXPECT_FALSE(session.stats().padding_nonzero);
    auto dirty = std::vector<std::byte>(type.bytes().begin(), type.bytes().end());
    dirty.push_back(std::byte{0}); dirty.push_back(std::byte{1});
    session.on_event(transport::StreamDataEvent{10, dirty, true});
    EXPECT_EQ(session.stats().padding_streams, 2u);
    EXPECT_TRUE(session.stats().padding_nonzero);
}
TEST(Draft23Session, FlagsAnObjectIdDeltaThatOverflows) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session); subscribed(session);
    wire::ByteWriter stream(32);
    for (const auto b : bytes({0x30, 7, 0, 0, 1, 'a'})) ASSERT_TRUE(stream.append_bytes(std::span(&b, 1)));
    ASSERT_TRUE(wire::write_vi64(~std::uint64_t{0}, stream));  // delta 2^64-1 after Object 0
    ASSERT_TRUE(stream.append_bytes(bytes({1, 'b'})));
    session.on_event(transport::StreamDataEvent{6, {stream.bytes().begin(), stream.bytes().end()}, false});
    EXPECT_TRUE(session.error());
    EXPECT_TRUE(session.stats().delta_overflow);
}
TEST(Draft23Session, ParsesARequestErrorReplyOnTheProbeStream) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-subscribe-reserved-namespace"));
    connect(session); setup(session);
    ASSERT_TRUE(session.stats().probe->injected);
    EXPECT_FALSE(session.stats().probe->reply.has_value());
    // REQUEST_ERROR: DOES_NOT_EXIST, retry 0, empty reason, then Error Detail type 1 (REDIRECT) twice (delta 1, then 0).
    session.on_event(transport::StreamDataEvent{1, bytes({5, 9, 0x10, 0, 0, 1, 1, 'a', 0, 1, 'b'}), false});
    ASSERT_TRUE(session.stats().probe->reply.has_value());
    EXPECT_EQ(session.stats().probe->reply->type, 5u);
    EXPECT_EQ(session.stats().probe->reply->error_code, std::optional<std::uint64_t>{0x10});
    EXPECT_EQ(session.stats().probe->reply->detail_types, (std::vector<std::uint64_t>{1, 1}));
}
TEST(Draft23Session, AReplyThatIsNotARequestErrorStillRecordsItsType) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-subscribe-reserved-namespace"));
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{1, bytes({4, 2, 7, 0}), false});
    ASSERT_TRUE(session.stats().probe->reply.has_value());
    EXPECT_EQ(session.stats().probe->reply->type, 4u);
    EXPECT_FALSE(session.stats().probe->reply->error_code.has_value());
}
TEST(Draft23Session, AReplySplitAcrossEventsIsParsedOnceComplete) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-subscribe-reserved-namespace"));
    connect(session); setup(session);
    session.on_event(transport::StreamDataEvent{1, bytes({5, 3, 0x10}), false});
    EXPECT_FALSE(session.stats().probe->reply.has_value());
    session.on_event(transport::StreamDataEvent{1, bytes({0, 0}), false});
    ASSERT_TRUE(session.stats().probe->reply.has_value());
    EXPECT_EQ(session.stats().probe->reply->error_code, std::optional<std::uint64_t>{0x10});
}
TEST(Draft23Session, ARejectionByResetIsRecordedAndDoesNotEndTheProbe) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-subscribe-reserved-namespace"));
    connect(session); setup(session);
    session.on_event(transport::PeerResetEvent{1, std::uint64_t{0x30}});
    EXPECT_FALSE(session.error());
    ASSERT_TRUE(session.stats().probe->stream_end.has_value());
    EXPECT_EQ(session.stats().probe->stream_end->kind, session::Draft23ProbeStreamEnd::Kind::Reset);
    EXPECT_EQ(session.stats().probe->stream_end->code, std::optional<std::uint64_t>{0x30});
    // The publisher may still close the session afterwards, and that close is still recorded.
    session.on_event(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 0, {}});
    ASSERT_TRUE(session.stats().probe->close.has_value());
}
TEST(Draft23Session, AStopSendingOnTheProbeStreamIsRecordedToo) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-subscribe-reserved-namespace"));
    connect(session); setup(session);
    session.on_event(transport::PeerStopSendingEvent{1, std::uint64_t{0x31}});
    EXPECT_FALSE(session.error());
    ASSERT_TRUE(session.stats().probe->stream_end.has_value());
    EXPECT_EQ(session.stats().probe->stream_end->kind, session::Draft23ProbeStreamEnd::Kind::StopSending);
}
TEST(Draft23Session, AResetOnARequestStreamEndsAnOrdinarySession) {
    Transport transport;
    session::Draft23Session session(transport, config());
    connect(session); setup(session); announce(session);
    session.on_event(transport::PeerResetEvent{0, std::uint64_t{1}});
    EXPECT_TRUE(session.error());
}
TEST(Draft23Session, AProbeRecordsAResetOfTheirOwnRequestStreamAndKeepsWatching) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-subscribe-reserved-namespace"));
    connect(session); setup(session); announce(session);
    session.on_event(transport::PeerResetEvent{0, std::uint64_t{0}});
    EXPECT_FALSE(session.error());
    ASSERT_EQ(session.stats().probe->other_stream_ends.size(), 1u);
    EXPECT_EQ(session.stats().probe->other_stream_ends[0].stream_id, 0u);
    EXPECT_EQ(session.stats().probe->other_stream_ends[0].end.kind, session::Draft23ProbeStreamEnd::Kind::Reset);
    // A reset of the control stream still ends it.
    session.on_event(transport::PeerResetEvent{2, std::uint64_t{0}});
    EXPECT_TRUE(session.error());
}
std::vector<std::byte> payload_of(std::string_view scenario, std::size_t index = 0) {
    return session::draft23_probe_payloads(*session::find_draft23_probe(scenario), 65535).at(index);
}
TEST(Draft23Session, SetupBytesProbeReplacesTheRunnersWholeSetup) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-setup-delta-overflow"));
    connect(session);
    ASSERT_FALSE(transport.writes.empty());
    EXPECT_EQ(transport.writes[0].id, 3u);
    EXPECT_EQ(transport.writes[0].data, payload_of("d23-probe-setup-delta-overflow"));
    EXPECT_TRUE(session.stats().probe->injected);
}
TEST(Draft23Session, ControlFinProbeFinishesTheRunnersControlStreamAfterThePublishersSetup) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-control-stream-closed"));
    connect(session);
    EXPECT_FALSE(session.stats().probe->injected);
    setup(session);
    EXPECT_TRUE(session.stats().probe->injected);
    bool fin_on_control = false;
    for (const auto& write : transport.writes) fin_on_control = fin_on_control || (write.id == 3 && write.fin && write.data.empty());
    EXPECT_TRUE(fin_on_control);
}
TEST(Draft23Session, DatagramProbeSendsOneDatagramAfterTheSetup) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-datagram-reserved-bit"));
    connect(session);
    EXPECT_TRUE(transport.datagrams.empty());
    setup(session);
    ASSERT_EQ(transport.datagrams.size(), 1u);
    EXPECT_EQ(transport.datagrams[0], payload_of("d23-probe-datagram-reserved-bit"));
    EXPECT_TRUE(session.stats().probe->injected);
}
TEST(Draft23Session, EveryPayloadOfABidiProbeGetsItsOwnStream) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-duplicate-request-id"));
    connect(session);
    const auto before = transport.writes.size();
    setup(session);
    EXPECT_EQ(transport.writes.size(), before + 2);
}
TEST(Draft23Session, AnnounceReplyProbeAnswersTheAnnouncementWithTheProbeInsteadOfRequestOk) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-redirect-without-detail"));
    connect(session); setup(session);
    EXPECT_FALSE(session.stats().probe->injected);
    const auto before = transport.writes.size();
    announce(session);
    ASSERT_GT(transport.writes.size(), before);
    EXPECT_EQ(transport.writes[before].id, 0u);
    EXPECT_EQ(transport.writes[before].data, payload_of("d23-probe-redirect-without-detail"));
    EXPECT_TRUE(session.stats().probe->injected);
    for (std::size_t i = before; i < transport.writes.size(); ++i) EXPECT_NE(transport.writes[i].data, bytes({7, 2, 0, 0}));  // no REQUEST_OK
}
TEST(Draft23Session, AnnounceExtraProbeFollowsTheRequestOkOnTheSameStream) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-publish-state-notify-on-announce"));
    connect(session); setup(session);
    const auto before = transport.writes.size();
    announce(session);
    ASSERT_GE(transport.writes.size(), before + 2);
    EXPECT_EQ(transport.writes[before].id, 0u);
    EXPECT_EQ(transport.writes[before + 1].id, 0u);
    EXPECT_EQ(transport.writes[before + 1].data, payload_of("d23-probe-publish-state-notify-on-announce"));
}
TEST(Draft23Session, SubscribeExtraProbeWaitsForSubscribeOk) {
    Transport transport;
    auto settings = probe_config("d23-probe-publish-state-notify-to-publisher");
    settings.subscribe_immediately = true;
    session::Draft23Session session(transport, settings);
    connect(session); setup(session);
    EXPECT_FALSE(session.stats().probe->injected);
    subscribed(session);
    EXPECT_TRUE(session.stats().probe->injected);
    EXPECT_EQ(transport.writes.back().id, 1u);
    EXPECT_EQ(transport.writes.back().data, payload_of("d23-probe-publish-state-notify-to-publisher"));
}
TEST(Draft23Session, ControlBytesProbeWritesOnTheRunnersControlStreamAfterTheSetup) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-goaway-twice"));
    connect(session);
    EXPECT_FALSE(session.stats().probe->injected);
    setup(session);
    EXPECT_TRUE(session.stats().probe->injected);
    EXPECT_EQ(transport.writes.back().id, 3u);
    EXPECT_EQ(transport.writes.back().data, payload_of("d23-probe-goaway-twice"));
}
TEST(Draft23Session, FilterRangeProbeSizesItsInputFromThePublishersAdvertisedLimit) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-filter-ranges-over-limit"));
    connect(session);
    // SETUP with MAX_FILTER_RANGES (option 6, an even type so a plain varint) = 2.
    session.on_event(transport::StreamDataEvent{2, bytes({0xaf,0,2, 6,2}), false});
    ASSERT_TRUE(session.stats().probe->injected);
    session::Draft23PeerLimits limits;
    limits.filter_ranges = 2;
    limits.track_namespace = config().track_namespace;  // the session names the track it was configured with
    limits.track_name = config().track_name;
    EXPECT_EQ(transport.writes.back().data,
              session::draft23_probe_payloads(*session::find_draft23_probe("d23-probe-filter-ranges-over-limit"), limits).front());
    EXPECT_NE(transport.writes.back().data, payload_of("d23-probe-filter-ranges-over-limit"));  // the default limit is 0
}
TEST(Draft23Session, PublishReplyProbeReplacesTheRejectionOfADeclinedTrack) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-unknown-request-error-code"));
    connect(session); setup(session);
    EXPECT_FALSE(session.stats().probe->injected);
    wire::draft23::PublishMessage other{2, {text_bytes("media")}, text_bytes("catalog"), 9, {}, {}};  // not the harness's track
    const auto before = transport.writes.size();
    session.on_event(transport::StreamDataEvent{4, encoded(other), false});
    EXPECT_TRUE(session.stats().probe->injected);
    ASSERT_GT(transport.writes.size(), before);
    EXPECT_EQ(transport.writes[before].id, 4u);
    EXPECT_EQ(transport.writes[before].data, payload_of("d23-probe-unknown-request-error-code"));
    EXPECT_TRUE(transport.writes[before].fin);
}
TEST(Draft23Session, PublishReplyProbeWaitsForADeclinedTrackAndIgnoresTheHarnessOwn) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-unknown-request-error-code"));
    connect(session); setup(session);
    wire::draft23::PublishMessage own{2, {text_bytes("media")}, text_bytes("vide"), 7, {}, {}};  // the configured track
    session.on_event(transport::StreamDataEvent{4, encoded(own), false});
    EXPECT_FALSE(session.stats().probe->injected);
}
TEST(Draft23Session, SubscribeProbesNameTheConfiguredTrack) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-wrong-request-id-parity"));
    connect(session); setup(session);
    wire::Cursor cursor(transport.writes.back().data);
    const auto message = wire::draft23::decode_message(cursor);
    ASSERT_TRUE(std::holds_alternative<wire::draft23::Message>(message));
    const auto& subscribe = std::get<wire::draft23::SubscribeMessage>(std::get<wire::draft23::Message>(message));
    EXPECT_EQ(subscribe.track_namespace, config().track_namespace);
    EXPECT_EQ(subscribe.track_name, config().track_name);
}
TEST(Draft23Session, PublishExtraProbeFollowsTheRequestOkToTheFixturePublish) {
    Transport transport;
    session::Draft23Session session(transport, probe_config("d23-probe-second-publish-response"));
    connect(session); setup(session);
    wire::draft23::PublishMessage own{2, {text_bytes("media")}, text_bytes("vide"), 7, {}, {}};
    const auto before = transport.writes.size();
    session.on_event(transport::StreamDataEvent{4, encoded(own), false});
    ASSERT_GE(transport.writes.size(), before + 2);
    EXPECT_EQ(transport.writes[before + 1].id, 4u);
    EXPECT_EQ(transport.writes[before + 1].data, payload_of("d23-probe-second-publish-response"));
    EXPECT_TRUE(session.stats().probe->injected);
}
}
