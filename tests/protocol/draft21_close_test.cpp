#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/draft21/successful_response.h"
#include "moq/interop/app/scenario_registry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <set>
#include <map>

namespace moq::interop::scenarios {
namespace {

std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

TEST(Draft21CloseProbes, DefinesIndependentReceiverStimuli) {
    const auto probes = draft21_close_probes();
    ASSERT_GE(probes.size(), 20u);
    std::set<std::string> ids;
    for (const auto& probe : probes) {
        EXPECT_TRUE(ids.insert(probe.definition.id).second);
        EXPECT_TRUE(app::executable_scenario(21, probe.definition.id));
        EXPECT_EQ(app::scenario_requires_track(21, probe.definition.id),
                  probe.definition.id == "d21-publish-state-notify-on-fetch" ||
                  probe.definition.id == "d21-subscriber-sends-publish-state-notify" ||
                  probe.definition.id == "d21-group-order-in-subscription-update" ||
                  probe.definition.id == "d21-duplicate-request-update-id" ||
                  probe.definition.id == "d21-publish-established-subscriber-sends-publish-state-notify");
        EXPECT_FALSE(probe.requirement_id.empty());
        EXPECT_FALSE(probe.evaluator_id.empty());
        EXPECT_TRUE(probe.definition.peer_setup_ready);
        EXPECT_FALSE(probe.definition.setup_bytes.empty());
        EXPECT_EQ(probe.definition.writes.empty(),
                  !probe.definition.start_after_peer_setup);
    }
}

TEST(Draft21CloseProbes, NamespaceAndParameterViolationsHaveLiteralDraft21Bytes) {
    const auto probes = draft21_close_probes();
    const auto find = [&](const char* id) -> const Draft21CloseProbe* {
        const auto found = std::find_if(probes.begin(), probes.end(),
            [&](const auto& probe) { return probe.definition.id == id; });
        return found == probes.end() ? nullptr : &*found;
    };
    // Draft 21 sections 8.7, 9.6 and 9.20.19. Frames are literal fixtures,
    // not the production encoder's output.
    const auto* empty = find("d21-subscribe-empty-namespace-field");
    ASSERT_NE(empty, nullptr);
    ASSERT_EQ(empty->definition.writes.size(), 1u);
    EXPECT_EQ(empty->definition.writes[0].bytes,
              bytes({0x03, 0x00, 0x06, 0x01, 0x01, 0x00, 0x01, 'x', 0x00}));
    EXPECT_EQ(empty->expected_close, 3u);
    const auto* forward = find("d21-forward-value-two");
    ASSERT_NE(forward, nullptr);
    EXPECT_EQ(forward->definition.writes[0].bytes,
              bytes({0x03, 0x00, 0x07, 0x01, 0x00, 0x01, 'x', 0x01, 0x10, 0x02}));
    const auto* parity = find("d21-request-id-wrong-sender-parity");
    ASSERT_NE(parity, nullptr);
    EXPECT_EQ(parity->definition.writes[0].bytes,
              bytes({0x50, 0x00, 0x03, 0x00, 0x00, 0x00}));
    EXPECT_EQ(parity->expected_close, 4u);
}

TEST(Draft21CloseProbes, DuplicateRequestIdAcrossStreamsFirstRequestNeedsNoPublisherNamespace) {
    const auto probes = draft21_close_probes();
    const auto found = std::find_if(probes.begin(), probes.end(), [](const auto& probe) {
        return probe.definition.id == "d21-duplicate-request-id-across-streams";
    });
    ASSERT_NE(found, probes.end());
    ASSERT_EQ(found->definition.writes.size(), 2u);
    // Section 6.4.2.1: the second request reuses Request ID 1 on another
    // stream. The first request is a SUBSCRIBE_NAMESPACE with zero Track
    // Namespace fields (Section 4.1: all namespaces), so no publisher has a
    // reason to refuse it and end the session before the duplicate arrives; a
    // refused request (an unknown namespace prefix) made moqxr close with
    // NO_ERROR, which cannot be told apart from ignoring the duplicate.
    EXPECT_EQ(found->definition.writes[0].channel, RawProbeChannel::NewBidi);
    EXPECT_EQ(found->definition.writes[0].bytes, bytes({0x50, 0x00, 0x03, 0x01, 0x00, 0x00}));
    EXPECT_EQ(found->definition.writes[1].channel, RawProbeChannel::NewBidi);
    EXPECT_EQ(found->definition.writes[1].bytes,
              bytes({0x50, 0x00, 0x05, 0x01, 0x01, 0x01, 'm', 0x00}));
    EXPECT_EQ(found->expected_close, 4u);
}

TEST(Draft21CloseProbes, SetupReaderRetainsFragmentedInputAndRejectsOtherStreams) {
    const auto probes = draft21_close_probes();
    ASSERT_FALSE(probes.empty());
    const auto& ready = probes.front().definition.peer_setup_ready;
    EXPECT_FALSE(ready(bytes({0xaf})));
    EXPECT_FALSE(ready(bytes({0xaf, 0x00, 0x00})));
    EXPECT_TRUE(ready(bytes({0xaf, 0x00, 0x00, 0x00})));
    EXPECT_FALSE(ready(bytes({0x50, 0x00, 0x00, 0x00})));
}

TEST(Draft21CloseProbes, ParameterOverflowAndUndecodableTokenHaveLiteralRequestBytes) {
    const auto probes = draft21_close_probes();
    const auto find = [&](const char* id) -> const Draft21CloseProbe* {
        const auto found = std::find_if(probes.begin(), probes.end(),
            [&](const auto& probe) { return probe.definition.id == id; });
        return found == probes.end() ? nullptr : &*found;
    };
    // Section 9.20: known OBJECT_DELIVERY_TIMEOUT Type 2 precedes the
    // UINT64_MAX delta, so overflow is reached before an unknown Type.
    const auto* overflow = find("d21-parameter-type-delta-overflow");
    ASSERT_NE(overflow, nullptr);
    EXPECT_EQ(overflow->requirement_id, "D21-9-20-MUST-396");
    EXPECT_EQ(overflow->evaluator_id,
              "d21-parameter-type-overflow-protocol-violation");
    ASSERT_EQ(overflow->definition.writes.size(), 1u);
    EXPECT_EQ(overflow->definition.writes[0].channel, RawProbeChannel::NewBidi);
    EXPECT_EQ(overflow->definition.writes[0].bytes,
              bytes({0x03, 0x00, 0x10, 0x01, 0x00, 0x01, 'x', 0x02,
                     0x02, 0x00, 0xff, 0xff, 0xff, 0xff, 0xff,
                     0xff, 0xff, 0xff, 0xff}));
    EXPECT_EQ(overflow->expected_close, 3u);

    // Section 8.9 Figure 3: an AUTHORIZATION_TOKEN value containing only
    // USE_VALUE Alias Type 3 omits the required Token Type.
    const auto* token = find("d21-request-undecodable-authorization-token");
    ASSERT_NE(token, nullptr);
    EXPECT_EQ(token->requirement_id, "D21-8-9-MUST-267");
    EXPECT_EQ(token->evaluator_id, "d21-token-decode-key-value-formatting-error");
    ASSERT_EQ(token->definition.writes.size(), 1u);
    EXPECT_EQ(token->definition.writes[0].channel, RawProbeChannel::NewBidi);
    EXPECT_EQ(token->definition.writes[0].bytes,
              bytes({0x03, 0x00, 0x08, 0x01, 0x00, 0x01, 'x', 0x01,
                     0x03, 0x01, 0x03}));
    EXPECT_EQ(token->expected_close, 6u);
}

TEST(Draft21CloseProbes, UnknownDatagramUsesIndependentNonObjectNonPaddingType) {
    const auto probes = draft21_close_probes();
    const auto found = std::find_if(probes.begin(), probes.end(),
        [](const auto& probe) {
            return probe.definition.id == "d21-unknown-datagram-type";
        });
    ASSERT_NE(found, probes.end());
    EXPECT_EQ(found->requirement_id, "D21-11-MUST-503");
    EXPECT_EQ(found->evaluator_id, "d21-unknown-datagram-session-close");
    // Section 11 does not mandate a particular application close code.
    EXPECT_FALSE(found->expected_close.has_value());
    ASSERT_EQ(found->definition.writes.size(), 1u);
    EXPECT_EQ(found->definition.writes[0].channel, RawProbeChannel::Datagram);
    EXPECT_FALSE(found->definition.writes[0].fin);
    // Section 8.1 encoding of 0x132b3e2a, distinct from padding 0x132b3e29
    // and the single-byte OBJECT_DATAGRAM flags in Section 11.2.1.
    EXPECT_EQ(found->definition.writes[0].bytes,
              bytes({0xf0, 0x13, 0x2b, 0x3e, 0x2a}));
}

TEST(Draft21CloseProbes, ForbiddenFillParametersHaveLiteralNestedScopeBytes) {
    // Draft 21 sections 9.6, 9.20 and 9.20.16 Table 6. The length-bounded
    // nested scope starts its Type Deltas at zero; it has no count field.
    struct Fixture {
        const char* scenario;
        std::vector<std::byte> frame;
    };
    const Fixture fixtures[]{
        {"d21-fill-forbidden-nested-authorization",
         bytes({0x03, 0x00, 0x0b, 0x01, 0x00, 0x01, 'x', 0x01,
                0x23, 0x04, 0x03, 0x02, 0x03, 0x00})},
        {"d21-fill-forbidden-track-property-filter",
         bytes({0x03, 0x00, 0x0c, 0x01, 0x00, 0x01, 'x', 0x01,
                0x23, 0x05, 0x29, 0x03, 0x00, 0x00, 0x00})},
        {"d21-fill-recursive-parameter",
         bytes({0x03, 0x00, 0x09, 0x01, 0x00, 0x01, 'x', 0x01,
                0x23, 0x02, 0x23, 0x00})},
    };
    const auto probes = draft21_close_probes();
    for (const auto& fixture : fixtures) {
        SCOPED_TRACE(fixture.scenario);
        const auto found = std::find_if(probes.begin(), probes.end(),
            [&](const auto& probe) { return probe.definition.id == fixture.scenario; });
        ASSERT_NE(found, probes.end());
        EXPECT_EQ(found->requirement_id, "D21-9-20-16-MUST-447");
        EXPECT_EQ(found->evaluator_id,
                  "d21-fill-parameter-whitelist-protocol-violation");
        EXPECT_EQ(found->expected_close, 3u);
        ASSERT_EQ(found->definition.writes.size(), 1u);
        EXPECT_EQ(found->definition.writes[0].channel, RawProbeChannel::NewBidi);
        EXPECT_FALSE(found->definition.writes[0].fin);
        EXPECT_EQ(found->definition.writes[0].bytes, fixture.frame);
    }
}

TEST(Draft21CloseProbes, TrackStatusUpdateSharesItsRequestStream) {
    // Draft 21 sections 9.5 and 9.13: the update follows TRACK_STATUS
    // on the same request stream.
    const auto probes = draft21_close_probes();
    const auto find = [&](const char* scenario) -> const Draft21CloseProbe* {
        const auto found = std::find_if(probes.begin(), probes.end(),
            [&](const auto& probe) { return probe.definition.id == scenario; });
        return found == probes.end() ? nullptr : &*found;
    };
    const auto* update = find("d21-update-on-track-status");
    ASSERT_NE(update, nullptr);
    EXPECT_EQ(update->requirement_id, "D21-9-5-MUST-344");
    EXPECT_EQ(update->evaluator_id, "d21-request-update-context-and-direction");
    EXPECT_EQ(update->expected_close, 3u);
    ASSERT_EQ(update->definition.writes.size(), 1u);
    EXPECT_EQ(update->definition.writes[0].channel, RawProbeChannel::NewBidi);
    EXPECT_EQ(update->definition.writes[0].bytes,
              bytes({0x0d, 0, 5, 1, 0, 1, 'x', 0, 2, 0, 2, 3, 0}));
}

TEST(Draft21CloseProbes, TokenCacheViolationsUseLiteralTokensAndNegotiatedLimits) {
    // Draft 21 sections 8.9 and 9.1.3: an empty REGISTER costs 16 bytes,
    // while a one-byte Token Value costs 17. Unknown/unauthorized Tokens
    // still register if their containing message causes no session error.
    const auto probes = draft21_close_probes();
    const auto find = [&](const char* scenario) -> const Draft21CloseProbe* {
        const auto found = std::find_if(probes.begin(), probes.end(),
            [&](const auto& probe) { return probe.definition.id == scenario; });
        return found == probes.end() ? nullptr : &*found;
    };
    const auto* duplicate = find("d21-token-duplicate-registration");
    ASSERT_NE(duplicate, nullptr);
    EXPECT_EQ(duplicate->requirement_id, "D21-8-9-MUST-268");
    EXPECT_EQ(duplicate->evaluator_id, "d21-duplicate-token-alias-session-error");
    EXPECT_EQ(duplicate->expected_close, 0x14u);
    ASSERT_EQ(duplicate->definition.writes.size(), 2u);
    EXPECT_EQ(duplicate->definition.writes[0].bytes,
              bytes({3, 0, 10, 1, 0, 1, 'x', 1, 3, 3, 1, 0, 0}));
    EXPECT_EQ(duplicate->definition.writes[1].bytes,
              bytes({3, 0, 10, 3, 0, 1, 'x', 1, 3, 3, 1, 0, 0}));
    for (const auto& write : duplicate->definition.writes) {
        EXPECT_EQ(write.channel, RawProbeChannel::NewBidi);
        EXPECT_FALSE(write.fin);
    }
    EXPECT_FALSE(duplicate->definition.peer_setup_ready(bytes({0xaf, 0, 0, 0})));
    EXPECT_FALSE(duplicate->definition.peer_setup_ready(bytes({0xaf, 0, 0, 2, 4, 31})));
    EXPECT_TRUE(duplicate->definition.peer_setup_ready(bytes({0xaf, 0, 0, 2, 4, 32})));
    const auto* overflow = find("d21-request-token-cache-overflow");
    ASSERT_NE(overflow, nullptr);
    EXPECT_EQ(overflow->requirement_id, "D21-8-9-MUST-277");
    EXPECT_EQ(overflow->evaluator_id, "d21-token-cache-overflow-session-error");
    EXPECT_EQ(overflow->expected_close, 0x13u);
    ASSERT_EQ(overflow->definition.writes.size(), 1u);
    EXPECT_EQ(overflow->definition.writes[0].bytes,
              bytes({3, 0, 11, 1, 0, 1, 'x', 1, 3, 4, 1, 0, 0, 'x'}));
    EXPECT_TRUE(overflow->definition.peer_setup_ready(bytes({0xaf, 0, 0, 0})));
    EXPECT_TRUE(overflow->definition.peer_setup_ready(bytes({0xaf, 0, 0, 2, 4, 16})));
    EXPECT_FALSE(overflow->definition.peer_setup_ready(bytes({0xaf, 0, 0, 2, 4, 17})));
}

TEST(Draft21CloseProbes, CacheCloseProofRequiresCompleteStimulusAndPeerCapacity) {
    // Draft 21 sections 8.9 and 9.1.3: optional peer limits are proven
    // from recorded SETUP bytes, including the omitted-option default.
    const auto probes = draft21_close_probes();
    const auto check = [&](const char* scenario, unsigned sufficient_capacity,
                           unsigned unsuitable_capacity, unsigned close_code) {
        SCOPED_TRACE(scenario);
        const auto found = std::find_if(probes.begin(), probes.end(),
            [&](const auto& probe) { return probe.definition.id == scenario; });
        ASSERT_NE(found, probes.end());
        RawProbeTranscript observed;
        observed.scenario_id = found->definition.id;
        observed.setup = {{RawProbeChannel::NewUni, found->definition.setup_bytes, false},
                          3, found->definition.setup_bytes.size(), false};
        for (std::size_t index = 0; index < found->definition.writes.size(); ++index) {
            const auto& write = found->definition.writes[index];
            observed.writes.push_back({write, 1 + index * 4, write.bytes.size(), false});
        }
        observed.stimulus_delivered = observed.complete = true;
        observed.transport_established = observed.peer_setup_received = true;
        observed.delivery_event_count = 2;
        observed.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 2, 4, sufficient_capacity}), false},
            transport::PeerCloseEvent{transport::CloseErrorSpace::Application, close_code, {}}};
        EXPECT_EQ(evaluate_raw_probe_close(observed, found->definition, found->expected_close), true);
        --observed.writes.back().accepted;
        EXPECT_FALSE(evaluate_raw_probe_close(observed, found->definition, found->expected_close).has_value());
        ++observed.writes.back().accepted;
        auto& peer_setup = std::get<transport::StreamDataEvent>(observed.events[1]);
        peer_setup.data = bytes({0xaf, 0, 0, 2, 4, unsuitable_capacity});
        EXPECT_FALSE(evaluate_raw_probe_close(observed, found->definition, found->expected_close).has_value());
        peer_setup.data = bytes({0xaf, 0, 0, 0});
        const auto default_result = evaluate_raw_probe_close(observed, found->definition, found->expected_close);
        if (close_code == 0x13) EXPECT_EQ(default_result, true);
        else EXPECT_FALSE(default_result.has_value());
    };
    check("d21-token-duplicate-registration", 32, 31, 0x14);
    check("d21-request-token-cache-overflow", 16, 17, 0x13);
}

struct MalformedFixture {
    const char* scenario;
    const char* requirement;
    const char* evaluator;
    std::vector<std::byte> payload;
    bool fin{false};
    std::optional<std::uint64_t> close{3};
};

std::vector<MalformedFixture> independent_malformed_fixtures() {
    // Sections 9, 9.6 and 9.20: each nested scope restarts Type Delta
    // at zero. UINT64_MAX occupies nine ff octets in the draft's vi64.
    return {
        {"d21-unknown-request-stream-message", "D21-9-MUST-284",
         "d21-unknown-message-session-close",
         bytes({3, 0, 5, 1, 0, 1, 'x', 0, 0x7e, 0, 0}), false, std::nullopt},
        {"d21-request-message-truncated-at-fin", "D21-9-MUST-285",
         "d21-message-body-length-protocol-violation",
         bytes({3, 0, 7, 1, 0, 1, 'x', 1, 0x10}), true},
        {"d21-group-order-above-two", "D21-9-20-9-MUST-429",
         "d21-group-order-bounds-protocol-violation",
         bytes({3, 0, 7, 1, 0, 1, 'x', 1, 0x22, 3})},
        {"d21-fill-invalid-group-order", "D21-9-20-9-MUST-429",
         "d21-group-order-bounds-protocol-violation",
         bytes({3, 0, 9, 1, 0, 1, 'x', 1, 0x23, 2, 0x22, 3})},
        {"d21-fill-location-filter-end-group-overflow", "D21-9-20-10-MUST-432",
         "d21-location-filter-overflow-protocol-violation",
         bytes({3, 0, 20, 1, 0, 1, 'x', 1, 0x23, 13, 0x21, 11,
                0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0, 1})},
        {"d21-forward-value-255", "D21-9-20-19-MUST-460",
         "d21-forward-bounds-protocol-violation",
         bytes({3, 0, 7, 1, 0, 1, 'x', 1, 0x10, 255})},
        {"d21-include-properties-value-255", "D21-9-20-22-MUST-475",
         "d21-include-properties-bounds-protocol-violation",
         bytes({3, 0, 7, 1, 0, 1, 'x', 1, 0x35, 255})},
        {"d21-request-alias-registration-with-default-zero-cache", "D21-8-9-MUST-277",
         "d21-token-cache-overflow-session-error",
         bytes({3, 0, 11, 1, 0, 1, 'x', 1, 3, 4, 1, 0, 0x80, 0x9d}), false, 0x13},
        {"d21-fill-timeout-outside-fill-or-fetch", "D21-9-20-1-MUST-404",
         "d21-out-of-scope-parameter-protocol-violation",
         bytes({3, 0, 7, 1, 0, 1, 'x', 1, 0x0a, 0})},
    };
}

TEST(Draft21CloseProbes, MissingMalformedVariantsHaveIndependentLiteralWireOperations) {
    const auto probes = draft21_close_probes();
    for (const auto& fixture : independent_malformed_fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto found = std::find_if(probes.begin(), probes.end(),
            [&](const auto& probe) { return probe.definition.id == fixture.scenario; });
        EXPECT_NE(found, probes.end());
        if (found == probes.end()) continue;
        EXPECT_EQ(found->requirement_id, fixture.requirement);
        EXPECT_EQ(found->evaluator_id, fixture.evaluator);
        EXPECT_EQ(found->expected_close, fixture.close);
        ASSERT_EQ(found->definition.writes.size(), 1u);
        EXPECT_EQ(found->definition.writes.front().channel, RawProbeChannel::NewBidi);
        EXPECT_EQ(found->definition.writes.front().bytes, fixture.payload);
        EXPECT_EQ(found->definition.writes.front().fin, fixture.fin);
    }
}

TEST(Draft21CloseProbes, IndependentMalformedProofRejectsIncompleteWritesAndEarlyClose) {
    const auto probes = draft21_close_probes();
    for (const auto& fixture : independent_malformed_fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto found = std::find_if(probes.begin(), probes.end(),
            [&](const auto& probe) { return probe.definition.id == fixture.scenario; });
        EXPECT_NE(found, probes.end());
        if (found == probes.end()) continue;
        RawProbeTranscript observed;
        observed.scenario_id = fixture.scenario;
        observed.setup = {{RawProbeChannel::NewUni, bytes({0xaf, 0, 0, 0}), false},
                          3, 4, false};
        observed.writes = {{{RawProbeChannel::NewBidi, fixture.payload, fixture.fin},
                            1, fixture.payload.size(), fixture.fin}};
        observed.stimulus_delivered = observed.complete = true;
        observed.transport_established = observed.peer_setup_received = true;
        observed.delivery_event_count = 2;
        observed.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 0}), false},
            transport::PeerCloseEvent{transport::CloseErrorSpace::Application,
                                      fixture.close.value_or(9), {}}};
        const auto evaluate = [&] {
            return evaluate_raw_probe_close(observed, found->definition, found->expected_close);
        };
        EXPECT_EQ(evaluate(), true);
        if (fixture.close == 0x13) {
            auto& setup = std::get<transport::StreamDataEvent>(observed.events[1]);
            setup.data = bytes({0xaf, 0, 0, 2, 4, 0});
            EXPECT_FALSE(evaluate().has_value());
            setup.data = bytes({0xaf, 0, 0, 0});
        }
        --observed.writes.front().accepted;
        EXPECT_FALSE(evaluate().has_value());
        ++observed.writes.front().accepted;
        if (fixture.fin) {
            observed.writes.front().fin_accepted = false;
            EXPECT_FALSE(evaluate().has_value());
            observed.writes.front().fin_accepted = true;
        }
        observed.delivery_event_count = 3;
        EXPECT_FALSE(evaluate().has_value());
        observed.delivery_event_count = 2;
        auto& closed = std::get<transport::PeerCloseEvent>(observed.events.back());
        closed.error_space = transport::CloseErrorSpace::Transport;
        EXPECT_FALSE(evaluate().has_value());
        closed.error_space = transport::CloseErrorSpace::Application;
        closed.error_code = 2;
        EXPECT_EQ(evaluate(), fixture.close ? false : true);
    }
}

TEST(Draft21CloseProbes, DefaultZeroCacheRegistrationRequiresOmittedPeerOption) {
    const auto probes = draft21_close_probes();
    const auto found = std::find_if(probes.begin(), probes.end(), [](const auto& probe) {
        return probe.definition.id == "d21-request-alias-registration-with-default-zero-cache";
    });
    ASSERT_NE(found, probes.end());
    const auto& ready = found->definition.peer_setup_ready;
    EXPECT_TRUE(ready(bytes({0xaf, 0, 0, 0})));
    EXPECT_TRUE(ready(bytes({0xaf, 0, 0, 2, 6, 1})));
    EXPECT_FALSE(ready(bytes({0xaf, 0, 0, 2, 4, 0})));
    EXPECT_FALSE(ready(bytes({0xaf, 0, 0, 2, 4, 16})));
    EXPECT_FALSE(ready(bytes({0xaf, 0, 0})));
}

using wire::draft21::ResponseContext;
using wire::draft21::SuccessfulResponse;

TEST(Draft21SuccessfulResponse, DecodesTypedParametersAndLeavesNextFrameUnread) {
    // Sections 9.7 and 9.20.17-18: odd Parameter9 is a Location,
    // with two vi64 values; it is not an odd Key-Value length/value.
    auto input = bytes({4, 0, 12, 0x80, 0x81, 2, 8, 0x80, 0x82, 1, 3, 0x80, 0x84, 4, 1});
    input.insert(input.end(), {std::byte{7}, std::byte{0}, std::byte{1}, std::byte{0}});
    wire::Cursor cursor(input, 20);
    const auto decoded = wire::draft21::decode_successful_response(cursor, ResponseContext::Subscribe);
    ASSERT_TRUE(std::holds_alternative<SuccessfulResponse>(decoded));
    const auto& response = std::get<SuccessfulResponse>(decoded);
    EXPECT_EQ(response.track_alias, 129u);
    ASSERT_EQ(response.parameters.size(), 2u);
    EXPECT_EQ(response.parameters[0].type, 8u);
    EXPECT_EQ(std::get<std::uint64_t>(response.parameters[0].value), 130u);
    EXPECT_EQ(response.parameters[1].type, 9u);
    const auto location = std::get<wire::draft21::Location>(response.parameters[1].value);
    EXPECT_EQ(location.group, 3u);
    EXPECT_EQ(location.object, 132u);
    ASSERT_EQ(response.track_properties.size(), 1u);
    EXPECT_EQ(response.track_properties[0].type, 4u);
    EXPECT_EQ(cursor.remaining(), 4u);
    EXPECT_EQ(cursor.offset(), 35u);
}

TEST(Draft21SuccessfulResponse, AcceptsLegalPropertyValuesImmutableAndUnknownProperties) {
    // Type deltas restart at zero inside Immutable Properties. All Track
    // Properties defined in Table14 are present; unknown odd127 is skipped.
    const auto input = bytes({4, 0, 23, 0, 0, 2, 0, 2, 1, 2, 2,
                              5, 2, 4, 3, 3, 0x80, 0xff, 0x14, 2,
                              0x0e, 1, 0x80, 0x4f, 1, 'z'});
    wire::Cursor cursor(input);
    const auto decoded = wire::draft21::decode_successful_response(cursor, ResponseContext::Subscribe);
    ASSERT_TRUE(std::holds_alternative<SuccessfulResponse>(decoded));
    EXPECT_EQ(std::get<SuccessfulResponse>(decoded).track_properties.size(), 8u);
    EXPECT_EQ(cursor.remaining(), 0u);
    // Nested Immutable Track Properties are not the forbidden nested
    // Immutable *Object* Properties case in Section10.7.
    const auto nested = bytes({4, 0, 8, 0, 0, 0x0b, 4, 0x0b, 2, 4, 1});
    wire::Cursor nested_cursor(nested);
    EXPECT_TRUE(std::holds_alternative<SuccessfulResponse>(
        wire::draft21::decode_successful_response(nested_cursor, ResponseContext::Subscribe)));
}

TEST(Draft21SuccessfulResponse, DistinguishesDiscoveryAndUpdateResponseScopes) {
    for (const auto context : {ResponseContext::SubscribeTracks, ResponseContext::RequestUpdate}) {
        const auto input = bytes({7, 0, 3, 1, 8, 10});
        wire::Cursor cursor(input);
        const auto decoded = wire::draft21::decode_successful_response(cursor, context);
        ASSERT_TRUE(std::holds_alternative<SuccessfulResponse>(decoded));
        EXPECT_FALSE(std::get<SuccessfulResponse>(decoded).track_alias);
        EXPECT_EQ(cursor.remaining(), 0u);
    }
    const auto with_largest = bytes({7, 0, 4, 1, 9, 2, 3});
    wire::Cursor update(with_largest);
    EXPECT_TRUE(std::holds_alternative<SuccessfulResponse>(
        wire::draft21::decode_successful_response(update, ResponseContext::RequestUpdate)));
    wire::Cursor discovery(with_largest);
    EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(
        wire::draft21::decode_successful_response(discovery, ResponseContext::SubscribeTracks)));
    const auto discovery_properties = bytes({7, 0, 3, 0, 4, 1});
    wire::Cursor properties(discovery_properties);
    EXPECT_TRUE(std::holds_alternative<SuccessfulResponse>(
        wire::draft21::decode_successful_response(properties, ResponseContext::SubscribeTracks)));
}

TEST(Draft21SuccessfulResponse, FragmentedFrameRollsBackAndMalformedCompleteBodyIsError) {
    const auto input = bytes({4, 0, 9, 0, 2, 8, 10, 1, 0, 0, 4, 1});
    for (std::size_t length = 0; length < input.size(); ++length) {
        wire::Cursor cursor(std::span{input}.first(length), 12);
        EXPECT_TRUE(std::holds_alternative<wire::NeedMore>(
            wire::draft21::decode_successful_response(cursor, ResponseContext::Subscribe)));
        EXPECT_EQ(cursor.offset(), 12u);
    }
    struct Invalid { ResponseContext context; std::vector<std::byte> input; };
    const Invalid invalid[]{
        {ResponseContext::Subscribe, bytes({7, 0, 1, 0})},
        {ResponseContext::SubscribeTracks, bytes({4, 0, 2, 0, 0})},
        {ResponseContext::Subscribe, bytes({5, 0, 3, 3, 0, 0})},
        {ResponseContext::Subscribe, bytes({4, 0, 0})},
        {ResponseContext::Subscribe, bytes({4, 0, 2, 0, 1})},
        {ResponseContext::Subscribe, bytes({4, 0, 3, 0, 1, 8})},
        {ResponseContext::Subscribe, bytes({4, 0, 4, 0, 1, 9, 0})},
        {ResponseContext::Subscribe, bytes({4, 0, 4, 0, 1, 0x22, 1})},
        {ResponseContext::Subscribe, bytes({4, 0, 3, 0, 0, 4})},
        {ResponseContext::Subscribe, bytes({4, 0, 4, 0, 0, 0x22, 0})},
        {ResponseContext::Subscribe, bytes({4, 0, 4, 0, 0, 0x30, 2})},
        {ResponseContext::Subscribe, bytes({4, 0, 5, 0, 0, 0x0e, 0x81, 0})},
        {ResponseContext::Subscribe, bytes({4, 0, 5, 0, 0, 0x0b, 1, 4})},
        {ResponseContext::RequestUpdate, bytes({7, 0, 3, 0, 4, 1})},
    };
    for (const auto& fixture : invalid) {
        wire::Cursor cursor(fixture.input, 12);
        EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(
            wire::draft21::decode_successful_response(cursor, fixture.context)));
        EXPECT_EQ(cursor.offset(), 12u);
    }
}

struct EstablishedFixture {
    const char* scenario;
    const char* requirement;
    const char* evaluator;
    std::vector<std::byte> initial;
    std::vector<std::byte> update;
    std::vector<std::byte> initial_ok;
    bool duplicate{false};
    std::uint64_t close{3};
};

std::vector<EstablishedFixture> established_fixtures() {
    // Section9.5 has a new RequestID but no original ID on the same stream.
    return {
        {"d21-group-order-in-subscription-update", "D21-9-20-1-MUST-404",
         "d21-out-of-scope-parameter-protocol-violation",
         bytes({3, 0, 5, 1, 0, 1, 'x', 0}), bytes({2, 0, 4, 3, 1, 0x22, 1}),
         bytes({4, 0, 4, 0, 0, 4, 1})},
        {"d21-discovery-update-invalid-forward", "D21-9-20-19-MUST-460",
         "d21-forward-bounds-protocol-violation",
         bytes({0x51, 0, 5, 1, 0, 1, 0x10, 0}), bytes({2, 0, 4, 3, 1, 0x10, 255}),
         bytes({7, 0, 1, 0})},
        {"d21-duplicate-request-update-id", "D21-6-4-2-1-MUST-155",
         "d21-duplicate-invalid-request-id",
         bytes({3, 0, 5, 1, 0, 1, 'x', 0}), bytes({2, 0, 2, 3, 0}),
         bytes({4, 0, 4, 0, 0, 4, 1}), true, 4},
        {"d21-publish-state-notify-on-namespace-request", "D21-9-10-MUST-369",
         "d21-publish-state-notify-request-type-error",
         bytes({0x50, 0, 3, 1, 0, 0}), bytes({0x22, 0, 1, 0}),
         bytes({7, 0, 1, 0})},
        {"d21-subscriber-sends-publish-state-notify", "D21-9-10-MUST-370",
         "d21-publish-state-notify-direction-error",
         bytes({3, 0, 5, 1, 0, 1, 'x', 0}), bytes({0x22, 0, 1, 0}),
         bytes({4, 0, 4, 0, 0, 4, 1})},
        {"d21-publish-state-notify-on-fetch", "D21-9-10-MUST-369",
         "d21-publish-state-notify-request-type-error",
         bytes({0x16, 0, 5, 1, 0, 1, 'x', 0}), bytes({0x22, 0, 1, 0}),
         bytes({0x18, 0, 4, 0, 0, 0, 0})},
    };
}

const Draft21CloseProbe* established_probe(const std::vector<Draft21CloseProbe>& probes,
                                         const char* scenario) {
    const auto found = std::find_if(probes.begin(), probes.end(),
        [&](const auto& probe) { return probe.definition.id == scenario; });
    return found == probes.end() ? nullptr : &*found;
}

TEST(Draft21CloseProbes, NamespaceNotifyRequiresEstablishedRequestAndLiteralNotification) {
    const auto probes = draft21_close_probes();
    const auto* probe = established_probe(probes, "d21-publish-state-notify-on-namespace-request");
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(probe->requirement_id, "D21-9-10-MUST-369");
    EXPECT_EQ(probe->evaluator_id, "d21-publish-state-notify-request-type-error");
    EXPECT_EQ(probe->expected_close, 3u);
    ASSERT_EQ(probe->definition.writes.size(), 2u);
    EXPECT_EQ(probe->definition.writes[0].bytes, bytes({0x50, 0, 3, 1, 0, 0}));
    EXPECT_FALSE(probe->definition.writes[0].fin);
    const auto& notify = probe->definition.writes[1];
    EXPECT_EQ(notify.bytes, bytes({0x22, 0, 1, 0}));
    EXPECT_EQ(notify.reuse_write_stream, 0u);
    ASSERT_TRUE(notify.peer_response_ready);
    EXPECT_TRUE(notify.peer_response_ready(bytes({7, 0, 1, 0})));
    EXPECT_TRUE(notify.peer_response_ready(bytes({7, 0, 3, 1, 8, 10})));
    EXPECT_FALSE(notify.peer_response_ready(bytes({7, 0, 3, 0, 4, 1})));
    EXPECT_FALSE(notify.peer_response_ready(bytes({0x18, 0, 4, 0, 0, 0, 0})));
}

TEST(Draft21CloseProbes, EstablishedUpdateCasesUseLiteralFramesAndPriorSuccessGates) {
    const auto probes = draft21_close_probes();
    for (const auto& fixture : established_fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto* probe = established_probe(probes, fixture.scenario);
        ASSERT_NE(probe, nullptr);
        EXPECT_EQ(probe->requirement_id, fixture.requirement);
        EXPECT_EQ(probe->evaluator_id, fixture.evaluator);
        EXPECT_EQ(probe->expected_close, fixture.close);
        ASSERT_EQ(probe->definition.writes.size(), fixture.duplicate ? 3u : 2u);
        EXPECT_EQ(probe->definition.writes[0].bytes, fixture.initial);
        for (std::size_t index = 1; index < probe->definition.writes.size(); ++index) {
            const auto& write = probe->definition.writes[index];
            EXPECT_EQ(write.channel, RawProbeChannel::NewBidi);
            EXPECT_FALSE(write.fin);
            EXPECT_EQ(write.bytes, fixture.update);
            EXPECT_EQ(write.reuse_write_stream, index - 1);
            ASSERT_TRUE(write.peer_response_ready);
            const auto response = index == 1 ? fixture.initial_ok : bytes({7, 0, 1, 0});
            EXPECT_TRUE(write.peer_response_ready(response));
            for (std::size_t length = 0; length < response.size(); ++length)
                EXPECT_FALSE(write.peer_response_ready(std::span{response}.first(length)));
            EXPECT_FALSE(write.peer_response_ready(bytes({5, 0, 3, 3, 0, 0})));
            auto coalesced = response;
            coalesced.insert(coalesced.end(), response.begin(), response.end());
            EXPECT_FALSE(write.peer_response_ready(coalesced));
        }
    }
}

TEST(Draft21SuccessfulResponse, MandatoryAndImmutableInvalidPropertiesCannotEstablishRequest) {
    // Section3.6: no extension capability is offered by these probes, so
    // unknown mandatory Track Properties cause cancellation, not establishment.
    const std::vector<std::byte> invalid[]{
        bytes({4, 0, 6, 0, 0, 0xc0, 0x40, 0, 0}),
        bytes({4, 0, 8, 0, 0, 0x0b, 4, 0xc0, 0x40, 0, 0}),
        bytes({4, 0, 6, 0, 0, 0x0b, 2, 0x22, 3}),
        bytes({4, 0, 6, 0, 0, 0x0b, 2, 0x30, 2}),
        bytes({4, 0, 4, 0, 0, 0x3c, 0}),
        bytes({4, 0, 13, 0, 2, 8, 0, 0xff, 0xff, 0xff,
               0xff, 0xff, 0xff, 0xff, 0xff, 0xff}),
    };
    for (const auto& input : invalid) {
        wire::Cursor cursor(input);
        EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(
            wire::draft21::decode_successful_response(cursor, ResponseContext::Subscribe)));
        EXPECT_EQ(cursor.offset(), 0u);
    }
    const auto unknown_optional = bytes({4, 0, 5, 0, 0, 0xb8, 0, 0});
    wire::Cursor cursor(unknown_optional);
    EXPECT_TRUE(std::holds_alternative<SuccessfulResponse>(
        wire::draft21::decode_successful_response(cursor, ResponseContext::Subscribe)));
}

RawProbeTranscript established_transcript(const EstablishedFixture& fixture) {
    RawProbeTranscript result;
    result.scenario_id = fixture.scenario;
    result.setup = {{RawProbeChannel::NewUni, bytes({0xaf, 0, 0, 0}), false}, 3, 4, false};
    result.transport_established = result.peer_setup_received = true;
    result.stimulus_delivered = result.complete = true;
    // A literal negotiated update capacity of one demonstrates that the
    // duplicate-ID case retires the first update before sending the second.
    result.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 2, 8, 1}), false},
        transport::StreamDataEvent{1, fixture.initial_ok, false}};
    result.writes = {
        {{RawProbeChannel::NewBidi, fixture.initial, false}, 1, fixture.initial.size(), false, 2},
        {{RawProbeChannel::NewBidi, fixture.update, false, 0, [](auto) { return true; }}, 1, fixture.update.size(), false, 3}};
    if (fixture.duplicate) {
        result.events.push_back(transport::StreamDataEvent{1, bytes({7, 0, 1, 0}), false});
        result.writes.push_back({{RawProbeChannel::NewBidi, fixture.update, false, 1, [](auto) { return true; }},
                                  1, fixture.update.size(), false, 4});
    }
    result.delivery_event_count = result.events.size();
    result.events.push_back(transport::PeerCloseEvent{
        transport::CloseErrorSpace::Application, fixture.close, {}});
    return result;
}

TEST(Draft21CloseProbes, EstablishedProofRejectsMissingEarlyUnrelatedAndRejectedInitialResponses) {
    const auto probes = draft21_close_probes();
    for (const auto& fixture : established_fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto* probe = established_probe(probes, fixture.scenario);
        ASSERT_NE(probe, nullptr);
        const auto valid = established_transcript(fixture);
        const auto evaluate = [&](const RawProbeTranscript& observed) {
            return evaluate_raw_probe_close(observed, probe->definition, probe->expected_close);
        };
        EXPECT_EQ(evaluate(valid), true);
        for (std::size_t stage = 0; stage < valid.writes.size(); ++stage) {
            auto changed = valid;
            changed.writes[stage].delivery_event_count.reset();
            EXPECT_FALSE(evaluate(changed).has_value());
            changed = valid;
            --changed.writes[stage].accepted;
            EXPECT_FALSE(evaluate(changed).has_value());
            changed = valid;
            changed.writes[stage].stream_id = 5;
            EXPECT_FALSE(evaluate(changed).has_value());
        }
        for (std::size_t event = 2; event < *valid.delivery_event_count; ++event) {
            auto changed = valid;
            auto& response = std::get<transport::StreamDataEvent>(changed.events[event]);
            response.stream_id = 5;
            EXPECT_FALSE(evaluate(changed).has_value());
            changed = valid;
            std::get<transport::StreamDataEvent>(changed.events[event]).data.pop_back();
            EXPECT_FALSE(evaluate(changed).has_value());
            changed = valid;
            std::get<transport::StreamDataEvent>(changed.events[event]).fin = true;
            EXPECT_FALSE(evaluate(changed).has_value());
            changed = valid;
            std::get<transport::StreamDataEvent>(changed.events[event]).data = bytes({5, 0, 3, 3, 0, 0});
            EXPECT_FALSE(evaluate(changed).has_value());
            changed = valid;
            ++*changed.writes[event - 2].delivery_event_count;
            EXPECT_FALSE(evaluate(changed).has_value());
        }
        for (const auto& cancellation : std::vector<transport::TransportEvent>{
                 transport::PeerResetEvent{1, 0},
                 transport::PeerStopSendingEvent{1, 0}}) {
            auto changed = valid;
            changed.events[2] = cancellation;
            EXPECT_FALSE(evaluate(changed).has_value());
        }
        auto changed = valid;
        changed.delivery_event_count = valid.events.size();
        EXPECT_FALSE(evaluate(changed).has_value());
        changed = valid;
        std::get<transport::PeerCloseEvent>(changed.events.back()).error_code = 9;
        EXPECT_EQ(evaluate(changed), false);
    }
}

class EstablishedTransport final : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override {
        ++opened_bidi;
        return {transport::TransportStatus::Success, 1};
    }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, 3}; }
    transport::OperationResult write(transport::StreamId stream,
                                     std::span<const std::byte> data, bool) override {
        if (stream == 1 && block_request) {
            if (output[stream].empty() && !data.empty()) {
                output[stream].push_back(data.front());
                return {transport::TransportStatus::Partial, 1, {}};
            }
            return {transport::TransportStatus::WouldBlock, 0, {}};
        }
        if ((stream == 0 || stream == 1) && write_budget) {
            const auto count = std::min(data.size(), *write_budget);
            output[stream].insert(output[stream].end(), data.begin(), data.begin() + count);
            *write_budget -= count;
            return {count == 0 ? transport::TransportStatus::WouldBlock
                    : count == data.size() ? transport::TransportStatus::Success
                                          : transport::TransportStatus::Partial,
                    count, {}};
        }
        output[stream].insert(output[stream].end(), data.begin(), data.end());
        return {transport::TransportStatus::Success, data.size(), {}};
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
    std::vector<transport::TransportEvent> events;
    std::map<transport::StreamId, std::vector<std::byte>> output;
    std::size_t opened_bidi{0};
    bool block_request{false};
    std::optional<std::size_t> write_budget;
};

TEST(Draft21CloseProbes, NotifyProofWaitsForCompleteNotificationAcceptanceAfterTypedAck) {
    const auto probes = draft21_close_probes();
    for (const auto& fixture : established_fixtures()) {
        if (fixture.requirement != std::string{"D21-9-10-MUST-369"} &&
            fixture.requirement != std::string{"D21-9-10-MUST-370"}) continue;
        SCOPED_TRACE(fixture.scenario);
        const auto* probe = established_probe(probes, fixture.scenario);
        ASSERT_NE(probe, nullptr);
        EstablishedTransport transport;
        transport.write_budget = fixture.initial.size() + 1;
        RawProbeController controller(transport, probe->definition);
        const auto now = RawProbeClock::time_point{};
        transport.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 0}), false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        transport.events = {transport::StreamDataEvent{1, fixture.initial_ok, false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        EXPECT_EQ(controller.transcript().writes[1].accepted, 1u);
        EXPECT_FALSE(controller.transcript().writes[1].delivery_event_count);
        EXPECT_FALSE(evaluate_draft21_close_probe(controller.transcript(), *probe).has_value());
        transport.write_budget.reset();
        EXPECT_TRUE(controller.poll(now).stimulus_delivered);
        EXPECT_EQ(transport.opened_bidi, 1u);
        transport.events = {transport::PeerCloseEvent{
            transport::CloseErrorSpace::Application, 3, {}}};
        EXPECT_TRUE(controller.poll(now).complete);
        EXPECT_EQ(evaluate_draft21_close_probe(controller.transcript(), *probe), true);
    }
}

TEST(Draft21CloseProbes, ControllerRequiresFreshFragmentedSuccessOnOriginalStreamAtEachStage) {
    const auto probes = draft21_close_probes();
    for (const auto& fixture : established_fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto* probe = established_probe(probes, fixture.scenario);
        ASSERT_NE(probe, nullptr);
        EstablishedTransport transport;
        RawProbeController controller(transport, probe->definition);
        const auto now = RawProbeClock::time_point{};
        // A response delivered before request acceptance cannot open the gate.
        transport.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 2, 8, 1}), false},
            transport::StreamDataEvent{1, fixture.initial_ok, false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        EXPECT_EQ(transport.output[1], fixture.initial);
        auto expected = fixture.initial;
        for (std::size_t stage = 1; stage < probe->definition.writes.size(); ++stage) {
            // Legal EXPIRES in all OKs, plus LARGEST_OBJECT and Track
            // Properties in SUBSCRIBE_OK, exercise the decoder at runtime.
            const auto response = stage == 1 && fixture.initial[0] == std::byte{0x16}
                ? bytes({0x18, 0, 6, 0, 0, 0, 0, 0x22, 1})
                : stage == 1
                ? (fixture.initial[0] == std::byte{3}
                   ? bytes({4, 0, 9, 0, 2, 8, 10, 1, 0, 0, 4, 1})
                   : bytes({7, 0, 3, 1, 8, 10}))
                : bytes({7, 0, 3, 1, 8, 10});
            transport.events = {transport::StreamDataEvent{5, response, false}};
            EXPECT_FALSE(controller.poll(now).stimulus_delivered);
            EXPECT_EQ(transport.output[1], expected);
            transport.events = {transport::StreamDataEvent{1,
                {response.begin(), response.begin() + 3}, false}};
            EXPECT_FALSE(controller.poll(now).stimulus_delivered);
            EXPECT_EQ(transport.output[1], expected);
            transport.events = {transport::StreamDataEvent{1,
                {response.begin() + 3, response.end()}, false}};
            EXPECT_EQ(controller.poll(now).stimulus_delivered,
                      stage + 1 == probe->definition.writes.size());
            expected.insert(expected.end(), fixture.update.begin(), fixture.update.end());
            EXPECT_EQ(transport.output[1], expected);
            EXPECT_EQ(controller.transcript().writes[stage].stream_id, 1u);
        }
        EXPECT_EQ(transport.opened_bidi, 1u);
        transport.events = {transport::PeerCloseEvent{
            transport::CloseErrorSpace::Application, fixture.close, {}}};
        ASSERT_TRUE(controller.poll(now).complete);
        EXPECT_EQ(evaluate_raw_probe_close(controller.transcript(), probe->definition,
                                          probe->expected_close), true);
    }
}

TEST(Draft21CloseProbes, ResponseBeforeCompleteInitialRequestAcceptanceDoesNotReleaseUpdate) {
    const auto probes = draft21_close_probes();
    for (const auto& fixture : established_fixtures()) {
        SCOPED_TRACE(fixture.scenario);
        const auto* probe = established_probe(probes, fixture.scenario);
        ASSERT_NE(probe, nullptr);
        EstablishedTransport transport;
        transport.block_request = true;
        RawProbeController controller(transport, probe->definition);
        const auto now = RawProbeClock::time_point{};
        transport.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 0}), false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        ASSERT_EQ(transport.output[1].size(), 1u);
        transport.events = {transport::StreamDataEvent{1, fixture.initial_ok, false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        transport.block_request = false;
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        EXPECT_EQ(transport.output[1], fixture.initial);
        EXPECT_FALSE(controller.transcript().writes[1].delivery_event_count);
        transport.events = {transport::StreamDataEvent{1, fixture.initial_ok, false}};
        EXPECT_EQ(controller.poll(now).stimulus_delivered, !fixture.duplicate);
    }
}

TEST(Draft21CloseProbes, ConfiguredFetchUsesExactTrackAndScopedTypedSuccess) {
    const auto probes = draft21_close_probes(std::chrono::milliseconds{250},
                                            {bytes({'n'})}, bytes({'t'}));
    const auto* probe = established_probe(probes, "d21-publish-state-notify-on-fetch");
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(probe->definition.deadline, std::chrono::milliseconds{250});
    ASSERT_EQ(probe->definition.writes.size(), 2u);
    EXPECT_EQ(probe->definition.writes[0].bytes,
              bytes({0x16, 0, 7, 1, 1, 1, 'n', 1, 't', 0}));
    EXPECT_FALSE(probe->definition.writes[0].fin);
    EXPECT_FALSE(probe->definition.writes[1].fin);
    const auto& ready = probe->definition.writes[1].peer_response_ready;
    ASSERT_TRUE(ready);
    EXPECT_TRUE(ready(bytes({0x18, 0, 4, 0, 1, 2, 0})));
    EXPECT_TRUE(ready(bytes({0x18, 0, 6, 1, 1, 2, 0, 0x22, 1})));
    const std::vector<std::byte> invalid[]{
        bytes({7, 0, 1, 0}), bytes({4, 0, 2, 0, 0}),
        bytes({0x18, 0, 4, 2, 1, 2, 0}),
        bytes({0x18, 0, 6, 0, 1, 2, 1, 8, 1}),
        bytes({0x18, 0, 5, 0, 1, 2, 0, 0}),
        bytes({0x18, 0, 6, 0, 1, 2, 0, 0x22, 3})};
    for (const auto& input : invalid) EXPECT_FALSE(ready(input));
    EXPECT_THROW(draft21_close_probes(std::chrono::milliseconds{1000}, {{}}, bytes({'t'})),
                 std::invalid_argument);
    EXPECT_THROW(draft21_close_probes(std::chrono::milliseconds{1000},
                 {std::vector<std::byte>(4096, std::byte{'n'})}, bytes({'t'})),
                 std::invalid_argument);
}

TEST(Draft21CloseProbes, DiscoveryUpdateStartsWithForwardZeroSubscribeTracks) {
    // Sections 9.18.1 and 9.20.19: FORWARD 0 is legal on SUBSCRIBE_TRACKS.
    // The default FORWARD of 1 made publishers push PUBLISH messages the
    // probe never answers, ending the session before the invalid update.
    const auto probes = draft21_close_probes();
    const auto* probe = established_probe(probes, "d21-discovery-update-invalid-forward");
    ASSERT_NE(probe, nullptr);
    ASSERT_EQ(probe->definition.writes.size(), 2u);
    EXPECT_EQ(probe->definition.writes[0].bytes, bytes({0x51, 0, 5, 1, 0, 1, 0x10, 0}));
    EXPECT_EQ(probe->definition.writes[1].bytes, bytes({2, 0, 4, 3, 1, 0x10, 255}));
}

TEST(Draft21CloseProbes, UpdateBaselineSubscribesUseConfiguredTrack) {
    const auto probes = draft21_close_probes(std::chrono::milliseconds{250},
                                            {bytes({'n'})}, bytes({'t'}));
    for (const char* id : {"d21-group-order-in-subscription-update",
                          "d21-duplicate-request-update-id"}) {
        SCOPED_TRACE(id);
        const auto* probe = established_probe(probes, id);
        ASSERT_NE(probe, nullptr);
        EXPECT_EQ(probe->definition.writes[0].bytes, bytes({3, 0, 7, 1, 1, 1, 'n', 1, 't', 0}));
        EXPECT_TRUE(app::scenario_requires_track(21, id));
        EXPECT_TRUE(app::established_update_scenario(21, id));
    }
}

TEST(Draft21CloseProbes, UpdateBaselineProofRecoversConfiguredTrack) {
    const auto probes = draft21_close_probes();
    for (const char* id : {"d21-group-order-in-subscription-update",
                          "d21-duplicate-request-update-id"}) {
        SCOPED_TRACE(id);
        const auto* probe = established_probe(probes, id);
        ASSERT_NE(probe, nullptr);
        auto fixture = established_fixtures()[std::string(id) == "d21-duplicate-request-update-id" ? 2 : 0];
        fixture.initial = bytes({3, 0, 7, 1, 1, 1, 'n', 1, 't', 0});
        EXPECT_EQ(evaluate_draft21_close_probe(established_transcript(fixture), *probe), true);
        fixture.initial = bytes({3, 0, 7, 1, 1, 1, 'n', 1, 't', 1});
        EXPECT_FALSE(evaluate_draft21_close_probe(established_transcript(fixture), *probe).has_value());
    }
}

TEST(Draft21CloseProbes, ConfiguredFetchProofRecoversOnlyStrictActualRequestTarget) {
    const auto profiles = draft21_close_probes();
    const auto* profile = established_probe(profiles, "d21-publish-state-notify-on-fetch");
    ASSERT_NE(profile, nullptr);
    auto fixture = established_fixtures().back();
    fixture.initial = bytes({0x16, 0, 7, 1, 1, 1, 'n', 1, 't', 0});
    const auto valid = established_transcript(fixture);
    EXPECT_EQ(evaluate_draft21_close_probe(valid, *profile), true);
    const std::vector<std::byte> invalid[]{
        bytes({0x16, 0, 7, 0, 1, 1, 'n', 1, 't', 0}),
        bytes({0x16, 0, 7, 3, 1, 1, 'n', 1, 't', 0}),
        bytes({0x16, 0, 6, 1, 1, 0, 1, 't', 0}),
        bytes({0x16, 0, 7, 1, 1, 1, 'n', 1, 't', 1}),
        bytes({0x16, 0, 8, 1, 1, 1, 'n', 1, 't', 0, 0}),
        bytes({0x16, 0, 7, 1, 33, 1, 'n', 1, 't', 0}),
    };
    for (const auto& input : invalid) {
        auto changed = valid;
        changed.writes[0].write.bytes = input;
        changed.writes[0].accepted = input.size();
        EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value());
    }
    for (const auto& input : {bytes({0x22, 0, 0}), bytes({0x22, 0, 2, 1, 0}),
                              bytes({0x22, 0, 2, 0, 0})}) {
        auto changed = valid;
        changed.writes[1].write.bytes = input;
        changed.writes[1].accepted = input.size();
        EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value());
    }
    auto changed = valid;
    changed.events.pop_back();
    EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value());
    changed = valid;
    std::get<transport::PeerCloseEvent>(changed.events.back()).error_code = 9;
    EXPECT_EQ(evaluate_draft21_close_probe(changed, *profile), false);
}

TEST(Draft21CloseProbes, ReservedFetchTargetsCannotCreateProbesOrEstablishReplay) {
    const auto profiles = draft21_close_probes();
    const auto* profile = established_probe(profiles, "d21-publish-state-notify-on-fetch");
    ASSERT_NE(profile, nullptr);
    struct ReservedTarget {
        std::vector<std::byte> field;
        std::vector<std::byte> name;
        std::vector<std::byte> request;
    };
    const ReservedTarget targets[]{
        {bytes({'.'}), bytes({'x'}),
         bytes({0x16, 0, 7, 1, 1, 1, '.', 1, 'x', 0})},
        {bytes({'.', 's', 'e', 's', 's', 'i', 'o', 'n'}), {},
         bytes({0x16, 0, 13, 1, 1, 8, '.', 's', 'e', 's', 's', 'i', 'o', 'n', 0, 0})},
    };
    for (const auto& target : targets) {
        EXPECT_THROW(draft21_close_probes(std::chrono::milliseconds{1000},
                                         {target.field}, target.name), std::invalid_argument);
        auto fixture = established_fixtures().back();
        fixture.initial = target.request;
        const auto transcript = established_transcript(fixture);
        EXPECT_NO_THROW({
            EXPECT_FALSE(evaluate_draft21_close_probe(transcript, *profile).has_value());
        });
    }
}

TEST(Draft21CloseProbes, SubscriberNotifyDefinesBothIndependentSubscriptionContexts) {
    const auto probes = draft21_close_probes(std::chrono::milliseconds{250},
                                            {bytes({'n'})}, bytes({'t'}));
    for (const char* id : {"d21-subscriber-sends-publish-state-notify",
                          "d21-publish-established-subscriber-sends-publish-state-notify"}) {
        const auto* probe = established_probe(probes, id);
        ASSERT_NE(probe, nullptr);
        EXPECT_EQ(probe->requirement_id, "D21-9-10-MUST-370");
        EXPECT_EQ(probe->evaluator_id, "d21-publish-state-notify-direction-error");
        EXPECT_EQ(probe->expected_close, 3u);
        EXPECT_EQ(probe->definition.deadline, std::chrono::milliseconds{250});
        ASSERT_EQ(probe->definition.writes.size(), 2u);
        EXPECT_FALSE(probe->definition.writes[0].fin);
        EXPECT_FALSE(probe->definition.writes[1].fin);
        EXPECT_EQ(probe->definition.writes[1].bytes, bytes({0x22, 0, 1, 0}));
        EXPECT_EQ(probe->definition.writes[1].reuse_write_stream, 0u);
    }
    const auto* subscribe = established_probe(probes, "d21-subscriber-sends-publish-state-notify");
    ASSERT_NE(subscribe, nullptr);
    EXPECT_EQ(subscribe->definition.writes[0].bytes, bytes({3, 0, 7, 1, 1, 1, 'n', 1, 't', 0}));
    EXPECT_EQ(subscribe->definition.writes[0].channel, RawProbeChannel::NewBidi);
    ASSERT_TRUE(subscribe->definition.writes[1].peer_response_ready);
    const auto& ack = subscribe->definition.writes[1].peer_response_ready;
    EXPECT_TRUE(ack(bytes({4, 0, 4, 0, 0, 4, 1})));
    EXPECT_FALSE(ack(bytes({7, 0, 1, 0})));
    EXPECT_FALSE(ack(bytes({4, 0, 4, 0, 0, 0x22, 0})));
    const auto* publish = established_probe(probes, "d21-publish-established-subscriber-sends-publish-state-notify");
    ASSERT_NE(publish, nullptr);
    EXPECT_EQ(publish->definition.writes[0].bytes, bytes({7, 0, 1, 0}));
    EXPECT_EQ(publish->definition.writes[0].channel, RawProbeChannel::PeerBidi);
    EXPECT_EQ(publish->definition.writes[1].channel, RawProbeChannel::PeerBidi);
    EXPECT_FALSE(publish->definition.writes[1].peer_response_ready);
    EXPECT_TRUE(publish->definition.writes[1].evidence_ready);
    ASSERT_TRUE(publish->definition.peer_request_ready);
    const auto opener = bytes({0x1d, 0, 10, 0, 1, 1, 'n', 1, 't', 0, 0, 4, 1});
    EXPECT_TRUE(publish->definition.peer_request_ready(opener));
    EXPECT_TRUE(publish->definition.peer_request_ready(bytes({0x1d,0,8,0,1,1,'n',1,'t',0,0})));
    EXPECT_TRUE(publish->definition.peer_request_ready(bytes({0x1d,0,12,0,1,1,'n',1,'t',0,1,0x10,1,4,1})));
    for (std::size_t n = 0; n < opener.size(); ++n)
        EXPECT_FALSE(publish->definition.peer_request_ready(std::span(opener).first(n)));
    for (const auto& invalid : {
        bytes({0x1d,0,10,2,1,1,'n',1,'t',0,0,4,1}),
        bytes({0x1d,0,10,0,1,1,'n',1,'x',0,0,4,1}),
        bytes({0x1d,0,10,0,1,1,'n',1,'t',0,0,0x22,0}),
        bytes({0x1d,0,10,0,1,1,'n',1,'t',0,0,0x30,2}),
        bytes({0x1d,0,12,0,1,1,'n',1,'t',0,0,0xc0,0x40,0,0}),
        bytes({0x1d,0,14,0,1,1,'n',1,'t',0,1,3,2,2,0,4,1}),
        bytes({0x1d,0,13,0,1,1,'n',1,'t',0,1,0x21,1,0x80,4,1})})
        EXPECT_FALSE(publish->definition.peer_request_ready(invalid));
}

TEST(Draft21CloseProbes, SubscriberNotifySubscribeProofPreservesStrictTargetAndAcceptance) {
    const auto profiles = draft21_close_probes();
    const auto* profile = established_probe(profiles, "d21-subscriber-sends-publish-state-notify");
    ASSERT_NE(profile, nullptr);
    const EstablishedFixture fixture{profile->definition.id.c_str(), "D21-9-10-MUST-370",
        "d21-publish-state-notify-direction-error",
        bytes({3, 0, 7, 1, 1, 1, 'n', 1, 't', 0}), bytes({0x22, 0, 1, 0}),
        bytes({4, 0, 4, 0, 0, 4, 1})};
    const auto valid = established_transcript(fixture);
    EXPECT_EQ(evaluate_draft21_close_probe(valid, *profile), true);
    for (const auto& input : {bytes({3,0,7,3,1,1,'n',1,'t',0}),
                              bytes({3,0,7,1,1,1,'.',1,'t',0}),
                              bytes({3,0,9,1,1,1,'n',1,'t',1,0x10,1})}) {
        auto changed = valid;
        changed.writes[0].write.bytes = input;
        changed.writes[0].accepted = input.size();
        EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value());
    }
    auto changed = valid;
    changed.writes[1].stream_id = 5;
    EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value());
    changed = valid;
    --changed.writes[1].accepted;
    EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value());
    changed = valid;
    std::get<transport::StreamDataEvent>(changed.events[2]).data = bytes({7,0,1,0});
    EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value());
    changed = valid;
    changed.events.pop_back();
    EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value());
    changed = valid;
    std::get<transport::PeerCloseEvent>(changed.events.back()).error_code = 9;
    EXPECT_EQ(evaluate_draft21_close_probe(changed, *profile), false);
}

TEST(Draft21CloseProbes, PublishEstablishedSubscriberNotifyWaitsForCompleteAckWithoutPeerReply) {
    const auto probes = draft21_close_probes(std::chrono::milliseconds{250},
                                            {bytes({'n'})}, bytes({'t'}));
    const auto* probe = established_probe(probes, "d21-publish-established-subscriber-sends-publish-state-notify");
    ASSERT_NE(probe, nullptr);
    EstablishedTransport transport;
    transport.write_budget = 1;
    RawProbeController controller(transport, probe->definition);
    const auto now = RawProbeClock::time_point{};
    transport.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2, bytes({0xaf,0,0,0}), false},
        transport::StreamDataEvent{0, bytes({0x1d,0,10}), false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    EXPECT_EQ(controller.transcript().writes[0].accepted, 0u);
    transport.events = {transport::StreamDataEvent{0, bytes({0,1,1,'n',1,'t',0,0,4,1}), false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    ASSERT_EQ(controller.transcript().writes[0].accepted, 1u);
    EXPECT_EQ(controller.transcript().writes[1].accepted, 0u);
    transport.write_budget.reset();
    EXPECT_TRUE(controller.poll(now).stimulus_delivered);
    EXPECT_EQ(transport.output[0], bytes({7,0,1,0,0x22,0,1,0}));
    EXPECT_EQ(transport.opened_bidi, 0u);
    transport.events = {transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    EXPECT_TRUE(controller.poll(now).complete);
    const auto valid = controller.transcript();
    const auto canonical = draft21_close_probes();
    const auto* profile = established_probe(canonical, probe->definition.id.c_str());
    ASSERT_NE(profile, nullptr);
    EXPECT_EQ(evaluate_draft21_close_probe(valid, *profile), true);
    for (unsigned change = 0; change < 12; ++change) {
        auto altered = valid;
        auto& opener = std::get<transport::StreamDataEvent>(altered.events[3]);
        switch (change) {
        case 0: opener.stream_id = 4; break;
        case 1: opener.fin = true; break;
        case 2: opener.data[0] = std::byte{2}; break;
        case 3: opener.data[9] = std::byte{2}; opener.data[8] = std::byte{0x30}; break;
        case 4: --altered.writes[0].accepted; break;
        case 5: altered.writes[0].write.bytes = bytes({7,0,3,0,4,1}); altered.writes[0].accepted=6; break;
        case 6: altered.writes[1].stream_id=4; break;
        case 7: --altered.writes[1].accepted; break;
        case 8: altered.writes[1].write.bytes=bytes({0x22,0,0}); altered.writes[1].accepted=3; break;
        case 9: altered.writes[0].delivery_event_count=2; break;
        case 10: opener.data[3]=std::byte{'.'}; break;
        case 11: altered.writes[1].write.evidence_ready={}; break;
        }
        EXPECT_FALSE(evaluate_draft21_close_probe(altered, *profile).has_value()) << change;
    }
    auto altered=valid;
    altered.events.pop_back();
    EXPECT_FALSE(evaluate_draft21_close_probe(altered,*profile).has_value());
    altered=valid;
    std::get<transport::PeerCloseEvent>(altered.events.back()).error_space=transport::CloseErrorSpace::Transport;
    EXPECT_FALSE(evaluate_draft21_close_probe(altered,*profile).has_value());
    altered=valid;
    std::get<transport::PeerCloseEvent>(altered.events.back()).error_code=9;
    EXPECT_EQ(evaluate_draft21_close_probe(altered,*profile),false);
}

TEST(Draft21CloseProbes, PublishNotifyCancellationCannotReviveFragmentedOpenerOrBlockedAck) {
    const auto probes = draft21_close_probes(std::chrono::milliseconds{250},
                                            {bytes({'n'})}, bytes({'t'}));
    const auto* probe = established_probe(probes, "d21-publish-established-subscriber-sends-publish-state-notify");
    ASSERT_NE(probe, nullptr);
    const auto prefix = bytes({0x1d,0,10});
    const auto suffix = bytes({0,1,1,'n',1,'t',0,0,4,1});
    const std::vector<transport::TransportEvent> cancellations{
        transport::PeerResetEvent{0, 3}, transport::PeerStopSendingEvent{0, 3},
        transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}},
        transport::LocalCloseEvent{transport::CloseErrorSpace::Application,3,{}},
        transport::IdleTimeoutEvent{}};
    for (unsigned stage = 0; stage < 3; ++stage) {
        for (std::size_t cancel = 0; cancel < cancellations.size(); ++cancel) {
            SCOPED_TRACE(stage);
            SCOPED_TRACE(cancel);
            EstablishedTransport transport;
            if (stage != 0) transport.write_budget = stage == 1 ? 0 : 1;
            RawProbeController controller(transport, probe->definition);
            const auto now = RawProbeClock::time_point{};
            transport.events = {transport::ConnectionEstablishedEvent{},
                transport::StreamDataEvent{2, bytes({0xaf,0,0,0}), false},
                transport::StreamDataEvent{0, prefix, false}};
            EXPECT_FALSE(controller.poll(now).stimulus_delivered);
            if (stage != 0) {
                transport.events = {transport::StreamDataEvent{0, suffix, false}};
                EXPECT_FALSE(controller.poll(now).stimulus_delivered);
                EXPECT_EQ(controller.transcript().writes[0].accepted, stage == 1 ? 0u : 1u);
            }
            const auto before = transport.output[0];
            transport.events = {cancellations[cancel]};
            EXPECT_FALSE(controller.poll(now).stimulus_delivered);
            transport.write_budget.reset();
            // A later valid remainder and complete opener on the canceled
            // stream cannot undo cancellation, even when writes unblock.
            transport.events = {transport::StreamDataEvent{0, suffix, false},
                transport::StreamDataEvent{0, bytes({0x1d,0,10,0,1,1,'n',1,'t',0,0,4,1}), false}};
            EXPECT_FALSE(controller.poll(now).stimulus_delivered);
            EXPECT_EQ(transport.output[0], before);
            EXPECT_EQ(controller.transcript().writes[1].accepted, 0u);
            EXPECT_FALSE(evaluate_draft21_close_probe(controller.transcript(), *probe).has_value());
        }
    }
}

RawProbeTranscript publish_notify_transcript(const Draft21CloseProbe& probe) {
    EstablishedTransport transport;
    RawProbeController controller(transport, probe.definition);
    const auto now = RawProbeClock::time_point{};
    transport.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2, bytes({0xaf,0,0,0}), false},
        transport::StreamDataEvent{0, bytes({0x1d,0,10}), false}};
    controller.poll(now);
    transport.events = {transport::StreamDataEvent{0, bytes({0,1,1,'n',1,'t',0,0,4,1}), false}};
    controller.poll(now);
    transport.events = {transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    controller.poll(now);
    return controller.transcript();
}

TEST(Draft21CloseProbes, PublishNotifyProofRejectsMissingFutureAndCanceledAcceptancePrefixes) {
    const auto probes = draft21_close_probes(std::chrono::milliseconds{250},
                                            {bytes({'n'})}, bytes({'t'}));
    const auto* probe = established_probe(probes, "d21-publish-established-subscriber-sends-publish-state-notify");
    ASSERT_NE(probe, nullptr);
    const auto canonical = draft21_close_probes();
    const auto* profile = established_probe(canonical, probe->definition.id.c_str());
    ASSERT_NE(profile, nullptr);
    const auto valid = publish_notify_transcript(*probe);
    ASSERT_EQ(evaluate_draft21_close_probe(valid, *profile), true);
    for (std::size_t stage = 0; stage < 2; ++stage) {
        for (unsigned marker = 0; marker < 3; ++marker) {
            auto changed = valid;
            if (marker == 0) changed.writes[stage].delivery_event_count.reset();
            if (marker == 1) changed.writes[stage].delivery_event_count = changed.events.size() + 1;
            if (marker == 2) changed.writes[stage].delivery_event_count = 3;
            EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value()) << stage << ':' << marker;
        }
    }
    for (const auto& cancellation : std::vector<transport::TransportEvent>{
             transport::PeerResetEvent{0,3}, transport::PeerStopSendingEvent{0,3},
             transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}},
             transport::LocalCloseEvent{transport::CloseErrorSpace::Application,3,{}},
             transport::IdleTimeoutEvent{}}) {
        for (const std::size_t position : {3u, 4u}) {
            auto changed = valid;
            changed.events.insert(changed.events.begin() + position, cancellation);
            changed.writes[0].delivery_event_count = position == 3 ? 5 : 4;
            changed.writes[1].delivery_event_count = 5;
            changed.delivery_event_count = 5;
            EXPECT_FALSE(evaluate_draft21_close_probe(changed, *profile).has_value());
        }
    }
}

TEST(Draft21CloseProbes, PublishNotifyEvidenceGateRequiresAcceptedAckAndUncanceledOpenerPrefix) {
    const auto probes = draft21_close_probes(std::chrono::milliseconds{250},
                                            {bytes({'n'})}, bytes({'t'}));
    const auto* probe = established_probe(probes, "d21-publish-established-subscriber-sends-publish-state-notify");
    ASSERT_NE(probe, nullptr);
    const auto& gate = probe->definition.writes[1].evidence_ready;
    ASSERT_TRUE(gate);
    const auto valid = publish_notify_transcript(*probe);
    const auto ready = [&](const RawProbeTranscript& input, std::size_t end) {
        return gate({std::span(input.writes).first(1), std::span(input.events).first(end)});
    };
    ASSERT_TRUE(ready(valid, 4));
    EXPECT_FALSE(ready(valid, 3));
    for (unsigned change = 0; change < 3; ++change) {
        auto changed = valid;
        if (change == 0) changed.writes[0].delivery_event_count.reset();
        if (change == 1) changed.writes[0].delivery_event_count = 5;
        if (change == 2) changed.writes[0].delivery_event_count = 3;
        EXPECT_FALSE(ready(changed, 4)) << change;
    }
    for (const auto& cancellation : std::vector<transport::TransportEvent>{
             transport::PeerResetEvent{0,3}, transport::PeerStopSendingEvent{0,3},
             transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}},
             transport::LocalCloseEvent{transport::CloseErrorSpace::Application,3,{}}}) {
        auto changed = valid;
        changed.events.insert(changed.events.begin() + 3, cancellation);
        changed.writes[0].delivery_event_count = 5;
        EXPECT_FALSE(ready(changed, 5));
    }
}

TEST(Draft21SuccessfulResponse, NonrepeatableParametersCannotEstablishRequest) {
    // Section9.20 forbids senders repeating these parameter types.
    const std::vector<std::byte> repeated[]{
        bytes({4, 0, 6, 0, 2, 8, 1, 0, 2}),
        bytes({4, 0, 8, 0, 2, 9, 0, 0, 0, 1, 1}),
    };
    for (const auto& input : repeated) {
        wire::Cursor cursor(input);
        EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(
            wire::draft21::decode_successful_response(cursor, ResponseContext::Subscribe)));
        EXPECT_EQ(cursor.offset(), 0u);
    }
    for (const auto context : {ResponseContext::SubscribeTracks, ResponseContext::RequestUpdate}) {
        const auto input = bytes({7, 0, 5, 2, 8, 1, 0, 2});
        wire::Cursor cursor(input);
        EXPECT_TRUE(std::holds_alternative<wire::DecodeError>(
            wire::draft21::decode_successful_response(cursor, context)));
    }
}

TEST(Draft21SuccessfulResponse, BodyAndImmutableFailuresRetainAbsoluteInputOffsets) {
    struct Fixture { std::vector<std::byte> input; std::size_t expected_offset; };
    const Fixture invalid[]{
        {bytes({4, 0, 4, 0, 1, 0x22, 1}), 25},
        {bytes({4, 0, 4, 0, 0, 0x22, 3}), 25},
        {bytes({4, 0, 6, 0, 0, 0x0b, 2, 0x22, 3}), 27},
        {bytes({4, 0, 3, 0, 1, 8}), 26},
        {bytes({4, 0, 5, 0, 0, 0x0b, 1, 4}), 28},
    };
    for (const auto& fixture : invalid) {
        wire::Cursor cursor(fixture.input, 20);
        const auto decoded = wire::draft21::decode_successful_response(
            cursor, ResponseContext::Subscribe);
        ASSERT_TRUE(std::holds_alternative<wire::DecodeError>(decoded));
        EXPECT_EQ(std::get<wire::DecodeError>(decoded).offset, fixture.expected_offset);
        EXPECT_EQ(cursor.offset(), 20u);
    }
}

}  // namespace
}  // namespace moq::interop::scenarios
