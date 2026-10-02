#include "draft18_contribution_internal.h"

namespace moq::interop::scenarios::contribution {
namespace {

constexpr std::uint64_t kSubscribeId = 1;
constexpr std::uint64_t kSecondId = 3;
constexpr std::uint64_t kThirdId = 5;
constexpr std::uint64_t kFourthId = 7;
constexpr std::uint64_t kErrorRedirect = 0x34;
// Section 14: Auth Token Type values 0x7f * N + 0x9D are reserved for greasing.
constexpr std::uint64_t kUnknownTokenType = 0x9D;
// A Start Location this far past the Largest Object cannot be reached by a
// live publisher while the probe runs.
constexpr std::uint64_t kUnreachableGroupGap = 1'000'000;
// Updates sent back to back on one SUBSCRIBE stream (section 10.9.1).
constexpr std::size_t kUpdateCount = 3;

RawProbeDefinition definition(const char* id, std::vector<RawProbeWrite> writes,
                              std::function<bool(const RawProbeTranscript&)> ready,
                              std::chrono::milliseconds deadline) {
    return {id, setup_message({}), std::move(writes), true, setup_ready, deadline,
            std::move(ready), {}};
}
RawProbeWrite bidi(Bytes bytes) { return {RawProbeChannel::NewBidi, std::move(bytes), false}; }
RawProbeWrite on_stream(std::size_t stream_write, Bytes bytes,
                        std::function<bool(std::span<const std::byte>)> response_ready = {}) {
    RawProbeWrite write{RawProbeChannel::NewBidi, std::move(bytes), false, stream_write,
                        std::move(response_ready)};
    return write;
}
RawProbeWrite after_subscribe_ok(Bytes bytes) {
    RawProbeWrite write{RawProbeChannel::NewBidi, std::move(bytes), false};
    write.evidence_ready = [](const RawProbeGateInput& input) {
        const auto reply = gate_reply(input, 0);
        return !reply.messages.empty() && subscribe_ok(reply);
    };
    return write;
}

bool has_reply(const RawProbeTranscript& transcript, std::size_t write_index) {
    return !write_reply(transcript, write_index).messages.empty();
}

// Section 10.12.2 and 10.12.3: REQUEST_ERROR INVALID_RANGE is mandatory when
// the range cannot be served. DOES_NOT_EXIST is wrong for a track whose
// SUBSCRIBE_OK was just received; other codes do not say.
std::optional<bool> invalid_range_response(const Reply& reply) {
    if (reply.messages.empty()) return std::nullopt;
    if (std::holds_alternative<d18::FetchOkMessage>(reply.messages.front())) return false;
    if (const auto* error = request_error(reply)) {
        if (error->error_code == kErrorInvalidRange) return true;
        if (error->error_code == kErrorDoesNotExist) return false;
    }
    return std::nullopt;
}

// SUBSCRIBE_OK on the first write's stream, if it arrived first.
const d18::SubscribeOkMessage* established(const Reply& reply) { return subscribe_ok(reply); }

std::optional<bool> subscribe_answered_with_subscribe_ok(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    if (reply.messages.empty()) return std::nullopt;
    if (subscribe_ok(reply)) return true;
    // A refusal says nothing about how a successful SUBSCRIBE is answered.
    if (request_error(reply)) return std::nullopt;
    return false;
}

// Section 10.7: a matching new Object follows a successful forwarding subscription.
std::optional<bool> object_delivered_on_subscription(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    const auto* ok = established(reply);
    if (!ok) return std::nullopt;
    for (const auto& [id, stream] : peer_data_streams(transcript)) {
        (void)id;
        d18::SubgroupDecoder decoder;
        const auto pushed = decoder.push(stream.bytes, stream.fin);
        if (pushed.header && pushed.header->track_alias == ok->track_alias && !pushed.objects.empty())
            return true;
    }
    for (const auto& event : transcript.events) {
        if (terminal(event)) break;
        const auto* datagram = std::get_if<transport::DatagramEvent>(&event);
        if (!datagram) continue;
        const auto decoded = d18::decode_datagram(datagram->data, {});
        const auto* object = std::get_if<d18::ObjectEvent>(&decoded);
        if (object && object->track_alias == ok->track_alias) return true;
    }
    return std::nullopt;
}
bool object_seen(const RawProbeTranscript& transcript) {
    return object_delivered_on_subscription(transcript, false).value_or(false);
}

// Typed replies to REQUEST_UPDATE follow SUBSCRIBE_OK on the same stream.
std::size_t update_replies(const Reply& reply) {
    return static_cast<std::size_t>(std::count_if(reply.messages.begin() + (reply.messages.empty() ? 0 : 1),
                                                  reply.messages.end(), is_typed_response));
}
bool update_settled_ready(const RawProbeTranscript& transcript) {
    const auto reply = write_reply(transcript, 0);
    return established(reply) && update_replies(reply) >= 1;
}

std::optional<bool> exactly_one_update_response(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    if (!established(reply)) return std::nullopt;
    const auto count = update_replies(reply);
    if (count == 0) return std::nullopt;
    return count == 1;
}

// Section 10.9.1: every successful update gets its REQUEST_OK even when the
// publisher coalesces processing. One REQUEST_ERROR may cover a failed batch.
std::optional<bool> one_ok_per_coalesced_update(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    if (!established(reply)) return std::nullopt;
    std::size_t oks = 0;
    for (std::size_t i = 1; i < reply.messages.size(); ++i) {
        if (std::holds_alternative<d18::RequestErrorMessage>(reply.messages[i])) return std::nullopt;
        if (std::holds_alternative<d18::RequestOkMessage>(reply.messages[i])) ++oks;
    }
    if (oks == 0) return std::nullopt;
    return oks == kUpdateCount;
}
bool coalesced_ready(const RawProbeTranscript& transcript) {
    const auto reply = write_reply(transcript, 0);
    if (!established(reply)) return false;
    return update_replies(reply) >= kUpdateCount ||
           std::any_of(reply.messages.begin(), reply.messages.end(), [](const auto& message) {
               return std::holds_alternative<d18::RequestErrorMessage>(message);
           });
}

// Section 10.11: with Forward State 0 the publisher opens no data stream, so
// its PUBLISH_DONE must report a Stream Count of zero.
const d18::PublishDoneMessage* publish_done(const Reply& reply) {
    for (const auto& message : reply.messages)
        if (const auto* done = std::get_if<d18::PublishDoneMessage>(&message)) return done;
    return nullptr;
}
std::optional<bool> publish_done_counts_no_streams(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    const auto* done = publish_done(reply);
    if (!established(reply) || !done) return std::nullopt;
    if (!peer_data_streams(transcript).empty()) return std::nullopt;
    return done->stream_count == 0;
}

std::optional<bool> invalid_range_for_forward_zero(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto subscription = write_reply(transcript, 0);
    if (!established(subscription)) return std::nullopt;
    return invalid_range_response(write_reply(transcript, 1));
}

// A SUBSCRIBE_OK without LARGEST_OBJECT states that no Object has been
// published (section 10.2.11), which is the precondition of the empty-track rows.
std::optional<bool> invalid_range_for_empty_track(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto subscription = write_reply(transcript, 0);
    const auto* ok = established(subscription);
    if (!ok || largest_object(ok->parameters)) return std::nullopt;
    return invalid_range_response(write_reply(transcript, 1));
}

std::optional<bool> invalid_range_beyond_largest(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto subscription = write_reply(transcript, 0);
    const auto* ok = established(subscription);
    if (!ok || !largest_object(ok->parameters)) return std::nullopt;
    return invalid_range_response(write_reply(transcript, 1));
}

// Sections 5.1 and 10.12.2: the update is processed first, and the Joining
// Fetch ends at the Joining Location from its REQUEST_UPDATE_OK.
std::optional<bool> joining_fetch_after_update(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto subscription = write_reply(transcript, 0);
    if (!established(subscription) || subscription.messages.size() < 2) return std::nullopt;
    const auto* update = std::get_if<d18::RequestOkMessage>(&subscription.messages[1]);
    if (!update) return std::nullopt;
    const auto joining = largest_object(update->parameters);
    const auto fetch = write_reply(transcript, 2);
    if (fetch.messages.empty()) return std::nullopt;
    if (const auto* ok = std::get_if<d18::FetchOkMessage>(&fetch.messages.front()))
        return joining && ok->end_location == d18::Location{joining->group, joining->object + 1};
    if (const auto* error = request_error(fetch)) {
        if (error->error_code != kErrorInvalidRange) return std::nullopt;
        // Forward State is 1 after the update, so only an empty track justifies it.
        return !joining;
    }
    return std::nullopt;
}
bool fetch_and_update_ready(const RawProbeTranscript& transcript) {
    const auto subscription = write_reply(transcript, 0);
    return established(subscription) && subscription.messages.size() >= 2 &&
           is_typed_response(subscription.messages[1]) && has_reply(transcript, 2);
}

std::optional<bool> redirect_leaves_track_name_empty(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    const auto* error = request_error(reply);
    if (!error || error->error_code != kErrorRedirect || !error->redirect) return std::nullopt;
    return error->redirect->track_name.bytes.empty();
}

// Section 10.18: NAMESPACE_DONE only follows its NAMESPACE.
struct NamespaceOrder {
    bool violation{false};
    std::size_t withdrawals{0};
};
NamespaceOrder namespace_order(const Reply& reply) {
    NamespaceOrder result;
    std::vector<Namespace> active;
    for (std::size_t i = 1; i < reply.messages.size(); ++i) {
        if (const auto* announced = std::get_if<d18::NamespaceMessage>(&reply.messages[i])) {
            active.push_back(announced->track_namespace_suffix.fields);
        } else if (const auto* done = std::get_if<d18::NamespaceDoneMessage>(&reply.messages[i])) {
            const auto found = std::find(active.begin(), active.end(), done->track_namespace_suffix.fields);
            if (found == active.end()) {
                result.violation = true;
            } else {
                active.erase(found);
                ++result.withdrawals;
            }
        }
    }
    return result;
}
bool namespace_done_seen(const RawProbeTranscript& transcript) {
    const auto reply = write_reply(transcript, 0);
    if (reply.messages.empty() || !std::holds_alternative<d18::RequestOkMessage>(reply.messages.front()))
        return false;
    return std::any_of(reply.messages.begin(), reply.messages.end(), [](const auto& message) {
        return std::holds_alternative<d18::NamespaceDoneMessage>(message);
    });
}
std::optional<bool> namespace_precedes_done(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    if (reply.messages.empty() || !std::holds_alternative<d18::RequestOkMessage>(reply.messages.front()))
        return std::nullopt;
    const auto order = namespace_order(reply);
    if (order.violation) return false;
    if (order.withdrawals == 0) return std::nullopt;
    return true;
}

// Section 14: an unknown Auth Token Type alone must not close the session.
std::optional<bool> unknown_token_type_not_fatal(const RawProbeTranscript& transcript, bool) {
    if (!bounded(transcript)) return std::nullopt;
    const auto reply = write_reply(transcript, 0);
    if (!reply.messages.empty())
        return subscribe_ok(reply) || request_error(reply) ? std::optional<bool>{true} : std::nullopt;
    if (application_close(transcript)) return false;
    return std::nullopt;
}

Namespace discovery_prefix(const Fixture& fixture) {
    return fixture.track_namespace.empty() ? Namespace{text("a")} : fixture.track_namespace;
}

}  // namespace

std::vector<Draft18ContributionProbe> subscription_probes(std::chrono::milliseconds deadline,
                                                          const Fixture& fixture) {
    std::vector<Draft18ContributionProbe> result;
    const auto quiet = quiet_window(deadline);
    const auto add = [&](const char* requirement, const char* evaluator, RawProbeDefinition def,
                         Observe observe, bool track = true) {
        result.push_back(make_probe(requirement, evaluator, std::move(def), std::move(observe), track));
    };
    const auto first_reply = [](const RawProbeTranscript& transcript) { return has_reply(transcript, 0); };

    add("D18-10-7-MUST-001", "successful-subscribe-responds-subscribe-ok",
        definition("accept-subscribe-for-known-publisher-track",
                   {bidi(subscribe_request(fixture, kSubscribeId))}, first_reply, deadline),
        subscribe_answered_with_subscribe_ok);
    add("D18-10-7-MUST-002", "matching-new-object-delivered-on-established-subscription",
        definition("accept-forward-one-subscribe-then-publish-matching-object",
                   {bidi(subscribe_request(fixture, kSubscribeId, {forward_parameter(1)}))},
                   object_seen, deadline),
        object_delivered_on_subscription);

    const auto update_on_stream = [&](std::vector<Bytes> updates) {
        std::vector<RawProbeWrite> writes{
            bidi(subscribe_request(fixture, kSubscribeId, {forward_parameter(1)}))};
        for (auto& update : updates) {
            writes.push_back(on_stream(0, std::move(update),
                writes.size() == 1 ? std::function<bool(std::span<const std::byte>)>(first_message_is_subscribe_ok)
                                   : std::function<bool(std::span<const std::byte>)>{}));
        }
        return writes;
    };
    add("D18-10-9-MUST-001", "exactly-one-request-ok-or-request-error",
        definition("receive-one-request-update-on-established-publisher-request",
                   update_on_stream({request_update(kSecondId, {forward_parameter(1)})}),
                   settled_after(update_settled_ready, quiet), deadline),
        exactly_one_update_response);
    add("D18-10-9-1-MUST-005", "one-request-ok-per-successful-coalesced-update",
        definition("receive-multiple-successful-request-updates-on-one-stream",
                   update_on_stream({request_update(kSecondId, {priority_parameter(10)}),
                                     request_update(kThirdId, {priority_parameter(20)}),
                                     request_update(kFourthId, {priority_parameter(30)})}),
                   [settled = settled_after(update_settled_ready, quiet)](const RawProbeTranscript& transcript) {
                       // A single reply for the whole batch is only visible once the window has passed.
                       const bool window_elapsed = settled(transcript);
                       return coalesced_ready(transcript) || window_elapsed;
                   }, deadline),
        one_ok_per_coalesced_update);

    add("D18-10-11-MUST-001", "publish-done-stream-count-zero",
        definition("publisher-ends-subscription-with-no-data-streams",
                   {bidi(subscribe_request(fixture, kSubscribeId, {forward_parameter(0)}))},
                   [](const RawProbeTranscript& transcript) {
                       return publish_done(write_reply(transcript, 0)) != nullptr;
                   }, deadline),
        publish_done_counts_no_streams);

    add("D18-10-12-2-MUST-001", "request-error-invalid-range",
        definition("receive-joining-fetch-for-forward-zero-subscription",
                   {bidi(subscribe_request(fixture, kSubscribeId, {forward_parameter(0)})),
                    after_subscribe_ok(relative_joining_fetch(kSecondId, kSubscribeId, 0))},
                   [](const RawProbeTranscript& transcript) { return has_reply(transcript, 1); }, deadline),
        invalid_range_for_forward_zero);
    {
        RawProbeWrite update = on_stream(0, request_update(kSecondId, {forward_parameter(1)}),
                                         first_message_is_subscribe_ok);
        add("D18-10-12-2-MUST-002", "joining-fetch-uses-updated-forward-state-and-joining-location",
            definition("receive-forward-state-update-then-joining-fetch",
                       {bidi(subscribe_request(fixture, kSubscribeId, {forward_parameter(0)})),
                        std::move(update), bidi(relative_joining_fetch(kThirdId, kSubscribeId, 0))},
                       fetch_and_update_ready, deadline),
            joining_fetch_after_update);
    }
    add("D18-10-12-2-MUST-003", "request-error-invalid-range",
        definition("receive-joining-fetch-for-track-with-no-published-objects",
                   {bidi(subscribe_request(fixture, kSubscribeId)),
                    after_subscribe_ok(relative_joining_fetch(kSecondId, kSubscribeId, 0))},
                   [](const RawProbeTranscript& transcript) { return has_reply(transcript, 1); }, deadline),
        invalid_range_for_empty_track);
    add("D18-10-12-3-MUST-004", "request-error-invalid-range",
        definition("receive-standalone-fetch-for-track-with-no-published-objects",
                   {bidi(subscribe_request(fixture, kSubscribeId)),
                    after_subscribe_ok(standalone_fetch(fixture, kSecondId, {0, 0}, {0, 1}))},
                   [](const RawProbeTranscript& transcript) { return has_reply(transcript, 1); }, deadline),
        invalid_range_for_empty_track);
    {
        RawProbeWrite beyond{RawProbeChannel::NewBidi, {}, false};
        beyond.prepare_bytes = [fixture](const RawProbeGateInput& input)
                -> std::optional<Bytes> {
            const auto reply = gate_reply(input, 0);
            const auto* ok = subscribe_ok(reply);
            if (!ok) return std::nullopt;
            const auto largest = largest_object(ok->parameters);
            if (!largest) return std::nullopt;
            const auto group = largest->group + kUnreachableGroupGap;
            return standalone_fetch(fixture, kSecondId, {group, 0}, {group, 1});
        };
        add("D18-10-12-3-MUST-005", "request-error-invalid-range",
            definition("receive-fetch-start-beyond-largest-published-object",
                       {bidi(subscribe_request(fixture, kSubscribeId)), std::move(beyond)},
                       [](const RawProbeTranscript& transcript) { return has_reply(transcript, 1); },
                       deadline),
            invalid_range_beyond_largest);
    }

    const auto prefix = d18::TrackNamespace{discovery_prefix(fixture)};
    const Bytes namespace_request = encode(d18::SubscribeNamespaceMessage{kSubscribeId, prefix, {}});
    add("D18-10-6-1-MUST-002", "namespace-redirect-track-name-empty",
        definition("publisher-redirects-subscribe-namespace", {bidi(namespace_request)},
                   first_reply, deadline),
        redirect_leaves_track_name_empty);
    add("D18-10-18-MUST-NOT-001", "namespace-precedes-corresponding-namespace-done",
        definition("publish-and-withdraw-namespace-during-discovery", {bidi(namespace_request)},
                   namespace_done_seen, deadline),
        namespace_precedes_done);

    add("D18-14-MUST-007", "unknown-auth-token-type-does-not-close-session",
        definition("subscribe-unknown-auth-token-type",
                   {bidi(subscribe_request(fixture, kSubscribeId,
                        {{0x03, d18::Token{d18::TokenAliasType::UseValue, std::nullopt,
                                           kUnknownTokenType, text("opaque")}}}))},
                   first_response_or_close(0), deadline),
        unknown_token_type_not_fatal);
    return result;
}

}  // namespace moq::interop::scenarios::contribution
