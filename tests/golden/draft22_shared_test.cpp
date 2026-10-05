#include "moq/interop/wire/draft22/shared.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <type_traits>
#include <variant>
#include <vector>

namespace moq::interop::wire {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

// Every shared type is the draft 21 type, not a copy.
#define EXPECT_SHARED_TYPE(name) \
    static_assert(std::is_same_v<draft22::name, draft21::name>, #name " must be the draft 21 type")

EXPECT_SHARED_TYPE(KeyValue);
EXPECT_SHARED_TYPE(KeyValues);
EXPECT_SHARED_TYPE(Token);
EXPECT_SHARED_TYPE(TokenAliasType);
EXPECT_SHARED_TYPE(StreamRole);
EXPECT_SHARED_TYPE(MessageKind);
EXPECT_SHARED_TYPE(MessageTypeInfo);
EXPECT_SHARED_TYPE(RequestFrame);
EXPECT_SHARED_TYPE(RedirectTarget);
EXPECT_SHARED_TYPE(RequestErrorMessage);
EXPECT_SHARED_TYPE(GoawayMessage);
EXPECT_SHARED_TYPE(SetupOption);
EXPECT_SHARED_TYPE(SetupMessage);
EXPECT_SHARED_TYPE(ControlMessage);
EXPECT_SHARED_TYPE(PublishDoneMessage);
EXPECT_SHARED_TYPE(ResponseContext);
EXPECT_SHARED_TYPE(ResponseParameter);
EXPECT_SHARED_TYPE(SuccessfulResponse);
EXPECT_SHARED_TYPE(Location);
EXPECT_SHARED_TYPE(Limits);
EXPECT_SHARED_TYPE(ObjectEvent);
EXPECT_SHARED_TYPE(SubgroupHeader);
EXPECT_SHARED_TYPE(SubgroupDecoder);
EXPECT_SHARED_TYPE(FetchDecoder);
EXPECT_SHARED_TYPE(FetchEvent);
EXPECT_SHARED_TYPE(FetchHeader);

TEST(Draft22SharedSurface, SharedFunctionsAreTheDraft21Functions) {
    // draft-ietf-moq-transport-22 changes no figure these modules decode (see the audit test).
    EXPECT_EQ(&draft22::decode_key_values, &draft21::decode_key_values);
    EXPECT_EQ(&draft22::decode_key_values_to_end, &draft21::decode_key_values_to_end);
    EXPECT_EQ(&draft22::encode_key_values, &draft21::encode_key_values);
    EXPECT_EQ(&draft22::decode_token, &draft21::decode_token);
    EXPECT_EQ(&draft22::encode_token, &draft21::encode_token);
    EXPECT_EQ(&draft22::classify_message_type, &draft21::classify_message_type);
    EXPECT_EQ(&draft22::decode_message_type, &draft21::decode_message_type);
    EXPECT_EQ(&draft22::decode_request_frame, &draft21::decode_request_frame);
    EXPECT_EQ(&draft22::decode_request_error, &draft21::decode_request_error);
    EXPECT_EQ(&draft22::encode_request_error, &draft21::encode_request_error);
    EXPECT_EQ(&draft22::encode_empty_publish_ok, &draft21::encode_empty_publish_ok);
    EXPECT_EQ(&draft22::decode_goaway, &draft21::decode_goaway);
    EXPECT_EQ(&draft22::encode_goaway, &draft21::encode_goaway);
    EXPECT_EQ(&draft22::decode_setup, &draft21::decode_setup);
    EXPECT_EQ(&draft22::encode_setup, &draft21::encode_setup);
    EXPECT_EQ(&draft22::decode_control_message, &draft21::decode_control_message);
    EXPECT_EQ(&draft22::valid_reason_phrase, &draft21::valid_reason_phrase);
    EXPECT_EQ(&draft22::decode_publish_done, &draft21::decode_publish_done);
    EXPECT_EQ(&draft22::validate_track_properties, &draft21::validate_track_properties);
    EXPECT_EQ(&draft22::decode_successful_response, &draft21::decode_successful_response);
}

TEST(Draft22SharedSurface, SharedNamesWorkThroughTheDraft22Namespace) {
    // draft-ietf-moq-transport-22 section 8.3: Key-Value-Pair, even Type carries a varint.
    const auto pair_bytes = bytes({0x02, 0x05});
    Cursor pair_input(pair_bytes);
    const auto pairs = draft22::decode_key_values(pair_input, 1);
    ASSERT_TRUE(std::holds_alternative<draft22::KeyValues>(pairs));
    const auto& values = std::get<draft22::KeyValues>(pairs);
    ASSERT_EQ(values.size(), 1u);
    EXPECT_EQ(values[0].type, 2u);
    EXPECT_EQ(std::get<std::uint64_t>(values[0].value), 5u);

    // Section 9, Table 5: 0x1D is PUBLISH, a request starter.
    const auto info = draft22::classify_message_type(0x1d);
    ASSERT_TRUE(info.has_value());
    EXPECT_EQ(info->kind, draft22::MessageKind::Publish);
    EXPECT_TRUE(info->request_starter);
}

}  // namespace
}  // namespace moq::interop::wire
