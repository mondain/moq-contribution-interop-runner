#include "moq/interop/scenarios/wire_draft.h"

#include "moq/interop/wire/draft21/location_filter.h"

#include <gtest/gtest.h>

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
