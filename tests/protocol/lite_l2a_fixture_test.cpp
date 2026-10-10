// Fixture-level tests for the L2a behaviors of ConformingLitePublisher (TRACK, FETCH, PROBE, GOAWAY): the real
// LiteProbeController drives the scripted peer with simple probes defined here, and each test asserts the exact
// bytes and reactions the fixture produces. No evaluators are involved.
#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/session/lite_session.h"
#include "support/scripted_lite_peer.h"

namespace {

using namespace std::chrono_literals;
using moq::interop::scenarios::LiteProbeDefinition;
using moq::interop::scenarios::LiteStep;
using moq::interop::scenarios::LiteTranscript;
using moq::interop::scenarios::ManualLiteClock;
using moq::interop::session::LiteStreamKind;
using moq::interop::session::LiteStreamRecord;
using namespace moq::interop::test::lite;
namespace scen = moq::interop::scenarios;
namespace wire = moq::interop::wire;
namespace sess = moq::interop::session;

const std::string kBroadcast = "demo/live";
const std::string kTrack = "video";

Bytes track_stream(const std::string& broadcast = kBroadcast, const std::string& track = kTrack) {
    return join({stream_type(0x6), track_request({broadcast, track})});
}

l06::FetchRequest fetch_of(std::uint64_t group, std::uint64_t start = 0, std::uint64_t end = 0,
                           const std::string& broadcast = kBroadcast, const std::string& track = kTrack) {
    return l06::FetchRequest{broadcast, track, 0x80, group, start, end};
}

Bytes fetch_stream(const l06::FetchRequest& request) { return join({stream_type(0x3), fetch_request(request)}); }

// A FETCH body built by hand, so an inverted range (which the typed encoder refuses) can be sent.
Bytes raw_fetch_stream(std::uint64_t group, std::uint64_t start, std::uint64_t end) {
    wire::ByteWriter body(256);
    EXPECT_TRUE(l06::write_string(kBroadcast, body));
    EXPECT_TRUE(l06::write_string(kTrack, body));
    EXPECT_TRUE(body.append_byte(std::byte{0x80}));
    EXPECT_TRUE(l06::write_varint(group, body));
    EXPECT_TRUE(l06::write_varint(start, body));
    EXPECT_TRUE(l06::write_varint(end, body));
    wire::ByteWriter framed(256);
    EXPECT_TRUE(l06::write_framed_message(body.bytes(), framed));
    return join({stream_type(0x3), Bytes(framed.bytes().begin(), framed.bytes().end())});
}

Bytes probe_stream(const l06::ProbeMessage& target = {0, 0}) { return join({stream_type(0x4), probe_message(target)}); }
Bytes goaway_stream(const std::string& uri = "") { return join({stream_type(0x5), goaway_message({uri})}); }

// A GOAWAY whose URI length prefix claims 8193 bytes (the draft 7.18 limit is 8192); the bytes need not follow.
Bytes oversize_goaway_stream() {
    wire::ByteWriter body(32);
    EXPECT_TRUE(l06::write_varint(8193, body));
    wire::ByteWriter framed(32);
    EXPECT_TRUE(l06::write_framed_message(body.bytes(), framed));
    return join({stream_type(0x5), Bytes(framed.bytes().begin(), framed.bytes().end())});
}

ConformingLitePublisherConfig config_with_probe_level(std::uint64_t level) {
    ConformingLitePublisherConfig config;
    wire::ByteWriter value(8);
    EXPECT_TRUE(l06::write_varint(level, value));
    config.setup_parameters.push_back(
        l06::SetupParameter{l06::kParamProbe, Bytes(value.bytes().begin(), value.bytes().end())});
    return config;
}

LiteTranscript run(ConformingLitePublisher& publisher, std::vector<LiteStep> steps,
                   std::chrono::milliseconds window = 100ms) {
    ScriptedLitePeer peer(publisher.reaction());
    ManualLiteClock clock;
    LiteProbeDefinition definition;
    definition.deadline = 3000ms;
    definition.steps = std::move(steps);
    definition.observation_window = window;
    scen::LiteProbeController controller(std::move(definition), peer, clock);
    for (std::size_t i = 0; i < 100000 && controller.poll(); ++i) clock.advance(1ms);
    return controller.transcript();
}

const LiteStreamRecord* runner_stream_record(const LiteTranscript& t, std::size_t ordinal) {
    // Runner bidirectional stream ids are 1, 5, 9, ...
    for (const auto& record : t.streams) {
        if (record.stream_id == 1 + 4 * ordinal) return &record;
    }
    return nullptr;
}

std::size_t peer_group_streams(const LiteTranscript& t) {
    std::size_t count = 0;
    for (const auto& record : t.streams) {
        if (record.origin == sess::LiteOrigin::Peer && record.kind == LiteStreamKind::Group) ++count;
    }
    return count;
}

std::vector<std::int64_t> deltas(const LiteStreamRecord& record) {
    std::vector<std::int64_t> out;
    for (const auto* message : sess::peer_fetch_frames(record)) {
        out.push_back(std::get<l06::Frame>(message->message).timestamp_delta);
    }
    return out;
}

std::vector<std::string> payloads(const LiteStreamRecord& record) {
    std::vector<std::string> out;
    for (const auto* message : sess::peer_fetch_frames(record)) {
        const auto& payload = std::get<l06::Frame>(message->message).payload;
        out.emplace_back(reinterpret_cast<const char*>(payload.data()), payload.size());
    }
    return out;
}

// --- TRACK ---------------------------------------------------------------------------------------------------------

TEST(LiteL2aFixture, TrackAnswersTheConfiguredInfoThenFin) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher, {scen::lite_open_bidi(track_stream(), false, "track")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->kind, LiteStreamKind::Track);
    const auto infos = sess::peer_track_info(*record);
    ASSERT_EQ(infos.size(), 1u);
    EXPECT_EQ(std::get<l06::TrackInfo>(infos[0]->message), (l06::TrackInfo{60, 30000, 90000}));
    EXPECT_TRUE(record->fin_seen);
    EXPECT_FALSE(record->reset_seen);
    EXPECT_TRUE(record->issues.empty());
    EXPECT_FALSE(t.peer_close.has_value());
}

TEST(LiteL2aFixture, TrackInfoFollowsTheConfiguration) {
    ConformingLitePublisherConfig config;
    config.track_priority = 80;
    config.track_max_age_ms = 0;
    config.track_timescale = 48000;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(track_stream(), false, "track")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(sess::peer_track_info(*record).size(), 1u);
    EXPECT_EQ(std::get<l06::TrackInfo>(sess::peer_track_info(*record)[0]->message), (l06::TrackInfo{80, 0, 48000}));
}

TEST(LiteL2aFixture, TrackForAnUnknownTrackOrBroadcastIsResetNotFound) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher, {scen::lite_open_bidi(track_stream(kBroadcast, "audio"), false, "no-track"),
                                   scen::lite_open_bidi(track_stream("other/live", kTrack), false, "no-broadcast")});
    for (std::size_t i = 0; i < 2; ++i) {
        const auto* record = runner_stream_record(t, i);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(sess::peer_track_info(*record).empty());
        ASSERT_TRUE(record->reset_code.has_value()) << i;
        EXPECT_EQ(*record->reset_code, 0x33u);
    }
}

TEST(LiteL2aFixture, TwoTrackRequestsGetIdenticalInfo) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher, {scen::lite_open_bidi(track_stream(), false, "first"),
                                   scen::lite_open_bidi(track_stream(), false, "second")});
    const auto* a = runner_stream_record(t, 0);
    const auto* b = runner_stream_record(t, 1);
    ASSERT_TRUE(a != nullptr && b != nullptr);
    ASSERT_EQ(sess::peer_track_info(*a).size(), 1u);
    ASSERT_EQ(sess::peer_track_info(*b).size(), 1u);
    EXPECT_EQ(std::get<l06::TrackInfo>(sess::peer_track_info(*a)[0]->message),
              std::get<l06::TrackInfo>(sess::peer_track_info(*b)[0]->message));
}

TEST(LiteL2aFixture, DefectTrackInfoChangesBetweenRequests) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::TrackInfoChangesBetweenRequests;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(track_stream(), false, "first"),
                                   scen::lite_open_bidi(track_stream(), false, "second")});
    const auto* a = runner_stream_record(t, 0);
    const auto* b = runner_stream_record(t, 1);
    ASSERT_TRUE(a != nullptr && b != nullptr);
    ASSERT_EQ(sess::peer_track_info(*a).size(), 1u);
    ASSERT_EQ(sess::peer_track_info(*b).size(), 1u);
    EXPECT_NE(std::get<l06::TrackInfo>(sess::peer_track_info(*a)[0]->message),
              std::get<l06::TrackInfo>(sess::peer_track_info(*b)[0]->message));
}

TEST(LiteL2aFixture, DefectTrackInfoZeroTimescale) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::TrackInfoZeroTimescale;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(track_stream(), false, "track")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    ASSERT_EQ(sess::peer_track_info(*record).size(), 1u);
    EXPECT_EQ(std::get<l06::TrackInfo>(sess::peer_track_info(*record)[0]->message).timescale, 0u);
}

// --- FETCH ---------------------------------------------------------------------------------------------------------

TEST(LiteL2aFixture, FetchWholeGroupIsBareFramesThenFin) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher, {scen::lite_open_bidi(fetch_stream(fetch_of(3)), false, "fetch")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->kind, LiteStreamKind::Fetch);
    EXPECT_TRUE(record->issues.empty());
    EXPECT_TRUE(record->fin_seen);
    EXPECT_FALSE(record->reset_seen);
    // Six frames; the first delta is the absolute timestamp 3 * 1000, later deltas are 33.
    EXPECT_EQ(deltas(*record), (std::vector<std::int64_t>{3000, 33, 33, 33, 33, 33}));
    EXPECT_EQ(payloads(*record).front(), "frame-3-0");
    EXPECT_EQ(payloads(*record).back(), "frame-3-5");
    // No GROUP header is sent: the stream holds frames only.
    for (const auto* message : sess::peer_messages(*record)) {
        EXPECT_TRUE(std::holds_alternative<l06::Frame>(message->message));
    }
}

TEST(LiteL2aFixture, FetchRangeHonorsFrameStartAndFrameEndAsIndexPlusOne) {
    ConformingLitePublisher publisher;
    // Frames 2..4 (Frame End 5 is index 4 inclusive).
    const auto t = run(publisher, {scen::lite_open_bidi(fetch_stream(fetch_of(3, 2, 5)), false, "range"),
                                   // A single frame: start 4, end 5.
                                   scen::lite_open_bidi(fetch_stream(fetch_of(3, 4, 5)), false, "single"),
                                   // Frame End beyond the group: clipped to the group.
                                   scen::lite_open_bidi(fetch_stream(fetch_of(3, 4, 99)), false, "clipped")});
    const auto* range = runner_stream_record(t, 0);
    const auto* single = runner_stream_record(t, 1);
    const auto* clipped = runner_stream_record(t, 2);
    ASSERT_TRUE(range != nullptr && single != nullptr && clipped != nullptr);
    EXPECT_EQ(payloads(*range), (std::vector<std::string>{"frame-3-2", "frame-3-3", "frame-3-4"}));
    EXPECT_EQ(deltas(*range), (std::vector<std::int64_t>{3066, 33, 33}));
    EXPECT_EQ(payloads(*single), (std::vector<std::string>{"frame-3-4"}));
    EXPECT_EQ(deltas(*single), (std::vector<std::int64_t>{3132}));
    EXPECT_EQ(payloads(*clipped), (std::vector<std::string>{"frame-3-4", "frame-3-5"}));
    EXPECT_TRUE(range->fin_seen && single->fin_seen && clipped->fin_seen);
}

TEST(LiteL2aFixture, FetchBeyondTheRetainedHeadIsResetTooFarBehindNotTruncated) {
    ConformingLitePublisherConfig config;
    config.fetch_first_retained_frame = 3;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(fetch_stream(fetch_of(3, 1, 0)), false, "lagged"),
                                   scen::lite_open_bidi(fetch_stream(fetch_of(3, 3, 0)), false, "retained")});
    const auto* lagged = runner_stream_record(t, 0);
    const auto* retained = runner_stream_record(t, 1);
    ASSERT_TRUE(lagged != nullptr && retained != nullptr);
    EXPECT_TRUE(sess::peer_fetch_frames(*lagged).empty());
    ASSERT_TRUE(lagged->reset_code.has_value());
    EXPECT_EQ(*lagged->reset_code, 0x5u);
    EXPECT_EQ(sess::peer_fetch_frames(*retained).size(), 3u);
    EXPECT_FALSE(retained->reset_seen);
}

TEST(LiteL2aFixture, FetchOfAnUnknownGroupTrackOrBroadcastIsResetNotFound) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher,
                       {scen::lite_open_bidi(fetch_stream(fetch_of(99)), false, "group"),
                        scen::lite_open_bidi(fetch_stream(fetch_of(3, 0, 0, kBroadcast, "audio")), false, "track"),
                        scen::lite_open_bidi(fetch_stream(fetch_of(3, 0, 0, "other/live")), false, "broadcast")});
    for (std::size_t i = 0; i < 3; ++i) {
        const auto* record = runner_stream_record(t, i);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(sess::peer_fetch_frames(*record).empty()) << i;
        ASSERT_TRUE(record->reset_code.has_value()) << i;
        EXPECT_EQ(*record->reset_code, 0x33u);
    }
}

TEST(LiteL2aFixture, FetchWithAnInvertedRangeResetsTheStreamAndKeepsTheSession) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher, {scen::lite_open_bidi(raw_fetch_stream(3, 5, 3), false, "inverted")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    EXPECT_TRUE(sess::peer_fetch_frames(*record).empty());
    EXPECT_TRUE(record->reset_seen);
    EXPECT_FALSE(t.peer_close.has_value());
}

TEST(LiteL2aFixture, DefectFetchTruncatesOnShortRange) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::FetchTruncatesOnShortRange;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(fetch_stream(fetch_of(3, 1, 5)), false, "short")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(sess::peer_fetch_frames(*record).size(), 3u) << "frames 1..4 requested, one fewer returned";
    EXPECT_TRUE(record->fin_seen);
    EXPECT_FALSE(record->reset_seen);
}

TEST(LiteL2aFixture, DefectFetchIgnoresUnknownGroup) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::FetchIgnoresUnknownGroup;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(fetch_stream(fetch_of(99)), false, "group")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    EXPECT_FALSE(sess::peer_fetch_frames(*record).empty());
    EXPECT_TRUE(record->fin_seen);
    EXPECT_FALSE(record->reset_seen);
}

// --- PROBE ---------------------------------------------------------------------------------------------------------

TEST(LiteL2aFixture, ProbeAtReportLevelAnswersImmediatelyAndPeriodically) {
    ConformingLitePublisher publisher(config_with_probe_level(1));
    const auto t = run(publisher, {scen::lite_open_bidi(probe_stream({8000000, 0}), false, "probe")}, 300ms);
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->kind, LiteStreamKind::Probe);
    const auto reports = sess::peer_probe_reports(*record);
    ASSERT_GE(reports.size(), 3u);
    for (const auto* report : reports) {
        EXPECT_EQ(std::get<l06::ProbeMessage>(report->message), (l06::ProbeMessage{5000000, 25}));
    }
    // The target is read and ignored at Report level: no padding, no reset.
    EXPECT_FALSE(record->reset_seen);
    EXPECT_FALSE(t.peer_close.has_value());
}

TEST(LiteL2aFixture, ProbeReportsFollowTheConfiguredValues) {
    auto config = config_with_probe_level(2);
    config.probe_bitrate = 0;
    config.probe_rtt_ms = 0;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(probe_stream(), false, "probe")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    ASSERT_FALSE(sess::peer_probe_reports(*record).empty());
    EXPECT_EQ(std::get<l06::ProbeMessage>(sess::peer_probe_reports(*record)[0]->message), (l06::ProbeMessage{0, 0}));
}

TEST(LiteL2aFixture, ProbeAtLevelNoneOrWithoutTheParameterIsReset) {
    for (const bool parameter : {true, false}) {
        ConformingLitePublisher publisher(parameter ? config_with_probe_level(0) : ConformingLitePublisherConfig{});
        const auto t = run(publisher, {scen::lite_open_bidi(probe_stream({8000000, 0}), false, "probe")});
        const auto* record = runner_stream_record(t, 0);
        ASSERT_NE(record, nullptr);
        EXPECT_TRUE(sess::peer_probe_reports(*record).empty()) << parameter;
        EXPECT_TRUE(record->reset_seen) << parameter;
        EXPECT_FALSE(t.peer_close.has_value());
    }
}

TEST(LiteL2aFixture, DefectProbeNoneNotReset) {
    auto config = config_with_probe_level(0);
    config.defect = LiteDefect::ProbeNoneNotReset;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(probe_stream({8000000, 0}), false, "probe")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    EXPECT_FALSE(record->reset_seen);
    EXPECT_FALSE(sess::peer_probe_reports(*record).empty());
}

TEST(LiteL2aFixture, DefectProbeResetsOnTarget) {
    auto config = config_with_probe_level(1);
    config.defect = LiteDefect::ProbeResetsOnTarget;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(probe_stream({8000000, 0}), false, "probe")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    ASSERT_TRUE(record->reset_code.has_value());
    EXPECT_EQ(*record->reset_code, 0x12u) << "MALFORMED_TRACK, as the moq CLI does";
}

TEST(LiteL2aFixture, ASecondTargetOnTheSameProbeStreamIsReadAndIgnored) {
    ConformingLitePublisher publisher(config_with_probe_level(1));
    const auto t = run(publisher,
                       {scen::lite_open_bidi(probe_stream({8000000, 0}), false, "probe"),
                        scen::lite_send_on(0, probe_message({9000000, 0}), false, "second-target")},
                       200ms);
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    EXPECT_FALSE(record->reset_seen);
    EXPECT_FALSE(sess::peer_probe_reports(*record).empty());
}

// --- GOAWAY --------------------------------------------------------------------------------------------------------

TEST(LiteL2aFixture, AGoawayIsRecordedAndTheSessionStaysOpen) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher, {scen::lite_open_bidi(goaway_stream("moql://other.example/moq"), false, "goaway")});
    EXPECT_FALSE(t.peer_close.has_value());
    ASSERT_EQ(publisher.goaways().size(), 1u);
    EXPECT_EQ(publisher.goaways()[0], "moql://other.example/moq");
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    EXPECT_EQ(record->kind, LiteStreamKind::Goaway);
    EXPECT_TRUE(sess::peer_messages(*record).empty()) << "the publisher answers nothing on a GOAWAY stream";
    EXPECT_FALSE(record->reset_seen);
}

TEST(LiteL2aFixture, ASecondGoawayClosesTheSessionWithProtocolViolation) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher, {scen::lite_open_bidi(goaway_stream(), false, "first"),
                                   scen::lite_open_bidi(goaway_stream(), false, "second")});
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x3u);
}

TEST(LiteL2aFixture, AnOversizeGoawayUriClosesTheSessionWithProtocolViolation) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher, {scen::lite_open_bidi(oversize_goaway_stream(), false, "oversize")});
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x3u);
}

TEST(LiteL2aFixture, AMalformedGoawayClosesTheSessionWithProtocolViolation) {
    ConformingLitePublisher publisher;
    // Message Length 2 over a body of one empty-URI byte plus a stray one.
    const auto t = run(publisher,
                       {scen::lite_open_bidi(join({stream_type(0x5), Bytes{std::byte{2}, std::byte{0}, std::byte{0}}}),
                                             false, "malformed")});
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x3u);
}

TEST(LiteL2aFixture, DefectGoawayOversizeLoggedKeepsTheSessionOpen) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::GoawayOversizeLogged;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(oversize_goaway_stream(), false, "oversize")});
    EXPECT_FALSE(t.peer_close.has_value());
}

TEST(LiteL2aFixture, DefectGoawayDuplicateIgnoredKeepsTheSessionOpen) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::GoawayDuplicateIgnored;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(goaway_stream(), false, "first"),
                                   scen::lite_open_bidi(goaway_stream(), false, "second")});
    EXPECT_FALSE(t.peer_close.has_value());
    EXPECT_EQ(publisher.goaways().size(), 2u);
}

TEST(LiteL2aFixture, DefectGoawayClosesSessionOnFirst) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::GoawayClosesSessionOnFirst;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher, {scen::lite_open_bidi(goaway_stream(), false, "goaway")});
    ASSERT_TRUE(t.peer_close.has_value());
    EXPECT_EQ(t.peer_close->code, 0x0u);
}

// Draft 5.1.6: the receiver of a GOAWAY MUST NOT open new streams. A SUBSCRIBE sent after the GOAWAY is still
// answered on its own (runner-opened) stream, but no Group stream follows.
TEST(LiteL2aFixture, AfterAGoawayThePublisherOpensNoNewStreams) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher,
                       {scen::lite_open_bidi(goaway_stream(), false, "goaway"),
                        scen::lite_open_bidi(scen::lite_subscribe_stream_bytes(
                                                 l06::Subscribe{1, kBroadcast, kTrack, l06::SubscribeRange{}}),
                                             false, "subscribe")},
                       200ms);
    EXPECT_FALSE(t.peer_close.has_value());
    const auto* subscribe = runner_stream_record(t, 1);
    ASSERT_NE(subscribe, nullptr);
    EXPECT_FALSE(sess::peer_messages(*subscribe).empty()) << "SUBSCRIBE_OK is on the runner's own stream";
    EXPECT_EQ(peer_group_streams(t), 0u);
}

TEST(LiteL2aFixture, DefectOpensStreamsAfterGoaway) {
    ConformingLitePublisherConfig config;
    config.defect = LiteDefect::OpensStreamsAfterGoaway;
    ConformingLitePublisher publisher(config);
    const auto t = run(publisher,
                       {scen::lite_open_bidi(goaway_stream(), false, "goaway"),
                        scen::lite_open_bidi(scen::lite_subscribe_stream_bytes(
                                                 l06::Subscribe{1, kBroadcast, kTrack, l06::SubscribeRange{}}),
                                             false, "subscribe")},
                       200ms);
    EXPECT_FALSE(t.peer_close.has_value());
    EXPECT_GT(peer_group_streams(t), 0u);
}

// --- the L1 behavior is untouched --------------------------------------------------------------------------------

TEST(LiteL2aFixture, UnknownBidiStreamTypesAreStillRefusedAsBefore) {
    ConformingLitePublisher publisher;
    const auto t = run(publisher, {scen::lite_open_bidi(stream_type(0x7), false, "unknown")});
    const auto* record = runner_stream_record(t, 0);
    ASSERT_NE(record, nullptr);
    ASSERT_TRUE(record->reset_code.has_value());
    EXPECT_EQ(*record->reset_code, 0x0u);
}

}  // namespace
