#include "moq/interop/wire/draft21/request_error.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft21 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

TEST(Draft21RequestError, UnsupportedExtensionHasExactGoldenBytes) {
    // draft-ietf-moq-transport-21 sections 9.4 and 16.11.2, Table 19.
    const auto wire = bytes({0x05, 0x00, 0x03, 0x33, 0x00, 0x00});
    Cursor input(wire);
    const auto decoded = decode_request_error(input, false, false);
    ASSERT_TRUE(std::holds_alternative<RequestErrorMessage>(decoded));
    const auto& message = std::get<RequestErrorMessage>(decoded);
    EXPECT_EQ(message.error_code, 0x33u);
    EXPECT_EQ(message.retry_interval, 0u);
    EXPECT_TRUE(message.reason.empty());
    EXPECT_FALSE(message.redirect.has_value());
    EXPECT_EQ(input.offset(), wire.size());
    ByteWriter output(16);
    EXPECT_FALSE(encode_request_error(message, false, false, output).has_value());
    EXPECT_TRUE(std::ranges::equal(output.bytes(), wire));
}

TEST(Draft21RequestError, RedirectHasNamespaceAndTrackName) {
    // draft-ietf-moq-transport-21 section 9.4.1, Figure 8.
    const auto wire = bytes({0x05, 0x00, 0x09, 0x34, 0x00, 0x00,
                             0x00, 0x01, 0x01, 'x', 0x01, 't'});
    Cursor input(wire);
    const auto decoded = decode_request_error(input, false, false);
    ASSERT_TRUE(std::holds_alternative<RequestErrorMessage>(decoded));
    const auto& message = std::get<RequestErrorMessage>(decoded);
    ASSERT_TRUE(message.redirect.has_value());
    EXPECT_TRUE(message.redirect->connect_uri.empty());
    ASSERT_EQ(message.redirect->track_namespace.size(), 1u);
    EXPECT_EQ(message.redirect->track_namespace[0], bytes({'x'}));
    EXPECT_EQ(message.redirect->track_name, bytes({'t'}));
    ByteWriter output(20);
    EXPECT_FALSE(encode_request_error(message, false, false, output).has_value());
    EXPECT_TRUE(std::ranges::equal(output.bytes(), wire));
}

TEST(Draft21RequestError, RejectsClientRedirectUriAndNamespaceTrackName) {
    // draft-ietf-moq-transport-21 section 9.4.1.
    const auto uri = bytes({0x05, 0x00, 0x0a, 0x34, 0x00, 0x00,
                            0x01, 'u', 0x01, 0x01, 'x', 0x01, 't'});
    Cursor from_client(uri);
    const auto client_result = decode_request_error(from_client, true, false);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(client_result));
    EXPECT_EQ(std::get<DecodeError>(client_result).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(from_client.offset(), 0u);

    const auto named = bytes({0x05, 0x00, 0x09, 0x34, 0x00, 0x00,
                              0x00, 0x01, 0x01, 'x', 0x01, 't'});
    Cursor namespace_request(named);
    EXPECT_TRUE(std::holds_alternative<DecodeError>(
        decode_request_error(namespace_request, false, true)));
    EXPECT_EQ(namespace_request.offset(), 0u);
}

TEST(Draft21RequestError, RejectsIncompleteRedirectAndExtraBodyBytes) {
    for (const auto& wire : {
             bytes({0x05, 0x00, 0x03, 0x34, 0x00, 0x00}),
             bytes({0x05, 0x00, 0x04, 0x33, 0x00, 0x00, 0xff})}) {
        Cursor input(wire);
        const auto decoded = decode_request_error(input, false, false);
        ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
        EXPECT_EQ(std::get<DecodeError>(decoded).code,
                  DecodeErrorCode::ProtocolViolation);
        EXPECT_EQ(input.offset(), 0u);
    }
}

TEST(Draft21RequestError, RejectsReasonAbove1024Bytes) {
    // draft-ietf-moq-transport-21 section 8.5 caps Reason Phrase length.
    const auto wire = bytes({0x05, 0x00, 0x04, 0x33, 0x00, 0x84, 0x01});
    Cursor input(wire);
    const auto decoded = decode_request_error(input, false, false);
    ASSERT_TRUE(std::holds_alternative<DecodeError>(decoded));
    EXPECT_EQ(std::get<DecodeError>(decoded).code,
              DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(input.offset(), 0u);
}

TEST(Draft21RequestError, EncodeRequiresRedirectOnlyForRedirectCode) {
    ByteWriter output(32);
    EXPECT_EQ(encode_request_error(RequestErrorMessage{0x34, 0, {}, std::nullopt},
                                   false, false, output),
              RequestErrorEncodeError::InvalidValue);
    EXPECT_EQ(encode_request_error(
                  RequestErrorMessage{0x33, 0, {}, RedirectTarget{}},
                  false, false, output),
              RequestErrorEncodeError::InvalidValue);
    EXPECT_EQ(output.size(), 0u);
}

TEST(Draft21RequestError, PartialFrameAndEncodeFailureAreAtomic) {
    const auto partial = bytes({0x05, 0x00, 0x03, 0x33});
    Cursor input(partial, 30);
    EXPECT_TRUE(std::holds_alternative<NeedMore>(
        decode_request_error(input, false, false)));
    EXPECT_EQ(input.offset(), 30u);
    ByteWriter output(4);
    ASSERT_TRUE(output.append_byte(std::byte{0xcc}));
    const RequestErrorMessage message{0x33, 0, {}, std::nullopt};
    EXPECT_EQ(encode_request_error(message, false, false, output),
              RequestErrorEncodeError::OutputCapacity);
    EXPECT_EQ(output.size(), 1u);
}

}  // namespace
}  // namespace moq::interop::wire::draft21
