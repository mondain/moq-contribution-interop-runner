// Draft-21 contribution rows driven by requests the runner writes: concurrent
// subscriptions, conjunctive filters, Subgroup membership, fill fetch streams
// and SUBSCRIBE_TRACKS. See draft21_contribution_residual.cpp for the fixture
// contract these rows share.

#include "draft21_contribution_support.h"
#include "draft21_contribution_residual_internal.h"

#include "moq/interop/wire/draft21/publish.h"

#include <algorithm>
#include <set>

namespace moq::interop::scenarios::d21c {
namespace {

using namespace shared;
using namespace residual;

constexpr std::uint64_t kTargetGroup = 0;
constexpr std::uint64_t kTargetObject = 1;
constexpr std::uint64_t kFetchHeaderType = 0x5;
constexpr std::uint64_t kPublishSkipped = 0xf;
constexpr std::uint64_t kSubscribeTracks = 0x51;

// LOCATION_FILTER (0x21, Section 9.20.10): Start Group/Object, End Group delta and
// End Object. The range is inclusive (Section 3.3.1); an omitted End Object is the
// separate all-objects-of-the-End-Group form, while an End Object of 0 ends at
// Object 0 of the End Group.
Param location_range(std::uint64_t start_group, std::uint64_t start_object,
                     std::uint64_t end_group_delta, std::uint64_t end_object) {
    auto value = location_pair(start_group, start_object);
    put_vi(value, end_group_delta);
    put_vi(value, end_object);
    return param_lp(0x21, value);
}

// A single-Group range [first, last] in one Group.
Param bounded_filter(std::uint64_t group, std::uint64_t first, std::uint64_t last) {
    return location_range(group, first, 0, last);
}

// Follow-up written on the stream opened by write `base` once its
// SUBSCRIBE_OK is complete.
RawProbeWrite follow_up(Bytes bytes, std::size_t base, bool fin = false) {
    RawProbeWrite write{RawProbeChannel::NewBidi, std::move(bytes), fin, base};
    write.peer_response_ready = subscribe_ok_ready;
    return write;
}

// ---- Section 3.1: one copy per matching subscription (D21-3-1-MUST-041) -----
// Both subscriptions ask for exactly Group 0, Object 1 (kTargetGroup, kTargetObject). The publisher chooses
// the Track Aliases, so the same stimulus is judged under whichever alias
// assignment it produced; a context whose assignment is not the scenario's
// is not scored.
std::size_t copies_of_target(const std::vector<Object>& objects) {
    return static_cast<std::size_t>(std::count_if(objects.begin(), objects.end(), [](const Object& object) {
        return object.data && object.group == kTargetGroup && object.id == kTargetObject;
    }));
}

Spec concurrent_subscription_spec(const char* scenario, bool shared_alias) {
    return spec(scenario, {{"D21-3-1-MUST-041", "d21-object-per-matching-subscription"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            // Section 3.1: concurrent subscriptions to one Track are allowed.
            definition.writes.push_back(request_write(subscribe_frame(1, fixture,
                {param_u8(0x10, 1), bounded_filter(kTargetGroup, kTargetObject, kTargetObject)})));
            definition.writes.push_back(request_write(subscribe_frame(3, fixture,
                {param_u8(0x10, 1), bounded_filter(kTargetGroup, kTargetObject, kTargetObject)})));
            return definition;
        },
        [shared_alias](const View& view) -> Judgement {
            if (rejected(view, 0) || rejected(view, 1)) return {true, std::nullopt};
            const auto first = alias_of(view, 0);
            const auto second = alias_of(view, 1);
            if (!first || !second) return {view.close().has_value(), std::nullopt};
            if ((*first == *second) != shared_alias) return {true, std::nullopt};
            const auto on_first = copies_of_target(delivered_objects(view, *first));
            const auto on_second = shared_alias ? on_first : copies_of_target(delivered_objects(view, *second));
            // The Object must be sent once for each matching subscription, "even
            // when those subscriptions share the same Track Alias".
            const bool over = shared_alias ? on_first > 2 : (on_first > 1 || on_second > 1);
            if (over) return {true, false};
            if (!view.window_ended()) return {false, std::nullopt};
            const bool complete = shared_alias ? on_first == 2 : (on_first == 1 && on_second == 1);
            if (complete) return {true, true};
            // Nothing delivered means the fixture was unavailable, not a missing copy.
            const auto delivered = shared_alias ? on_first : on_first + on_second;
            return {true, delivered == 0 ? std::nullopt : std::optional<bool>{false}};
        },
        true);
}

// ---- Section 3.3.3: Forward AND Location filter AND Range filters ---------------
// Section 3.3.3 (lines 1264-1272): "The publisher MUST forward only objects that
// pass all filters. Pass = Forward AND Location Filters AND Range Filters". Two
// subscriptions to the track exercise the conjunction:
//   Request 1: FORWARD=0 with a Location filter that matches every Object. Forward
//              is a filter too, so nothing may be sent for it.
//   Request 3: FORWARD=1 with a Location filter from {0, 0} to {1, 0} inclusive and,
//              when the publisher allows Range Filters (MAX_FILTER_RANGES, Section
//              9.1.6, default 0 so none may be sent), an OBJECTID_FILTER for Object
//              1. Of Objects {0,0} {0,1} {1,0} {1,1} the Location filter passes the
//              first three and the OBJECTID_FILTER the second and fourth, so only
//              {0,1} passes both and {1,1} fails the Location filter alone.
// Both subscriptions may share one Track Alias, in which case a stray Object cannot
// be attributed to a subscription; it is outside every subscription's pass set
// either way, so it still shows a violation.
// OBJECTID_FILTER (0x26, Sections 8.6 and 9.20.12): SetID 0 and the single
// inclusive Range [first, last], whose End is delta encoded from its Start.
Param object_id_range(std::uint64_t first, std::uint64_t last) {
    Bytes value;
    value.push_back(std::byte{0});
    put_vi(value, first);
    put_vi(value, last - first);
    return param_lp(0x26, value);
}

bool range_filters_allowed(std::optional<std::uint64_t> max_filter_ranges) {
    return max_filter_ranges && *max_filter_ranges >= 1;
}

Spec filter_conjunction_spec() {
    return spec("d21-forward-location-and-range-filter-conjunction",
        {{"D21-3-3-3-MUST-066", "d21-only-objects-passing-all-filter-categories"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            definition.writes.push_back(request_write(subscribe_frame(1, fixture,
                {param_u8(0x10, 0), location_range(0, 0, 1, 1)})));
            RawProbeWrite filtered = request_write({});
            filtered.prepare_bytes = [fixture](const RawProbeGateInput& input) -> std::optional<Bytes> {
                const auto setup = peer_setup_from_events(input.events);
                if (!setup) return std::nullopt;
                std::vector<Param> params{param_u8(0x10, 1), location_range(0, 0, 1, 0)};
                if (range_filters_allowed(setup_option_value(*setup, 6))) params.push_back(object_id_range(1, 1));
                return subscribe_frame(3, fixture, params);
            };
            definition.writes.push_back(std::move(filtered));
            return definition;
        },
        [](const View& view) -> Judgement {
            if (rejected(view, 0) || rejected(view, 1)) return {true, std::nullopt};
            const auto forward_off = alias_of(view, 0);
            const auto filtered = alias_of(view, 1);
            if (!forward_off || !filtered) return {view.close().has_value(), std::nullopt};
            const bool ranges = range_filters_allowed(view.peer_option(6));
            const auto passes = [ranges](const Object& object) {
                const bool location = object.group == 0 || (object.group == 1 && object.id == 0);
                return location && (!ranges || object.id == 1);
            };
            std::size_t passing = 0;
            for (const auto& object : delivered_objects(view, *filtered)) {
                if (!object.data) continue;
                if (!passes(object)) return {true, false};
                ++passing;
            }
            // With its own alias the FORWARD=0 subscription must have delivered nothing.
            if (*forward_off != *filtered)
                for (const auto& object : delivered_objects(view, *forward_off))
                    if (object.data) return {true, false};
            if (!view.window_ended()) return {false, std::nullopt};
            return {true, passing != 0 ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

// ---- Section 2.2: one Subgroup per stream (D21-2-2-MUST-NOT-017) -------------
std::optional<int> membership(const ObjectRecord& object) {
    if (object.group != 0 || object.status) return std::nullopt;
    if (object.object <= 4) return 0;
    if (object.object <= 9) return 1;
    return std::nullopt;
}

Spec mixed_subgroup_spec() {
    return spec("d21-subscribe-multiple-subgroups",
        {{"D21-2-2-MUST-NOT-017", "d21-no-mixed-subgroups-on-subscription-stream"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            // The whole of Group 0: Start {0, 0} with an End Group delta of 0 and End
            // Object omitted (three fields), which includes every Object of the End
            // Group (Section 9.20.10). Two fields would instead mean the Next Object.
            definition.writes.push_back(request_write(subscribe_frame(1, fixture,
                {param_u8(0x10, 1), param_lp(0x21, [] {
                    auto value = location_pair(0, 0);
                    put_vi(value, 0);
                    return value;
                }())})));
            return definition;
        },
        [](const View& view) -> Judgement {
            if (rejected(view, 0)) return {true, std::nullopt};
            const auto alias = alias_of(view, 0);
            if (!alias) return {view.close().has_value(), std::nullopt};
            // A Subgroup stream carries a single Subgroup ID, so two cells on one stream
            // are only a violation when something else shows the publisher puts them in
            // different Subgroups (the cells are the fixture's assumption, not the wire's):
            // a stream holding just one cell, or two distinct Subgroup IDs among streams.
            std::set<int> cells_seen;
            std::set<std::uint64_t> subgroup_ids;
            bool stream_with_both = false;
            bool stream_with_one = false;
            for (const auto& stream : subgroup_streams(view, *alias)) {
                std::set<int> cells;
                for (const auto& object : stream.parsed.objects)
                    if (const auto cell = membership(object)) cells.insert(*cell);
                if (cells.empty()) continue;
                if (stream.parsed.subgroup) subgroup_ids.insert(*stream.parsed.subgroup);
                (cells.size() > 1 ? stream_with_both : stream_with_one) = true;
                cells_seen.insert(cells.begin(), cells.end());
            }
            if (stream_with_both && (stream_with_one || subgroup_ids.size() > 1)) return {true, false};
            if (!view.window_ended()) return {false, std::nullopt};
            // Both Subgroups must have been observed, each on streams of its own, for the
            // split to be exercised and kept.
            return {true, cells_seen.size() == 2 && !stream_with_both ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

// ---- Section 3.4: fill fetch streams ------------------------------------------
// A fill is only owed when its range is not empty and does not start after the
// Largest Object, so every fill context first subscribes plainly and waits for
// an Object. The publisher then has content and reports a Largest Object, and
// the second SUBSCRIBE asks for the whole track so far: FILL_PARAMETERS (0x23,
// Section 9.20.16) whose nested LOCATION_FILTER is zero-length, so "the fill
// range is the entire track up to Largest Object" (Section 3.4, lines 1282-1288).
Param fill_whole_track() {
    return param_lp(0x23, encode_params({param_lp(0x21, {})}));
}

// LARGEST_OBJECT (parameter 0x9) of a SUBSCRIBE_OK. A Message Parameter is not a
// Key-Value-Pair: a Location value is "two consecutive varints (Group, Object)" with
// no Length (Section 9.20, lines 4732-4735), and SUBSCRIBE_OK carries only EXPIRES
// (8) and LARGEST_OBJECT (9), so nothing but EXPIRES can precede it.
std::optional<std::pair<std::uint64_t, std::uint64_t>> largest_object(const View& view, std::size_t write) {
    const auto frames = view.write_frames(write);
    if (frames.empty() || frames.front().type != kSubscribeOk) return std::nullopt;
    wire::Cursor body(frames.front().body);
    if (!read_vi(body)) return std::nullopt;
    const auto count = read_vi(body);
    if (!count) return std::nullopt;
    std::uint64_t type = 0;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto delta = read_vi(body);
        if (!delta) return std::nullopt;
        type += *delta;
        if (type == 8) {
            if (!read_vi(body)) return std::nullopt;
        } else if (type == 9) {
            const auto group = read_vi(body);
            const auto object = group ? read_vi(body) : std::nullopt;
            if (!group || !object) return std::nullopt;
            return std::make_pair(*group, *object);
        } else {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

struct FillStream {
    transport::StreamId id{0};
    const StreamRecord* record{nullptr};
    std::size_t header_size{0};
};

// A fill fetch stream is a unidirectional stream beginning with a FETCH_HEADER
// (Section 11.4.1) naming the Request ID of the SUBSCRIBE or REQUEST_UPDATE
// that opened it.
std::optional<FillStream> fill_stream(const View& view, std::uint64_t request_id) {
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        wire::Cursor cursor(record.bytes);
        const auto type = read_vi(cursor);
        const auto request = type ? read_vi(cursor) : std::nullopt;
        if (!type || !request || *type != kFetchHeaderType || *request != request_id) continue;
        return FillStream{id, &record, cursor.offset()};
    }
    return std::nullopt;
}

bool fill_open(const View& view, std::uint64_t request_id) {
    const auto fill = fill_stream(view, request_id);
    return fill && !fill->record->fin && !fill->record->reset;
}

// The plain subscription (Request ID 1) has begun delivering: a Subgroup stream for
// its Track Alias has started or an Object Datagram has arrived.
bool warmed_up(const View& view) {
    const auto alias = alias_of(view, 0);
    if (!view.valid() || !alias) return false;
    return !subgroup_streams(view, *alias).empty() || !delivered_objects(view, *alias).empty();
}

RawProbeWrite after_warm_up(Bytes bytes, bool fin = false) {
    RawProbeWrite write = request_write(std::move(bytes), fin);
    write.evidence_ready = [](const RawProbeGateInput& input) {
        return warmed_up(View(input.prior_writes, input.events));
    };
    return write;
}

Spec failed_fill_spec() {
    // One stimulus feeds both rows: the failure is signalled only by the stream.
    const auto judge = [](const View& view) -> Judgement {
        if (rejected(view, 0) || rejected(view, 1)) return {true, std::nullopt};
        const auto fill = fill_stream(view, 3);
        if (!fill) return {view.close().has_value(), std::nullopt};
        // An empty fill (nothing at or before the Largest Object) opens no stream.
        if (!largest_object(view, 1)) return {true, std::nullopt};
        const auto& record = *fill->record;
        if (record.reset) {
            // Reset directly after the FETCH_HEADER, before any fill Object.
            return {true, record.bytes.size() == fill->header_size ? std::optional<bool>{true} : std::nullopt};
        }
        // A FIN is a completed fill and Objects mean the fill did not fail early;
        // neither says the failure was known before the first Object.
        if (record.fin) return {true, std::nullopt};
        return {false, std::nullopt};
    };
    return spec("d21-fill-fails-before-first-object",
        {{"D21-3-4-1-MUST-068", "d21-fill-failure-opens-fetch-header-stream"},
         {"D21-3-4-1-MUST-069", "d21-fill-failure-resets-after-fetch-header"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            definition.writes.push_back(request_write(subscribe_frame(1, fixture, {param_u8(0x10, 1)})));
            definition.writes.push_back(after_warm_up(subscribe_frame(3, fixture,
                {param_u8(0x10, 1), fill_whole_track()})));
            return definition;
        },
        judge, false);
}

Spec cancelled_fill_spec() {
    return spec("d21-cancel-subscription-with-concurrent-fill-streams",
        {{"D21-3-4-1-MUST-067", "d21-cancel-subscription-resets-all-fill-streams"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            // Fill streams stay open while the runner holds back stream credit, so there is
            // something to reset when the subscription is cancelled.
            definition.initial_peer_uni_stream_data = 64;
            definition.hold_uni_stream_credit = true;
            definition.writes.push_back(request_write(subscribe_frame(1, fixture, {param_u8(0x10, 1)})));
            // The initial fill (Request ID 3) and one opened by a REQUEST_UPDATE
            // (Request ID 5) are both open before the subscription is cancelled.
            definition.writes.push_back(after_warm_up(subscribe_frame(3, fixture,
                {param_u8(0x10, 1), fill_whole_track()})));
            definition.writes.push_back(follow_up(
                request_update_frame(5, {param_u8(0x10, 1), fill_whole_track()}), 1, true));
            RawProbeWrite cancel{RawProbeChannel::NewBidi, {}, false, 1, {}};
            cancel.operation = RawProbeOperation::StopSending;
            cancel.application_error = 1;
            cancel.evidence_ready = [](const RawProbeGateInput& input) {
                const View view(input.prior_writes, input.events);
                return view.valid() && fill_open(view, 3) && fill_open(view, 5);
            };
            definition.writes.push_back(std::move(cancel));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto marker = view.write_event(3);
            if (!marker) return {view.close().has_value(), std::nullopt};
            // Fill streams still open when the cancellation was sent.
            std::vector<const StreamRecord*> targets;
            for (const std::uint64_t request : {3u, 5u}) {
                const auto fill = fill_stream(view, request);
                if (!fill) continue;
                const auto& record = *fill->record;
                if (record.first_event >= *marker) continue;
                if ((record.fin_event && *record.fin_event < *marker) ||
                    (record.reset_event && *record.reset_event < *marker)) continue;
                targets.push_back(&record);
            }
            if (targets.empty()) return {view.close().has_value() || view.window_ended(), std::nullopt};
            bool all_reset = true;
            for (const auto* record : targets) {
                // A FIN may already have been committed before the cancellation arrived.
                if (record->fin) return {true, std::nullopt};
                all_reset = all_reset && record->reset;
            }
            if (all_reset) return {true, true};
            // "MUST reset any open fill fetch streams": one left open is a failure.
            if (view.window_ended()) return {true, false};
            return {false, std::nullopt};
        },
        true);
}

// ---- Section 4.1 lines 1536-1545: PUBLISH_SKIPPED -----------------------------------
// SUBSCRIBE_TRACKS (0x51, Section 9.18) for the fixture namespace, with the
// publisher allowed one request stream: it can open one PUBLISH, and must say
// PUBLISH_SKIPPED (0xF, Section 9.19) on the SUBSCRIBE_TRACKS response stream for
// any other track it cannot start. The runner then rejects the one PUBLISH and
// closes that stream, handing the publisher capacity again, and watches whether a
// skipped track is published afterwards. "The Publisher MUST NOT send a PUBLISH
// for a Track for a given SUBSCRIBE_TRACKS after PUBLISH_SKIPPED has been sent."
struct SkippedTrack {
    TrackName track;
    std::size_t event{0};
};

// PUBLISH_SKIPPED messages on the SUBSCRIBE_TRACKS response stream (write 0); the
// suffix extends the prefix the request carried.
std::vector<SkippedTrack> skipped_tracks(const View& view, const Namespace& prefix) {
    std::vector<SkippedTrack> result;
    for (const auto& frame : view.write_frames(0)) {
        if (frame.type != kPublishSkipped) continue;
        wire::Cursor body(frame.body);
        auto suffix = read_namespace(body);
        const auto length = suffix ? read_vi(body) : std::nullopt;
        const auto name = length ? read_n(body, static_cast<std::size_t>(*length)) : std::nullopt;
        if (!suffix || !name) continue;
        TrackName track{prefix, Bytes(name->begin(), name->end())};
        track.track_namespace.insert(track.track_namespace.end(), suffix->begin(), suffix->end());
        result.push_back({std::move(track), frame.event});
    }
    return result;
}

// Tracks the publisher announced with PUBLISH on a request stream it opened.
struct PublishedTrack {
    TrackName track;
    std::size_t event{0};
};

std::vector<PublishedTrack> published_tracks(const View& view) {
    std::vector<PublishedTrack> result;
    for (const auto& record : publish_records(view)) result.push_back({record.track, record.first_event});
    return result;
}

Spec skipped_publish_spec() {
    return spec("d21-subscribe-tracks-publish-skipped-then-capacity-recovers",
        {{"D21-4-1-MUST-NOT-084", "d21-skipped-publish-not-later-sent-for-same-attempt"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            definition.initial_peer_bidi_streams = 1;
            // The stream limit can leave a publisher waiting for credit until it gives up.
            definition.publisher_exit_is_evidence = true;
            definition.peer_request_ready = [](auto input) {
                wire::Cursor cursor(input);
                return std::holds_alternative<wire::draft21::PublishMessage>(wire::draft21::decode_publish(cursor));
            };
            Bytes body;
            put_vi(body, 1);
            put_namespace(body, fixture.track_namespace);
            put_vi(body, 0);
            definition.writes.push_back(request_write(frame(kSubscribeTracks, body)));
            // REQUEST_ERROR UNINTERESTED (0x20, Section 12.3) with no retry and no reason,
            // then FIN: the request is over and its stream is returned to the publisher.
            Bytes error;
            put_vi(error, 0x20);
            put_vi(error, 0);
            put_vi(error, 0);
            RawProbeWrite reject{RawProbeChannel::PeerBidi, frame(kRequestError, error), true};
            reject.evidence_ready = [namespace_fields = fixture.track_namespace](const RawProbeGateInput& input) {
                const View view(input.prior_writes, input.events);
                return view.valid() && !skipped_tracks(view, namespace_fields).empty();
            };
            definition.writes.push_back(std::move(reject));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto frames = view.write_frames(0);
            if (!frames.empty() && frames.front().type == kRequestError) return {true, std::nullopt};
            // The prefix is the namespace of the SUBSCRIBE_TRACKS this context sent.
            const auto request = recover_fixture(view.write_bytes(0));
            if (!request) return {view.close().has_value(), std::nullopt};
            const auto skipped = skipped_tracks(view, request->track_namespace);
            const auto published = published_tracks(view);
            for (const auto& skip : skipped)
                for (const auto& publish : published)
                    if (publish.track == skip.track && publish.event > skip.event) return {true, false};
            if (!view.window_ended()) return {false, std::nullopt};
            // Pass needs a track that was skipped and the capacity hand-back to have happened.
            return {true, !skipped.empty() && view.write_event(1) ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

}  // namespace

std::vector<Spec> residual_subscription_specs() {
    std::vector<Spec> result;
    result.push_back(concurrent_subscription_spec("d21-overlapping-subscriptions-shared-alias", true));
    result.push_back(concurrent_subscription_spec("d21-overlapping-subscriptions-distinct-aliases", false));
    result.push_back(filter_conjunction_spec());
    result.push_back(mixed_subgroup_spec());
    result.push_back(failed_fill_spec());
    result.push_back(cancelled_fill_spec());
    result.push_back(skipped_publish_spec());
    return result;
}

}  // namespace moq::interop::scenarios::d21c
