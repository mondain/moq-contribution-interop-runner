#include "moq/interop/wire/moqlite06/setup.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <random>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::wire::moqlite06 {
namespace {

using Bytes = std::vector<std::byte>;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

DecodeResult<SetupMessage> decode(const Bytes& data, const DecodeLimits& limits = kDefaultLimits) {
    Cursor input(data);
    return decode_setup(input, limits);
}

const SetupMessage& ok(const DecodeResult<SetupMessage>& result) {
    EXPECT_TRUE(std::holds_alternative<SetupMessage>(result));
    return std::get<SetupMessage>(result);
}

DecodeErrorCode error_code(const DecodeResult<SetupMessage>& result) {
    const auto* error = std::get_if<DecodeError>(&result);
    EXPECT_NE(error, nullptr);
    return error ? error->code : DecodeErrorCode::InvalidValue;
}

SetupCapabilities capabilities_of(const SetupMessage& message) {
    const auto result = read_capabilities(message);
    EXPECT_TRUE(std::holds_alternative<SetupCapabilities>(result));
    return std::get<SetupCapabilities>(result);
}

SetupMessage with_param(std::uint64_t id, Bytes value) {
    SetupMessage message;
    message.parameters.push_back(SetupParameter{id, std::move(value)});
    return message;
}

Bytes encode(const SetupMessage& message) {
    ByteWriter out(4096);
    EXPECT_FALSE(encode_setup(message, out).has_value());
    return Bytes(out.bytes().begin(), out.bytes().end());
}

// moq.dev rs/moq-net/src/lite/setup.rs, test parameter_values_use_the_version_codec (Lite06):
// Message Length 5, Parameter Count 1, id 4 (Cost), Parameter Length 2, value 40 64 (QUIC varint 100).
TEST(Moqlite06Setup, MoqDevCostVectorDecodesAndEncodes) {
    const auto wire = bytes({0x05, 0x01, 0x04, 0x02, 0x40, 0x64});
    Cursor input(wire);
    const auto result = decode_setup(input);
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(result));
    EXPECT_EQ(input.remaining(), 0u);
    const auto& message = std::get<SetupMessage>(result);
    ASSERT_EQ(message.parameters.size(), 1u);
    EXPECT_EQ(message.parameters[0].id, kParamCost);
    EXPECT_EQ(message.parameters[0].value, bytes({0x40, 0x64}));

    const auto caps = capabilities_of(message);
    ASSERT_TRUE(caps.cost.has_value());
    EXPECT_EQ(*caps.cost, 100u);

    SetupCapabilities built;
    built.cost = 100;
    EXPECT_EQ(encode(*build_setup(built)), wire);
}

// Draft 6.3.1: an empty parameter list is how an endpoint with no capabilities speaks.
// Body is just Parameter Count 0, so Message Length is 1.
TEST(Moqlite06Setup, EmptySetupDecodesToNoParameters) {
    const auto wire = bytes({0x01, 0x00});
    const auto result = decode(wire);
    EXPECT_TRUE(ok(result).parameters.empty());
    EXPECT_EQ(encode(SetupMessage{}), wire);
    EXPECT_EQ(encode(*build_setup(SetupCapabilities{})), wire);
}

TEST(Moqlite06Setup, AllFiveParametersRoundTripInEncodedOrder) {
    // Path "/a" alone. Body = Parameter Count 01, id 02, Parameter Length 02, value 2f 61 (the raw UTF-8 bytes,
    // no nested length prefix) = 5 bytes, so Message Length is 05 (the task brief's "07" does not match its own
    // body, which is 5 bytes): 05 01 02 02 2f 61.
    {
        SetupCapabilities caps;
        caps.path = "/a";
        EXPECT_EQ(encode(*build_setup(caps)), bytes({0x05, 0x01, 0x02, 0x02, 0x2f, 0x61}));
    }

    // Everything at once. Bodies, in id order:
    //   count          05
    //   Probe  1       01 01 02        (value 02)
    //   Path   2       02 02 2f 61
    //   Role   3       03 01 01        (value 01)
    //   Cost   4       04 02 40 64     (value 100)
    //   Hop    5       05 01 07        (value 7)
    // Body length = 1 + 3 + 4 + 3 + 4 + 3 = 18 = 0x12.
    const auto wire = bytes({0x12, 0x05, 0x01, 0x01, 0x02, 0x02, 0x02, 0x2f, 0x61, 0x03, 0x01, 0x01, 0x04,
                             0x02, 0x40, 0x64, 0x05, 0x01, 0x07});
    SetupCapabilities caps;
    caps.probe = 2;
    caps.path = "/a";
    caps.role = 1;
    caps.cost = 100;
    caps.hop_id = 7;
    EXPECT_EQ(encode(*build_setup(caps)), wire);

    const auto decoded = decode(wire);
    const auto& message = ok(decoded);
    ASSERT_EQ(message.parameters.size(), 5u);
    for (std::size_t index = 0; index < 5; ++index) EXPECT_EQ(message.parameters[index].id, index + 1);
    EXPECT_EQ(encode(message), wire);

    const auto back = capabilities_of(message);
    EXPECT_EQ(back.probe, std::optional<std::uint64_t>(2));
    EXPECT_EQ(back.path, std::optional<std::string>("/a"));
    EXPECT_EQ(back.role, std::optional<std::uint64_t>(1));
    EXPECT_EQ(back.cost, std::optional<std::uint64_t>(100));
    EXPECT_EQ(back.hop_id, std::optional<std::uint64_t>(7));
}

TEST(Moqlite06Setup, DecodeKeepsWireOrderAndUnknownIds) {
    // count 3: Cost(4)=1, unknown id 0x7f (two byte varint 40 7f) with value aa bb, Probe(1)=1
    const auto wire = bytes({0x0c, 0x03, 0x04, 0x01, 0x01, 0x40, 0x7f, 0x02, 0xaa, 0xbb, 0x01, 0x01, 0x01});
    const auto result = decode(wire);
    const auto& message = ok(result);
    ASSERT_EQ(message.parameters.size(), 3u);
    EXPECT_EQ(message.parameters[0].id, 4u);
    EXPECT_EQ(message.parameters[1].id, 0x7fu);
    EXPECT_EQ(message.parameters[1].value, bytes({0xaa, 0xbb}));
    EXPECT_EQ(message.parameters[2].id, 1u);

    const auto caps = capabilities_of(message);
    EXPECT_EQ(caps.cost, std::optional<std::uint64_t>(1));
    EXPECT_EQ(caps.probe, std::optional<std::uint64_t>(1));
    EXPECT_FALSE(caps.path.has_value());
    EXPECT_FALSE(caps.role.has_value());
    EXPECT_FALSE(caps.hop_id.has_value());

    // Unknown ids are re-encoded verbatim and in the same order.
    EXPECT_EQ(encode(message), bytes({0x0c, 0x03, 0x04, 0x01, 0x01, 0x40, 0x7f, 0x02, 0xaa, 0xbb, 0x01, 0x01, 0x01}));
}

TEST(Moqlite06Setup, DuplicateParameterIdIsAProtocolViolation) {
    // count 2, Cost=1 twice
    const auto wire = bytes({0x07, 0x02, 0x04, 0x01, 0x01, 0x04, 0x01, 0x02});
    EXPECT_EQ(error_code(decode(wire)), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Setup, ParameterCountAboveLimitIsRejectedBeforeAnyParameterIsRead) {
    // count 65 (two-byte varint 40 41) with no parameters following at all: the limit check comes first.
    EXPECT_EQ(error_code(decode(bytes({0x02, 0x40, 0x41}))), DecodeErrorCode::LengthExceedsLimit);
    // 64 is at the limit, so it falls through to the body-shortfall violation.
    EXPECT_EQ(error_code(decode(bytes({0x02, 0x40, 0x40}))), DecodeErrorCode::ProtocolViolation);
    DecodeLimits tight;
    tight.max_parameters = 1;
    EXPECT_EQ(error_code(decode(bytes({0x01, 0x02}), tight)), DecodeErrorCode::LengthExceedsLimit);
}

TEST(Moqlite06Setup, ParameterCountLargerThanPresentIsAProtocolViolation) {
    // count 3, only two parameters in the body (Cost=1, Role=1).
    const auto wire = bytes({0x07, 0x03, 0x04, 0x01, 0x01, 0x03, 0x01, 0x01});
    EXPECT_EQ(error_code(decode(wire)), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Setup, ParameterLengthPastMessageLengthIsAProtocolViolation) {
    // Message Length 4: count 1, id 4, length 5, one value byte. The value claims more than the body holds
    // (the input itself has further bytes, which must not be read as part of the value).
    const auto wire = bytes({0x04, 0x01, 0x04, 0x05, 0x01, 0xff, 0xff, 0xff, 0xff});
    EXPECT_EQ(error_code(decode(wire)), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Setup, TrailingBytesAfterLastParameterAreAProtocolViolation) {
    // Message Length 5: count 1, Cost=1, then one stray byte inside the body.
    const auto wire = bytes({0x05, 0x01, 0x04, 0x01, 0x01, 0x00});
    EXPECT_EQ(error_code(decode(wire)), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Setup, ZeroLengthBodyIsAProtocolViolation) {
    EXPECT_EQ(error_code(decode(bytes({0x00}))), DecodeErrorCode::ProtocolViolation);
}

TEST(Moqlite06Setup, TruncatedInputIsNeedMoreAndCursorDoesNotMove) {
    const auto full = bytes({0x05, 0x01, 0x04, 0x02, 0x40, 0x64});
    for (std::size_t cut = 0; cut < full.size(); ++cut) {
        const Bytes data(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut));
        Cursor input(data);
        const auto result = decode_setup(input);
        EXPECT_TRUE(std::holds_alternative<NeedMore>(result)) << cut;
        EXPECT_EQ(input.offset(), 0u) << cut;
    }
}

TEST(Moqlite06Setup, FailureLeavesCursorUnmovedAndSuccessConsumesOnlyTheMessage) {
    const auto bad = bytes({0x05, 0x01, 0x04, 0x01, 0x01, 0x00});
    Cursor input(bad);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(decode_setup(input)));
    EXPECT_EQ(input.offset(), 0u);

    const auto two = bytes({0x01, 0x00, 0xee});
    Cursor second(two);
    EXPECT_TRUE(std::holds_alternative<SetupMessage>(decode_setup(second)));
    EXPECT_EQ(second.remaining(), 1u);
}

TEST(Moqlite06Setup, MessageLengthAboveLimitIsRejected) {
    DecodeLimits tight;
    tight.max_message_length = 4;
    EXPECT_EQ(error_code(decode(bytes({0x05, 0x01, 0x04, 0x02, 0x40, 0x64}), tight)),
              DecodeErrorCode::LengthExceedsLimit);
}

TEST(Moqlite06Setup, ProbeLevelIsClamped) {
    SetupCapabilities caps;
    EXPECT_EQ(probe_level(caps), ProbeLevel::None);
    for (const auto& [raw, level] : std::vector<std::pair<unsigned, ProbeLevel>>{
             {0x00, ProbeLevel::None}, {0x01, ProbeLevel::Report}, {0x02, ProbeLevel::Increase},
             {0x05, ProbeLevel::Increase}}) {
        const auto message = with_param(kParamProbe, bytes({raw}));
        const auto view = capabilities_of(message);
        EXPECT_EQ(probe_level(view), level) << raw;
    }
    // The raw value survives the typed view.
    EXPECT_EQ(capabilities_of(with_param(kParamProbe, bytes({0x05}))).probe, std::optional<std::uint64_t>(5));
}

TEST(Moqlite06Setup, RoleEffectiveValues) {
    EXPECT_EQ(effective_role(SetupCapabilities{}), Role::Both);
    EXPECT_EQ(effective_role(capabilities_of(with_param(kParamRole, bytes({0x00})))), Role::Both);
    EXPECT_EQ(effective_role(capabilities_of(with_param(kParamRole, bytes({0x01})))), Role::Publisher);
    EXPECT_EQ(effective_role(capabilities_of(with_param(kParamRole, bytes({0x02})))), Role::Subscriber);
    const auto three = capabilities_of(with_param(kParamRole, bytes({0x03})));
    EXPECT_EQ(effective_role(three), Role::Both);  // draft 7.3.3: unknown means both directions
    EXPECT_EQ(three.role, std::optional<std::uint64_t>(3));
}

TEST(Moqlite06Setup, CostDefaultsAndZero) {
    EXPECT_EQ(effective_cost(SetupCapabilities{}), 1u);  // draft 7.3.4
    const auto zero = capabilities_of(with_param(kParamCost, bytes({0x00})));
    EXPECT_EQ(zero.cost, std::optional<std::uint64_t>(0));
    EXPECT_EQ(effective_cost(zero), 0u);
    EXPECT_EQ(effective_cost(capabilities_of(with_param(kParamCost, bytes({0x40, 0x64})))), 100u);
}

TEST(Moqlite06Setup, KnownVarintParameterNeedsExactlyOneVarint) {
    for (const auto id : {kParamProbe, kParamRole, kParamCost, kParamHop}) {
        for (const auto& value : {bytes({0x01, 0x01}),  // two varints
                                  bytes({}),            // none
                                  bytes({0x40}),        // a truncated two-byte varint
                                  bytes({0x01, 0x00}),
                                  bytes({0xc0, 0, 0, 0, 0, 0, 0, 0x01, 0x00}),  // 8-byte varint plus a stray byte
                                  bytes({0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00})}) {
            const auto result = read_capabilities(with_param(id, value));
            const auto* error = std::get_if<DecodeError>(&result);
            ASSERT_NE(error, nullptr) << id;
            EXPECT_EQ(error->code, DecodeErrorCode::ProtocolViolation) << id;
        }
    }
}

TEST(Moqlite06Setup, NonMinimalSingleVarintValueIsAccepted) {
    // RFC 9000 allows a sender to use more bytes than needed: 40 01 is the value 1, and the 8-byte form too.
    EXPECT_EQ(capabilities_of(with_param(kParamCost, bytes({0x40, 0x01}))).cost, std::optional<std::uint64_t>(1));
    EXPECT_EQ(capabilities_of(with_param(kParamCost, bytes({0xc0, 0, 0, 0, 0, 0, 0, 0x01}))).cost,
              std::optional<std::uint64_t>(1));
    EXPECT_EQ(capabilities_of(with_param(kParamHop, bytes({0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}))).hop_id,
              std::optional<std::uint64_t>(kMaxVarint));
}

TEST(Moqlite06Setup, BuildSetupRefusesValuesThatDoNotFitAVarint) {
    using Field = std::optional<std::uint64_t> SetupCapabilities::*;
    for (const Field field : {&SetupCapabilities::probe, &SetupCapabilities::role, &SetupCapabilities::cost,
                              &SetupCapabilities::hop_id}) {
        SetupCapabilities at_max;
        at_max.*field = kMaxVarint;
        const auto accepted = build_setup(at_max);
        ASSERT_TRUE(accepted.has_value());
        ASSERT_EQ(accepted->parameters.size(), 1u);
        EXPECT_EQ(accepted->parameters[0].value.size(), 8u);
        EXPECT_EQ(capabilities_of(*accepted).*field, std::optional<std::uint64_t>(kMaxVarint));

        SetupCapabilities over;
        over.*field = kMaxVarint + 1;
        EXPECT_FALSE(build_setup(over).has_value());
    }
    SetupCapabilities long_path;
    long_path.path = std::string(4, 'a');
    DecodeLimits tight;
    tight.max_string_length = 3;
    EXPECT_FALSE(build_setup(long_path, tight).has_value());
    long_path.path = std::string(3, 'a');
    EXPECT_TRUE(build_setup(long_path, tight).has_value());
}

TEST(Moqlite06Setup, EncodeEnforcesTheGivenLimits) {
    SetupMessage two;
    two.parameters.push_back({kParamCost, bytes({0x01})});
    two.parameters.push_back({kParamHop, bytes({0x01})});
    DecodeLimits tight;
    tight.max_parameters = 1;
    ByteWriter out(64);
    EXPECT_EQ(encode_setup(two, out, tight), std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_EQ(out.size(), 0u);

    const auto path = with_param(kParamPath, bytes({'a', 'b', 'c', 'd'}));
    DecodeLimits short_strings;
    short_strings.max_string_length = 3;
    EXPECT_EQ(encode_setup(path, out, short_strings), std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_EQ(out.size(), 0u);
    EXPECT_FALSE(encode_setup(path, out).has_value());
}

TEST(Moqlite06Setup, HopZeroIsCarriedRaw) {
    // Draft 7.3.5: 0 is equivalent to absent for the peer; the codec keeps the raw value.
    const auto caps = capabilities_of(with_param(kParamHop, bytes({0x00})));
    EXPECT_EQ(caps.hop_id, std::optional<std::uint64_t>(0));
}

TEST(Moqlite06Setup, PathVariants) {
    const auto empty = capabilities_of(with_param(kParamPath, bytes({})));
    ASSERT_TRUE(empty.path.has_value());
    EXPECT_TRUE(empty.path->empty());

    // Bytes are not validated as UTF-8.
    const auto raw = capabilities_of(with_param(kParamPath, bytes({0xff, 0xfe})));
    ASSERT_TRUE(raw.path.has_value());
    EXPECT_EQ(raw.path->size(), 2u);

    DecodeLimits tight;
    tight.max_string_length = 3;
    const auto over = read_capabilities(with_param(kParamPath, bytes({'a', 'b', 'c', 'd'})), tight);
    const auto* error = std::get_if<DecodeError>(&over);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(error->code, DecodeErrorCode::LengthExceedsLimit);
    EXPECT_TRUE(std::holds_alternative<SetupCapabilities>(
        read_capabilities(with_param(kParamPath, bytes({'a', 'b', 'c'})), tight)));

    SetupCapabilities caps;
    caps.path = "";
    EXPECT_EQ(encode(*build_setup(caps)), bytes({0x03, 0x01, 0x02, 0x00}));
}

TEST(Moqlite06Setup, BuildSetupEmitsKnownParametersInIdOrder) {
    SetupCapabilities caps;
    caps.hop_id = 1;
    caps.cost = 0;
    caps.probe = 1;
    const auto built = build_setup(caps);
    ASSERT_TRUE(built.has_value());
    const auto& message = *built;
    ASSERT_EQ(message.parameters.size(), 3u);
    EXPECT_EQ(message.parameters[0].id, kParamProbe);
    EXPECT_EQ(message.parameters[1].id, kParamCost);
    EXPECT_EQ(message.parameters[2].id, kParamHop);
}

TEST(Moqlite06Setup, EncodeRefusesDuplicatesTooManyParametersAndNoCapacity) {
    {
        SetupMessage message;
        message.parameters.push_back({kParamCost, bytes({0x01})});
        message.parameters.push_back({kParamCost, bytes({0x02})});
        ByteWriter out(64);
        EXPECT_EQ(encode_setup(message, out), std::optional<EncodeError>(EncodeError::InvalidValue));
        EXPECT_EQ(out.size(), 0u);
    }
    {
        SetupMessage message;
        for (std::uint64_t id = 100; id < 100 + 65; ++id) message.parameters.push_back({id, {}});
        ByteWriter out(4096);
        EXPECT_EQ(encode_setup(message, out), std::optional<EncodeError>(EncodeError::LimitExceeded));
        EXPECT_EQ(out.size(), 0u);
    }
    {
        SetupMessage message;
        for (std::uint64_t id = 100; id < 100 + 64; ++id) message.parameters.push_back({id, {}});
        ByteWriter out(4096);
        EXPECT_FALSE(encode_setup(message, out).has_value());
    }
    {
        SetupMessage message;
        message.parameters.push_back({kParamCost, bytes({0x01})});
        ByteWriter out(3);
        EXPECT_EQ(encode_setup(message, out), std::optional<EncodeError>(EncodeError::OutputCapacity));
        EXPECT_EQ(out.size(), 0u);
    }
    {
        SetupMessage message;
        message.parameters.push_back({kMaxVarint + 1, {}});
        ByteWriter out(64);
        EXPECT_EQ(encode_setup(message, out), std::optional<EncodeError>(EncodeError::InvalidValue));
    }
}

TEST(Moqlite06Setup, EncodeRefusesAMessageItsOwnDecoderWouldRefuse) {
    SetupMessage message;
    message.parameters.push_back({kParamPath, Bytes(kDefaultLimits.max_message_length, std::byte{'a'})});
    ByteWriter out(kDefaultLimits.max_message_length + 64);
    EXPECT_EQ(encode_setup(message, out), std::optional<EncodeError>(EncodeError::LimitExceeded));
    EXPECT_EQ(out.size(), 0u);
}

TEST(Moqlite06SetupRobustness, RandomInputsNeverCrashOrAdvanceOnFailure) {
    std::mt19937_64 rng(0x5e7u);
    for (int iteration = 0; iteration < 20000; ++iteration) {
        Bytes data(static_cast<std::size_t>(rng() % 24));
        for (auto& byte : data) byte = static_cast<std::byte>(rng() & 0xffu);
        // Bias a share of the inputs toward a plausible frame so deeper paths run.
        if (!data.empty() && (rng() % 2) == 0) data[0] = static_cast<std::byte>(data.size() - 1);

        Cursor input(data);
        const auto result = decode_setup(input);
        if (std::holds_alternative<SetupMessage>(result)) {
            EXPECT_LE(input.offset(), data.size());
            const auto& message = std::get<SetupMessage>(result);
            (void)read_capabilities(message);
            ByteWriter out(4096);
            EXPECT_FALSE(encode_setup(message, out).has_value());
            // The encoder is minimal, so when it is no longer than what was consumed the input used only
            // minimal varints and the bytes must be identical; otherwise it is strictly shorter.
            if (out.size() == input.offset()) {
                EXPECT_TRUE(std::equal(out.bytes().begin(), out.bytes().end(), data.begin()));
            } else {
                EXPECT_LT(out.size(), input.offset());
            }
        } else {
            EXPECT_EQ(input.offset(), 0u);
        }
    }
}

}  // namespace
}  // namespace moq::interop::wire::moqlite06
