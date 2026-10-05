#include "moq/interop/session/draft21_control_state.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <initializer_list>
#include <variant>
#include <vector>

namespace moq::interop::session::draft21 {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

const auto kAlpn = bytes({'m', 'o', 'q', 't', '-', '2', '1'});

TEST(Draft21ControlState, ActivatesAfterBothSetupsAcrossFragments) {
    // draft-ietf-moq-transport-21 sections 6.2 and 6.3.
    ControlState state;
    EXPECT_FALSE(state.on_transport_established(kAlpn).has_value());
    EXPECT_EQ(state.phase(), ControlPhase::AwaitingSetup);
    const auto first = state.on_peer_data(6, bytes({0xaf, 0x00}), false);
    EXPECT_TRUE(first.messages.empty());
    EXPECT_FALSE(first.close_error.has_value());
    const auto second = state.on_peer_data(6, bytes({0x00, 0x00}), false);
    ASSERT_EQ(second.messages.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<wire::draft21::SetupMessage>(
        second.messages[0]));
    EXPECT_EQ(state.phase(), ControlPhase::AwaitingSetup);
    state.on_local_setup_sent();
    EXPECT_EQ(state.phase(), ControlPhase::Active);
}

TEST(Draft21ControlState, ParsesPipelinedGoawayWithoutSecondSetup) {
    ControlState state;
    state.on_local_setup_sent();
    state.on_transport_established(kAlpn);
    const auto result = state.on_peer_data(
        2, bytes({0xaf, 0x00, 0x00, 0x00,
                  0x10, 0x00, 0x02, 0x00, 0x05}), false);
    ASSERT_EQ(result.messages.size(), 2u);
    EXPECT_TRUE(std::holds_alternative<wire::draft21::SetupMessage>(
        result.messages[0]));
    EXPECT_TRUE(std::holds_alternative<wire::draft21::GoawayMessage>(
        result.messages[1]));
    EXPECT_EQ(state.phase(), ControlPhase::Active);
}

TEST(Draft21ControlState, ClosingControlStreamIsProtocolViolation) {
    // draft-ietf-moq-transport-21 section 6.3 mandates an open control stream.
    ControlState state;
    state.on_transport_established(kAlpn);
    const auto result = state.on_peer_data(
        2, bytes({0xaf, 0x00, 0x00, 0x00}), true);
    EXPECT_EQ(result.close_error, 0x3u);
    EXPECT_EQ(state.phase(), ControlPhase::Closing);
}

TEST(Draft21ControlState, RejectsSecondPeerControlStreamAndWrongAlpn) {
    ControlState state;
    EXPECT_EQ(state.on_transport_established(
                  bytes({'m', 'o', 'q', 't', '-', '1', '8'})), 0x3u);
    EXPECT_EQ(state.phase(), ControlPhase::Closing);

    ControlState second;
    second.on_transport_established(kAlpn);
    second.on_peer_data(2, bytes({0xaf, 0x00, 0x00, 0x00}), false);
    EXPECT_EQ(second.on_peer_data(6, bytes({0xaf, 0x00, 0x00, 0x00}), false)
                  .close_error, 0x3u);
}

TEST(Draft21ControlState, SeparatesHarnessBufferLimitFromPeerViolation) {
    ControlState state(3);
    state.on_transport_established(kAlpn);
    const auto result = state.on_peer_data(
        2, bytes({0xaf, 0x00, 0x00, 0x00}), false);
    EXPECT_TRUE(result.harness_limit);
    EXPECT_FALSE(result.close_error.has_value());
    EXPECT_EQ(state.phase(), ControlPhase::Closing);
}

TEST(Draft21ControlState, ADraft22LineageRunAcceptsOnlyTheDraft22Alpn) {
    const auto alpn22 = bytes({'m', 'o', 'q', 't', '-', '2', '2'});
    {
        ControlState state;
        EXPECT_TRUE(state.on_transport_established(alpn22).has_value()) << "draft 21 runs refuse moqt-22";
    }
    const scenarios::ScopedWireDraft wire(22);
    {
        ControlState state;
        EXPECT_FALSE(state.on_transport_established(alpn22).has_value());
        EXPECT_EQ(state.phase(), ControlPhase::AwaitingSetup);
    }
    ControlState state;
    EXPECT_TRUE(state.on_transport_established(kAlpn).has_value()) << "a draft 22 run refuses moqt-21";
    EXPECT_EQ(state.phase(), ControlPhase::Closing);
}

}  // namespace
}  // namespace moq::interop::session::draft21
