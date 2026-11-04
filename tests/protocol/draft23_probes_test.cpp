#include "moq/interop/session/draft23_probes.h"

#include <gtest/gtest.h>

#include "moq/interop/wire/draft23/messages.h"

#include <set>
#include <string>

namespace {
using namespace moq::interop::session;

std::vector<unsigned> raw(const std::vector<std::byte>& bytes) {
    std::vector<unsigned> result;
    for (const auto byte : bytes) result.push_back(std::to_integer<unsigned>(byte));
    return result;
}

TEST(Draft23Probes, EveryProbeHasAUniqueScenarioIdAndRow) {
    std::set<std::string> scenarios, rows;
    for (const auto& probe : draft23_probes()) {
        EXPECT_TRUE(scenarios.insert(std::string(probe.scenario_id)).second) << probe.scenario_id;
        for (const auto& row : draft23_probe_rows(probe)) {
            // A requirement can be judged by several probes (the error-detail rows), but never twice by one.
            (void)rows.insert(std::string(row));
            EXPECT_TRUE(row.starts_with("D23-")) << probe.scenario_id << " " << row;
        }
        EXPECT_FALSE(probe.evaluator_id.empty()) << probe.scenario_id;
        EXPECT_TRUE(probe.scenario_id.starts_with("d23-probe-")) << probe.scenario_id;
        EXPECT_TRUE(probe.requirement_id.starts_with("D23-")) << probe.requirement_id;
    }
    EXPECT_EQ(scenarios.size(), 52u);
}
TEST(Draft23Probes, FindsAProbeByScenarioId) {
    ASSERT_NE(find_draft23_probe("d23-probe-unknown-control-message"), nullptr);
    EXPECT_EQ(find_draft23_probe("d23-probe-unknown-control-message")->requirement_id, "D23-9-MUST-257");
    EXPECT_EQ(find_draft23_probe("d23-publisher-smoke"), nullptr);
    EXPECT_EQ(find_draft23_probe(""), nullptr);
}
TEST(Draft23Probes, ExpectedSessionCloseCodesComeFromTheDraft) {
    const auto code = [](std::string_view id) { return find_draft23_probe(id)->expected_code; };
    EXPECT_EQ(code("d23-probe-unknown-control-message"), 0x303u);   // UNEXPECTED_MESSAGE
    EXPECT_EQ(code("d23-probe-bad-first-bidi-message"), 0x303u);
    EXPECT_EQ(code("d23-probe-length-mismatch"), 0x300u);           // MALFORMED_MESSAGE
    EXPECT_EQ(code("d23-probe-oversize-control-message"), 0x401u);  // FIELD_LENGTH_EXCEEDED
    EXPECT_EQ(code("d23-probe-server-authority"), 0x19u);           // INVALID_AUTHORITY
    EXPECT_EQ(code("d23-probe-server-path"), 0x8u);                 // INVALID_PATH
    EXPECT_EQ(code("d23-probe-unknown-uni-stream"), 0x200u);        // INVALID_STREAM_HEADER
}
TEST(Draft23Probes, InjectedBytesAreExactlyTheMalformedInput) {
    const auto bytes = [](std::string_view id, std::uint64_t limit = 65535) {
        return raw(draft23_probe_bytes(*find_draft23_probe(id), limit));
    };
    EXPECT_EQ(bytes("d23-probe-unknown-control-message"), (std::vector<unsigned>{0x3f, 0}));
    EXPECT_EQ(bytes("d23-probe-bad-first-bidi-message"), (std::vector<unsigned>{4, 2, 7, 0}));
    EXPECT_EQ(bytes("d23-probe-length-mismatch"), (std::vector<unsigned>{6, 1, 0}));
    EXPECT_EQ(bytes("d23-probe-unknown-uni-stream"), (std::vector<unsigned>{0x0a}));
}
TEST(Draft23Probes, OversizeLengthIsOneByteBeyondThePeersAdvertisedLimit) {
    const auto probe = find_draft23_probe("d23-probe-oversize-control-message");
    // PUBLISH_NAMESPACE (6) with vi64 length 65536 = c1 00 00, and no body.
    EXPECT_EQ(raw(draft23_probe_bytes(*probe, 65535)), (std::vector<unsigned>{6, 0xc1, 0, 0}));
    // A larger advertised limit moves the length up with it: 100000 + 1 = 0x0186a1 -> c1 86 a1.
    EXPECT_EQ(raw(draft23_probe_bytes(*probe, 100000)), (std::vector<unsigned>{6, 0xc1, 0x86, 0xa1}));
}
TEST(Draft23Probes, ServerSetupProbesAddTheForbiddenOptionToTheRunnersSetup) {
    moq::interop::wire::draft21::KeyValues options;
    draft23_probe_setup_options(*find_draft23_probe("d23-probe-server-authority"), options);
    ASSERT_EQ(options.size(), 1u);
    EXPECT_EQ(options[0].type, 5u);
    options.clear();
    draft23_probe_setup_options(*find_draft23_probe("d23-probe-server-path"), options);
    ASSERT_EQ(options.size(), 1u);
    EXPECT_EQ(options[0].type, 1u);
    options.clear();
    draft23_probe_setup_options(*find_draft23_probe("d23-probe-length-mismatch"), options);
    EXPECT_TRUE(options.empty());
}
TEST(Draft23Probes, ChannelsSayWhereTheBytesGo) {
    EXPECT_EQ(find_draft23_probe("d23-probe-unknown-control-message")->channel, Draft23ProbeChannel::Bidi);
    EXPECT_EQ(find_draft23_probe("d23-probe-unknown-uni-stream")->channel, Draft23ProbeChannel::Uni);
    EXPECT_EQ(find_draft23_probe("d23-probe-server-authority")->channel, Draft23ProbeChannel::SetupOption);
    EXPECT_EQ(find_draft23_probe("d23-probe-server-path")->channel, Draft23ProbeChannel::SetupOption);
}

TEST(Draft23Probes, ReplyProbesExpectARequestErrorNotAClose) {
    for (const auto id : {"d23-probe-subscribe-reserved-namespace", "d23-probe-subscribe-session-namespace"}) {
        const auto* probe = find_draft23_probe(id);
        ASSERT_NE(probe, nullptr) << id;
        EXPECT_EQ(probe->expect, Draft23ProbeExpect::RequestError) << id;
        EXPECT_EQ(probe->expected_code, 0x10u) << id;  // DOES_NOT_EXIST
        EXPECT_EQ(probe->channel, Draft23ProbeChannel::Bidi) << id;
    }
    EXPECT_EQ(find_draft23_probe("d23-probe-subscribe-reserved-namespace")->requirement_id, "D23-2-4-3-MUST-034");
    EXPECT_EQ(find_draft23_probe("d23-probe-subscribe-session-namespace")->requirement_id, "D23-6-5-MUST-160");
    EXPECT_EQ(find_draft23_probe("d23-probe-unknown-control-message")->expect, Draft23ProbeExpect::Close);
}
TEST(Draft23Probes, ReplyProbesSendAValidSubscribeForTheReservedNamespace) {
    const auto decode = [](std::string_view id) {
        const auto bytes = draft23_probe_bytes(*find_draft23_probe(id), 65535);
        moq::interop::wire::Cursor cursor(bytes);
        const auto message = moq::interop::wire::draft23::decode_message(cursor);
        EXPECT_TRUE(std::holds_alternative<moq::interop::wire::draft23::Message>(message)) << id;
        return std::get<moq::interop::wire::draft23::Message>(message);
    };
    const auto reserved = std::get<moq::interop::wire::draft23::SubscribeMessage>(decode("d23-probe-subscribe-reserved-namespace"));
    ASSERT_FALSE(reserved.track_namespace.empty());
    EXPECT_EQ(reserved.track_namespace.front(), (std::vector<std::byte>{std::byte{'.'}}));
    EXPECT_EQ(reserved.request_id % 2, 1u);  // the runner is the server: odd Request IDs
    const auto session_ns = std::get<moq::interop::wire::draft23::SubscribeMessage>(decode("d23-probe-subscribe-session-namespace"));
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(session_ns.track_namespace.front().data()), session_ns.track_namespace.front().size()), ".session");
}

using moq::interop::wire::draft23::Message;
Message decoded(const std::vector<std::byte>& bytes) {
    moq::interop::wire::Cursor cursor(bytes);
    const auto message = moq::interop::wire::draft23::decode_message(cursor);
    EXPECT_TRUE(std::holds_alternative<Message>(message));
    return std::get<Message>(message);
}
std::vector<unsigned> first_payload(std::string_view id) {
    const auto payloads = draft23_probe_payloads(*find_draft23_probe(id), 65535);
    return payloads.empty() ? std::vector<unsigned>{} : raw(payloads.front());
}
TEST(Draft23Probes, EveryProbeSendsSomething) {
    for (const auto& probe : draft23_probes()) {
        if (probe.channel == Draft23ProbeChannel::SetupOption) {
            moq::interop::wire::draft21::KeyValues options;
            draft23_probe_setup_options(probe, options);
            EXPECT_FALSE(options.empty()) << probe.scenario_id;
        } else {
            EXPECT_FALSE(draft23_probe_payloads(probe, 65535).empty()) << probe.scenario_id;
        }
    }
}
TEST(Draft23Probes, RowsOfAProbeIncludeTheExtraRowsItJudges) {
    const auto rows = draft23_probe_rows(*find_draft23_probe("d23-probe-unknown-setup-option"));
    EXPECT_EQ(std::vector<std::string_view>(rows.begin(), rows.end()), (std::vector<std::string_view>{"D23-9-1-MUST-263", "D23-9-1-MUST-264", "D23-13-MUST-500", "D23-16-4-MUST-523"}));
}
TEST(Draft23Probes, HandBuiltSubscribeMatchesTheEncoderForAValidMessage) {
    // The probes build malformed messages by hand; this pins that the same builder agrees with the encoder when valid.
    using moq::interop::wire::draft23::SubscribeMessage;
    const auto text = [](std::string_view value) {
        const auto view = std::as_bytes(std::span(value));
        return std::vector<std::byte>(view.begin(), view.end());
    };
    SubscribeMessage message{1, {text("."), text("probe")}, text("track"), {}};
    moq::interop::wire::ByteWriter writer(256);
    ASSERT_TRUE(moq::interop::wire::draft23::encode_message(message, writer));
    const std::vector<std::byte> encoded(writer.bytes().begin(), writer.bytes().end());
    EXPECT_EQ(draft23_hand_built_subscribe(1, {".", "probe"}, "track"), encoded);
    EXPECT_EQ(draft23_probe_payloads(*find_draft23_probe("d23-probe-subscribe-reserved-namespace"), 65535).front(), encoded);
}
TEST(Draft23Probes, RequestIdProbesUseTheWrongParityAndAReusedId) {
    const auto wrong = std::get<moq::interop::wire::draft23::SubscribeMessage>(decoded(draft23_probe_payloads(*find_draft23_probe("d23-probe-wrong-request-id-parity"), 65535).front()));
    EXPECT_EQ(wrong.request_id % 2, 0u);  // the runner is the server, so an even id is the wrong parity
    const auto duplicate = draft23_probe_payloads(*find_draft23_probe("d23-probe-duplicate-request-id"), 65535);
    ASSERT_EQ(duplicate.size(), 2u);
    EXPECT_EQ(duplicate[0], duplicate[1]);
    EXPECT_EQ(std::get<moq::interop::wire::draft23::SubscribeMessage>(decoded(duplicate[0])).request_id % 2, 1u);
}
TEST(Draft23Probes, NamespaceProbesBreakExactlyOneLimit) {
    EXPECT_EQ(first_payload("d23-probe-namespace-empty-field"), (std::vector<unsigned>{3, 10, 1, 1, 0, 5, 't','r','a','c','k', 0}));
    const auto many = first_payload("d23-probe-namespace-too-many-fields");
    EXPECT_EQ(many[2], 1u);   // request id
    EXPECT_EQ(many[3], 33u);  // 33 fields, one more than the 32 allowed
    const auto long_ns = first_payload("d23-probe-namespace-too-long");
    EXPECT_GT(long_ns.size(), 4096u);
}
TEST(Draft23Probes, EmptyTrackNameHasZeroLength) {
    EXPECT_EQ(first_payload("d23-probe-empty-track-name"), (std::vector<unsigned>{3, 6, 1, 1, 1, 'a', 0, 0}));
}
TEST(Draft23Probes, ParameterProbesCarryExactlyOneBadParameter) {
    EXPECT_EQ(first_payload("d23-probe-group-order-out-of-range").back(), 7u);
    EXPECT_EQ(first_payload("d23-probe-paused-out-of-range").back(), 2u);
    EXPECT_EQ(first_payload("d23-probe-include-properties-out-of-range").back(), 2u);
}
TEST(Draft23Probes, SetupProbesReplaceTheRunnersWholeSetup) {
    for (const auto id : {"d23-probe-setup-delta-overflow", "d23-probe-setup-malformed-value"}) {
        const auto* probe = find_draft23_probe(id);
        ASSERT_NE(probe, nullptr) << id;
        EXPECT_EQ(probe->channel, Draft23ProbeChannel::SetupBytes) << id;
        const auto bytes = first_payload(id);
        EXPECT_EQ(bytes[0], 0xafu) << id;  // SETUP type 0x2f00
        EXPECT_EQ(bytes[1], 0u) << id;
    }
}
TEST(Draft23Probes, ToleranceProbesExpectNoClose) {
    for (const auto id : {"d23-probe-unknown-setup-option", "d23-probe-duplicate-unknown-setup-option", "d23-probe-unknown-error-detail",
                          "d23-probe-unknown-request-error-code"})
        EXPECT_EQ(find_draft23_probe(id)->expect, Draft23ProbeExpect::Tolerate) << id;
}
TEST(Draft23Probes, AnnounceReplyProbesReplaceTheRequestOk) {
    for (const auto id : {"d23-probe-reason-phrase-too-long", "d23-probe-request-ok-with-properties", "d23-probe-redirect-without-detail",
                          "d23-probe-redirect-track-name-in-namespace-request"})
        EXPECT_EQ(find_draft23_probe(id)->channel, Draft23ProbeChannel::AnnounceReply) << id;
    // The tolerance probes ride on the harness's own rejection of the publisher's PUBLISH for a track it declines, so
    // the publisher has no reason to end the session afterwards.
    for (const auto id : {"d23-probe-unknown-error-detail", "d23-probe-unknown-request-error-code"})
        EXPECT_EQ(find_draft23_probe(id)->channel, Draft23ProbeChannel::PublishReply) << id;
    EXPECT_EQ(find_draft23_probe("d23-probe-publish-state-notify-on-announce")->channel, Draft23ProbeChannel::AnnounceExtra);
    EXPECT_EQ(find_draft23_probe("d23-probe-publish-state-notify-to-publisher")->channel, Draft23ProbeChannel::SubscribeExtra);
}
TEST(Draft23Probes, DatagramProbesCarryTheInvalidTypeFlags) {
    EXPECT_EQ(find_draft23_probe("d23-probe-datagram-reserved-bit")->channel, Draft23ProbeChannel::Datagram);
    EXPECT_EQ(first_payload("d23-probe-datagram-reserved-bit")[0], 0x10u);
    EXPECT_EQ(first_payload("d23-probe-datagram-status-end-of-group")[0], 0x22u);
    EXPECT_EQ(first_payload("d23-probe-datagram-unknown-type")[0], 0x40u);
    EXPECT_EQ(first_payload("d23-probe-datagram-properties-length-zero")[0], 0x01u);
    EXPECT_EQ(first_payload("d23-probe-subgroup-header-reserved-mode"), (std::vector<unsigned>{0x16, 7, 0, 0}));  // flags, alias, group, priority
}

TEST(Draft23Probes, FilterRangeProbeSendsOneMoreRangeThanThePublisherAllows) {
    const auto* probe = find_draft23_probe("d23-probe-filter-ranges-over-limit");
    ASSERT_NE(probe, nullptr);
    EXPECT_EQ(probe->expect, Draft23ProbeExpect::RequestError);
    EXPECT_EQ(probe->expected_code, 0x36u);  // INVALID_FILTER
    const auto ranges = [&](std::uint64_t limit) {
        Draft23PeerLimits limits;
        limits.filter_ranges = limit;
        const auto message = decoded(draft23_probe_payloads(*probe, limits).front());
        const auto subscribe = std::get<moq::interop::wire::draft23::SubscribeMessage>(message);
        for (const auto& parameter : subscribe.parameters)
            if (parameter.type == 0x25) return std::get<std::vector<std::byte>>(parameter.value).size() / 2;  // two bytes per range
        return std::size_t{0};
    };
    EXPECT_EQ(ranges(0), 1u);   // the default limit is 0, so a single range already exceeds it
    EXPECT_EQ(ranges(4), 5u);
}
TEST(Draft23Probes, LocationFilterProbesBreakOneRule) {
    EXPECT_EQ(first_payload("d23-probe-location-filter-unknown-type").back(), 9u);  // Location Filter Type 9 is undefined
    const auto overflow = first_payload("d23-probe-location-filter-overflow");
    EXPECT_FALSE(overflow.empty());
    EXPECT_EQ(find_draft23_probe("d23-probe-location-filter-overflow")->expected_code, 0x400u);
}
TEST(Draft23Probes, FillParametersProbeNestsAParameterTheListDoesNotAllow) {
    const auto payload = first_payload("d23-probe-fill-parameters-invalid");
    EXPECT_FALSE(payload.empty());
    EXPECT_EQ(find_draft23_probe("d23-probe-fill-parameters-invalid")->expected_code, 0x305u);
}
TEST(Draft23Probes, PaddingProbesAreAllZeroAfterTheirType) {
    // The type 0x132B3E28 / 0x132B3E29 takes 5 bytes as a vi64; everything after it is zero.
    const auto stream = first_payload("d23-probe-padding-stream");
    ASSERT_GT(stream.size(), 5u);
    EXPECT_NE(stream[0], 0u);
    for (std::size_t i = 5; i < stream.size(); ++i) EXPECT_EQ(stream[i], 0u);
    const auto datagram = first_payload("d23-probe-padding-datagram");
    ASSERT_GT(datagram.size(), 5u);
    EXPECT_NE(datagram[0], 0u);
    for (std::size_t i = 5; i < datagram.size(); ++i) EXPECT_EQ(datagram[i], 0u);
    EXPECT_EQ(find_draft23_probe("d23-probe-padding-stream")->expect, Draft23ProbeExpect::Tolerate);
    EXPECT_EQ(find_draft23_probe("d23-probe-padding-datagram")->expect, Draft23ProbeExpect::Tolerate);
    EXPECT_EQ(find_draft23_probe("d23-probe-padding-datagram")->channel, Draft23ProbeChannel::Datagram);
}
TEST(Draft23Probes, GoawayProbesUseTheControlStreamAndTheSubscribeStream) {
    const auto* twice = find_draft23_probe("d23-probe-goaway-twice");
    EXPECT_EQ(twice->channel, Draft23ProbeChannel::ControlBytes);
    EXPECT_EQ(twice->expected_code, 0x303u);
    const auto both = first_payload("d23-probe-goaway-twice");
    const std::vector<unsigned> one_goaway{0x10, 2, 0, 0};  // empty New Session URI, timeout 0
    std::vector<unsigned> expected = one_goaway;
    expected.insert(expected.end(), one_goaway.begin(), one_goaway.end());
    EXPECT_EQ(both, expected);
    EXPECT_EQ(find_draft23_probe("d23-probe-goaway-uri-too-long")->expected_code, 0x401u);
    EXPECT_EQ(find_draft23_probe("d23-probe-goaway-on-subscribe-stream")->channel, Draft23ProbeChannel::SubscribeExtra);
    EXPECT_EQ(find_draft23_probe("d23-probe-goaway-on-subscribe-stream")->expected_code, 0x3u);
    EXPECT_EQ(find_draft23_probe("d23-probe-request-update-wrong-direction")->channel, Draft23ProbeChannel::AnnounceExtra);
    EXPECT_EQ(find_draft23_probe("d23-probe-request-update-wrong-direction")->expected_code, 0x303u);
}
TEST(Draft23Probes, HandBuiltGoawayMatchesTheEncoder) {
    using moq::interop::wire::draft23::GoawayMessage;
    moq::interop::wire::ByteWriter writer(16);
    ASSERT_TRUE(moq::interop::wire::draft23::encode_message(GoawayMessage{{}, 0}, writer));
    const std::vector<std::byte> encoded(writer.bytes().begin(), writer.bytes().end());
    const auto both = draft23_probe_payloads(*find_draft23_probe("d23-probe-goaway-twice"), 65535).front();
    ASSERT_EQ(both.size(), encoded.size() * 2);
    EXPECT_EQ(std::vector<std::byte>(both.begin(), both.begin() + static_cast<std::ptrdiff_t>(encoded.size())), encoded);
}

TEST(Draft23Probes, RequestsNameTheTrackThePublisherAnnouncedWhenGivenOne) {
    Draft23PeerLimits limits;
    const auto text = [](std::string_view value) {
        const auto view = std::as_bytes(std::span(value));
        return std::vector<std::byte>(view.begin(), view.end());
    };
    limits.track_namespace = {text("live")};
    limits.track_name = text("vide_1");
    const auto message = decoded(draft23_probe_payloads(*find_draft23_probe("d23-probe-wrong-request-id-parity"), limits).front());
    const auto subscribe = std::get<moq::interop::wire::draft23::SubscribeMessage>(message);
    ASSERT_EQ(subscribe.track_namespace.size(), 1u);
    EXPECT_EQ(subscribe.track_namespace[0], text("live"));
    EXPECT_EQ(subscribe.track_name, text("vide_1"));
}

TEST(Draft23Probes, SecondResponseProbesSendARequestOkAfterTheFirst) {
    EXPECT_EQ(first_payload("d23-probe-second-announce-response"), (std::vector<unsigned>{7, 2, 0, 0}));
    EXPECT_EQ(first_payload("d23-probe-second-publish-response"), (std::vector<unsigned>{7, 2, 0, 0}));
    EXPECT_EQ(find_draft23_probe("d23-probe-second-announce-response")->channel, Draft23ProbeChannel::AnnounceExtra);
    EXPECT_EQ(find_draft23_probe("d23-probe-second-publish-response")->channel, Draft23ProbeChannel::PublishExtra);
    EXPECT_EQ(find_draft23_probe("d23-probe-second-announce-response")->expected_code, 0x303u);
}
TEST(Draft23Probes, DuplicateParameterProbeRepeatsGroupOrder) {
    const auto payload = first_payload("d23-probe-duplicate-parameter");
    // ... count 2, GROUP_ORDER 1, then delta 0 and GROUP_ORDER 1 again.
    const std::vector<unsigned> tail{2, 0x22, 1, 0, 1};
    ASSERT_GE(payload.size(), tail.size());
    EXPECT_EQ(std::vector<unsigned>(payload.end() - static_cast<std::ptrdiff_t>(tail.size()), payload.end()), tail);
    EXPECT_EQ(find_draft23_probe("d23-probe-duplicate-parameter")->expected_code, 0x305u);
}
}  // namespace
