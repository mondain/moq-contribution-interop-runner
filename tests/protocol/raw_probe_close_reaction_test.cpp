#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/scenarios/draft18_close.h"
#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/scenarios/request_probe.h"
#include <gtest/gtest.h>
#include <algorithm>
#include <chrono>
#include <map>
#include <memory>

// A close counts as the publisher's reaction to a probe only when it follows the delivered
// stimulus and arrives within a bounded window of it; and when any code satisfies the rule, a
// NO_ERROR close does not count. Otherwise a publisher that closes on its own schedule (a read
// timeout, a process deadline) would pass or fail a row for input it never reacted to.
namespace moq::interop::scenarios {
namespace {
using namespace std::chrono_literals;
using Bytes = std::vector<std::byte>;

Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

class ScriptedPeer : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::Success, next_bidi += 4}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, next_uni += 4}; }
    transport::OperationResult write(transport::StreamId, std::span<const std::byte> bytes, bool) override {
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
    std::vector<transport::TransportEvent> events;
};

// One scripted session against a RawProbeController on a manual clock.
class Session {
public:
    explicit Session(RawProbeDefinition definition)
        : definition_(std::move(definition)),
          controller_(std::make_unique<RawProbeController>(peer, definition_)),
          t0_(RawProbeClock::time_point{} + 1000s) {
        peer.events.push_back(transport::ConnectionEstablishedEvent{{}, {}, {}, 1200});
        peer.events.push_back(transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false});
    }
    const RawProbeTranscript& poll(std::chrono::milliseconds at) { return controller_->poll(t0_ + at); }
    void close_with(std::uint64_t code, transport::CloseErrorSpace space = transport::CloseErrorSpace::Application) {
        peer.events.push_back(transport::PeerCloseEvent{space, code, {}});
    }
    const RawProbeDefinition& definition() const { return definition_; }
    ScriptedPeer peer;

private:
    RawProbeDefinition definition_;
    std::unique_ptr<RawProbeController> controller_;
    RawProbeClock::time_point t0_;
};

// The base window applies when no liveness follow-up widens it.
RawProbeDefinition close_probe(const std::string& id) {
    auto definition = draft18_close_probe(id, 5000ms);
    definition.liveness.reset();
    return definition;
}

constexpr auto kAnyCode = "receive-unknown-message-type";
constexpr auto kExactCode = "receive-known-message-with-mismatched-payload-length";  // PROTOCOL_VIOLATION (3)

// Runs the stimulus at t=0, injects a close at `at`, and evaluates.
std::optional<bool> judge(const std::string& id, std::optional<std::uint64_t> expected,
                          std::uint64_t code, std::chrono::milliseconds at,
                          transport::CloseErrorSpace space = transport::CloseErrorSpace::Application) {
    Session session(close_probe(id));
    session.poll(0ms);
    session.close_with(code, space);
    const auto& done = session.poll(at);
    return evaluate_raw_probe_close(done, session.definition(), expected);
}

TEST(CloseReaction, AnyCodeRuleAcceptsAnErrorCloseSoonAfterTheStimulus) {
    EXPECT_EQ(judge(kAnyCode, std::nullopt, 3, 100ms), std::optional<bool>{true});
    EXPECT_EQ(judge(kAnyCode, std::nullopt, 9, 1400ms), std::optional<bool>{true});
}

TEST(CloseReaction, AnyCodeRuleDoesNotCountANoErrorClose) {
    // moqxr closes with NO_ERROR when its own read of a pending request times out.
    EXPECT_FALSE(judge(kAnyCode, std::nullopt, 0, 100ms).has_value());
}

TEST(CloseReaction, ACloseLongAfterTheStimulusIsNotAttributedToIt) {
    EXPECT_FALSE(judge(kAnyCode, std::nullopt, 3, 2000ms).has_value());
    EXPECT_FALSE(judge(kExactCode, 3, 3, 2000ms).has_value());
}

TEST(CloseReaction, ExactCodeRuleStillPassesAndFailsOnAReaction) {
    EXPECT_EQ(judge(kExactCode, 3, 3, 100ms), std::optional<bool>{true});
    // Within the window a wrong code, including NO_ERROR, is the publisher's reaction.
    EXPECT_EQ(judge(kExactCode, 3, 0, 100ms), std::optional<bool>{false});
    EXPECT_EQ(judge(kExactCode, 3, 4, 100ms), std::optional<bool>{false});
}

TEST(CloseReaction, ExactCodeRuleIgnoresALateNoErrorClose) {
    EXPECT_FALSE(judge(kExactCode, 3, 0, 2000ms).has_value());
}

TEST(CloseReaction, TransportLevelCloseIsStillUnscored) {
    EXPECT_FALSE(judge(kAnyCode, std::nullopt, 3, 100ms, transport::CloseErrorSpace::Transport).has_value());
}

TEST(CloseReaction, ALivenessFollowUpWidensTheWindowByItsDelayAndGrace) {
    // The follow-up waits 500 ms and then 500 ms of grace, so a close that comes after the
    // follow-up was served is still the reaction.
    auto definition = draft18_close_probe(kAnyCode, 5000ms);
    ASSERT_TRUE(definition.liveness.has_value());
    Session session(definition);
    session.poll(0ms);
    session.close_with(3);
    const auto& done = session.poll(2400ms);
    EXPECT_EQ(evaluate_raw_probe_close(done, session.definition(), std::nullopt), std::optional<bool>{true});
    Session late(definition);
    late.poll(0ms);
    late.close_with(3);
    EXPECT_FALSE(evaluate_raw_probe_close(late.poll(2600ms), late.definition(), std::nullopt).has_value());
}

TEST(CloseReaction, EveryEventHasAnArrivalTime) {
    Session session(close_probe(kAnyCode));
    session.poll(0ms);
    session.close_with(3);
    const auto& done = session.poll(300ms);
    EXPECT_EQ(done.event_times.size(), done.events.size());
}

TEST(CloseReaction, RequestProbePendingResponseNeedsAnAttributableErrorClose) {
    const auto profiles = draft21_request_profiles(5000ms);
    ASSERT_FALSE(profiles.empty());
    const auto run = [&](std::uint64_t code, std::chrono::milliseconds at) {
        Session session(profiles.front().definition);
        session.poll(0ms);
        session.close_with(code);
        const auto& done = session.poll(at);
        return evaluate_raw_probe_request_error(done, profiles.front());
    };
    // The publisher closed with an error instead of answering the request.
    EXPECT_EQ(run(3, 100ms), std::optional<bool>{false});
    // NO_ERROR, or a close long after the request, says nothing about the request.
    EXPECT_FALSE(run(0, 100ms).has_value());
    EXPECT_FALSE(run(3, 2000ms).has_value());
}

}  // namespace
}  // namespace moq::interop::scenarios
