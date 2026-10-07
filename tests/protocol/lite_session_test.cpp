#include "moq/interop/session/lite_session.h"
#include "moq/interop/session/lite_stream_reader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "moq/interop/transport/session_transport.h"
#include "moq/interop/wire/moqlite06/announce.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/group.h"
#include "moq/interop/wire/moqlite06/setup.h"
#include "moq/interop/wire/moqlite06/subscribe.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::session {
namespace {

using transport::CloseErrorSpace;
using transport::ConnectionEstablishedEvent;
using transport::EventQueueOverflowEvent;
using transport::PeerCloseEvent;
using transport::PeerResetEvent;
using transport::PeerStopSendingEvent;
using transport::StreamDataEvent;
using wire::moqlite06::AnnounceEnd;
using wire::moqlite06::AnnounceOk;
using wire::moqlite06::AnnounceRequest;
using wire::moqlite06::AnnounceStart;
using wire::moqlite06::AnnounceUpdate;
using wire::moqlite06::DecodeLimits;
using wire::moqlite06::Frame;
using wire::moqlite06::GroupHeader;
using wire::moqlite06::kMaxFrameDelta;
using wire::moqlite06::kMaxVarint;
using wire::moqlite06::kMinFrameDelta;
using wire::moqlite06::RouteMetadata;
using wire::moqlite06::SetupMessage;
using wire::moqlite06::SetupParameter;
using wire::moqlite06::Subscribe;
using wire::moqlite06::SubscribeDrop;
using wire::moqlite06::SubscribeEnd;
using wire::moqlite06::SubscribeOk;
using wire::moqlite06::SubscribeRange;
using wire::moqlite06::SubscribeUpdate;

using Bytes = std::vector<std::byte>;

// Stream ids (RFC 9000 2.1): bit 0 set = server (runner) initiated, bit 1 set = unidirectional.
constexpr std::uint64_t kPeerBidi = 0;
constexpr std::uint64_t kRunnerBidi = 1;
constexpr std::uint64_t kPeerUni = 2;
constexpr std::uint64_t kRunnerUni = 3;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

Bytes concat(std::initializer_list<Bytes> parts) {
    Bytes result;
    for (const auto& part : parts) result.insert(result.end(), part.begin(), part.end());
    return result;
}

Bytes repeat(unsigned value, std::size_t count) { return Bytes(count, static_cast<std::byte>(value)); }

Bytes text(std::string_view value) {
    Bytes result;
    for (const char c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}

bool same_setup(const SetupMessage& a, const SetupMessage& b) {
    if (a.parameters.size() != b.parameters.size()) return false;
    for (std::size_t i = 0; i < a.parameters.size(); ++i) {
        if (a.parameters[i].id != b.parameters[i].id || a.parameters[i].value != b.parameters[i].value) return false;
    }
    return true;
}

bool same(const LiteMessage& a, const LiteMessage& b) {
    if (a.index() != b.index()) return false;
    return std::visit(
        [&](const auto& left) {
            using T = std::decay_t<decltype(left)>;
            const auto& right = std::get<T>(b);
            if constexpr (std::is_same_v<T, SetupMessage>) {
                return same_setup(left, right);
            } else {
                return left == right;
            }
        },
        a);
}

std::size_t count_issues(const LiteStreamRecord& record, std::string_view code) {
    return static_cast<std::size_t>(std::count_if(record.issues.begin(), record.issues.end(),
                                                  [&](const auto& issue) { return issue.code == code; }));
}

std::string describe_issues(const LiteStreamRecord& record) {
    std::string out;
    for (const auto& issue : record.issues) out += issue.code + "(" + issue.detail + ") ";
    return out;
}

bool is_known_issue_code(std::string_view code) {
    return std::find(kAllIssueCodes.begin(), kAllIssueCodes.end(), code) != kAllIssueCodes.end();
}

StreamDataEvent data_event(std::uint64_t id, Bytes data, bool fin = false) {
    StreamDataEvent event;
    event.stream_id = id;
    event.data = std::move(data);
    event.fin = fin;
    return event;
}

// ---- the valid vectors, copied from tests/golden/moqlite06_*_test.cpp -----------------------------------------

// Where a vector travels. PeerUni: the peer opens a unidirectional stream (the bytes carry the STREAM_TYPE).
// Local: the runner writes on a runner-opened bidirectional stream (STREAM_TYPE included). PeerResponse: the
// runner first writes `local` on a runner-opened bidirectional stream, then the peer answers with the bytes.
enum class Path { PeerUni, Local, PeerResponse };

struct Vector {
    std::string name;
    Path path;
    Bytes local;      // PeerResponse only: the runner's STREAM_TYPE and request, written whole first
    Bytes stream;     // the bytes delivered in chunks: any preamble the vector needs, then the vector itself
    LiteMessage expected;
    std::size_t expected_count;  // messages in the record once everything is delivered
    LiteStreamKind kind;
};

const Bytes kGroupPreamble = bytes({0x00, 0x03, 0x01, 0x07, 0x00});  // Group stream type, GROUP{1, 7, 0}
const Bytes kAnnounceLocal = bytes({0x01, 0x02, 0x01, 0x61});      // Announce stream type, ANNOUNCE_REQUEST "a"
const Bytes kAnnounceOkWire = bytes({0x02, 0x00, 0x00});           // ANNOUNCE_OK{0, 0}
const Bytes kSubscribeMinimal = bytes({0x0b, 0x00, 0x01, 0x62, 0x01, 0x74, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00});
const Bytes kSubscribeLocal = concat({bytes({0x02}), kSubscribeMinimal});

Vector setup(std::string name, Bytes wire, SetupMessage message) {
    return {std::move(name), Path::PeerUni, {}, concat({bytes({0x01}), wire}), std::move(message), 1,
            LiteStreamKind::Setup};
}
Vector group(std::string name, Bytes wire, GroupHeader header) {
    return {std::move(name), Path::PeerUni, {}, concat({bytes({0x00}), wire}), header, 1, LiteStreamKind::Group};
}
Vector frame(std::string name, Bytes wire, Frame value) {
    return {std::move(name), Path::PeerUni, {}, concat({kGroupPreamble, wire}), std::move(value), 2,
            LiteStreamKind::Group};
}
Vector announce_request(std::string name, Bytes wire, AnnounceRequest value) {
    return {std::move(name), Path::Local, {}, concat({bytes({0x01}), wire}), std::move(value), 1,
            LiteStreamKind::Announce};
}
Vector announce_ok(std::string name, Bytes wire, AnnounceOk value) {
    return {std::move(name), Path::PeerResponse, kAnnounceLocal, wire, value, 2, LiteStreamKind::Announce};
}
Vector announce_message(std::string name, Bytes wire, LiteMessage value) {
    return {std::move(name), Path::PeerResponse, kAnnounceLocal, concat({kAnnounceOkWire, wire}), std::move(value), 3,
            LiteStreamKind::Announce};
}
Vector subscribe(std::string name, Bytes wire, Subscribe value) {
    return {std::move(name), Path::Local, {}, concat({bytes({0x02}), wire}), std::move(value), 1,
            LiteStreamKind::Subscribe};
}
Vector subscribe_update(std::string name, Bytes wire, SubscribeUpdate value) {
    return {std::move(name), Path::Local, {}, concat({kSubscribeLocal, wire}), value, 2, LiteStreamKind::Subscribe};
}
Vector subscribe_response(std::string name, Bytes wire, LiteMessage value) {
    return {std::move(name), Path::PeerResponse, kSubscribeLocal, wire, std::move(value), 2,
            LiteStreamKind::Subscribe};
}

const std::vector<Vector>& vectors() {
    static const std::vector<Vector> table = {
        // moqlite06_setup_test.cpp
        setup("setup_cost_100", bytes({0x05, 0x01, 0x04, 0x02, 0x40, 0x64}),
              SetupMessage{{SetupParameter{4, bytes({0x40, 0x64})}}}),
        setup("setup_empty", bytes({0x01, 0x00}), SetupMessage{}),
        setup("setup_all_known",
              bytes({0x12, 0x05, 0x01, 0x01, 0x02, 0x02, 0x02, 0x2f, 0x61, 0x03, 0x01, 0x01, 0x04, 0x02, 0x40, 0x64,
                     0x05, 0x01, 0x07}),
              SetupMessage{{SetupParameter{1, bytes({0x02})}, SetupParameter{2, bytes({0x2f, 0x61})},
                            SetupParameter{3, bytes({0x01})}, SetupParameter{4, bytes({0x40, 0x64})},
                            SetupParameter{5, bytes({0x07})}}}),
        setup("setup_unknown_ids_kept",
              bytes({0x0c, 0x03, 0x04, 0x01, 0x01, 0x40, 0x7f, 0x02, 0xaa, 0xbb, 0x01, 0x01, 0x01}),
              SetupMessage{{SetupParameter{4, bytes({0x01})}, SetupParameter{0x7f, bytes({0xaa, 0xbb})},
                            SetupParameter{1, bytes({0x01})}}}),
        // moqlite06_group_test.cpp
        group("group_frame_start_0", bytes({0x03, 0x01, 0x07, 0x00}), GroupHeader{1, 7, 0}),
        group("group_frame_start_3", bytes({0x03, 0x01, 0x07, 0x03}), GroupHeader{1, 7, 3}),
        group("group_multibyte", bytes({0x08, 0x40, 0x40, 0x41, 0x2c, 0x80, 0x00, 0x40, 0x00}),
              GroupHeader{64, 300, 16384}),
        group("group_max", concat({bytes({0x18}), repeat(0xff, 24)}), GroupHeader{kMaxVarint, kMaxVarint, kMaxVarint}),
        frame("frame_delta_0", bytes({0x00, 0x03, 0x61, 0x62, 0x63}), Frame{0, text("abc")}),
        frame("frame_delta_1", bytes({0x02, 0x03, 0x61, 0x62, 0x63}), Frame{1, text("abc")}),
        frame("frame_delta_minus_1", bytes({0x01, 0x03, 0x61, 0x62, 0x63}), Frame{-1, text("abc")}),
        frame("frame_delta_33_two_byte_varint", bytes({0x40, 0x42, 0x03, 0x61, 0x62, 0x63}), Frame{33, text("abc")}),
        frame("frame_delta_minus_33_empty", bytes({0x40, 0x41, 0x00}), Frame{-33, {}}),
        frame("frame_empty_payload", bytes({0x00, 0x00}), Frame{0, {}}),
        frame("frame_first_absolute_1000", bytes({0x47, 0xd0, 0x01, 0x61}), Frame{1000, text("a")}),
        frame("frame_max_positive_delta", bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0x00}),
              Frame{kMaxFrameDelta, {}}),
        frame("frame_min_negative_delta", bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00}),
              Frame{kMinFrameDelta, {}}),
        frame("frame_payload_64", concat({bytes({0x00, 0x40, 0x40}), repeat(0xaa, 64)}), Frame{0, repeat(0xaa, 64)}),
        // moqlite06_announce_test.cpp
        announce_request("announce_request_a", bytes({0x02, 0x01, 0x61}), AnnounceRequest{"a"}),
        announce_request("announce_request_empty", bytes({0x01, 0x00}), AnnounceRequest{""}),
        announce_ok("announce_ok_zero", bytes({0x02, 0x00, 0x00}), AnnounceOk{0, 0}),
        announce_ok("announce_ok_hop_1234", bytes({0x03, 0x52, 0x34, 0x03}), AnnounceOk{0x1234, 3}),
        announce_message("announce_start_two_hops", bytes({0x00, 0x07, 0x01, 0x62, 0x02, 0x07, 0x09, 0x00, 0x01}),
                         AnnounceStart{"b", RouteMetadata{{7, 9}, 0, 1}}),
        announce_message("announce_start_empty_suffix", bytes({0x00, 0x04, 0x00, 0x00, 0x04, 0x05}),
                         AnnounceStart{"", RouteMetadata{{}, 4, 5}}),
        announce_message("announce_start_costs_raw",
                         bytes({0x00, 0x0d, 0x01, 0x62, 0x00, 0x40, 0x64, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
                                0xff}),
                         AnnounceStart{"b", RouteMetadata{{}, 100, kMaxVarint}}),
        announce_message("announce_end", bytes({0x01, 0x01, 0x05}), AnnounceEnd{5}),
        announce_message("announce_update", bytes({0x02, 0x05, 0x05, 0x01, 0x07, 0x02, 0x03}),
                         AnnounceUpdate{5, RouteMetadata{{7}, 2, 3}}),
        // moqlite06_subscribe_test.cpp
        subscribe("subscribe_minimal", kSubscribeMinimal, Subscribe{0, "b", "t", SubscribeRange{0x80, 0, 0, 0, 0, 0}}),
        subscribe("subscribe_priority_ff",
                  bytes({0x0b, 0x00, 0x01, 0x62, 0x01, 0x74, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00}),
                  Subscribe{0, "b", "t", SubscribeRange{0xff, 0, 0, 0, 0, 0}}),
        subscribe("subscribe_all_fields",
                  bytes({0x0e, 0x01, 0x02, 0x61, 0x62, 0x01, 0x63, 0x7f, 0x41, 0x2c, 0x05, 0x44, 0x00, 0x02, 0x03}),
                  Subscribe{1, "ab", "c", SubscribeRange{0x7f, 300, 5, 1024, 2, 3}}),
        subscribe_update("update_minimal", bytes({0x06, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00}),
                         SubscribeUpdate{SubscribeRange{0x10, 0, 0, 0, 0, 0}}),
        subscribe_update("update_all_fields", bytes({0x07, 0xff, 0x40, 0x40, 0x01, 0x02, 0x03, 0x04}),
                         SubscribeUpdate{SubscribeRange{0xff, 64, 1, 2, 3, 4}}),
        subscribe_response("subscribe_ok", bytes({0x00, 0x01, 0x06}), SubscribeOk{6}),
        subscribe_response("subscribe_end_zero", bytes({0x01, 0x01, 0x00}), SubscribeEnd{0}),
        subscribe_response("subscribe_drop", bytes({0x02, 0x03, 0x03, 0x05, 0x00}), SubscribeDrop{3, 5, 0}),
        subscribe_response("subscribe_drop_multibyte",
                           bytes({0x02, 0x08, 0x41, 0x00, 0x41, 0x2c, 0x80, 0x00, 0x40, 0x00}),
                           SubscribeDrop{256, 300, 16384}),
    };
    return table;
}

// Delivers a vector with the stream bytes cut at the given offsets (ascending, strictly inside the stream).
LiteStreamRecord deliver(const Vector& vector, const std::vector<std::size_t>& cuts) {
    const bool peer_uni = vector.path == Path::PeerUni;
    LiteStreamReader reader(peer_uni ? kPeerUni : kRunnerBidi, peer_uni ? LiteOrigin::Peer : LiteOrigin::Runner,
                            !peer_uni);
    std::uint64_t clock = 100;
    if (vector.path == Path::PeerResponse) reader.feed_local(vector.local, false, clock++);
    std::size_t begin = 0;
    auto send = [&](std::size_t end) {
        const std::span<const std::byte> chunk(vector.stream.data() + begin, end - begin);
        if (vector.path == Path::Local) {
            reader.feed_local(chunk, false, clock++);
        } else {
            reader.feed(chunk, false, clock++);
        }
        begin = end;
    };
    for (const auto cut : cuts) send(cut);
    send(vector.stream.size());
    return reader.record();
}

void expect_decoded(const Vector& vector, const LiteStreamRecord& record, const std::string& how) {
    SCOPED_TRACE(vector.name + " " + how);
    EXPECT_TRUE(record.issues.empty()) << describe_issues(record);
    EXPECT_EQ(record.kind, vector.kind);
    ASSERT_EQ(record.messages.size(), vector.expected_count);
    EXPECT_TRUE(same(record.messages.back().message, vector.expected));
    EXPECT_EQ(record.messages.back().from, vector.path == Path::Local ? LiteOrigin::Runner : LiteOrigin::Peer);
}

TEST(LiteStreamReaderProperty, EveryVectorDecodesWhole) {
    for (const auto& vector : vectors()) expect_decoded(vector, deliver(vector, {}), "whole");
}

TEST(LiteStreamReaderProperty, EveryVectorDecodesInOneByteChunks) {
    for (const auto& vector : vectors()) {
        std::vector<std::size_t> cuts;
        for (std::size_t i = 1; i < vector.stream.size(); ++i) cuts.push_back(i);
        const auto record = deliver(vector, cuts);
        expect_decoded(vector, record, "one-byte chunks");
        // The message completes on the event that carried its last byte, never earlier.
        if (!record.messages.empty()) {
            const std::size_t local_events = vector.path == Path::PeerResponse ? 1 : 0;
            EXPECT_EQ(record.messages.back().stream_event_index, local_events + vector.stream.size() - 1)
                << vector.name;
        }
    }
}

TEST(LiteStreamReaderProperty, EveryVectorDecodesSplitAtEveryOffset) {
    for (const auto& vector : vectors()) {
        for (std::size_t cut = 1; cut < vector.stream.size(); ++cut) {
            expect_decoded(vector, deliver(vector, {cut}), "split at " + std::to_string(cut));
        }
    }
}

TEST(LiteStreamReaderProperty, NoMessageBeforeItsLastByte) {
    for (const auto& vector : vectors()) {
        const bool peer_uni = vector.path == Path::PeerUni;
        LiteStreamReader reader(peer_uni ? kPeerUni : kRunnerBidi,
                                peer_uni ? LiteOrigin::Peer : LiteOrigin::Runner, !peer_uni);
        if (vector.path == Path::PeerResponse) reader.feed_local(vector.local, false, 1);
        const std::span<const std::byte> all(vector.stream);
        const auto prefix = all.first(all.size() - 1);
        if (vector.path == Path::Local) {
            reader.feed_local(prefix, false, 2);
        } else {
            reader.feed(prefix, false, 2);
        }
        EXPECT_EQ(reader.record().messages.size(), vector.expected_count - 1) << vector.name;
        EXPECT_TRUE(reader.record().issues.empty()) << vector.name << " " << describe_issues(reader.record());
    }
}

// ---- Setup stream ---------------------------------------------------------------------------------------------

TEST(LiteStreamReader, SetupThenFinInOneEventIsAccepted) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    const auto wire = bytes({0x01, 0x05, 0x01, 0x04, 0x02, 0x40, 0x64});
    reader.feed(wire, true, 7);
    const auto& record = reader.record();
    EXPECT_EQ(record.kind, LiteStreamKind::Setup);
    ASSERT_TRUE(record.stream_type.has_value());
    EXPECT_EQ(*record.stream_type, 1u);
    EXPECT_TRUE(record.fin_seen);
    EXPECT_EQ(record.bytes, wire.size());
    EXPECT_EQ(record.opened_ns, 7u);
    EXPECT_TRUE(record.issues.empty()) << describe_issues(record);
    ASSERT_EQ(record.messages.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<SetupMessage>(record.messages[0].message));
    EXPECT_EQ(record.messages[0].at_ns, 7u);
    EXPECT_EQ(record.messages[0].stream_event_index, 0u);
}

TEST(LiteStreamReader, BytesAfterSetupAreAnIssueAndOnlyCounted) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(bytes({0x01, 0x01, 0x00, 0xee}), false, 1);
    reader.feed(bytes({0x01, 0x00}), true, 2);  // a second SETUP is not decoded
    const auto& record = reader.record();
    EXPECT_EQ(record.messages.size(), 1u);
    EXPECT_EQ(count_issues(record, kIssueTrailingAfterSetup), 1u) << describe_issues(record);
    EXPECT_EQ(record.issues.size(), 1u) << describe_issues(record);
    EXPECT_EQ(record.bytes, 6u);
    EXPECT_TRUE(record.fin_seen);
}

TEST(LiteStreamReader, FinBeforeAnyByte) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed({}, true, 3);
    const auto& record = reader.record();
    EXPECT_EQ(record.kind, LiteStreamKind::Unknown);
    EXPECT_FALSE(record.stream_type.has_value());
    EXPECT_TRUE(record.fin_seen);
    EXPECT_EQ(record.bytes, 0u);
    EXPECT_TRUE(record.messages.empty());
    EXPECT_EQ(count_issues(record, kIssueTruncatedAtFin), 1u) << describe_issues(record);
}

TEST(LiteStreamReader, FinInsideTheStreamTypeVarint) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(bytes({0x40}), true, 3);
    EXPECT_FALSE(reader.record().stream_type.has_value());
    EXPECT_EQ(count_issues(reader.record(), kIssueTruncatedAtFin), 1u);
}

TEST(LiteStreamReader, SetupStreamEndingWithoutSetupIsTruncated) {
    for (const auto& wire : {bytes({0x01}), bytes({0x01, 0x05, 0x01, 0x04})}) {
        LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
        reader.feed(wire, true, 1);
        EXPECT_EQ(reader.record().kind, LiteStreamKind::Setup);
        EXPECT_TRUE(reader.record().messages.empty());
        EXPECT_EQ(count_issues(reader.record(), kIssueTruncatedAtFin), 1u) << describe_issues(reader.record());
    }
}

TEST(LiteStreamReader, DataAfterFinIsAnIssue) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(bytes({0x01, 0x01, 0x00}), true, 1);
    reader.feed(bytes({0x00}), false, 2);
    EXPECT_EQ(reader.record().messages.size(), 1u);
    EXPECT_EQ(count_issues(reader.record(), kIssueTrailingAfterFin), 1u) << describe_issues(reader.record());
    EXPECT_EQ(reader.record().bytes, 4u);
}

TEST(LiteStreamReader, SetupDecodeErrorStopsTheStream) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    // SETUP with a duplicate parameter id (ProtocolViolation in the codec).
    reader.feed(bytes({0x01, 0x07, 0x02, 0x04, 0x01, 0x01, 0x04, 0x01, 0x02}), false, 1);
    reader.feed(bytes({0x01, 0x00}), true, 2);
    EXPECT_TRUE(reader.record().messages.empty());
    EXPECT_EQ(count_issues(reader.record(), kIssueProtocolViolation), 1u) << describe_issues(reader.record());
    EXPECT_EQ(reader.record().issues.size(), 1u) << describe_issues(reader.record());
}

// ---- Group stream ---------------------------------------------------------------------------------------------

TEST(LiteStreamReader, GroupThenSeveralFramesThenFin) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    const auto wire = concat({kGroupPreamble, bytes({0x00, 0x03, 0x61, 0x62, 0x63}), bytes({0x02, 0x00}),
                              bytes({0x02, 0x01, 0x78})});
    reader.feed(wire, true, 9);
    const auto& record = reader.record();
    EXPECT_EQ(record.kind, LiteStreamKind::Group);
    EXPECT_TRUE(record.issues.empty()) << describe_issues(record);
    ASSERT_EQ(record.messages.size(), 4u);
    EXPECT_TRUE(same(record.messages[0].message, GroupHeader{1, 7, 0}));
    EXPECT_TRUE(same(record.messages[1].message, Frame{0, text("abc")}));
    EXPECT_TRUE(same(record.messages[2].message, Frame{1, {}}));  // empty payload frame
    EXPECT_TRUE(same(record.messages[3].message, Frame{1, text("x")}));
    EXPECT_TRUE(record.fin_seen);
}

TEST(LiteStreamReader, GroupWithZeroFramesIsAccepted) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(kGroupPreamble, true, 1);
    EXPECT_EQ(reader.record().messages.size(), 1u);
    EXPECT_TRUE(reader.record().issues.empty()) << describe_issues(reader.record());
}

TEST(LiteStreamReader, FrameSplitMidVarint) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(concat({kGroupPreamble, bytes({0x40})}), false, 1);  // first byte of a 2-byte delta
    EXPECT_EQ(reader.record().messages.size(), 1u);
    reader.feed(bytes({0x42, 0x03, 0x61}), false, 2);  // the delta completes, the payload does not
    EXPECT_EQ(reader.record().messages.size(), 1u);
    reader.feed(bytes({0x62, 0x63}), false, 3);
    ASSERT_EQ(reader.record().messages.size(), 2u);
    EXPECT_TRUE(same(reader.record().messages[1].message, Frame{33, text("abc")}));
    EXPECT_EQ(reader.record().messages[1].at_ns, 3u);
    EXPECT_EQ(reader.record().messages[1].stream_event_index, 2u);
    EXPECT_TRUE(reader.record().issues.empty());
}

TEST(LiteStreamReader, ResetMidFrameKeepsWhatWasDecoded) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(concat({kGroupPreamble, bytes({0x00, 0x03, 0x61})}), false, 1);
    reader.reset(0x10);
    const auto& record = reader.record();
    ASSERT_TRUE(record.reset_code.has_value());
    EXPECT_EQ(*record.reset_code, 0x10u);
    EXPECT_TRUE(record.reset_seen);
    EXPECT_EQ(record.messages.size(), 1u);
    EXPECT_TRUE(record.issues.empty()) << describe_issues(record);  // a reset is not a decode error
    reader.feed(bytes({0x62, 0x63}), false, 2);                      // late bytes: counted only
    EXPECT_EQ(record.messages.size(), 1u);
    EXPECT_EQ(record.bytes, kGroupPreamble.size() + 5u);
}

TEST(LiteStreamReader, FinMidFrameIsTruncated) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(concat({kGroupPreamble, bytes({0x00, 0x03, 0x61})}), true, 1);
    EXPECT_EQ(reader.record().messages.size(), 1u);
    EXPECT_EQ(count_issues(reader.record(), kIssueTruncatedAtFin), 1u) << describe_issues(reader.record());
}

TEST(LiteStreamReader, GroupStreamEndingWithoutGroupIsTruncated) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(bytes({0x00}), true, 1);
    EXPECT_EQ(reader.record().kind, LiteStreamKind::Group);
    EXPECT_EQ(count_issues(reader.record(), kIssueTruncatedAtFin), 1u) << describe_issues(reader.record());
}

TEST(LiteStreamReader, StopSendingIsRecorded) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(kGroupPreamble, false, 1);
    reader.stop_sending(7);
    ASSERT_TRUE(reader.record().stop_sending_code.has_value());
    EXPECT_EQ(*reader.record().stop_sending_code, 7u);
    EXPECT_TRUE(reader.record().stop_sending_seen);
    reader.feed(bytes({0x00, 0x00}), false, 2);  // STOP_SENDING does not stop the peer's direction
    EXPECT_EQ(reader.record().messages.size(), 2u);
}

// ---- unregistered and unexpected streams ----------------------------------------------------------------------

TEST(LiteStreamReader, UnregisteredUniTypeIsIgnoredAndCounted) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(bytes({0x07, 0xaa, 0xbb}), false, 1);
    reader.feed(bytes({0xcc}), true, 2);
    const auto& record = reader.record();
    EXPECT_EQ(record.kind, LiteStreamKind::UnregisteredUni);
    ASSERT_TRUE(record.stream_type.has_value());
    EXPECT_EQ(*record.stream_type, 7u);
    EXPECT_EQ(record.bytes, 4u);
    EXPECT_TRUE(record.messages.empty());
    EXPECT_TRUE(record.issues.empty()) << describe_issues(record);
}

TEST(LiteStreamReader, BidiStreamTypesOnAUniStreamAreUnregistered) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(bytes({0x02, 0x00, 0x01, 0x06}), false, 1);  // Subscribe's number, on a uni stream
    EXPECT_EQ(reader.record().kind, LiteStreamKind::UnregisteredUni);
    EXPECT_TRUE(reader.record().messages.empty());
}

TEST(LiteStreamReader, PeerOpenedBidiIsFlagged) {
    LiteStreamReader reader(kPeerBidi, LiteOrigin::Peer, true);
    reader.feed(bytes({0x01, 0x02, 0x01, 0x61}), false, 1);
    const auto& record = reader.record();
    EXPECT_EQ(record.kind, LiteStreamKind::Unknown);
    ASSERT_TRUE(record.stream_type.has_value());
    EXPECT_EQ(*record.stream_type, 1u);
    EXPECT_TRUE(record.messages.empty());
    EXPECT_EQ(count_issues(record, kIssuePublisherOpenedBidi), 1u) << describe_issues(record);
    EXPECT_EQ(record.bytes, 4u);
}

// ---- runner-opened bidirectional streams ----------------------------------------------------------------------

TEST(LiteStreamReader, AnnounceStreamSkipsUnknownTypeByLengthAndContinues) {
    const auto peer = concat({kAnnounceOkWire, bytes({0x00, 0x07, 0x01, 0x62, 0x02, 0x07, 0x09, 0x00, 0x01}),
                              bytes({0x04, 0x02, 0xaa, 0xbb}),  // unknown Type 4, Message Length 2
                              bytes({0x01, 0x01, 0x05}), bytes({0x02, 0x05, 0x05, 0x01, 0x07, 0x02, 0x03})});
    for (const bool one_byte : {false, true}) {
        SCOPED_TRACE(one_byte ? "one-byte chunks" : "whole");
        LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
        reader.feed_local(kAnnounceLocal, false, 1);
        if (one_byte) {
            for (std::size_t i = 0; i < peer.size(); ++i) reader.feed(std::span(peer).subspan(i, 1), false, 2 + i);
        } else {
            reader.feed(peer, false, 2);
        }
        const auto& record = reader.record();
        EXPECT_EQ(record.kind, LiteStreamKind::Announce);
        ASSERT_EQ(record.messages.size(), 5u) << describe_issues(record);
        EXPECT_TRUE(same(record.messages[0].message, AnnounceRequest{"a"}));
        EXPECT_EQ(record.messages[0].from, LiteOrigin::Runner);
        EXPECT_TRUE(same(record.messages[1].message, AnnounceOk{0, 0}));
        EXPECT_EQ(record.messages[1].from, LiteOrigin::Peer);
        EXPECT_TRUE(same(record.messages[2].message, AnnounceStart{"b", RouteMetadata{{7, 9}, 0, 1}}));
        EXPECT_TRUE(same(record.messages[3].message, AnnounceEnd{5}));
        EXPECT_TRUE(same(record.messages[4].message, AnnounceUpdate{5, RouteMetadata{{7}, 2, 3}}));
        ASSERT_EQ(record.issues.size(), 1u) << describe_issues(record);
        EXPECT_EQ(record.issues[0].code, kIssueUnknownAnnounceType);
        EXPECT_EQ(record.issues[0].from, LiteOrigin::Peer);
        EXPECT_NE(record.issues[0].detail.find('4'), std::string::npos) << record.issues[0].detail;
        EXPECT_EQ(record.local_bytes, kAnnounceLocal.size());
        EXPECT_EQ(record.bytes, peer.size());
    }
}

TEST(LiteStreamReader, UnknownAnnounceTypeAboveTheLimitStops) {
    DecodeLimits tight;
    tight.max_message_length = 4;
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true, tight);
    reader.feed_local(kAnnounceLocal, false, 1);
    reader.feed(concat({kAnnounceOkWire, bytes({0x04, 0x05}), repeat(0, 5), bytes({0x01, 0x01, 0x05})}), false, 2);
    EXPECT_EQ(reader.record().messages.size(), 2u);
    EXPECT_EQ(count_issues(reader.record(), kIssueLengthExceedsLimit), 1u) << describe_issues(reader.record());
}

TEST(LiteStreamReader, AnnounceDecodeErrorStopsTheStream) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed_local(kAnnounceLocal, false, 1);
    // ANNOUNCE_END with a trailing byte inside its Message Length, then a valid END that must not be decoded.
    reader.feed(concat({kAnnounceOkWire, bytes({0x01, 0x02, 0x05, 0x00}), bytes({0x01, 0x01, 0x05})}), false, 2);
    EXPECT_EQ(reader.record().messages.size(), 2u);
    EXPECT_EQ(count_issues(reader.record(), kIssueProtocolViolation), 1u) << describe_issues(reader.record());
    EXPECT_EQ(reader.record().issues.size(), 1u);
}

TEST(LiteStreamReader, SubscribeStreamOkEndDrop) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed_local(kSubscribeLocal, false, 1);
    reader.feed(concat({bytes({0x00, 0x01, 0x06}), bytes({0x02, 0x03, 0x03, 0x05, 0x00}), bytes({0x01, 0x01, 0x07})}),
                true, 2);
    const auto& record = reader.record();
    EXPECT_EQ(record.kind, LiteStreamKind::Subscribe);
    EXPECT_TRUE(record.issues.empty()) << describe_issues(record);
    ASSERT_EQ(record.messages.size(), 4u);
    EXPECT_TRUE(same(record.messages[0].message, Subscribe{0, "b", "t", SubscribeRange{0x80, 0, 0, 0, 0, 0}}));
    EXPECT_TRUE(same(record.messages[1].message, SubscribeOk{6}));
    EXPECT_TRUE(same(record.messages[2].message, SubscribeDrop{3, 5, 0}));
    EXPECT_TRUE(same(record.messages[3].message, SubscribeEnd{7}));
    EXPECT_TRUE(record.fin_seen);
}

TEST(LiteStreamReader, UnknownSubscribeResponseTypeStops) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed_local(kSubscribeLocal, false, 1);
    reader.feed(concat({bytes({0x03, 0x01, 0x00}), bytes({0x00, 0x01, 0x06})}), false, 2);
    EXPECT_EQ(reader.record().messages.size(), 1u);
    EXPECT_EQ(count_issues(reader.record(), kIssueInvalidValue), 1u) << describe_issues(reader.record());
}

TEST(LiteStreamReader, RunnerTypeWrittenInPiecesBuffersThePeerResponse) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed_local(bytes({0x40}), false, 1);  // a non-minimal 2-byte STREAM_TYPE, first byte only
    reader.feed(bytes({0x00, 0x01, 0x06}), false, 2);
    EXPECT_TRUE(reader.record().messages.empty());
    reader.feed_local(concat({bytes({0x02}), kSubscribeMinimal}), false, 3);
    EXPECT_EQ(reader.record().kind, LiteStreamKind::Subscribe);
    ASSERT_EQ(reader.record().messages.size(), 2u) << describe_issues(reader.record());
    EXPECT_TRUE(same(reader.record().messages[1].message, SubscribeOk{6}));
}

TEST(LiteStreamReader, PeerBytesBufferedBeforeTheKindAreBounded) {
    DecodeLimits tight;
    tight.max_message_length = 8;
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true, tight);
    reader.feed_local(bytes({0x40}), false, 1);  // the kind is not known yet
    reader.feed(repeat(0, 30), false, 2);
    EXPECT_TRUE(reader.record().issues.empty());
    reader.feed(repeat(0, 30), false, 3);  // 60 bytes > 8 + 32 of slack
    EXPECT_EQ(count_issues(reader.record(), kIssueBufferLimitReached), 1u) << describe_issues(reader.record());
    EXPECT_EQ(classify_issue(kIssueBufferLimitReached), LiteIssueClass::Harness);
    reader.feed_local(concat({bytes({0x02}), kSubscribeMinimal}), false, 4);  // completes the type: Subscribe
    EXPECT_EQ(reader.record().kind, LiteStreamKind::Subscribe);
    // The peer direction stays stopped (the runner's SUBSCRIBE is itself over this tight limit).
    EXPECT_TRUE(reader.record().messages.empty());
    EXPECT_EQ(reader.record().bytes, 60u);
}

TEST(LiteStreamReader, L2StreamsAreRecordedRawAndFlagged) {
    const std::vector<std::pair<unsigned, LiteStreamKind>> kinds = {
        {0x3, LiteStreamKind::Fetch}, {0x4, LiteStreamKind::Probe}, {0x5, LiteStreamKind::Goaway},
        {0x6, LiteStreamKind::Track}};
    for (const auto& [type, kind] : kinds) {
        LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
        reader.feed_local(bytes({type, 0x01, 0x00}), false, 1);
        reader.feed(bytes({0x00, 0x01, 0x06, 0xff}), true, 2);
        const auto& record = reader.record();
        EXPECT_EQ(record.kind, kind);
        EXPECT_TRUE(record.messages.empty());
        EXPECT_EQ(count_issues(record, kIssueL2StreamNotDecoded), 1u) << describe_issues(record);
        EXPECT_EQ(record.issues.size(), 1u) << describe_issues(record);
        EXPECT_EQ(record.bytes, 4u);
    }
}

TEST(LiteStreamReader, UnregisteredBidiTypeWrittenByTheRunner) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed_local(bytes({0x3f}), false, 1);
    reader.feed(bytes({0x01}), true, 2);
    EXPECT_EQ(reader.record().kind, LiteStreamKind::UnregisteredBidi);
    EXPECT_EQ(reader.record().stream_type.value_or(0), 0x3fu);
    EXPECT_TRUE(reader.record().issues.empty()) << describe_issues(reader.record());
}

TEST(LiteStreamReader, PeerBytesOnAnUndeclaredRunnerStream) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed(bytes({0x00, 0x01, 0x06}), false, 1);
    EXPECT_EQ(reader.record().kind, LiteStreamKind::Unknown);
    EXPECT_EQ(count_issues(reader.record(), kIssueUndeclaredRunnerStream), 1u);
    EXPECT_EQ(reader.record().bytes, 3u);
}

TEST(LiteStreamReader, RunnerSetupStreamIsDecodedAsTheRunners) {
    LiteStreamReader reader(kRunnerUni, LiteOrigin::Runner, false);
    reader.feed_local(bytes({0x01, 0x01, 0x00}), true, 5);
    EXPECT_EQ(reader.record().kind, LiteStreamKind::Setup);
    ASSERT_EQ(reader.record().messages.size(), 1u);
    EXPECT_EQ(reader.record().messages[0].from, LiteOrigin::Runner);
    EXPECT_TRUE(reader.record().local_fin);
    EXPECT_FALSE(reader.record().fin_seen);
    EXPECT_TRUE(reader.record().issues.empty()) << describe_issues(reader.record());
}

// ---- garbage and limits ---------------------------------------------------------------------------------------

TEST(LiteStreamReader, TruncatedGarbageNeverCompletes) {
    // A SETUP claiming 63 body bytes that never arrive, then FIN.
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    reader.feed(bytes({0x01, 0x3f}), false, 1);
    for (unsigned i = 0; i < 10; ++i) reader.feed(bytes({0xff}), false, 2 + i);
    EXPECT_TRUE(reader.record().messages.empty());
    EXPECT_TRUE(reader.record().issues.empty());
    reader.feed({}, true, 20);
    EXPECT_TRUE(reader.record().messages.empty());
    EXPECT_EQ(count_issues(reader.record(), kIssueTruncatedAtFin), 1u);
}

TEST(LiteStreamReader, MessageLengthAboveTheLimit) {
    {
        // SETUP Message Length (1 << 20) + 1 in the 4-byte varint form: above the default 1 MiB limit.
        LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
        reader.feed(bytes({0x01, 0x80, 0x10, 0x00, 0x01}), false, 1);
        EXPECT_EQ(count_issues(reader.record(), kIssueLengthExceedsLimit), 1u) << describe_issues(reader.record());
        reader.feed(repeat(0, 100), false, 2);  // counted, not buffered or decoded
        EXPECT_EQ(reader.record().bytes, 105u);
        EXPECT_EQ(reader.record().issues.size(), 1u);
    }
    {
        // A FRAME payload over a tight limit.
        DecodeLimits tight;
        tight.max_message_length = 8;
        LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false, tight);
        reader.feed(concat({kGroupPreamble, bytes({0x00, 0x09})}), false, 1);
        EXPECT_EQ(reader.record().messages.size(), 1u);
        EXPECT_EQ(count_issues(reader.record(), kIssueLengthExceedsLimit), 1u) << describe_issues(reader.record());
    }
    {
        // GROUP over a tight limit.
        DecodeLimits tight;
        tight.max_message_length = 2;
        LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false, tight);
        reader.feed(kGroupPreamble, false, 1);
        EXPECT_TRUE(reader.record().messages.empty());
        EXPECT_EQ(count_issues(reader.record(), kIssueLengthExceedsLimit), 1u) << describe_issues(reader.record());
    }
}

TEST(LiteStreamReader, MessageLimitStopsDecoding) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false, wire::moqlite06::kDefaultLimits, 3);
    Bytes wire = kGroupPreamble;
    for (int i = 0; i < 5; ++i) wire.insert(wire.end(), {std::byte{0x00}, std::byte{0x00}});
    reader.feed(wire, false, 1);
    EXPECT_EQ(reader.record().messages.size(), 3u);
    EXPECT_TRUE(reader.message_limit_reached());
    EXPECT_EQ(count_issues(reader.record(), kIssueMessageLimitReached), 1u) << describe_issues(reader.record());
    EXPECT_EQ(reader.record().bytes, wire.size());
}

// ---- the session ----------------------------------------------------------------------------------------------

TEST(LiteSession, ClassifiesStreamsFromTheStreamId) {
    LiteSession session;
    session.on_event(data_event(kPeerUni, bytes({0x01, 0x01, 0x00}), true), 10);
    session.on_event(data_event(kPeerUni + 4, concat({kGroupPreamble, bytes({0x00, 0x00})})), 11);
    session.on_event(data_event(kPeerBidi, bytes({0x01})), 12);
    ASSERT_EQ(session.streams().size(), 3u);
    const auto& setup_stream = session.streams()[0];
    EXPECT_EQ(setup_stream.stream_id, kPeerUni);
    EXPECT_EQ(setup_stream.origin, LiteOrigin::Peer);
    EXPECT_FALSE(setup_stream.bidirectional);
    EXPECT_EQ(setup_stream.kind, LiteStreamKind::Setup);
    EXPECT_EQ(setup_stream.opened_ns, 10u);
    const auto& group_stream = session.streams()[1];
    EXPECT_EQ(group_stream.kind, LiteStreamKind::Group);
    EXPECT_EQ(group_stream.messages.size(), 2u);
    const auto& bidi = session.streams()[2];
    EXPECT_TRUE(bidi.bidirectional);
    EXPECT_EQ(bidi.origin, LiteOrigin::Peer);
    EXPECT_EQ(count_issues(bidi, kIssuePublisherOpenedBidi), 1u);
    ASSERT_NE(session.find(kPeerUni + 4), nullptr);
    EXPECT_EQ(session.find(kPeerUni + 4)->kind, LiteStreamKind::Group);
    EXPECT_EQ(session.find(99), nullptr);
    EXPECT_FALSE(session.limit_reached());
}

TEST(LiteSession, RunnerStreamsTakeTheirKindFromTheLocalWrite) {
    LiteSession session;
    session.note_local_write(kRunnerUni, false, bytes({0x01, 0x01, 0x00}), true, 1);
    session.note_local_write(kRunnerBidi, true, kAnnounceLocal, false, 2);
    session.on_event(data_event(kRunnerBidi, concat({kAnnounceOkWire, bytes({0x04, 0x00}), bytes({0x01, 0x01, 0x05})})),
                     3);
    ASSERT_EQ(session.streams().size(), 2u);
    EXPECT_EQ(session.streams()[0].kind, LiteStreamKind::Setup);
    EXPECT_EQ(session.streams()[0].origin, LiteOrigin::Runner);
    EXPECT_TRUE(session.streams()[0].local_fin);
    const auto& announce = session.streams()[1];
    EXPECT_EQ(announce.kind, LiteStreamKind::Announce);
    EXPECT_EQ(announce.origin, LiteOrigin::Runner);
    ASSERT_EQ(announce.messages.size(), 3u);
    EXPECT_TRUE(same(announce.messages[2].message, AnnounceEnd{5}));
    EXPECT_EQ(count_issues(announce, kIssueUnknownAnnounceType), 1u);
    EXPECT_EQ(session.total_bytes(), 3u + kAnnounceLocal.size() + 3u + 2u + 3u);
}

TEST(LiteSession, PeerDataOnAnUndeclaredRunnerStream) {
    LiteSession session;
    session.on_event(data_event(kRunnerBidi + 4, bytes({0x00, 0x01, 0x06})), 1);
    ASSERT_EQ(session.streams().size(), 1u);
    EXPECT_EQ(session.streams()[0].origin, LiteOrigin::Runner);
    EXPECT_EQ(count_issues(session.streams()[0], kIssueUndeclaredRunnerStream), 1u);
}

TEST(LiteSession, ResetAndStopSendingAttach) {
    LiteSession session;
    session.on_event(data_event(kPeerUni, concat({kGroupPreamble, bytes({0x00, 0x05})})), 1);
    session.on_event(PeerResetEvent{kPeerUni, 0x3}, 2);
    session.on_event(PeerStopSendingEvent{kPeerUni, 0x4}, 3);
    session.on_event(PeerResetEvent{kPeerUni + 4, std::nullopt}, 4);  // a reset for a stream never seen
    ASSERT_EQ(session.streams().size(), 2u);
    const auto& stream = session.streams()[0];
    EXPECT_EQ(stream.reset_code.value_or(0), 0x3u);
    EXPECT_EQ(stream.stop_sending_code.value_or(0), 0x4u);
    EXPECT_TRUE(stream.issues.empty()) << describe_issues(stream);
    EXPECT_TRUE(session.streams()[1].reset_seen);
    EXPECT_FALSE(session.streams()[1].reset_code.has_value());
}

TEST(LiteSession, PeerCloseIsCaptured) {
    LiteSession session;
    EXPECT_FALSE(session.peer_close().has_value());
    PeerCloseEvent close;
    close.error_space = CloseErrorSpace::Application;
    close.error_code = 0x3;
    close.reason = text("bye");
    session.on_event(close, 42);
    PeerCloseEvent later;
    later.error_code = 0x9;
    session.on_event(later, 50);  // only the first close is kept
    const auto info = session.peer_close();
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->space, CloseErrorSpace::Application);
    EXPECT_EQ(info->code, 0x3u);
    EXPECT_EQ(info->reason, "bye");
    EXPECT_EQ(info->at_ns, 42u);
}

TEST(LiteSession, ConnectionEstablishedIsTimed) {
    LiteSession session;
    EXPECT_FALSE(session.established_ns().has_value());
    session.on_event(ConnectionEstablishedEvent{}, 5);
    EXPECT_EQ(session.established_ns().value_or(0), 5u);
}

TEST(LiteSession, MaxStreamsStopsRecording) {
    LiteSessionLimits limits;
    limits.max_streams = 2;
    LiteSession session(limits);
    session.on_event(data_event(2, bytes({0x07})), 1);
    session.on_event(data_event(6, bytes({0x07})), 2);
    EXPECT_FALSE(session.limit_reached());
    session.on_event(data_event(10, bytes({0x07})), 3);
    EXPECT_TRUE(session.limit_reached());
    EXPECT_EQ(session.streams().size(), 2u);
    session.on_event(data_event(2, bytes({0x00})), 4);  // recording has stopped, even for known streams
    EXPECT_EQ(session.streams()[0].bytes, 1u);
    PeerCloseEvent close;
    close.error_code = 1;
    session.on_event(close, 5);
    EXPECT_TRUE(session.peer_close().has_value());  // the close is still captured
}

TEST(LiteSession, MaxBytesStopsRecording) {
    LiteSessionLimits limits;
    limits.max_bytes = 10;
    LiteSession session(limits);
    session.on_event(data_event(2, bytes({0x07, 1, 2, 3, 4, 5, 6, 7})), 1);
    EXPECT_FALSE(session.limit_reached());
    session.on_event(data_event(2, bytes({8, 9, 10})), 2);
    EXPECT_TRUE(session.limit_reached());
    EXPECT_EQ(session.streams()[0].bytes, 8u);
    EXPECT_EQ(session.total_bytes(), 8u);
    session.note_local_write(3, false, bytes({0x01, 0x01, 0x00}), true, 3);
    EXPECT_EQ(session.streams().size(), 1u);
}

TEST(LiteSession, MaxMessagesPerStreamSetsTheLimit) {
    LiteSessionLimits limits;
    limits.max_messages_per_stream = 2;
    LiteSession session(limits);
    session.on_event(data_event(2, concat({kGroupPreamble, bytes({0x00, 0x00, 0x00, 0x00})})), 1);
    EXPECT_TRUE(session.limit_reached());
    EXPECT_EQ(session.streams()[0].messages.size(), 2u);
}

TEST(LiteSession, EventQueueOverflowSetsTheLimit) {
    LiteSession session;
    session.on_event(EventQueueOverflowEvent{}, 1);
    EXPECT_TRUE(session.limit_reached());
}

// Offsets are per-stream buffers starting at 0, so Cursor offset arithmetic near SIZE_MAX cannot arise here; what
// can arise is arbitrary bytes. A fixed-seed soup of stream events must never throw, and memory stays bounded.
TEST(LiteSession, RandomByteSoupNeverThrows) {
    std::mt19937_64 rng(0x5eed1d);
    LiteSessionLimits limits;
    limits.max_bytes = std::size_t{1} << 24;
    LiteSession session(limits);
    std::uniform_int_distribution<int> byte_dist(0, 255);
    std::uniform_int_distribution<int> len_dist(0, 24);
    std::uniform_int_distribution<int> action(0, 99);
    std::uniform_int_distribution<std::uint64_t> stream_dist(0, 31);
    const std::vector<Bytes> seeds = {kGroupPreamble, kAnnounceLocal, kSubscribeLocal, bytes({0x01}),
                                      bytes({0x01, 0x05, 0x01, 0x04}), kAnnounceOkWire};
    for (int i = 0; i < 20000; ++i) {
        const auto id = stream_dist(rng);
        Bytes data;
        if (action(rng) < 20) {
            const auto& seed = seeds[static_cast<std::size_t>(action(rng)) % seeds.size()];
            data = seed;
        }
        const int length = len_dist(rng);
        for (int j = 0; j < length; ++j) data.push_back(static_cast<std::byte>(byte_dist(rng)));
        const int what = action(rng);
        const auto now = static_cast<std::uint64_t>(i);
        EXPECT_NO_THROW({
            if (what < 70) {
                session.on_event(data_event(id, data, what < 3), now);
            } else if (what < 85) {
                session.note_local_write(id, (id & 2) == 0, data, what < 72, now);
            } else if (what < 92) {
                session.on_event(PeerResetEvent{id, static_cast<std::uint64_t>(what)}, now);
            } else if (what < 98) {
                session.on_event(PeerStopSendingEvent{id, std::nullopt}, now);
            } else {
                PeerCloseEvent close;
                close.reason = data;
                session.on_event(close, now);
            }
        });
    }
    EXPECT_LE(session.streams().size(), 32u);
    EXPECT_LE(session.total_bytes(), limits.max_bytes);
    // Each reader on its own, too, including the decoders' error paths on tight limits.
    DecodeLimits tight;
    tight.max_message_length = 16;
    tight.max_string_length = 4;
    tight.max_parameters = 2;
    tight.max_hops = 2;
    for (int i = 0; i < 20000; ++i) {
        const auto id = stream_dist(rng);
        const bool runner = (id & 1) != 0;
        LiteStreamReader reader(id, runner ? LiteOrigin::Runner : LiteOrigin::Peer, (id & 2) == 0,
                                (i % 2) == 0 ? wire::moqlite06::kDefaultLimits : tight, 64);
        if (runner) reader.feed_local(seeds[static_cast<std::size_t>(i) % seeds.size()], false, 0);
        Bytes data = seeds[static_cast<std::size_t>(i / 2) % seeds.size()];
        const int length = len_dist(rng) * 2;
        for (int j = 0; j < length; ++j) data.push_back(static_cast<std::byte>(byte_dist(rng)));
        std::size_t begin = 0;
        EXPECT_NO_THROW({
            while (begin < data.size()) {
                const auto take = std::min<std::size_t>(data.size() - begin,
                                                        static_cast<std::size_t>(len_dist(rng) % 5 + 1));
                reader.feed(std::span(data).subspan(begin, take), begin + take == data.size(), 1);
                begin += take;
            }
        });
        EXPECT_LE(reader.record().messages.size(), 128u);  // 64 per direction
        EXPECT_EQ(reader.record().bytes, data.size());
        for (const auto& issue : reader.record().issues) EXPECT_TRUE(is_known_issue_code(issue.code)) << issue.code;
    }
    for (const auto& record : session.streams()) {
        for (const auto& issue : record.issues) EXPECT_TRUE(is_known_issue_code(issue.code)) << issue.code;
    }
}

// ---- review fixes: memory accounting (I1) ---------------------------------------------------------------------

Bytes empty_frames_group(std::size_t frames) {
    Bytes wire = kGroupPreamble;
    wire.resize(wire.size() + 2 * frames, std::byte{0});  // FRAME{delta 0, empty payload} is 00 00
    return wire;
}

TEST(LiteSessionBudget, MessagesAreChargedAgainstMaxBytes) {
    LiteSession session;
    const auto wire = empty_frames_group(3);
    session.on_event(data_event(kPeerUni, wire, true), 1);
    EXPECT_EQ(session.message_count(), 4u);
    EXPECT_EQ(session.total_bytes(), wire.size());
    EXPECT_EQ(session.charged_bytes(), wire.size() + 4 * detail::kMessageCharge);
    EXPECT_GE(detail::kMessageCharge, sizeof(LiteDecoded));
    // Issues are charged too.
    session.on_event(data_event(kPeerBidi, bytes({0x01})), 2);  // publisher_opened_bidi at creation is free
    session.on_event(data_event(kPeerUni + 4, bytes({0x01}), true), 3);  // truncated_at_fin
    const auto* truncated = session.find(kPeerUni + 4);
    ASSERT_NE(truncated, nullptr);
    ASSERT_EQ(truncated->issues.size(), 1u);
    EXPECT_EQ(session.charged_bytes(), wire.size() + 4 * detail::kMessageCharge + 2 +
                                           detail::issue_charge(truncated->issues[0].code,
                                                                truncated->issues[0].detail));
}

TEST(LiteSessionBudget, EmptyFrameFloodAcrossStreamsIsBoundedByDefaults) {
    LiteSession session;  // defaults
    const LiteSessionLimits defaults;
    const auto wire = empty_frames_group(99000);
    std::uint64_t id = kPeerUni;
    std::size_t sent = 0;
    while (!session.limit_reached() && sent < 400) {
        session.on_event(data_event(id, wire, true), 1);
        id += 4;
        ++sent;
    }
    EXPECT_TRUE(session.limit_reached());
    std::size_t messages = 0;
    for (const auto& record : session.streams()) messages += record.messages.size();
    EXPECT_EQ(messages, session.message_count());
    EXPECT_LE(messages, defaults.max_messages_total);
    EXPECT_LE(session.charged_bytes(), defaults.max_bytes);
    // The resident estimate (stored messages plus the wire bytes behind them) stays within the byte budget, far
    // below the ~4.8 GiB the unaccounted recorder reached.
    EXPECT_LE(messages * sizeof(LiteDecoded) + session.total_bytes(), defaults.max_bytes);
}

TEST(LiteSessionBudget, ByteBudgetStopsAFloodBeforeTheMessageCap) {
    LiteSessionLimits limits;
    limits.max_bytes = std::size_t{1} << 20;
    limits.max_messages_total = std::numeric_limits<std::size_t>::max();
    LiteSession session(limits);
    const auto wire = empty_frames_group(1000);
    for (std::uint64_t id = kPeerUni; id < 4 * 200 && !session.limit_reached(); id += 4) {
        session.on_event(data_event(id, wire), 1);
    }
    EXPECT_TRUE(session.limit_reached());
    std::size_t messages = 0;
    for (const auto& record : session.streams()) messages += record.messages.size();
    EXPECT_LE(messages * sizeof(LiteDecoded) + session.total_bytes(), limits.max_bytes);
    EXPECT_LE(session.charged_bytes(), limits.max_bytes);
}

TEST(LiteSessionBudget, TotalMessageCapSetsTheLimit) {
    LiteSessionLimits limits;
    limits.max_messages_total = 5;
    LiteSession session(limits);
    session.on_event(data_event(kPeerUni, empty_frames_group(3)), 1);  // 4 messages
    EXPECT_FALSE(session.limit_reached());
    session.on_event(data_event(kPeerUni + 4, empty_frames_group(3)), 2);  // the 6th message is refused
    EXPECT_TRUE(session.limit_reached());
    EXPECT_EQ(session.message_count(), 5u);
    EXPECT_EQ(session.streams()[1].messages.size(), 1u);
}

// ---- review fixes: evaluator accessors and issue classes (I2) ---------------------------------------------------

TEST(LiteAccessors, PeerMessagesNeverSeeTheRunnersOwnSetup) {
    LiteSession session;
    session.note_local_write(kRunnerUni, false, bytes({0x01, 0x01, 0x00}), true, 1);  // the runner's SETUP
    // A naive scan of all streams' messages finds a SETUP although the peer sent none.
    bool naive = false;
    for (const auto& record : session.streams()) {
        for (const auto& message : record.messages) naive |= std::holds_alternative<SetupMessage>(message.message);
    }
    EXPECT_TRUE(naive);
    bool peer_setup = false;
    for (const auto* record : peer_streams(session)) {
        for (const auto* message : peer_messages(*record)) {
            peer_setup |= std::holds_alternative<SetupMessage>(message->message);
        }
    }
    EXPECT_FALSE(peer_setup);
    EXPECT_TRUE(peer_streams(session).empty());
    ASSERT_EQ(runner_streams(session).size(), 1u);
    EXPECT_EQ(runner_messages(*runner_streams(session)[0]).size(), 1u);
    EXPECT_TRUE(peer_messages(session.streams()[0]).empty());
    // Once the peer sends its SETUP, the accessors see exactly that one.
    session.on_event(data_event(kPeerUni, bytes({0x01, 0x01, 0x00}), true), 2);
    ASSERT_EQ(peer_streams(session).size(), 1u);
    ASSERT_EQ(peer_messages(*peer_streams(session)[0]).size(), 1u);
}

TEST(LiteAccessors, RunnerIssuesAreNotPeerIssues) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    // A deliberately malformed runner ANNOUNCE_REQUEST (trailing byte inside its length), then a peer error.
    reader.feed_local(bytes({0x01, 0x03, 0x01, 0x61, 0x00}), false, 1);
    reader.feed(concat({kAnnounceOkWire, bytes({0x04, 0x00}), bytes({0x01, 0x02, 0x05, 0x00})}), false, 2);
    const auto& record = reader.record();
    ASSERT_EQ(record.issues.size(), 3u) << describe_issues(record);
    const auto peer = peer_issues(record);
    ASSERT_EQ(peer.size(), 2u);
    EXPECT_EQ(peer[0]->code, kIssueUnknownAnnounceType);
    EXPECT_EQ(peer[1]->code, kIssueProtocolViolation);
    const auto protocol = peer_protocol_issues(record);
    ASSERT_EQ(protocol.size(), 1u);  // the runner's violation and the Inconclusive skip are excluded
    EXPECT_EQ(protocol[0]->code, kIssueProtocolViolation);
    EXPECT_EQ(protocol[0]->from, LiteOrigin::Peer);
    EXPECT_EQ(peer_messages(record).size(), 1u);  // ANNOUNCE_OK
}

TEST(LiteAccessors, HarnessLimitsAreNeverPeerProtocol) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false, wire::moqlite06::kDefaultLimits, 2);
    reader.feed(empty_frames_group(5), false, 1);
    EXPECT_EQ(count_issues(reader.record(), kIssueMessageLimitReached), 1u);
    EXPECT_TRUE(peer_protocol_issues(reader.record()).empty());
    EXPECT_EQ(harness_issues(reader.record()).size(), 1u);
    // A SETUP over the (defensive, undrafted) default length limit is a harness limit, not a peer violation.
    LiteStreamReader big(kPeerUni, LiteOrigin::Peer, false);
    big.feed(bytes({0x01, 0x80, 0x10, 0x00, 0x01}), false, 1);
    EXPECT_EQ(count_issues(big.record(), kIssueLengthExceedsLimit), 1u);
    EXPECT_TRUE(peer_protocol_issues(big.record()).empty());
}

TEST(LiteIssueClasses, EveryIssueCodeHasAnExplicitClass) {
    const std::vector<std::pair<std::string_view, LiteIssueClass>> expected = {
        {kIssueProtocolViolation, LiteIssueClass::PeerProtocol},
        {kIssueInvalidValue, LiteIssueClass::PeerProtocol},
        {kIssueKeyValueFormattingError, LiteIssueClass::PeerProtocol},
        {kIssueTrailingAfterSetup, LiteIssueClass::PeerProtocol},
        {kIssueTrailingAfterRequest, LiteIssueClass::PeerProtocol},
        {kIssueTruncatedAtFin, LiteIssueClass::PeerProtocol},
        {kIssueUnknownAnnounceType, LiteIssueClass::Inconclusive},
        {kIssueLengthExceedsLimit, LiteIssueClass::Harness},
        {kIssueLengthNotRepresentable, LiteIssueClass::Harness},
        {kIssueOffsetOverflow, LiteIssueClass::Harness},
        {kIssueTrailingAfterFin, LiteIssueClass::Harness},
        {kIssueUndeclaredRunnerStream, LiteIssueClass::Harness},
        {kIssueMessageLimitReached, LiteIssueClass::Harness},
        {kIssueBufferLimitReached, LiteIssueClass::Harness},
        {kIssueLocalBidiMismatch, LiteIssueClass::Harness},
        {kIssuePublisherOpenedBidi, LiteIssueClass::Informational},
        {kIssueL2StreamNotDecoded, LiteIssueClass::Informational},
    };
    ASSERT_EQ(expected.size(), kAllIssueCodes.size());
    for (const auto code : kAllIssueCodes) {
        const auto found = std::find_if(expected.begin(), expected.end(),
                                        [&](const auto& entry) { return entry.first == code; });
        ASSERT_NE(found, expected.end()) << code;
        ASSERT_TRUE(explicit_issue_class(code).has_value()) << code;
        EXPECT_EQ(*explicit_issue_class(code), found->second) << code;
        EXPECT_EQ(classify_issue(code), found->second) << code;
    }
    // No duplicates in the registry.
    std::vector<std::string_view> sorted(kAllIssueCodes.begin(), kAllIssueCodes.end());
    std::sort(sorted.begin(), sorted.end());
    EXPECT_EQ(std::adjacent_find(sorted.begin(), sorted.end()), sorted.end());
    // An unknown code is never a peer Fail.
    EXPECT_FALSE(explicit_issue_class("not_a_code").has_value());
    EXPECT_EQ(classify_issue("not_a_code"), LiteIssueClass::Harness);
}

// ---- review fixes: minors ---------------------------------------------------------------------------------------

TEST(LiteStreamReader, HeldPeerBytesKeepTheirArrival) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed_local(bytes({0x40}), false, 100);          // event 0
    reader.feed(bytes({0x00, 0x01}), false, 200);          // event 1: SUBSCRIBE_OK, first two bytes
    reader.feed(bytes({0x06, 0x01, 0x01}), false, 300);    // event 2: its last byte, then 2 of SUBSCRIBE_END
    reader.feed_local(concat({bytes({0x02}), kSubscribeMinimal}), false, 900);  // event 3: the kind is known
    reader.feed(bytes({0x07}), false, 1000);               // event 4: SUBSCRIBE_END's last byte
    const auto& record = reader.record();
    ASSERT_EQ(record.messages.size(), 3u) << describe_issues(record);
    EXPECT_TRUE(same(record.messages[0].message, Subscribe{0, "b", "t", SubscribeRange{0x80, 0, 0, 0, 0, 0}}));
    EXPECT_EQ(record.messages[0].at_ns, 900u);
    EXPECT_TRUE(same(record.messages[1].message, SubscribeOk{6}));
    EXPECT_EQ(record.messages[1].at_ns, 300u);
    EXPECT_EQ(record.messages[1].stream_event_index, 2u);
    EXPECT_TRUE(same(record.messages[2].message, SubscribeEnd{7}));
    EXPECT_EQ(record.messages[2].at_ns, 1000u);
    EXPECT_EQ(record.messages[2].stream_event_index, 4u);
}

TEST(LiteStreamReader, SubscribeOkHeldAt200Stays200) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed_local(bytes({0x40}), false, 100);
    reader.feed(bytes({0x00, 0x01, 0x06}), false, 200);
    reader.feed_local(concat({bytes({0x02}), kSubscribeMinimal}), false, 900);
    const auto peer = peer_messages(reader.record());
    ASSERT_EQ(peer.size(), 1u);
    EXPECT_EQ(peer[0]->at_ns, 200u);
    EXPECT_EQ(peer[0]->stream_event_index, 1u);
}

TEST(LiteSession, LocalWriteDirectionComesFromTheStreamId) {
    LiteSession session;
    session.note_local_write(kRunnerUni, true, bytes({0x01, 0x01, 0x00}), true, 1);  // uni id, caller says bidi
    ASSERT_EQ(session.streams().size(), 1u);
    const auto& record = session.streams()[0];
    EXPECT_FALSE(record.bidirectional);
    EXPECT_EQ(record.kind, LiteStreamKind::Setup);
    EXPECT_EQ(count_issues(record, kIssueLocalBidiMismatch), 1u) << describe_issues(record);
    EXPECT_EQ(record.issues[0].from, LiteOrigin::Runner);
    EXPECT_TRUE(peer_issues(record).empty());
    session.note_local_write(kRunnerBidi, true, kAnnounceLocal, false, 2);  // agreeing flag: no issue
    EXPECT_TRUE(session.streams()[1].issues.empty());
}

TEST(LiteStreamReader, PeerOpenedGoawayTypeIsUnknownWithItsRawType) {
    LiteStreamReader reader(kPeerBidi, LiteOrigin::Peer, true);
    reader.feed(bytes({0x05, 0x00}), false, 1);
    EXPECT_EQ(reader.record().kind, LiteStreamKind::Unknown);
    EXPECT_EQ(reader.record().stream_type.value_or(0), 0x5u);
    EXPECT_EQ(count_issues(reader.record(), kIssuePublisherOpenedBidi), 1u);
}

TEST(LiteStreamReader, RunnerWritesDoNotUseUpThePeersMessageCap) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true, wire::moqlite06::kDefaultLimits, 2);
    const auto update = bytes({0x06, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00});
    reader.feed_local(concat({kSubscribeLocal, update, update}), false, 1);  // the runner's third message hits it
    reader.feed(concat({bytes({0x00, 0x01, 0x06}), bytes({0x01, 0x01, 0x07})}), false, 2);
    const auto& record = reader.record();
    EXPECT_TRUE(reader.message_limit_reached());
    EXPECT_EQ(runner_messages(record).size(), 2u);
    EXPECT_EQ(peer_messages(record).size(), 2u);  // the peer's allowance is its own
    ASSERT_EQ(count_issues(record, kIssueMessageLimitReached), 1u);
    EXPECT_EQ(record.issues[0].from, LiteOrigin::Runner);
    EXPECT_TRUE(peer_issues(record).empty());
}

TEST(LiteSession, PerStreamMessageCapEndsRecordingForTheSession) {
    LiteSessionLimits limits;
    limits.max_messages_per_stream = 2;
    LiteSession session(limits);
    session.on_event(data_event(kPeerUni, empty_frames_group(4)), 1);
    EXPECT_TRUE(session.limit_reached());
    session.on_event(data_event(kPeerUni + 4, bytes({0x01, 0x01, 0x00}), true), 2);  // another stream: ignored
    EXPECT_EQ(session.streams().size(), 1u);
}

TEST(LiteStreamReaderProperty, UnknownAnnounceTypeSkipAtEveryOffsetAndPairOfOffsets) {
    const auto peer = concat({kAnnounceOkWire, bytes({0x04, 0x02, 0xaa, 0xbb}), bytes({0x01, 0x01, 0x05}),
                              bytes({0x05, 0x00}), bytes({0x02, 0x05, 0x05, 0x01, 0x07, 0x02, 0x03})});
    auto check = [&](const std::vector<std::size_t>& cuts) {
        LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
        reader.feed_local(kAnnounceLocal, false, 1);
        std::size_t begin = 0;
        std::uint64_t clock = 2;
        for (const auto cut : cuts) {
            reader.feed(std::span(peer).subspan(begin, cut - begin), false, clock++);
            begin = cut;
        }
        reader.feed(std::span(peer).subspan(begin), false, clock);
        const auto& record = reader.record();
        ASSERT_EQ(record.messages.size(), 4u) << describe_issues(record);
        EXPECT_TRUE(same(record.messages[1].message, AnnounceOk{0, 0}));
        EXPECT_TRUE(same(record.messages[2].message, AnnounceEnd{5}));
        EXPECT_TRUE(same(record.messages[3].message, AnnounceUpdate{5, RouteMetadata{{7}, 2, 3}}));
        ASSERT_EQ(record.issues.size(), 2u) << describe_issues(record);
        EXPECT_EQ(record.issues[0].code, kIssueUnknownAnnounceType);
        EXPECT_EQ(record.issues[1].code, kIssueUnknownAnnounceType);
    };
    for (std::size_t cut = 0; cut <= peer.size(); ++cut) {
        SCOPED_TRACE(cut);
        check({cut});
    }
    for (std::size_t first = 0; first <= peer.size(); ++first) {
        for (std::size_t second = first; second <= peer.size(); ++second) {
            SCOPED_TRACE(std::to_string(first) + "," + std::to_string(second));
            check({first, second});
        }
    }
}

TEST(LiteStreamReader, PeerFinWhileWaitingForTheRunnersStreamType) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed_local(bytes({0x40}), false, 1);
    reader.feed(bytes({0x00, 0x01, 0x06, 0x00}), true, 2);  // SUBSCRIBE_OK, then a partial message, FIN
    EXPECT_TRUE(reader.record().issues.empty());              // judged once the kind is known
    reader.feed_local(concat({bytes({0x02}), kSubscribeMinimal}), false, 3);
    const auto& record = reader.record();
    ASSERT_EQ(record.messages.size(), 2u) << describe_issues(record);
    EXPECT_TRUE(same(record.messages[1].message, SubscribeOk{6}));
    EXPECT_EQ(record.messages[1].at_ns, 2u);
    EXPECT_TRUE(record.fin_seen);
    EXPECT_EQ(count_issues(record, kIssueTruncatedAtFin), 1u) << describe_issues(record);
}

TEST(LiteStreamReader, ResetWhileWaitingForTheRunnersStreamType) {
    LiteStreamReader reader(kRunnerBidi, LiteOrigin::Runner, true);
    reader.feed_local(bytes({0x40}), false, 1);
    reader.feed(bytes({0x00, 0x01}), false, 2);
    reader.reset(5);
    reader.feed_local(concat({bytes({0x02}), kSubscribeMinimal}), false, 3);
    EXPECT_EQ(reader.record().messages.size(), 1u);  // only the runner's SUBSCRIBE
    EXPECT_TRUE(reader.record().issues.empty()) << describe_issues(reader.record());
    EXPECT_EQ(reader.record().reset_code.value_or(0), 5u);
}

TEST(LiteSession, ResetAndStopSendingAreIgnoredAfterTheLimit) {
    LiteSessionLimits limits;
    limits.max_streams = 1;
    LiteSession session(limits);
    session.on_event(data_event(kPeerUni, kGroupPreamble), 1);
    session.on_event(data_event(kPeerUni + 4, kGroupPreamble), 2);
    ASSERT_TRUE(session.limit_reached());
    session.on_event(PeerResetEvent{kPeerUni, 9}, 3);
    session.on_event(PeerStopSendingEvent{kPeerUni, 9}, 4);
    EXPECT_FALSE(session.streams()[0].reset_seen);
    EXPECT_FALSE(session.streams()[0].stop_sending_seen);
    EXPECT_EQ(session.streams().size(), 1u);
}

TEST(LiteStreamReader, LargeFrameInSmallChunks) {
    LiteStreamReader reader(kPeerUni, LiteOrigin::Peer, false);
    // FRAME payload 0xffff0 bytes (4-byte length 80 0f ff f0), then an empty frame.
    Bytes wire = concat({kGroupPreamble, bytes({0x00, 0x80, 0x0f, 0xff, 0xf0})});
    wire.resize(wire.size() + 0xffff0, std::byte{1});
    wire.push_back(std::byte{0});
    wire.push_back(std::byte{0});
    for (std::size_t i = 0; i < wire.size(); i += 1000) {
        reader.feed(std::span(wire).subspan(i, std::min<std::size_t>(1000, wire.size() - i)), false, i);
    }
    ASSERT_EQ(reader.record().messages.size(), 3u) << describe_issues(reader.record());
    EXPECT_EQ(std::get<Frame>(reader.record().messages[1].message).payload.size(), 0xffff0u);
    EXPECT_TRUE(reader.record().issues.empty());
}

TEST(LiteNames, KindAndMessageNames) {
    EXPECT_EQ(to_string(LiteStreamKind::Group), "group");
    EXPECT_EQ(to_string(LiteStreamKind::UnregisteredUni), "unregistered_uni");
    EXPECT_EQ(lite_message_name(LiteMessage{AnnounceOk{}}), "ANNOUNCE_OK");
    EXPECT_EQ(lite_message_name(LiteMessage{Frame{}}), "FRAME");
    EXPECT_EQ(lite_message_name(LiteMessage{SubscribeDrop{}}), "SUBSCRIBE_DROP");
    EXPECT_EQ(kLiteStreamOpenedKind, "lite_stream_opened");
    EXPECT_EQ(kLiteMessageKind, "lite_message");
    EXPECT_EQ(kLiteDecodeErrorKind, "lite_decode_error");
}

}  // namespace
}  // namespace moq::interop::session
