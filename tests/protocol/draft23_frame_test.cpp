#include "moq/interop/wire/draft23/frame.h"
#include <gtest/gtest.h>
#include <limits>
#include <vector>
using namespace moq::interop::wire;
using namespace moq::interop::wire::draft23;
TEST(Draft23Frame, ReadsVariableLengthAndLeavesFollowingMessage) {
    std::vector<std::byte> data{std::byte{1}, std::byte{0x80}, std::byte{0x80}};
    data.insert(data.end(), 128, std::byte{0xaa});
    data.push_back(std::byte{0xff});
    Cursor cursor(data);
    auto result = decode_frame(cursor, 65535);
    ASSERT_TRUE(std::holds_alternative<Frame>(result));
    EXPECT_EQ(std::get<Frame>(result).body.size(), 128u);
    EXPECT_EQ(cursor.remaining(), 1u);
}
TEST(Draft23Frame, RejectsOversizeBeforeBodyArrivesWithoutAdvancing) {
    std::vector<std::byte> data{std::byte{1}, std::byte{0xc1}, std::byte{0}, std::byte{0}};
    Cursor cursor(data);
    auto result = decode_frame(cursor, 65535);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(result));
    EXPECT_EQ(std::get<DecodeError>(result).code, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_EQ(cursor.offset(), 0u);
}
TEST(Draft23Frame, IncompleteBodyDoesNotAdvance) {
    std::vector<std::byte> data{std::byte{1}, std::byte{2}, std::byte{0}};
    Cursor cursor(data);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_frame(cursor, 65535)));
    EXPECT_EQ(cursor.offset(), 0u);
}

#include "moq/interop/wire/draft23/setup.h"
TEST(Draft23Setup, ReadsMaximumControlMessageSize) {
    // SETUP with option 9 containing vi64 65535.
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{5},
        std::byte{9}, std::byte{3}, std::byte{0xc0}, std::byte{0xff}, std::byte{0xff}};
    Cursor cursor(data);
    auto result = decode_setup(cursor);
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
    EXPECT_EQ(std::get<SetupMessage>(result).maximum_control_message_size, 65535u);
}
TEST(Draft23Setup, RejectsMaximumBelowMinimum) {
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{3},
        std::byte{9}, std::byte{1}, std::byte{63}};
    Cursor cursor(data);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_setup(cursor)));
    EXPECT_EQ(cursor.offset(), 0u);
}
TEST(Draft23Setup, UsesDefaultWhenOptionAbsent) {
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{0}};
    Cursor cursor(data);
    auto result = decode_setup(cursor);
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
    EXPECT_EQ(std::get<SetupMessage>(result).maximum_control_message_size, 65535u);
}
TEST(Draft23Setup, RejectsDuplicateMaximum) {
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{10},
        std::byte{9}, std::byte{3}, std::byte{0xc0}, std::byte{0xff}, std::byte{0xff},
        std::byte{0}, std::byte{3}, std::byte{0xc0}, std::byte{0xff}, std::byte{0xff}};
    Cursor cursor(data);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_setup(cursor)));
    EXPECT_EQ(cursor.offset(), 0u);
}
TEST(Draft23Frame, EncodesVarintLengthAndHonorsPeerLimit) {
    const std::vector<std::byte> body(128, std::byte{0x42});
    ByteWriter writer(256);
    ASSERT_TRUE(encode_frame(3, body, 65535, writer));
    const auto bytes = writer.bytes();
    ASSERT_EQ(bytes.size(), 131u);
    EXPECT_EQ(bytes[0], std::byte{3});
    EXPECT_EQ(bytes[1], std::byte{0x80});
    EXPECT_EQ(bytes[2], std::byte{0x80});
    ByteWriter limited(256);
    EXPECT_FALSE(encode_frame(3, body, 127, limited));
    EXPECT_EQ(limited.size(), 0u);
}
TEST(Draft23Frame, EncodeIsAtomicWhenOutputTooSmall) {
    const std::vector<std::byte> body(128, std::byte{0});
    ByteWriter writer(130);
    EXPECT_FALSE(encode_frame(3, body, 65535, writer));
    EXPECT_EQ(writer.size(), 0u);
}
TEST(Draft23Setup, EncodesOptionsInDraft23Frame) {
    SetupMessage setup;
    setup.options.push_back({9, std::vector<std::byte>{std::byte{0xc0}, std::byte{0xff}, std::byte{0xff}}});
    ByteWriter writer(32);
    ASSERT_TRUE(encode_setup(setup, writer));
    const std::vector<std::byte> expected{std::byte{0xaf}, std::byte{0}, std::byte{5},
        std::byte{9}, std::byte{3}, std::byte{0xc0}, std::byte{0xff}, std::byte{0xff}};
    EXPECT_EQ(std::vector<std::byte>(writer.bytes().begin(), writer.bytes().end()), expected);
}

#include "moq/interop/wire/draft23/messages.h"
TEST(Draft23Message, NamespaceAnnouncementUsesVarintLength) {
    PublishNamespaceMessage announcement{0, {{std::byte{'a'}}}, {}};
    ByteWriter output(64);
    ASSERT_TRUE(encode_message(announcement, output));
    const std::vector<std::byte> expected{std::byte{6}, std::byte{5}, std::byte{0}, std::byte{1},
        std::byte{1}, std::byte{'a'}, std::byte{0}};
    EXPECT_EQ(std::vector<std::byte>(output.bytes().begin(), output.bytes().end()), expected);
    Cursor input(output.bytes());
    const auto decoded = decode_message(input);
    ASSERT_TRUE(std::holds_alternative<Message>(decoded));
    const auto& value = std::get<PublishNamespaceMessage>(std::get<Message>(decoded));
    EXPECT_EQ(value.track_namespace, announcement.track_namespace);
    EXPECT_EQ(input.remaining(), 0u);
}
TEST(Draft23Message, PublishAcceptsPausedAndCountedFillParameters) {
    PublishMessage publish{0, {{std::byte{'n'}}}, {std::byte{'t'}}, 5,
        {{0x11, std::uint8_t{0}}, {0x23, std::vector<Parameter>{{0x0a, std::uint64_t{7}}}}}, {}};
    ByteWriter output(128);
    ASSERT_TRUE(encode_message(publish, output));
    Cursor input(output.bytes());
    const auto decoded = decode_message(input);
    ASSERT_TRUE(std::holds_alternative<Message>(decoded));
    const auto& value = std::get<PublishMessage>(std::get<Message>(decoded));
    EXPECT_EQ(value.track_alias, 5u);
    ASSERT_EQ(value.parameters.size(), 2u);
    const auto& fill = std::get<std::vector<Parameter>>(value.parameters[1].value);
    ASSERT_EQ(fill.size(), 1u);
    EXPECT_EQ(fill[0].type, 0x0au);
    EXPECT_EQ(std::get<std::uint64_t>(fill[0].value), 7u);
}
TEST(Draft23Message, UnknownMessageAndEmptyTrackAreRejectedAtomically) {
    const std::vector<std::byte> unknown{std::byte{0x3f}, std::byte{0}};
    Cursor input(unknown);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_message(input)));
    EXPECT_EQ(input.offset(), 0u);
    ByteWriter output(128);
    EXPECT_FALSE(encode_message(SubscribeMessage{0, {}, {}, {}}, output));
    EXPECT_EQ(output.size(), 0u);
}
TEST(Draft23Message, ErrorDetailsRoundTripAndIncompleteDoesNotAdvance) {
    RequestErrorMessage message{3, 0, {std::byte{'x'}}, {{2, std::uint64_t{42}}}};
    ByteWriter output(128);
    ASSERT_TRUE(encode_message(message, output));
    Cursor partial(output.bytes().first(output.size() - 1));
    EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_message(partial)));
    EXPECT_EQ(partial.offset(), 0u);
    Cursor input(output.bytes());
    const auto decoded = decode_message(input);
    ASSERT_TRUE(std::holds_alternative<Message>(decoded));
    EXPECT_EQ(std::get<RequestErrorMessage>(std::get<Message>(decoded)).error_details.size(), 1u);
}
TEST(Draft23Message, RejectsRetiredForwardAndInvalidPaused) {
    // PUBLISH: id=0, namespace n, track t, alias=0, one parameter.
    for (const auto& value : {std::vector<unsigned>{0x10, 1}, std::vector<unsigned>{0x11, 2}}) {
        std::vector<std::byte> bytes{std::byte{0x1d}, std::byte{10}, std::byte{0}, std::byte{1},
            std::byte{1}, std::byte{'n'}, std::byte{1}, std::byte{'t'}, std::byte{0}, std::byte{1},
            std::byte{static_cast<unsigned char>(value[0])}, std::byte{static_cast<unsigned char>(value[1])}};
        Cursor input(bytes);
        EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_message(input)));
        EXPECT_EQ(input.offset(), 0u);
    }
}
TEST(Draft23Message, FillUsesCountAndAnIndependentTypeDeltaScope) {
    SubscribeMessage message{1, {{std::byte{'n'}}}, {std::byte{'t'}},
        {{0x20, std::uint8_t{99}}, {0x23, Parameters{{0x20, std::uint8_t{42}}}}}};
    ByteWriter output(128);
    ASSERT_TRUE(encode_message(message, output));
    const std::vector<std::byte> expected{std::byte{3}, std::byte{13}, std::byte{1}, std::byte{1},
        std::byte{1}, std::byte{'n'}, std::byte{1}, std::byte{'t'}, std::byte{2}, std::byte{0x20},
        std::byte{99}, std::byte{3}, std::byte{1}, std::byte{0x20}, std::byte{42}};
    EXPECT_EQ(std::vector<std::byte>(output.bytes().begin(), output.bytes().end()), expected);
}
TEST(Draft23Message, RedirectRequiresRedirectErrorDetail) {
    ByteWriter output(32);
    EXPECT_FALSE(encode_message(RequestErrorMessage{0x34, 0, {}, {}}, output));
    EXPECT_EQ(output.size(), 0u);
}
TEST(Draft23Message, PropertyValueStillHas65535ByteLimitWithLargerFrameLimit) {
    ByteWriter output(100000);
    RequestOkMessage message{{}, {{1, std::vector<std::byte>(65536)}}};
    EXPECT_FALSE(encode_message(message, output, 100000));
    EXPECT_EQ(output.size(), 0u);
}
TEST(Draft23Message, BodyMustConsumeItsDeclaredLength) {
    // PUBLISH_NAMESPACE with a trailing octet after its zero parameter count.
    const std::vector<std::byte> bytes{std::byte{6}, std::byte{6}, std::byte{0}, std::byte{1},
        std::byte{1}, std::byte{'n'}, std::byte{0}, std::byte{0}};
    Cursor input(bytes);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_message(input)));
    EXPECT_EQ(input.offset(), 0u);
}

#include "moq/interop/wire/draft23/fetch.h"
TEST(Draft23Fetch, ObjectDeltaWithinGroupAddsTwo) {
    // flags=0x1c: absolute Group/Object and explicit priority for first Object.
    const std::vector<std::byte> bytes{std::byte{0x1c}, std::byte{7}, std::byte{3}, std::byte{128},
        std::byte{1}, std::byte{'a'}, std::byte{4}, std::byte{0}, std::byte{1}, std::byte{'b'}};
    FetchState state;
    Cursor input(bytes);
    const auto first = decode_fetch_entry(input, state);
    ASSERT_TRUE(std::holds_alternative<FetchEntry>(first));
    EXPECT_EQ(std::get<FetchObject>(std::get<FetchEntry>(first)).location.object, 3u);
    const auto second = decode_fetch_entry(input, state);
    ASSERT_TRUE(std::holds_alternative<FetchEntry>(second));
    const auto& object = std::get<FetchObject>(std::get<FetchEntry>(second));
    EXPECT_EQ(object.location.object, 5u);
    EXPECT_EQ(object.location.group, 7u);
    EXPECT_EQ(object.priority, 128u);
}
TEST(Draft23Fetch, GoneRangeUsesDeltaAndRetainsPriorObjectFields) {
    FetchState state;
    // First Gone range uses absolute IDs, next one within that group adds delta+1.
    const std::vector<std::byte> bytes{std::byte{0x84}, std::byte{0x0c}, std::byte{5}, std::byte{6},
        std::byte{0x84}, std::byte{0x0c}, std::byte{0}, std::byte{2}};
    Cursor input(bytes);
    const auto first = decode_fetch_entry(input, state);
    ASSERT_TRUE(std::holds_alternative<FetchEntry>(first));
    EXPECT_EQ(std::get<FetchRange>(std::get<FetchEntry>(first)).kind, FetchRangeKind::Gone);
    const auto second = decode_fetch_entry(input, state);
    ASSERT_TRUE(std::holds_alternative<FetchEntry>(second));
    EXPECT_EQ(std::get<FetchRange>(std::get<FetchEntry>(second)).location.object, 9u);
    EXPECT_FALSE(state.prior_priority.has_value());
}
TEST(Draft23Fetch, RejectsPriorReferencesAndIncompleteWithoutAdvancing) {
    const std::vector<std::byte> bytes{std::byte{0}, std::byte{0}};
    FetchState state;
    Cursor input(bytes);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_fetch_entry(input, state)));
    EXPECT_EQ(input.offset(), 0u);
    EXPECT_FALSE(state.prior_location.has_value());
    const std::vector<std::byte> partial{std::byte{0x1c}, std::byte{7}, std::byte{3}, std::byte{128}, std::byte{2}, std::byte{'a'}};
    Cursor incomplete(partial);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(decode_fetch_entry(incomplete, state)));
    EXPECT_EQ(incomplete.offset(), 0u);
    EXPECT_FALSE(state.prior_location.has_value());
}
TEST(Draft23Fetch, EndOfGroupForcesNewGroupAndDetectsOverflow) {
    FetchState state;
    state.prior_location = moq::interop::wire::draft21::Location{7, 8};
    state.prior_priority = 128;
    state.prior_subgroup = 0;
    const std::vector<std::byte> bytes{std::byte{0x80}, std::byte{0x80}, std::byte{0}, std::byte{0}};
    Cursor input(bytes);
    ASSERT_TRUE(std::holds_alternative<FetchEntry>(decode_fetch_entry(input, state)));
    EXPECT_EQ(state.prior_location->object, std::numeric_limits<std::uint64_t>::max());
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_fetch_entry(input, state)));
}
TEST(Draft23Fetch, DescendingRangeUsesGroupDeltaWithoutPlusOne) {
    FetchState state;
    state.descending = true;
    state.prior_location = moq::interop::wire::draft21::Location{7, 8};
    const std::vector<std::byte> bytes{std::byte{0x82}, std::byte{0x0c}, std::byte{2}, std::byte{3}};
    Cursor input(bytes);
    const auto decoded = decode_fetch_entry(input, state);
    ASSERT_TRUE(std::holds_alternative<FetchEntry>(decoded));
    const auto& range = std::get<FetchRange>(std::get<FetchEntry>(decoded));
    EXPECT_EQ(range.location.group, 5u);
    EXPECT_EQ(range.location.object, 3u);
}
TEST(Draft23Message, RejectsMismatchedParameterValueTypes) {
    SubscribeMessage message{1, {}, {std::byte{'t'}}, {{0x11, std::vector<std::byte>{}}}};
    ByteWriter output(64);
    EXPECT_FALSE(encode_message(message, output));
    EXPECT_EQ(output.size(), 0u);
}
TEST(Draft23Message, RedirectDetailMustContainCompleteRedirectStructure) {
    ByteWriter output(64);
    EXPECT_FALSE(encode_message(RequestErrorMessage{0x34, 0, {}, {{1, std::vector<std::byte>{}}}}, output));
    EXPECT_EQ(output.size(), 0u);
    // Unknown or inapplicable detail serialization is ignored semantically.
    EXPECT_TRUE(encode_message(RequestErrorMessage{1, 0, {}, {{1, std::vector<std::byte>{}}}}, output));
}
TEST(Draft23Message, FetchRequestUsesSameTrackStructureAndFetchParameterScope) {
    FetchMessage fetch{3, {{std::byte{'n'}}}, {std::byte{'t'}}, {{0x0a, std::uint64_t{0}}}};
    ByteWriter output(64);
    ASSERT_TRUE(encode_message(fetch, output));
    Cursor input(output.bytes());
    const auto decoded = decode_message(input);
    ASSERT_TRUE(std::holds_alternative<Message>(decoded));
    EXPECT_EQ(std::get<FetchMessage>(std::get<Message>(decoded)).request_id, 3u);
    fetch.parameters = {{0x11, std::uint8_t{0}}};
    ByteWriter rejected(64);
    EXPECT_FALSE(encode_message(fetch, rejected));
    EXPECT_EQ(rejected.size(), 0u);
}
TEST(Draft23Message, DetailedErrorsUseDraft23SessionCodes) {
    struct Case { std::vector<std::byte> bytes; std::uint64_t code; };
    const std::vector<Case> cases{
        {{std::byte{0x3f},std::byte{0}},0x303},
        {{std::byte{6},std::byte{4},std::byte{0},std::byte{1},std::byte{0},std::byte{0}},0x302},
        {{std::byte{0x1d},std::byte{10},std::byte{0},std::byte{1},std::byte{1},std::byte{'n'},std::byte{1},std::byte{'t'},std::byte{0},std::byte{1},std::byte{0x11},std::byte{2}},0x305},
        {{std::byte{6},std::byte{0}},0x300},
    };
    for (const auto& item : cases) {
        Cursor input(item.bytes);
        const auto result = decode_message_with_error(input);
        ASSERT_TRUE(std::holds_alternative<MessageDecodeFailure>(result));
        EXPECT_EQ(session_error_code(std::get<MessageDecodeFailure>(result)),item.code);
        EXPECT_EQ(input.offset(),0u);
    }
}
TEST(Draft23Message, UnsupportedKnownMessageIsAHarnessLimit) {
    const std::vector<std::byte> bytes{std::byte{2},std::byte{0}};
    Cursor input(bytes);
    const auto decoded = decode_message_with_error(input);
    ASSERT_TRUE(std::holds_alternative<MessageDecodeFailure>(decoded));
    EXPECT_TRUE(std::get<MessageDecodeFailure>(decoded).unsupported);
    const std::vector<std::byte> unknown{std::byte{0x3f},std::byte{0}};
    Cursor other(unknown);
    const auto rejected = decode_message_with_error(other);
    ASSERT_TRUE(std::holds_alternative<MessageDecodeFailure>(rejected));
    EXPECT_FALSE(std::get<MessageDecodeFailure>(rejected).unsupported);
}
TEST(Draft23Message, DetailedLengthAndDeltaErrorsTakePrecedence) {
    ByteWriter long_namespace(32);
    ASSERT_TRUE(write_vi64(0,long_namespace));
    ASSERT_TRUE(write_vi64(1,long_namespace));
    ASSERT_TRUE(write_vi64(4097,long_namespace));
    ByteWriter frame(128);
    ASSERT_TRUE(encode_frame(6,long_namespace.bytes(),65535,frame));
    Cursor oversized(frame.bytes());
    const auto length_error = decode_message_with_error(oversized);
    ASSERT_TRUE(std::holds_alternative<MessageDecodeFailure>(length_error));
    EXPECT_EQ(session_error_code(std::get<MessageDecodeFailure>(length_error)),0x401u);

    // PUBLISH with priority parameter 0x20, followed by a delta that wraps its type.
    ByteWriter body(64);
    for (const auto v : {0u,0u,1u}) ASSERT_TRUE(write_vi64(v,body));
    ASSERT_TRUE(body.append_byte(std::byte{'t'}));
    for (const auto v : {0u,2u,0x20u,1u}) ASSERT_TRUE(write_vi64(v,body));
    ASSERT_TRUE(write_vi64(std::numeric_limits<std::uint64_t>::max(),body));
    ByteWriter wrapped(128);
    ASSERT_TRUE(encode_frame(0x1d,body.bytes(),65535,wrapped));
    Cursor overflow(wrapped.bytes());
    const auto delta_error = decode_message_with_error(overflow);
    ASSERT_TRUE(std::holds_alternative<MessageDecodeFailure>(delta_error));
    EXPECT_EQ(session_error_code(std::get<MessageDecodeFailure>(delta_error)),0x400u);
}
TEST(Draft23Setup, CompleteFrameWithTruncatedOptionIsMalformed) {
    const std::vector<std::byte> bytes{std::byte{0xaf},std::byte{0},std::byte{2},std::byte{1},std::byte{1}};
    Cursor input(bytes);
    const auto decoded = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
    EXPECT_EQ(std::get<DecodeError>(decoded).code,DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(),0u);
}
TEST(Draft23Setup, InvalidMaximumIsDistinguishedFromMalformedFraming) {
    const std::vector<std::byte> bytes{std::byte{0xaf},std::byte{0},std::byte{3},std::byte{9},std::byte{1},std::byte{1}};
    Cursor input(bytes);
    const auto decoded = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
    EXPECT_EQ(std::get<DecodeError>(decoded).code,DecodeErrorCode::InvalidValue);
    EXPECT_EQ(input.offset(),0u);
}

TEST(Draft23SetupObservation, KeepsOptionsAndFrameLengthThatStrictDecodeRejects) {
    // Duplicate MAX_CONTROL_MESSAGE_SIZE (option 9), the first below the minimum of 65535.
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{6},
        std::byte{9}, std::byte{1}, std::byte{0x40}, std::byte{0}, std::byte{1}, std::byte{0x40}};
    Cursor strict(data);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_setup(strict)));
    Cursor lenient(data);
    const auto observed = decode_setup_observation(lenient);
    ASSERT_TRUE(std::holds_alternative<SetupObservation>(observed));
    const auto& value = std::get<SetupObservation>(observed);
    EXPECT_EQ(value.frame_length, 6u);
    ASSERT_EQ(value.options.size(), 2u);
    EXPECT_EQ(value.options[0].type, 9u);
    EXPECT_EQ(value.options[1].type, 9u);
    EXPECT_EQ(lenient.remaining(), 0u);
}
TEST(Draft23SetupObservation, StillReportsMalformedFraming) {
    std::vector<std::byte> data{std::byte{0xaf}, std::byte{0}, std::byte{2}, std::byte{9}, std::byte{5}};
    Cursor input(data);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_setup_observation(input)));
}
