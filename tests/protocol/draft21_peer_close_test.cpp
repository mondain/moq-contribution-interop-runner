#include "moq/interop/scenarios/draft21_peer_close.h"
#include "moq/interop/scenarios/draft18_peer_close.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

struct Fixture {
    const char* scenario;
    const char* requirement;
    const char* evaluator;
    bool publish;
    Bytes response;
};

std::vector<Fixture> fixtures() {
    // Section 8.5: 1025 = vi64 0x8401, and the two code fields plus
    // length and phrase occupy 1029 (0x0405) body bytes.
    auto oversized = bytes({5, 4, 5, 0, 0, 0x84, 1});
    oversized.insert(oversized.end(), 1025, std::byte{'x'});
    return {
        {"d21-publish-request-error-oversized-reason", "D21-8-5-MUST-248",
         "d21-oversized-reason-phrase-protocol-violation", true, oversized},
        // Sections 9.3 and 10.3: MAX_CACHE_DURATION is a legal Track
        // Property encoded as Type4, Value1, forbidden in these OKs.
        {"d21-publish-ok-with-track-properties", "D21-9-3-MUST-337",
         "d21-request-ok-forbidden-track-properties", true,
         bytes({7, 0, 3, 0, 4, 1})},
        {"d21-publish-namespace-ok-with-track-properties", "D21-9-3-MUST-337",
         "d21-request-ok-forbidden-track-properties", false,
         bytes({7, 0, 3, 0, 4, 1})},
        // Sections 9.4.1 and 16.11.2: REDIRECT0x34, zero retry/reason,
        // empty URI, namespace n and a forbidden nonempty Track Name x.
        {"d21-publish-namespace-redirect-nonempty-track-name", "D21-9-4-1-MUST-341",
         "d21-namespace-redirect-track-name-protocol-violation", false,
         bytes({5, 0, 9, 0x34, 0, 0, 0, 1, 1, 'n', 1, 'x'})},
    };
}

Bytes client_request(bool publish) {
    // Sections 9.8 and 9.14: Request ID0 belongs to the client. PUBLISH
    // carries a Track Alias and a legal trailing MAX_CACHE_DURATION.
    return publish
        ? bytes({0x1d, 0, 10, 0, 1, 1, 'n', 1, 'x', 0, 0, 4, 1})
        : bytes({6, 0, 5, 0, 1, 1, 'n', 0});
}

const Draft21PeerCloseProbe* find(const std::vector<Draft21PeerCloseProbe>& probes,
                                const char* scenario) {
    const auto found = std::find_if(probes.begin(), probes.end(),
        [&](const auto& probe) { return probe.definition.id == scenario; });
    return found == probes.end() ? nullptr : &*found;
}

RawProbeTranscript transcript(const Fixture& fixture) {
    RawProbeTranscript observed;
    observed.scenario_id = fixture.scenario;
    observed.setup = {{RawProbeChannel::NewUni, bytes({0xaf, 0, 0, 0}), false},
                      3, 4, false};
    observed.writes = {{{RawProbeChannel::PeerBidi, fixture.response, false},
                        0, fixture.response.size(), false}};
    observed.complete = observed.stimulus_delivered = true;
    observed.transport_established = observed.peer_setup_received = true;
    observed.delivery_event_count = 3;
    observed.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 0}), false},
        transport::StreamDataEvent{0, client_request(fixture.publish), false},
        transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}}};
    return observed;
}

TEST(Draft21PeerClose, UsesIndependentMalformedResponsesOnActualClientRequests) {
    const auto probes = draft21_peer_close_probes(std::chrono::milliseconds{71});
    EXPECT_EQ(probes.size(), 6u);
    for (const auto& fixture : fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto* probe = find(probes, fixture.scenario);
        EXPECT_NE(probe, nullptr);
        if (!probe) continue;
        EXPECT_EQ(probe->requirement_id, fixture.requirement);
        EXPECT_EQ(probe->evaluator_id, fixture.evaluator);
        EXPECT_EQ(probe->expected_close, 3u);
        EXPECT_EQ(probe->definition.setup_bytes, bytes({0xaf, 0, 0, 0}));
        EXPECT_TRUE(probe->definition.start_after_peer_setup);
        EXPECT_EQ(probe->definition.deadline, std::chrono::milliseconds{71});
        EXPECT_TRUE(probe->definition.peer_setup_ready(bytes({0xaf, 0, 0, 0})));
        EXPECT_FALSE(probe->definition.peer_setup_ready(bytes({0xaf, 0, 0})));
        ASSERT_EQ(probe->definition.writes.size(), 1u);
        EXPECT_EQ(probe->definition.writes.front().channel, RawProbeChannel::PeerBidi);
        EXPECT_FALSE(probe->definition.writes.front().fin);
        EXPECT_EQ(probe->definition.writes.front().bytes, fixture.response);
    }
}

TEST(Draft21PeerClose, RequestReadinessRequiresCorrectCompleteClientOpener) {
    const auto probes = draft21_peer_close_probes();
    for (const auto& fixture : fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto* probe = find(probes, fixture.scenario);
        EXPECT_NE(probe, nullptr);
        if (!probe) continue;
        ASSERT_TRUE(probe->definition.peer_request_ready);
        const auto& ready = probe->definition.peer_request_ready;
        const auto request = client_request(fixture.publish);
        EXPECT_TRUE(ready(request));
        for (std::size_t length = 0; length < request.size(); ++length)
            EXPECT_FALSE(ready(std::span{request}.first(length)));
        EXPECT_FALSE(ready(client_request(!fixture.publish)));
        auto odd_id = request;
        odd_id[3] = std::byte{1};
        EXPECT_FALSE(ready(odd_id));
        auto malformed = request;
        malformed[5] = std::byte{0};
        EXPECT_FALSE(ready(malformed));
        auto incomplete_body = request;
        incomplete_body[2] = std::byte{1};
        EXPECT_FALSE(ready(incomplete_body));
        // Namespace requests admit complete, well-encoded AUTHORIZATION
        // tokens, but an in-body truncated Token cannot open the gate.
        if (!fixture.publish) {
            EXPECT_TRUE(ready(bytes({6, 0, 10, 0, 1, 1, 'n', 1,
                                     3, 3, 3, 0x80, 0x9d})));
            EXPECT_FALSE(ready(bytes({6, 0, 8, 0, 1, 1, 'n', 1, 3, 1, 3})));
        }
    }
}

TEST(Draft21PeerClose, CloseProofRequiresMatchingStreamAndCompleteRequestBeforeResponse) {
    const auto probes = draft21_peer_close_probes();
    for (const auto& fixture : fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto* probe = find(probes, fixture.scenario);
        EXPECT_NE(probe, nullptr);
        if (!probe) continue;
        auto observed = transcript(fixture);
        const auto evaluate = [&] {
            return evaluate_raw_probe_close(observed, probe->definition, probe->expected_close);
        };
        EXPECT_EQ(evaluate(), true);
        --observed.writes.front().accepted;
        EXPECT_FALSE(evaluate().has_value());
        ++observed.writes.front().accepted;
        observed.writes.front().stream_id = 4;
        EXPECT_FALSE(evaluate().has_value());
        observed.writes.front().stream_id = 0;
        auto& request = std::get<transport::StreamDataEvent>(observed.events[2]);
        const auto saved = request.data;
        request.data.pop_back();
        EXPECT_FALSE(evaluate().has_value());
        request.data = saved;
        observed.delivery_event_count = 2;
        EXPECT_FALSE(evaluate().has_value());
        observed.delivery_event_count = 3;
        auto& close = std::get<transport::PeerCloseEvent>(observed.events.back());
        close.error_code = 4;
        EXPECT_EQ(evaluate(), false);
        close.error_code = 3;
        close.error_space = transport::CloseErrorSpace::Transport;
        EXPECT_FALSE(evaluate().has_value());
    }
}

TEST(Draft21PeerClose, FragmentedRequestProofWaitsForCompleteTypedOpener) {
    const auto probes = draft21_peer_close_probes();
    for (const auto& fixture : fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto* probe = find(probes, fixture.scenario);
        EXPECT_NE(probe, nullptr);
        if (!probe) continue;
        auto observed = transcript(fixture);
        const auto request = client_request(fixture.publish);
        std::get<transport::StreamDataEvent>(observed.events[2]).data =
            Bytes(request.begin(), request.begin() + 4);
        observed.events.insert(observed.events.begin() + 3,
            transport::StreamDataEvent{0, Bytes(request.begin() + 4, request.end()), false});
        observed.delivery_event_count = 4;
        EXPECT_EQ(evaluate_raw_probe_close(observed, probe->definition, probe->expected_close), true);
        observed.delivery_event_count = 3;
        EXPECT_FALSE(evaluate_raw_probe_close(observed, probe->definition, probe->expected_close).has_value());
    }
}

TEST(PeerUpdateClose, EstablishmentAndActualFreshUpdateAreRequiredInBothDrafts) {
    const auto check = [](const auto& probe, Bytes opening, Bytes malformed_ok) {
        const auto& definition = probe.definition;
        ASSERT_EQ(definition.writes.size(), 2u);
        EXPECT_EQ(definition.writes[0].bytes, bytes({7, 0, 1, 0}));
        EXPECT_EQ(definition.writes[1].bytes, malformed_ok);
        EXPECT_EQ(definition.writes[1].reuse_write_stream, 0u);
        ASSERT_TRUE(definition.writes[1].peer_response_ready);
        const auto& gate = definition.writes[1].peer_response_ready;
        const auto update = bytes({2, 0, 2, 2, 0});
        EXPECT_TRUE(gate(update));
        EXPECT_FALSE(gate(bytes({2, 0, 2, 0, 0}))); // ID0 already opened PUBLISH.
        EXPECT_FALSE(gate(bytes({2, 0, 2, 1, 0}))); // Server parity.
        EXPECT_FALSE(gate(bytes({2, 0, 2, 2}))); // Truncated frame.
        EXPECT_FALSE(gate(bytes({7, 0, 1, 0}))); // Wrong message.
        auto other_id = opening;
        other_id[3] = std::byte{2};
        EXPECT_FALSE(definition.peer_request_ready(other_id));
        RawProbeTranscript observed;
        observed.scenario_id = definition.id;
        observed.setup = {{RawProbeChannel::NewUni, bytes({0xaf, 0, 0, 0}), false}, 3, 4, false};
        observed.writes = {{definition.writes[0], 4, 4, false, 3},
                           {definition.writes[1], 4, malformed_ok.size(), false, 5}};
        observed.complete = observed.stimulus_delivered = true;
        observed.transport_established = observed.peer_setup_received = true;
        observed.delivery_event_count = 5;
        observed.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 0}), false},
            transport::StreamDataEvent{4, opening, false},
            transport::StreamDataEvent{4, bytes({2, 0}), false},
            transport::StreamDataEvent{4, bytes({2, 2, 0}), false},
            transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}}};
        const auto valid = observed;
        const auto evaluate = [&] { return evaluate_raw_probe_close(observed, definition, 3); };
        EXPECT_EQ(evaluate(), true);
        observed.writes[0].delivery_event_count = 4; // First fragment preceded establishment.
        EXPECT_FALSE(evaluate().has_value());
        observed = valid;
        observed.writes[1].stream_id = 8;
        EXPECT_FALSE(evaluate().has_value());
        observed = valid;
        std::get<transport::StreamDataEvent>(observed.events[4]).stream_id = 8;
        EXPECT_FALSE(evaluate().has_value());
        observed = valid;
        observed.writes[1].delivery_event_count = 4; // Final response before full update.
        EXPECT_FALSE(evaluate().has_value());
        observed = valid;
        observed.writes[0].delivery_event_count.reset();
        EXPECT_FALSE(evaluate().has_value());
        observed = valid;
        observed.writes[1].accepted--;
        EXPECT_FALSE(evaluate().has_value());
        observed = valid;
        std::get<transport::PeerCloseEvent>(observed.events.back()).error_code = 4;
        EXPECT_EQ(evaluate(), false);
    };
    for (const auto& probe : draft18_peer_close_probes()) {
        if (probe.definition.id == "receive-request-update-ok-with-track-properties")
            check(probe, bytes({0x1d, 0, 8, 0, 1, 1, 'n', 1, 'x', 0, 0}), bytes({7, 0, 3, 0, 0x22, 1}));
    }
    for (const auto& probe : draft21_peer_close_probes()) {
        if (probe.definition.id == "d21-publish-update-ok-with-track-properties")
            check(probe, client_request(true), bytes({7, 0, 3, 0, 4, 1}));
    }
}

}  // namespace
}  // namespace moq::interop::scenarios
