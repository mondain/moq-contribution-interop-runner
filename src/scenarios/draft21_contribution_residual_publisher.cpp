// Draft-21 contribution rows observed on requests the publisher opens: the runner
// sends nothing and answers what arrives (RawProbeCourtesy), then reads what the
// publisher puts on the wire. See draft21_contribution_residual.cpp for the
// fixture contract these rows share.

#include "draft21_contribution_support.h"
#include "draft21_contribution_residual_internal.h"

#include <algorithm>

namespace moq::interop::scenarios::d21c {
namespace {

using namespace shared;
using namespace residual;

// ---- Requests the publisher opens ---------------------------------------------------
// The contexts below send nothing: the runner answers what the publisher opens
// (RawProbeCourtesy) and reads what the publisher puts on the wire.
constexpr std::uint64_t kGoaway = 0x10;

bool established(const PublishRecord& record) { return record.response == RawProbeCourtesyKind::PublishOk; }

// Both subscriptions were open at once: neither ended before the other began.
bool simultaneous(const PublishRecord& left, const PublishRecord& right) {
    return established(left) && established(right) &&
           (!left.terminated || *left.terminated > right.first_event) &&
           (!right.terminated || *right.terminated > left.first_event);
}

// ---- Section 3.1.2 lines 1080-1084: one Track Alias per Track (D21-3-1-2-MUST-NOT-048)
// "The same Track Alias MUST NOT be used by a publisher to refer to two different
// Tracks simultaneously in the same session." Every PUBLISH is accepted so the
// subscriptions are Established together.
Spec distinct_aliases_spec() {
    return spec("d21-concurrent-distinct-track-subscriptions",
        {{"D21-3-1-2-MUST-NOT-048", "d21-distinct-active-tracks-have-distinct-aliases"}},
        [](const Fixture&) { return observing(accepting_publishes()); },
        [](const View& view) -> Judgement {
            const auto records = publish_records(view);
            bool distinct_pair = false;
            for (std::size_t first = 0; first < records.size(); ++first) {
                for (std::size_t second = first + 1; second < records.size(); ++second) {
                    if (!simultaneous(records[first], records[second]) ||
                        records[first].track == records[second].track) continue;
                    if (records[first].alias == records[second].alias) return {true, false};
                    distinct_pair = true;
                }
            }
            if (!view.window_ended()) return {false, std::nullopt};
            return {true, distinct_pair ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

// ---- Section 2.5 lines 885-890: one Full Track Name per content (D21-2-5-MUST-032) ---
// Tracks published together whose Objects differ at a Location they share have
// different content, so they must have different Full Track Names. A pair that
// reuses a name for such Objects fails; a pair with distinct names and different
// content shows the rule holding.
const Object* object_at(const std::vector<Object>& objects, const Object& wanted) {
    for (const auto& object : objects)
        if (object.data && object.group == wanted.group && object.id == wanted.id) return &object;
    return nullptr;
}

std::optional<bool> contents_differ(const std::vector<Object>& left, const std::vector<Object>& right) {
    std::optional<bool> result;
    for (const auto& object : left) {
        if (!object.data) continue;
        const auto* other = object_at(right, object);
        if (!other) continue;
        if (object.payload != other->payload) return true;
        result = false;
    }
    return result;
}

Spec distinct_tracks_spec() {
    return spec("d21-publish-distinct-tracks-in-one-scope",
        {{"D21-2-5-MUST-032", "d21-different-track-content-has-distinct-full-track-name"}},
        [](const Fixture&) { return observing(accepting_publishes()); },
        [](const View& view) -> Judgement {
            const auto records = publish_records(view);
            bool distinct_content = false;
            for (std::size_t first = 0; first < records.size(); ++first) {
                for (std::size_t second = first + 1; second < records.size(); ++second) {
                    if (!simultaneous(records[first], records[second])) continue;
                    const auto differ = contents_differ(delivered_objects(view, records[first].alias),
                                                        delivered_objects(view, records[second].alias));
                    if (!differ || !*differ) continue;
                    if (records[first].track == records[second].track) return {true, false};
                    distinct_content = true;
                }
            }
            if (!view.window_ended()) return {false, std::nullopt};
            return {true, distinct_content ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

// ---- Section 3.1.1 lines 1068-1071: no Objects for a request that ends in error ---------
// The runner rejects the publisher's PUBLISH. Objects sent before the publisher could
// know are allowed (Section 3.1, lines 1038-1043), so only what the publisher starts
// after it has visibly reacted counts: a new Subgroup stream or datagram for the
// rejected Track Alias that arrives after the publisher ended its side of the
// rejected request stream with FIN or reset.
struct Production {
    std::optional<std::size_t> first;
    std::optional<std::size_t> last_start;
};

Production production_of(const View& view, std::uint64_t alias) {
    Production result;
    const auto note = [&](std::size_t event) {
        if (!result.first || event < *result.first) result.first = event;
        if (!result.last_start || event > *result.last_start) result.last_start = event;
    };
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        const auto parsed = parse_subgroup(record.bytes);
        if (parsed.header && parsed.alias == alias) note(record.first_event);
    }
    for (const auto& datagram : view.datagrams())
        if (parse_datagram_object(datagram, alias)) note(datagram.event);
    return result;
}

Spec rejected_publish_spec(const char* scenario, bool after_object) {
    return spec(scenario, {{"D21-3-1-1-MUST-NOT-047", "d21-error-subscription-has-no-object-delivery"}},
        [after_object](const Fixture&) {
            RawProbeCourtesy courtesy;
            courtesy.publish = after_object ? RawProbePublishResponse::RejectAfterObject
                                            : RawProbePublishResponse::Reject;
            return observing(courtesy);
        },
        [after_object](const View& view) -> Judgement {
            bool reacted = false;
            for (const auto& record : publish_records(view)) {
                if (record.response != RawProbeCourtesyKind::PublishError) continue;
                const auto& stream = *view.stream(record.stream);
                // The publisher's reaction: its own FIN or reset after the rejection was sent.
                std::optional<std::size_t> reaction;
                if (stream.fin_event && *stream.fin_event >= record.response_event) reaction = stream.fin_event;
                if (stream.reset_event && *stream.reset_event >= record.response_event)
                    reaction = earliest(reaction, stream.reset_event);
                const auto production = production_of(view, record.alias);
                // The second scenario needs the publisher seen producing before the rejection.
                if (after_object && (!production.first || *production.first >= record.response_event)) continue;
                if (!reaction) continue;
                reacted = true;
                if (production.last_start && *production.last_start > *reaction) return {true, false};
            }
            if (!view.window_ended()) return {false, std::nullopt};
            return {true, reacted ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

// ---- Section 9.1.7 lines 3611-3618: REQUEST_UPDATE credit (D21-9-1-7-MUST-NOT-316) -----
// The runner announces MAX_REQUEST_UPDATES (Option 0x08), accepts the publisher's
// PUBLISH and answers none of its REQUEST_UPDATEs, so every update stays outstanding
// and the count per stream is exactly what the publisher sent.
constexpr std::uint64_t kUpdateLimit = 2;

std::size_t streams_at_limit(const std::vector<PublishRecord>& records) {
    return static_cast<std::size_t>(std::count_if(records.begin(), records.end(), [](const PublishRecord& record) {
        return record.updates == kUpdateLimit;
    }));
}

bool over_limit(const std::vector<PublishRecord>& records) {
    return std::any_of(records.begin(), records.end(), [](const PublishRecord& record) {
        return record.updates > kUpdateLimit;
    });
}

Spec update_credit_spec(const char* scenario, bool per_stream) {
    return spec(scenario, {{"D21-9-1-7-MUST-NOT-316", "d21-publisher-update-outstanding-limit"}},
        [](const Fixture&) {
            auto definition = observing(accepting_publishes());
            // SETUP (0x2f00) with the numeric option MAX_REQUEST_UPDATES.
            definition.setup_bytes = bytes_of({0xaf, 0, 0, 2, 8, static_cast<unsigned>(kUpdateLimit)});
            // Updates are never answered, which can make a publisher give up.
            definition.publisher_exit_is_evidence = true;
            return definition;
        },
        [per_stream](const View& view) -> Judgement {
            const auto records = publish_records(view);
            if (over_limit(records)) return {true, false};
            if (!view.window_ended()) return {false, std::nullopt};
            // The limit is exercised only once a stream has spent it; for the per-stream
            // scenario two streams each spend all of it, so the limit is not shared.
            const auto spent = streams_at_limit(records);
            return {true, spent >= (per_stream ? 2u : 1u) ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

// "A value of 0 means the endpoint does not limit REQUEST_UPDATE concurrency. If not
// present, the default value is 0": no credit is ever exhausted, so the publisher may
// leave several updates outstanding on one stream.
Spec unlimited_updates_spec() {
    return spec("d21-publisher-update-zero-unlimited",
        {{"D21-9-1-7-MUST-NOT-316", "d21-publisher-update-outstanding-limit"}},
        [](const Fixture&) {
            auto definition = observing(accepting_publishes());
            definition.setup_bytes = bytes_of({0xaf, 0, 0, 2, 8, 0});
            definition.publisher_exit_is_evidence = true;
            return definition;
        },
        [](const View& view) -> Judgement {
            if (!view.window_ended()) return {false, std::nullopt};
            const auto records = publish_records(view);
            // Two outstanding updates show the publisher did not treat zero as no credit
            // or as a single credit.
            const bool several = std::any_of(records.begin(), records.end(), [](const PublishRecord& record) {
                return record.updates >= 2;
            });
            return {true, several ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

// ---- Section 9.2 lines 3650-3652: a client GOAWAY has an empty URI (D21-9-2-MUST-318) ----
// The publisher is the client. Its GOAWAY may sit on its control stream or on a request
// stream; each scenario reads one place. No stimulus can make a publisher send one, so
// the contexts only observe.
struct ObservedGoaway {
    bool empty_uri{false};
};

std::optional<ObservedGoaway> goaway_in(const Frame& frame) {
    if (frame.type != kGoaway) return std::nullopt;
    wire::Cursor body(frame.body);
    const auto length = read_vi(body);
    if (!length) return std::nullopt;
    return ObservedGoaway{*length == 0};
}

std::vector<ObservedGoaway> control_goaways(const View& view) {
    std::vector<ObservedGoaway> result;
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        const auto frames = view.frames(record);
        // The control stream is the one whose first message is SETUP (type 0x2f00).
        if (frames.empty() || frames.front().type != 0x2f00) continue;
        for (std::size_t index = 1; index < frames.size(); ++index)
            if (const auto goaway = goaway_in(frames[index])) result.push_back(*goaway);
    }
    return result;
}

std::vector<ObservedGoaway> request_goaways(const View& view) {
    std::vector<ObservedGoaway> result;
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 0u) continue;
        for (const auto& frame : view.frames(record))
            if (const auto goaway = goaway_in(frame)) result.push_back(*goaway);
    }
    return result;
}

Spec client_goaway_spec(const char* scenario, bool on_control_stream) {
    return spec(scenario, {{"D21-9-2-MUST-318", "d21-client-goaway-empty-uri"}},
        [](const Fixture&) { return observing(accepting_publishes()); },
        [on_control_stream](const View& view) -> Judgement {
            const auto goaways = on_control_stream ? control_goaways(view) : request_goaways(view);
            if (std::any_of(goaways.begin(), goaways.end(), [](const auto& goaway) { return !goaway.empty_uri; }))
                return {true, false};
            if (!view.window_ended()) return {false, std::nullopt};
            return {true, goaways.empty() ? std::nullopt : std::optional<bool>{true}};
        },
        true);
}

}  // namespace

std::vector<Spec> residual_publisher_specs() {
    std::vector<Spec> result;
    result.push_back(distinct_aliases_spec());
    result.push_back(distinct_tracks_spec());
    result.push_back(rejected_publish_spec("d21-reject-publish-before-object-production", false));
    result.push_back(rejected_publish_spec("d21-rejected-subscribe-no-delivery", true));
    result.push_back(update_credit_spec("d21-publisher-update-credit-limit", false));
    result.push_back(update_credit_spec("d21-publisher-update-credit-per-stream", true));
    result.push_back(unlimited_updates_spec());
    result.push_back(client_goaway_spec("d21-publisher-client-goaway-control", true));
    result.push_back(client_goaway_spec("d21-publisher-client-goaway-request", false));
    return result;
}

}  // namespace moq::interop::scenarios::d21c
