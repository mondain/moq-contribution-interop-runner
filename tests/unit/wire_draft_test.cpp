#include "moq/interop/scenarios/wire_draft.h"

#include "moq/interop/wire/draft21/location_filter.h"

#include <gtest/gtest.h>

#include <optional>
#include <thread>
#include <variant>
#include <vector>

namespace moq::interop::scenarios {
namespace {

std::vector<std::byte> publish_with(unsigned count, std::initializer_list<unsigned> parameters) {
    std::vector<unsigned> body = {0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a', 0x04, 't', 'e', 's', 't', 0x02, count};
    body.insert(body.end(), parameters.begin(), parameters.end());
    std::vector<unsigned> wire = {0x1d, static_cast<unsigned>(body.size() >> 8), static_cast<unsigned>(body.size() & 0xffu)};
    wire.insert(wire.end(), body.begin(), body.end());
    std::vector<std::byte> out;
    for (const auto v : wire) out.push_back(static_cast<std::byte>(v));
    return out;
}

const std::vector<std::byte>& filter_bytes(const wire::draft21::PublishMessage& m) {
    return std::get<std::vector<std::byte>>(m.parameters.at(0).value);
}

TEST(WireDraft, DefaultsToDraft21AndScopesRestore) {
    EXPECT_EQ(current_wire_draft(), 21u);
    {
        ScopedWireDraft outer(22);
        EXPECT_EQ(current_wire_draft(), 22u);
        {
            ScopedWireDraft inner(18);
            EXPECT_EQ(current_wire_draft(), 18u);
        }
        EXPECT_EQ(current_wire_draft(), 22u);
    }
    EXPECT_EQ(current_wire_draft(), 21u);
}

TEST(WireDraft, TheValueIsPerThread) {
    ScopedWireDraft scope(22);
    unsigned seen = 0;
    std::thread other([&] { seen = current_wire_draft(); });
    other.join();
    EXPECT_EQ(seen, 21u);
    EXPECT_EQ(current_wire_draft(), 22u);
}

TEST(WireDraft, Draft21PathIsExactlyTheDraft21Decoder) {
    // draft 21: type 0x21, Length 2, payload {0, 0} (Next Object).
    const auto wire_bytes = publish_with(1, {0x21, 0x02, 0x00, 0x00});
    wire::Cursor cursor(wire_bytes);
    const auto decoded = decode_publish_for_wire(cursor);
    ASSERT_TRUE(std::holds_alternative<wire::draft21::PublishMessage>(decoded));
    EXPECT_EQ(cursor.offset(), wire_bytes.size());
    const auto filter = wire::draft21::decode_location_filter(filter_bytes(std::get<wire::draft21::PublishMessage>(decoded)));
    ASSERT_TRUE(std::holds_alternative<wire::draft21::LocationFilter>(filter));
    EXPECT_EQ(std::get<wire::draft21::LocationFilter>(filter).kind, wire::draft21::LocationFilterKind::NextObject);
}

TEST(WireDraft, Draft22PublishIsPresentedInDraft21Form) {
    ScopedWireDraft scope(22);
    {
        const auto w = publish_with(1, {0x21, 0x05});  // Next Object
        wire::Cursor c(w);
        const auto d = decode_publish_for_wire(c);
        ASSERT_TRUE(std::holds_alternative<wire::draft21::PublishMessage>(d));
        EXPECT_EQ(filter_bytes(std::get<wire::draft21::PublishMessage>(d)), (std::vector<std::byte>{std::byte{0}, std::byte{0}}));
        EXPECT_EQ(c.offset(), w.size());
    }
    {
        const auto w = publish_with(1, {0x21, 0x01, 0x02});  // Relative start 2
        wire::Cursor c(w);
        const auto d = decode_publish_for_wire(c);
        ASSERT_TRUE(std::holds_alternative<wire::draft21::PublishMessage>(d));
        EXPECT_EQ(filter_bytes(std::get<wire::draft21::PublishMessage>(d)), (std::vector<std::byte>{std::byte{2}}));
    }
    {
        const auto w = publish_with(1, {0x21, 0x04, 0x01, 0x03, 0x05, 0x09});  // Absolute range
        wire::Cursor c(w);
        const auto d = decode_publish_for_wire(c);
        ASSERT_TRUE(std::holds_alternative<wire::draft21::PublishMessage>(d));
        EXPECT_EQ(filter_bytes(std::get<wire::draft21::PublishMessage>(d)),
                  (std::vector<std::byte>{std::byte{1}, std::byte{3}, std::byte{5}, std::byte{9}}));
    }
    {
        const auto w = publish_with(1, {0x21, 0x00});  // No filter
        wire::Cursor c(w);
        const auto d = decode_publish_for_wire(c);
        ASSERT_TRUE(std::holds_alternative<wire::draft21::PublishMessage>(d));
        EXPECT_TRUE(filter_bytes(std::get<wire::draft21::PublishMessage>(d)).empty());
    }
}

// Converts a draft 22 PUBLISH carrying one LOCATION_FILTER (given as the bytes after the 0x21 type) and
// returns the payload plus its reading by the draft 21 filter decoder.
struct Converted {
    std::vector<std::byte> payload;
    wire::draft21::LocationFilter filter;
};

Converted convert_filter(std::initializer_list<unsigned> filter_fields) {
    std::vector<unsigned> parameters = {0x21};
    parameters.insert(parameters.end(), filter_fields.begin(), filter_fields.end());
    std::vector<unsigned> body = {0x01, 0x01, 0x05, 'm', 'e', 'd', 'i', 'a', 0x04, 't', 'e', 's', 't', 0x02, 1};
    body.insert(body.end(), parameters.begin(), parameters.end());
    std::vector<unsigned> wire_values = {0x1d, static_cast<unsigned>(body.size() >> 8),
                                         static_cast<unsigned>(body.size() & 0xffu)};
    wire_values.insert(wire_values.end(), body.begin(), body.end());
    std::vector<std::byte> bytes;
    for (const auto v : wire_values) bytes.push_back(static_cast<std::byte>(v));
    wire::Cursor cursor(bytes);
    const auto decoded = decode_publish_for_wire(cursor);
    EXPECT_TRUE(std::holds_alternative<wire::draft21::PublishMessage>(decoded));
    Converted converted;
    if (const auto* message = std::get_if<wire::draft21::PublishMessage>(&decoded)) {
        EXPECT_EQ(cursor.offset(), bytes.size());
        converted.payload = filter_bytes(*message);
        const auto read = wire::draft21::decode_location_filter(converted.payload);
        EXPECT_TRUE(std::holds_alternative<wire::draft21::LocationFilter>(read));
        if (const auto* filter = std::get_if<wire::draft21::LocationFilter>(&read)) converted.filter = *filter;
    }
    return converted;
}

std::vector<std::byte> bytes_of(std::initializer_list<unsigned> values) {
    std::vector<std::byte> out;
    for (const auto v : values) out.push_back(static_cast<std::byte>(v));
    return out;
}

TEST(WireDraft, EveryDraft22FilterTypeReadsBackWithItsDraft22Meaning) {
    ScopedWireDraft scope(22);
    using Kind = wire::draft21::LocationFilterKind;
    {  // None
        const auto c = convert_filter({0x00});
        EXPECT_TRUE(c.payload.empty());
        EXPECT_EQ(c.filter.kind, Kind::None);
    }
    {  // RelativeGroup 2
        const auto c = convert_filter({0x01, 0x02});
        EXPECT_EQ(c.payload, bytes_of({2}));
        EXPECT_EQ(c.filter.kind, Kind::RelativeGroup);
        EXPECT_EQ(c.filter.start_group, 2u);
    }
    {  // Absolute (3,4) must stay Absolute, not collapse to RelativeGroup
        const auto c = convert_filter({0x02, 0x03, 0x04});
        EXPECT_EQ(c.payload, bytes_of({3, 4}));
        EXPECT_EQ(c.filter.kind, Kind::Absolute);
        EXPECT_EQ(c.filter.start_group, 3u);
        EXPECT_EQ(c.filter.start_object, 4u);
        EXPECT_FALSE(c.filter.end_group_delta.has_value());
        EXPECT_FALSE(c.filter.end_object.has_value());
    }
    {  // Absolute (0,5): accepted, only {0,0} is refused
        const auto c = convert_filter({0x02, 0x00, 0x05});
        EXPECT_EQ(c.payload, bytes_of({0, 5}));
        EXPECT_EQ(c.filter.kind, Kind::Absolute);
        EXPECT_EQ(c.filter.start_group, 0u);
        EXPECT_EQ(c.filter.start_object, 5u);
    }
    {  // Absolute (5,0): accepted
        const auto c = convert_filter({0x02, 0x05, 0x00});
        EXPECT_EQ(c.payload, bytes_of({5, 0}));
        EXPECT_EQ(c.filter.kind, Kind::Absolute);
        EXPECT_EQ(c.filter.start_group, 5u);
        EXPECT_EQ(c.filter.start_object, 0u);
    }
    {  // Absolute with a multi-byte varint start group (300 = 0x81 0x2c)
        const auto c = convert_filter({0x02, 0x81, 0x2c, 0x07});
        EXPECT_EQ(c.payload, bytes_of({0x81, 0x2c, 7}));
        EXPECT_EQ(c.filter.kind, Kind::Absolute);
        EXPECT_EQ(c.filter.start_group, 300u);
        EXPECT_EQ(c.filter.start_object, 7u);
    }
    {  // AbsoluteBounded (1,2) end group delta 5
        const auto c = convert_filter({0x03, 0x01, 0x02, 0x05});
        EXPECT_EQ(c.payload, bytes_of({1, 2, 5}));
        EXPECT_EQ(c.filter.kind, Kind::Absolute);
        EXPECT_EQ(c.filter.start_group, 1u);
        EXPECT_EQ(c.filter.start_object, 2u);
        ASSERT_TRUE(c.filter.end_group_delta.has_value());
        EXPECT_EQ(*c.filter.end_group_delta, 5u);
        EXPECT_FALSE(c.filter.end_object.has_value());
    }
    {  // AbsoluteRange (1,3) delta 5 end object 9
        const auto c = convert_filter({0x04, 0x01, 0x03, 0x05, 0x09});
        EXPECT_EQ(c.payload, bytes_of({1, 3, 5, 9}));
        EXPECT_EQ(c.filter.kind, Kind::Absolute);
        ASSERT_TRUE(c.filter.end_group_delta.has_value());
        ASSERT_TRUE(c.filter.end_object.has_value());
        EXPECT_EQ(*c.filter.end_group_delta, 5u);
        EXPECT_EQ(*c.filter.end_object, 9u);
    }
    {  // NextObject
        const auto c = convert_filter({0x05});
        EXPECT_EQ(c.payload, bytes_of({0, 0}));
        EXPECT_EQ(c.filter.kind, Kind::NextObject);
    }
}

TEST(WireDraft, Draft22SeveralParametersKeepTypesValuesAndOrder) {
    ScopedWireDraft scope(22);
    // Deltas: 0x02 varint 7; +1 = 0x03 token {UseValue, type 0, value 0xaa}; +6 = 0x09 Location (4,5);
    // +0x18 = 0x21 NextObject filter.
    const auto w = publish_with(4, {0x02, 0x07, 0x01, 0x03, 0x03, 0x00, 0xaa, 0x06, 0x04, 0x05, 0x18, 0x05});
    wire::Cursor c(w);
    const auto d = decode_publish_for_wire(c);
    ASSERT_TRUE(std::holds_alternative<wire::draft21::PublishMessage>(d));
    const auto& parameters = std::get<wire::draft21::PublishMessage>(d).parameters;
    EXPECT_EQ(c.offset(), w.size());
    ASSERT_EQ(parameters.size(), 4u);
    EXPECT_EQ(parameters[0].type, 0x02u);
    ASSERT_NE(std::get_if<std::uint64_t>(&parameters[0].value), nullptr);
    EXPECT_EQ(std::get<std::uint64_t>(parameters[0].value), 7u);
    EXPECT_EQ(parameters[1].type, 0x03u);
    const auto* token = std::get_if<wire::draft21::Token>(&parameters[1].value);
    ASSERT_NE(token, nullptr);
    EXPECT_EQ(token->alias_type, wire::draft21::TokenAliasType::UseValue);
    EXPECT_EQ(token->token_type, std::optional<std::uint64_t>(0));
    EXPECT_EQ(token->value, bytes_of({0xaa}));
    EXPECT_EQ(parameters[2].type, 0x09u);
    const auto* location = std::get_if<wire::draft21::Location>(&parameters[2].value);
    ASSERT_NE(location, nullptr);
    EXPECT_EQ(location->group, 4u);
    EXPECT_EQ(location->object, 5u);
    EXPECT_EQ(parameters[3].type, 0x21u);
    EXPECT_EQ(std::get<std::vector<std::byte>>(parameters[3].value), bytes_of({0, 0}));
}

TEST(WireDraft, Draft22NonFilterParametersPassThrough) {
    ScopedWireDraft scope(22);
    const auto w = publish_with(1, {0x20, 0xc8});
    wire::Cursor c(w);
    const auto d = decode_publish_for_wire(c);
    ASSERT_TRUE(std::holds_alternative<wire::draft21::PublishMessage>(d));
    const auto& message = std::get<wire::draft21::PublishMessage>(d);
    ASSERT_EQ(message.parameters.size(), 1u);
    EXPECT_EQ(message.parameters[0].type, 0x20u);
    const auto* octet = std::get_if<std::uint8_t>(&message.parameters[0].value);
    ASSERT_NE(octet, nullptr);
    EXPECT_EQ(*octet, 200u);
    EXPECT_EQ(c.offset(), w.size());
}

TEST(WireDraft, Draft22AbsoluteZeroZeroIsRefusedLoudlyAndTheCursorStays) {
    ScopedWireDraft scope(22);
    const auto w = publish_with(1, {0x21, 0x02, 0x00, 0x00});
    wire::Cursor c(w);
    const auto d = decode_publish_for_wire(c);
    ASSERT_TRUE(std::holds_alternative<wire::DecodeError>(d));
    EXPECT_EQ(std::get<wire::DecodeError>(d).code, wire::DecodeErrorCode::ProtocolViolation);
    EXPECT_EQ(std::get<wire::DecodeError>(d).detail, "draft-22 Absolute {0,0} filter has no draft-21 form");
    EXPECT_EQ(std::get<wire::DecodeError>(d).offset, 0u);
    EXPECT_EQ(c.offset(), 0u);
}

TEST(WireDraft, Draft22ErrorsAndPartialFramesAreTransactional) {
    ScopedWireDraft scope(22);
    const auto bad = publish_with(1, {0x21, 0x06});  // unknown Location Filter Type
    wire::Cursor c(bad);
    EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(decode_publish_for_wire(c)));
    EXPECT_EQ(c.offset(), 0u);
    const std::vector<std::byte> partial = {std::byte{0x1d}, std::byte{0x00}, std::byte{0x0f}};
    wire::Cursor p(partial, 7);
    EXPECT_TRUE(std::holds_alternative<wire::NeedMore>(decode_publish_for_wire(p)));
    EXPECT_EQ(p.offset(), 7u);
}

}  // namespace
}  // namespace moq::interop::scenarios
