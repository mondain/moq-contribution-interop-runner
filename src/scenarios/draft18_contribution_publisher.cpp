#include "draft18_contribution_internal.h"

namespace moq::interop::scenarios::contribution {
namespace {

constexpr std::uint64_t kPropertyUnknownEven = 0x00;
constexpr std::uint64_t kPropertyUnknownOdd = 0x01;
constexpr std::uint64_t kPropertyGroupOrder = 0x22;
// Section 14 reserves 0x7f * N + 0x9D for greasing in every registry below.
constexpr std::uint64_t kGreaseCode = 0x9D;
constexpr std::uint64_t kInvalidGroupOrder = 0;
constexpr std::uint64_t kValidGroupOrder = 1;

RawProbeDefinition publisher_flow(const char* id, std::vector<RawProbeWrite> writes,
                                  std::function<bool(std::span<const std::byte>)> opener,
                                  std::function<bool(const RawProbeTranscript&)> ready,
                                  std::chrono::milliseconds deadline) {
    return {id, setup_message({}), std::move(writes), true, setup_ready, deadline,
            std::move(ready), std::move(opener)};
}

RawProbeWrite on_peer_stream(Bytes bytes, bool fin = false) {
    return {RawProbeChannel::PeerBidi, std::move(bytes), fin};
}
RawProbeWrite survival_write() { return {RawProbeChannel::NewBidi, survival_request(), false}; }

// Sections 2.5 and 10.5: TRACK_STATUS_OK carries the Track Properties. Types
// ascend (section 1.4.3): unknown even and odd optional types, the recognized
// DEFAULT_PUBLISHER_GROUP_ORDER, then the greased types.
Bytes track_status_ok(std::uint64_t group_order, bool grease_after) {
    d18::TrackProperties properties;
    properties.entries.push_back(even_option(kPropertyUnknownEven, 5));
    properties.entries.push_back(odd_option(kPropertyUnknownOdd, text("skip-me")));
    properties.entries.push_back(even_option(kPropertyGroupOrder, group_order));
    if (grease_after) {
        properties.entries.push_back(odd_option(kGreaseOdd, text("grease")));
        properties.entries.push_back(even_option(kGreaseEven, 3));
    }
    return encode(d18::RequestOkMessage{{}, std::move(properties)});
}

bool survival_reply_or_close(const RawProbeTranscript& transcript) {
    return !write_reply(transcript, 1).messages.empty() || peer_close(transcript).has_value();
}

// Unknown optional Properties are skipped, so the publisher keeps working.
std::optional<bool> unknown_properties_skipped(const RawProbeTranscript& transcript, bool) {
    return session_survived(transcript, 1);
}

// Section 12.5: with the unknown Properties skipped, parsing reaches the
// recognized order Property, whose invalid value must close the session with
// PROTOCOL_VIOLATION. A misaligned parser closes with another code instead.
std::optional<bool> invalid_property_found_after_unknown_ones(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    if (const auto close = peer_close(transcript)) {
        if (close->space != transport::CloseErrorSpace::Application) return std::nullopt;
        return close->code == kCloseProtocolViolation;
    }
    const auto reply = write_reply(transcript, 1);
    if (!reply.messages.empty() && is_typed_response(reply.messages.front())) return false;
    return std::nullopt;
}

std::optional<bool> unknown_error_not_fatal(const RawProbeTranscript& transcript, bool) {
    return session_survived(transcript, 1);
}
std::optional<bool> unknown_reset_code_not_fatal(const RawProbeTranscript& transcript, bool) {
    return session_survived(transcript, 2);
}

}  // namespace

std::vector<Draft18ContributionProbe> publisher_initiated_probes(std::chrono::milliseconds deadline) {
    std::vector<Draft18ContributionProbe> result;
    const auto add = [&](const char* requirement, const char* evaluator, RawProbeDefinition definition,
                         Observe observe) {
        result.push_back(make_probe(requirement, evaluator, std::move(definition), std::move(observe)));
    };
    // Recovery queries are optional publisher behavior (section 2.4.3). Each
    // definition waits for an actual TRACK_STATUS before replying.
    const auto track_status_opener = publisher_opener<d18::TrackStatusMessage>;
    const auto skipped = "track-status-unknown-optional-properties-skipped";
    for (const char* requirement : {"D18-14-MUST-002", "D18-14-MUST-009", "D18-15-8-MUST-001"}) {
        add(requirement, skipped,
            publisher_flow("publisher-recovery-track-status-unknown-optional-properties",
                           {on_peer_stream(track_status_ok(kValidGroupOrder, true)), survival_write()},
                           track_status_opener, survival_reply_or_close, deadline),
            unknown_properties_skipped);
        add(requirement, skipped,
            publisher_flow("publisher-recovery-track-status-unknown-before-invalid-property",
                           {on_peer_stream(track_status_ok(kInvalidGroupOrder, false)), survival_write()},
                           track_status_opener, survival_reply_or_close, deadline),
            invalid_property_found_after_unknown_ones);
    }

    const auto unknown_value = "unknown-extensible-value-alone-does-not-close-session";
    add("D18-14-MUST-NOT-001", unknown_value,
        publisher_flow("publisher-recovery-track-status-unknown-optional-properties",
                       {on_peer_stream(track_status_ok(kValidGroupOrder, true)), survival_write()},
                       track_status_opener, survival_reply_or_close, deadline),
        unknown_properties_skipped);

    // Section 10.6: a REQUEST_ERROR code the publisher does not know, sent
    // with FIN, in response to the publisher's PUBLISH or PUBLISH_NAMESPACE.
    const auto publication_opener = publisher_opener<d18::PublishMessage, d18::PublishNamespaceMessage>;
    const auto unknown_error = [&](const char* requirement, const char* evaluator) {
        add(requirement, evaluator,
            publisher_flow("publisher-request-rejected-with-unknown-error",
                           {on_peer_stream(encode(d18::RequestErrorMessage{kGreaseCode, 0, {}, std::nullopt}), true),
                            survival_write()},
                           publication_opener, survival_reply_or_close, deadline),
            unknown_error_not_fatal);
    };
    unknown_error("D18-14-MUST-004", "session-survives-unknown-request-error");
    unknown_error("D18-14-MUST-NOT-002", "session-survives-unknown-request-error");
    unknown_error("D18-14-MUST-NOT-001", unknown_value);

    // Section 3.3.3: STOP_SENDING on a request stream with a Stream Reset
    // Error Code the publisher does not know, after accepting the request.
    const auto unknown_reset = [&](const char* requirement, const char* evaluator) {
        RawProbeWrite stop{RawProbeChannel::PeerBidi, {}, false, 0};
        stop.operation = RawProbeOperation::StopSending;
        stop.application_error = kGreaseCode;
        add(requirement, evaluator,
            publisher_flow("publisher-request-stream-reset-with-unknown-code",
                           {on_peer_stream(ok_response()), stop, survival_write()},
                           publication_opener,
                           [](const RawProbeTranscript& transcript) {
                               return !write_reply(transcript, 2).messages.empty() ||
                                      peer_close(transcript).has_value();
                           }, deadline),
            unknown_reset_code_not_fatal);
    };
    unknown_reset("D18-14-MUST-006", "session-survives-unknown-request-stream-reset-code");
    unknown_reset("D18-14-MUST-NOT-001", unknown_value);
    return result;
}

}  // namespace moq::interop::scenarios::contribution
