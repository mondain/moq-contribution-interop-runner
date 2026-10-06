// Draft 22 own scenarios for D22-9-20-9-MUST-424 (Section 9.20.9, StartGroup + EndGroupDelta overflow) and
// the unscored Location Filter probes: the bytes each probe sends, tied to the draft 22 codec, and the
// evaluators' verdicts on transcripts recorded by a RawProbeController against a scripted publisher (with a
// manual clock, so the reaction window and the liveness follow-up are exercised as in a run).
#include "moq/interop/scenarios/draft22_location_filter_probes.h"
#include "moq/interop/scenarios/raw_probe_liveness.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft22/location_filter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace moq::interop::scenarios {
namespace {

using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;
namespace d22 = wire::draft22;

Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
Bytes cat(Bytes left, const Bytes& right) {
    left.insert(left.end(), right.begin(), right.end());
    return left;
}
std::string hex(const Bytes& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : bytes) {
        out.push_back(digits[std::to_integer<unsigned>(byte) >> 4u]);
        out.push_back(digits[std::to_integer<unsigned>(byte) & 15u]);
    }
    return out;
}

constexpr std::uint64_t kLargest = std::numeric_limits<std::uint64_t>::max();
// vi64 2^64 - 1: 0xff and eight 0xff.
const Bytes kMaxVi = b({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff});

class Draft22LocationFilterProbes : public ::testing::Test {
protected:
    ScopedWireDraft wire{22};
};

RawProbeDefinition overflow_probe(std::chrono::milliseconds deadline = 5000ms) {
    return draft22_location_filter_overflow_probe(deadline, {b({'n'})}, b({'t'}));
}
RawProbeDefinition fill_overflow_probe(std::chrono::milliseconds deadline = 5000ms) {
    return draft22_fill_location_filter_overflow_probe(deadline, {b({'n'})}, b({'t'}));
}

// The definition as the run hands it to the controller: liveness bound to the fixture.
RawProbeDefinition bound(RawProbeDefinition definition) {
    bind_liveness_track(definition, {b({'n'})}, b({'t'}));
    return definition;
}

wire::DecodeResult<d22::LocationFilter> decode(const Bytes& value, std::size_t* consumed = nullptr) {
    wire::Cursor cursor(value);
    auto result = d22::decode_location_filter(cursor);
    if (consumed) *consumed = value.size() - cursor.remaining();
    return result;
}

// ------------------------------------------------------------------ bytes and codec

TEST_F(Draft22LocationFilterProbes, TopLevelProbeSendsTheType3OverflowAsTheOnlySubscribeParameter) {
    const auto p = overflow_probe();
    EXPECT_EQ(p.id, kDraft22LocationFilterOverflow);
    EXPECT_EQ(p.setup_bytes, b({0xaf, 0, 0, 0}));
    ASSERT_EQ(p.writes.size(), 1u);
    EXPECT_EQ(p.writes[0].channel, RawProbeChannel::NewBidi);
    EXPECT_FALSE(p.writes[0].fin);
    // SUBSCRIBE (0x3), Length 20: Request ID 1, (n), t, one parameter: 0x21, Type 03, StartGroup 2^64 - 1,
    // StartObject 0, EndGroupDelta 1.
    EXPECT_EQ(hex(p.writes[0].bytes), "03" "0014" "01" "01016e" "0174" "01" "21" "03" "ffffffffffffffffff" "00" "01");
    EXPECT_EQ(draft22_overflow_filter_value(kDraft22LocationFilterOverflow),
              cat(cat(b({0x03}), kMaxVi), b({0, 1})));
}

TEST_F(Draft22LocationFilterProbes, FillProbeNestsTheType4OverflowInFillParameters) {
    const auto p = fill_overflow_probe();
    EXPECT_EQ(p.id, kDraft22FillLocationFilterOverflow);
    ASSERT_EQ(p.writes.size(), 1u);
    // SUBSCRIBE, Length 23: Request ID 1, (n), t, one parameter: FILL_PARAMETERS 0x23, Length 14, then
    // LOCATION_FILTER 0x21 (no count, Section 9.20.15) Type 04 {2^64 - 1, 0, 1, 0}.
    EXPECT_EQ(hex(p.writes[0].bytes),
              "03" "0017" "01" "01016e" "0174" "01" "23" "0e" "21" "04" "ffffffffffffffffff" "00" "01" "00");
    EXPECT_EQ(draft22_overflow_filter_value(kDraft22FillLocationFilterOverflow),
              cat(cat(b({0x04}), kMaxVi), b({0, 1, 0})));
    EXPECT_TRUE(draft22_overflow_filter_value("d22-no-such-scenario").empty());
}

TEST_F(Draft22LocationFilterProbes, TheDecoderRejectsExactlyTheNestedBytesTheFillProbeSends) {
    const auto p = fill_overflow_probe();
    const auto value = draft22_overflow_filter_value(kDraft22FillLocationFilterOverflow);
    const auto& write = p.writes[0].bytes;
    // FILL_PARAMETERS' value is the nested LOCATION_FILTER parameter and nothing else.
    ASSERT_GT(write.size(), value.size() + 3);
    const auto tail = write.end() - static_cast<std::ptrdiff_t>(value.size());
    EXPECT_TRUE(std::equal(value.begin(), value.end(), tail));
    EXPECT_EQ(Bytes(tail - 3, tail), b({0x23, static_cast<unsigned>(value.size() + 1), 0x21}));
    const auto decoded = decode(value);
    const auto* error = std::get_if<wire::DecodeError>(&decoded);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, wire::DecodeErrorCode::ProtocolViolation);
    wire::ByteWriter output(64);
    EXPECT_EQ(d22::encode_location_filter({d22::LocationFilterType::AbsoluteRange, kLargest, 0, 1, 0}, output),
              d22::LocationFilterEncodeError::InvalidValue);
    // The boundary 2^64 - 2 + 1 decodes for Type 0x04 as well.
    const auto boundary = b({0x04, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0, 1, 0});
    EXPECT_TRUE(std::holds_alternative<d22::LocationFilter>(decode(boundary)));
}

TEST_F(Draft22LocationFilterProbes, TheDecoderRejectsExactlyTheBytesTheProbeSends) {
    const auto p = overflow_probe();
    const auto value = draft22_overflow_filter_value(kDraft22LocationFilterOverflow);
    ASSERT_FALSE(value.empty());
    // The value is the tail of the SUBSCRIBE, right after the parameter's type delta.
    const auto& write = p.writes[0].bytes;
    ASSERT_GT(write.size(), value.size());
    EXPECT_TRUE(std::equal(value.begin(), value.end(), write.end() - static_cast<std::ptrdiff_t>(value.size())));
    EXPECT_EQ(*(write.end() - static_cast<std::ptrdiff_t>(value.size()) - 1), std::byte{0x21});
    const auto decoded = decode(value);
    const auto* error = std::get_if<wire::DecodeError>(&decoded);
    ASSERT_NE(error, nullptr) << "Section 9.20.9: the sum exceeds 2^64 - 1";
    EXPECT_EQ(error->code, wire::DecodeErrorCode::ProtocolViolation);
    // ... and the encoder refuses to produce them, which is why the probe writes them by hand.
    wire::ByteWriter output(64);
    EXPECT_EQ(d22::encode_location_filter({d22::LocationFilterType::AbsoluteBounded, kLargest, 0, 1, std::nullopt},
                                          output),
              d22::LocationFilterEncodeError::InvalidValue);
}

TEST_F(Draft22LocationFilterProbes, OnlyTheOverflowIsTheViolation) {
    // The boundary: StartGroup 2^64 - 2 + EndGroupDelta 1 is exactly 2^64 - 1 and decodes.
    std::size_t consumed = 0;
    auto value = b({0x03, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe, 0, 1});
    const auto boundary = decode(value, &consumed);
    ASSERT_TRUE(std::holds_alternative<d22::LocationFilter>(boundary));
    EXPECT_EQ(std::get<d22::LocationFilter>(boundary).start_group, kLargest - 1);
    EXPECT_EQ(consumed, value.size());
    // The relative Type 0x01 with StartGroup 2^64 - 1 is clamped (start Group 0), not an error.
    value = cat(b({0x01}), kMaxVi);
    const auto relative = decode(value, &consumed);
    ASSERT_TRUE(std::holds_alternative<d22::LocationFilter>(relative));
    EXPECT_EQ(std::get<d22::LocationFilter>(relative).type, d22::LocationFilterType::RelativeGroup);
}

TEST_F(Draft22LocationFilterProbes, ProbesCarryTheLivenessFollowUpOfTheirDraft21Counterparts) {
    ASSERT_TRUE(liveness_follow_up_sound(21, "d21-location-filter-end-group-overflow"));
    ASSERT_TRUE(liveness_follow_up_sound(21, "d21-fill-location-filter-end-group-overflow"));
    for (const auto& p : {overflow_probe(), fill_overflow_probe()}) {
        ASSERT_TRUE(p.liveness.has_value());
        EXPECT_EQ(p.liveness->draft, 21u) << "draft 22 SUBSCRIBE_OK keeps the draft 21 encoding";
        EXPECT_TRUE(p.liveness->request.empty()) << "bound to the track fixture by the run";
        EXPECT_GT(p.liveness->request_id, 1u);
        EXPECT_TRUE(liveness_definition_eligible(p));
        const auto definition = bound(p);
        EXPECT_TRUE(liveness_request_valid(*definition.liveness));
    }
}

TEST(Draft22LocationFilterProbesWire, ProbesAreBuiltOnTheDraft22WireOnly) {
    const ScopedWireDraft wire(21);
    EXPECT_THROW(overflow_probe(), std::logic_error);
    EXPECT_THROW(fill_overflow_probe(), std::logic_error);
}

TEST_F(Draft22LocationFilterProbes, AnInvalidFixtureOrDeadlineIsRefused) {
    EXPECT_THROW(draft22_location_filter_overflow_probe(0ms, {b({'n'})}, b({'t'})), std::invalid_argument);
    EXPECT_THROW(draft22_location_filter_overflow_probe(1000ms, {Bytes{}}, b({'t'})), std::invalid_argument);
}

// ------------------------------------------------------------------ unscored: undefined Type

RawProbeDefinition unknown_type_probe(std::chrono::milliseconds deadline = 5000ms) {
    return draft22_location_filter_unknown_type_probe(deadline, {b({'n'})}, b({'t'}));
}

TEST_F(Draft22LocationFilterProbes, UnknownTypeProbeSendsType6WithNoFields) {
    const auto p = unknown_type_probe();
    EXPECT_EQ(p.id, kDraft22LocationFilterUnknownType);
    ASSERT_EQ(p.writes.size(), 1u);
    // SUBSCRIBE, Length 9: Request ID 1, (n), t, one parameter: 0x21, Type 06.
    EXPECT_EQ(hex(p.writes[0].bytes), "03" "0009" "01" "01016e" "0174" "01" "21" "06");
    const auto decoded = decode(b({0x06}));
    const auto* error = std::get_if<wire::DecodeError>(&decoded);
    ASSERT_NE(error, nullptr) << "Section 9.20.9: any other Location Filter Type is a PROTOCOL_VIOLATION";
    EXPECT_EQ(error->code, wire::DecodeErrorCode::ProtocolViolation);
    // Type 0x05, the last defined one, decodes (Next Object, no field).
    EXPECT_TRUE(std::holds_alternative<d22::LocationFilter>(decode(b({0x05}))));
    EXPECT_FALSE(p.liveness.has_value()) << "no BCP 14 MUST close behind the follow-up's argument";
    const ScopedWireDraft wire21(21);
    EXPECT_THROW(unknown_type_probe(), std::logic_error);
}

// ------------------------------------------------------------------ unscored: Absolute {0, 0}

RawProbeDefinition absolute_origin_probe(std::chrono::milliseconds deadline = 1000ms) {
    return draft22_location_filter_absolute_origin_probe(deadline, {b({'n'})}, b({'t'}));
}

TEST_F(Draft22LocationFilterProbes, AbsoluteOriginProbeSendsType2ZeroZeroFromTheEncoder) {
    const auto p = absolute_origin_probe();
    EXPECT_EQ(p.id, kDraft22LocationFilterAbsoluteOrigin);
    ASSERT_EQ(p.writes.size(), 1u);
    // SUBSCRIBE, Length 11: Request ID 1, (n), t, one parameter: 0x21, Type 02, StartGroup 0, StartObject 0.
    EXPECT_EQ(hex(p.writes[0].bytes), "03" "000b" "01" "01016e" "0174" "01" "21" "02" "00" "00");
    wire::ByteWriter output(16);
    ASSERT_FALSE(d22::encode_location_filter({d22::LocationFilterType::Absolute, 0, 0, {}, {}}, output));
    EXPECT_EQ(Bytes(output.bytes().begin(), output.bytes().end()), b({2, 0, 0}));
    const auto decoded = decode(b({2, 0, 0}));
    ASSERT_TRUE(std::holds_alternative<d22::LocationFilter>(decoded));
    EXPECT_EQ(std::get<d22::LocationFilter>(decoded).type, d22::LocationFilterType::Absolute)
        << "not Next Object (Type 05), which the draft 21 field list {0, 0} meant";
    EXPECT_FALSE(p.liveness.has_value());
    const ScopedWireDraft wire21(21);
    EXPECT_THROW(absolute_origin_probe(), std::logic_error);
}

// ------------------------------------------------------------------ recorded sessions

class ScriptedPeer : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::Success, next_bidi += 4}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, next_uni += 4}; }
    transport::OperationResult write(transport::StreamId id, std::span<const std::byte> bytes, bool) override {
        output[id].insert(output[id].end(), bytes.begin(), bytes.end());
        return {transport::TransportStatus::Success, bytes.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult stop_sending(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult send_datagram(std::span<const std::byte>) override { return {}; }
    transport::OperationResult close(std::uint64_t, std::span<const std::byte>) override { return {}; }
    std::vector<transport::TransportEvent> poll(std::size_t) override {
        auto result = std::move(events);
        events.clear();
        return result;
    }
    std::uint64_t next_bidi = static_cast<std::uint64_t>(-3);
    std::uint64_t next_uni = static_cast<std::uint64_t>(-1);
    std::map<std::uint64_t, Bytes> output;
    std::vector<transport::TransportEvent> events;
};

// One scripted session against a RawProbeController: the publisher's connection and SETUP are queued, the
// stimulus goes out on the first poll (stream 1), the follow-up 500 ms later (stream 5).
class Session {
public:
    explicit Session(RawProbeDefinition definition)
        : definition_(std::move(definition)), controller_(std::make_unique<RawProbeController>(peer, definition_)) {
        peer.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
        peer.events.push_back(transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false});
    }
    const RawProbeTranscript& poll(std::chrono::milliseconds at) { return controller_->poll(t0_ + at); }
    void send(transport::TransportEvent event) { peer.events.push_back(std::move(event)); }
    void close(std::uint64_t code) { send(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, code, {}}); }
    const RawProbeTranscript& transcript() const { return controller_->transcript(); }
    ScriptedPeer peer;

private:
    RawProbeDefinition definition_;
    std::unique_ptr<RawProbeController> controller_;
    RawProbeClock::time_point t0_{RawProbeClock::time_point{} + 1000s};
};

constexpr transport::StreamId kStimulus = 1;
constexpr transport::StreamId kFollowUp = 5;
const Bytes kSubscribeOk = b({4, 0, 2, 0, 0});        // Track Alias 0, no parameters
const Bytes kRequestError = b({5, 0, 3, 0x10, 0, 0});  // DOES_NOT_EXIST, Retry Interval 0, no reason

using Factory = RawProbeDefinition (*)(std::chrono::milliseconds);

class OverflowVerdicts : public ::testing::TestWithParam<Factory> {
protected:
    ScopedWireDraft wire{22};
    RawProbeDefinition probe() const { return bound(GetParam()(5000ms)); }
    static std::optional<bool> verdict(const RawProbeTranscript& t) {
        return evaluate_draft22_location_filter_overflow(t);
    }
    // The stimulus is accepted on the first poll; the publisher closes `after` it with `code`.
    std::optional<bool> close_after(std::chrono::milliseconds after, std::uint64_t code) const {
        Session session(probe());
        session.poll(0ms);
        EXPECT_EQ(session.peer.output[kStimulus], probe().writes[0].bytes);
        session.close(code);
        return verdict(session.poll(after));
    }
};

TEST_P(OverflowVerdicts, CloseWithProtocolViolationPasses) {
    EXPECT_EQ(close_after(10ms, 0x3), std::optional<bool>{true});
    EXPECT_EQ(close_after(1400ms, 0x3), std::optional<bool>{true}) << "within the reaction window";
}

TEST_P(OverflowVerdicts, CloseWithAnyOtherCodeFails) {
    EXPECT_EQ(close_after(10ms, 0x1), std::optional<bool>{false}) << "INTERNAL_ERROR";
    EXPECT_EQ(close_after(10ms, 0x6), std::optional<bool>{false}) << "KEY_VALUE_FORMATTING_ERROR";
    EXPECT_EQ(close_after(10ms, 0x0), std::optional<bool>{false}) << "NO_ERROR, as for the draft 21 counterpart";
}

TEST_P(OverflowVerdicts, ACloseTooLateOrAtTheTransportLevelIsNotJudged) {
    // Beyond the 1500 ms reaction window plus the follow-up's delay and grace.
    EXPECT_FALSE(close_after(2600ms, 0x3).has_value());
    Session session(probe());
    session.poll(0ms);
    session.send(transport::PeerCloseEvent{transport::CloseErrorSpace::Transport, 0x3, {}});
    EXPECT_FALSE(verdict(session.poll(10ms)).has_value());
}

TEST_P(OverflowVerdicts, ServingTheFollowUpWithoutClosingFails) {
    Session session(probe());
    session.poll(0ms);
    // The publisher even accepts the overflowing subscription.
    session.send(transport::StreamDataEvent{kStimulus, kSubscribeOk, false});
    session.poll(499ms);
    ASSERT_FALSE(session.transcript().liveness.has_value()) << "the follow-up waits 500 ms";
    session.poll(500ms);
    ASSERT_TRUE(session.transcript().liveness.has_value());
    ASSERT_EQ(*session.transcript().liveness->write.stream_id, kFollowUp);
    EXPECT_FALSE(verdict(session.transcript()).has_value()) << "not answered yet";
    session.send(transport::StreamDataEvent{kFollowUp, kSubscribeOk, false});
    session.poll(520ms);
    const auto& done = session.poll(1020ms);
    ASSERT_TRUE(done.complete);
    EXPECT_EQ(verdict(done), std::optional<bool>{false});
}

TEST_P(OverflowVerdicts, ARejectionWithoutACloseIsNotTheRequiredReaction) {
    // REQUEST_ERROR on the overflowing request, then silence: neither the close the row requires nor proof
    // that the session kept serving.
    Session session(probe());
    session.poll(0ms);
    session.send(transport::StreamDataEvent{kStimulus, kRequestError, true});
    session.poll(500ms);
    const auto& end = session.poll(6001ms);
    ASSERT_TRUE(end.timed_out);
    EXPECT_FALSE(verdict(end).has_value());
    // A refused follow-up proves nothing either.
    Session refused(probe());
    refused.poll(0ms);
    refused.poll(500ms);
    refused.send(transport::StreamDataEvent{kFollowUp, kRequestError, true});
    refused.poll(520ms);
    EXPECT_FALSE(verdict(refused.poll(6001ms)).has_value());
}

TEST_P(OverflowVerdicts, AServedFollowUpFollowedByACloseIsJudgedByTheClose) {
    Session session(probe());
    session.poll(0ms);
    session.poll(500ms);
    session.send(transport::StreamDataEvent{kFollowUp, kSubscribeOk, false});
    session.poll(520ms);
    session.close(0x3);
    EXPECT_EQ(verdict(session.poll(700ms)), std::optional<bool>{true});
}

TEST_P(OverflowVerdicts, AnUnprovenOrForeignTranscriptIsNotJudged) {
    Session session(probe());
    session.poll(0ms);
    session.close(0x3);
    const auto done = session.poll(10ms);
    ASSERT_EQ(verdict(done), std::optional<bool>{true});
    // An altered stimulus byte (the last field of the filter).
    auto altered = done;
    altered.writes[0].write.bytes.back() ^= std::byte{1};
    EXPECT_FALSE(verdict(altered).has_value());
    // Another scenario's transcript, a harness failure, a close before the stimulus was accepted.
    auto other = done;
    other.scenario_id = "d22-subscribe-bounded-location-range";
    EXPECT_FALSE(verdict(other).has_value());
    auto failed = done;
    failed.harness_failed = true;
    EXPECT_FALSE(verdict(failed).has_value());
    auto early = done;
    early.delivery_event_count = early.events.size();
    EXPECT_FALSE(verdict(early).has_value());
    // Judged on the draft 22 wire only.
    const ScopedWireDraft wire21(21);
    EXPECT_FALSE(verdict(done).has_value());
}

INSTANTIATE_TEST_SUITE_P(Overflow, OverflowVerdicts,
                         ::testing::Values(+[](std::chrono::milliseconds deadline) { return overflow_probe(deadline); },
                                           +[](std::chrono::milliseconds deadline) {
                                               return fill_overflow_probe(deadline);
                                           }));

// One scenario's transcript is never judged as the other's: the evaluator rebuilds the stimulus of the
// scenario the transcript names.
TEST_F(Draft22LocationFilterProbes, EachScenarioIsProvenAgainstItsOwnStimulus) {
    Session session(bound(overflow_probe()));
    session.poll(0ms);
    session.close(0x3);
    auto done = session.poll(10ms);
    ASSERT_EQ(evaluate_draft22_location_filter_overflow(done), std::optional<bool>{true});
    done.scenario_id = std::string(kDraft22FillLocationFilterOverflow);
    EXPECT_FALSE(evaluate_draft22_location_filter_overflow(done).has_value());
}

class UnknownTypeVerdicts : public ::testing::Test {
protected:
    ScopedWireDraft wire{22};
    static std::optional<bool> verdict(const RawProbeTranscript& t) {
        return evaluate_draft22_location_filter_unknown_type(t);
    }
    static std::optional<bool> close_after(std::chrono::milliseconds after, std::uint64_t code) {
        Session session(unknown_type_probe());
        session.poll(0ms);
        session.close(code);
        return verdict(session.poll(after));
    }
};

TEST_F(UnknownTypeVerdicts, CloseWithProtocolViolationPassesAndAnyOtherCodeFails) {
    EXPECT_EQ(close_after(10ms, 0x3), std::optional<bool>{true});
    EXPECT_EQ(close_after(10ms, 0x1), std::optional<bool>{false});
    EXPECT_FALSE(close_after(1600ms, 0x3).has_value()) << "beyond the reaction window";
}

TEST_F(UnknownTypeVerdicts, APublisherThatNeverClosesIsNotJudged) {
    // Even a SUBSCRIBE_OK on the request proves nothing the draft forbids with a MUST.
    Session session(unknown_type_probe());
    session.poll(0ms);
    session.send(transport::StreamDataEvent{kStimulus, kSubscribeOk, false});
    const auto& end = session.poll(5001ms);
    ASSERT_TRUE(end.timed_out);
    EXPECT_FALSE(end.liveness.has_value());
    EXPECT_FALSE(verdict(end).has_value());
}

TEST_F(UnknownTypeVerdicts, OnlyItsOwnScenarioIsJudged) {
    Session session(unknown_type_probe());
    session.poll(0ms);
    session.close(0x3);
    auto done = session.poll(10ms);
    ASSERT_EQ(verdict(done), std::optional<bool>{true});
    EXPECT_FALSE(evaluate_draft22_location_filter_overflow(done).has_value()) << "not an overflow scenario";
    auto altered = done;
    altered.writes[0].write.bytes.back() = std::byte{0x07};
    EXPECT_FALSE(verdict(altered).has_value());
    const ScopedWireDraft wire21(21);
    EXPECT_FALSE(verdict(done).has_value());
}

// Subgroup stream (Section 11.3, the draft 21 format): flags 0x30 (Subgroup ID 0, default priority), alias,
// group, then Objects (delta-encoded ID, length 1, payload 'x').
Bytes subgroup(unsigned alias, unsigned group, const std::vector<unsigned>& object_ids) {
    Bytes result = b({0x30, alias, group});
    unsigned previous = 0;
    bool first = true;
    for (const auto id : object_ids) {
        result.push_back(static_cast<std::byte>(first ? id : id - previous - 1));
        result.push_back(std::byte{1});
        result.push_back(std::byte{'x'});
        previous = id;
        first = false;
    }
    return result;
}

const Bytes kSubscribeOkAlias1 = b({4, 0, 2, 1, 0});

class AbsoluteOriginVerdicts : public ::testing::Test {
protected:
    ScopedWireDraft wire{22};
    static std::optional<bool> verdict(const RawProbeTranscript& t) {
        return evaluate_draft22_location_filter_absolute_origin(t);
    }
    // The publisher answers with `answer`, then sends `streams` (peer unidirectional 6, 10, ...), and the
    // window ends.
    static RawProbeTranscript played(const Bytes& answer, const std::vector<Bytes>& streams) {
        Session session(absolute_origin_probe());
        session.poll(0ms);
        if (!answer.empty()) session.send(transport::StreamDataEvent{kStimulus, answer, false});
        transport::StreamId stream = 6;
        for (const auto& bytes : streams) {
            session.send(transport::StreamDataEvent{stream, bytes, true});
            stream += 4;
        }
        session.poll(10ms);
        auto end = session.poll(1001ms);
        EXPECT_TRUE(end.timed_out);
        return end;
    }
};

TEST_F(AbsoluteOriginVerdicts, AcceptedAndDeliveringFromTheOriginPasses) {
    EXPECT_EQ(verdict(played(kSubscribeOkAlias1, {subgroup(1, 0, {0, 1})})), std::optional<bool>{true});
    // Any delivered Object is inside a range from {0, 0}.
    EXPECT_EQ(verdict(played(kSubscribeOkAlias1, {subgroup(1, 7, {9})})), std::optional<bool>{true});
}

TEST_F(AbsoluteOriginVerdicts, NoDeliveryOrNoAcceptanceIsNotJudged) {
    EXPECT_FALSE(verdict(played(kSubscribeOkAlias1, {})).has_value()) << "accepted, nothing delivered";
    EXPECT_FALSE(verdict(played(kSubscribeOkAlias1, {subgroup(2, 0, {0})})).has_value()) << "another alias";
    EXPECT_FALSE(verdict(played(kSubscribeOkAlias1, {b({0x30, 1, 0})})).has_value()) << "a header, no Object";
    EXPECT_FALSE(verdict(played(b({5, 0, 3, 0x10, 0, 0}), {subgroup(1, 0, {0})})).has_value()) << "REQUEST_ERROR";
    EXPECT_FALSE(verdict(played({}, {subgroup(1, 0, {0})})).has_value()) << "unanswered";
}

TEST_F(AbsoluteOriginVerdicts, AProtocolViolationCloseFailsAndOtherClosesAreNotJudged) {
    const auto close_after = [](std::chrono::milliseconds after, std::uint64_t code, bool accepted) {
        Session session(absolute_origin_probe());
        session.poll(0ms);
        if (accepted) session.send(transport::StreamDataEvent{kStimulus, kSubscribeOkAlias1, false});
        session.close(code);
        return verdict(session.poll(after));
    };
    EXPECT_EQ(close_after(10ms, 0x3, false), std::optional<bool>{false}) << "a valid filter read as malformed";
    EXPECT_EQ(close_after(10ms, 0x3, true), std::optional<bool>{false});
    EXPECT_FALSE(close_after(10ms, 0x1, false).has_value()) << "INTERNAL_ERROR says nothing about the filter";
    EXPECT_FALSE(close_after(10ms, 0x0, true).has_value());
    EXPECT_FALSE(close_after(1600ms, 0x3, false).has_value()) << "beyond the reaction window";
}

TEST_F(AbsoluteOriginVerdicts, AnUnprovenOrForeignTranscriptIsNotJudged) {
    const auto done = played(kSubscribeOkAlias1, {subgroup(1, 0, {0})});
    ASSERT_EQ(verdict(done), std::optional<bool>{true});
    auto altered = done;
    altered.writes[0].write.bytes.back() = std::byte{1};  // StartObject 1: not the origin
    EXPECT_FALSE(verdict(altered).has_value());
    auto other = done;
    other.scenario_id = std::string(kDraft22LocationFilterUnknownType);
    EXPECT_FALSE(verdict(other).has_value());
    auto failed = done;
    failed.harness_failed = true;
    EXPECT_FALSE(verdict(failed).has_value());
    const ScopedWireDraft wire21(21);
    EXPECT_FALSE(verdict(done).has_value());
}

}  // namespace
}  // namespace moq::interop::scenarios
