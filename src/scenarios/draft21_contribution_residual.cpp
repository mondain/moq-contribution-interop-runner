// Draft-21 contribution profiles that close the last required rows the earlier
// slices could not induce with canned stimulus alone: delivery to concurrent
// subscriptions, conjunctive filters, Subgroup membership, fill fetch stream
// failure and cancellation. Every expectation cites draft-ietf-moq-transport-21.
//
// Fixture contract (this slice does not use the Group 7 contract of
// draft21_gap_a.h). The configured track holds Groups 0 and 1, each with Objects
// 0 and 1, delivered to every subscription that asks for them; the first Group of
// an LOC/CMAF GOP-per-Group source has exactly this shape. The Subgroup row
// (D21-2-2-MUST-NOT-017) instead needs a Group 0 whose Objects 0-4 form one
// Subgroup and Objects 5-9 another, which a one-Subgroup-per-Group source cannot
// provide. Every context also asks the runner to acknowledge a PUBLISH_NAMESPACE
// the publisher opens, as any subscriber that wants a publisher to keep serving
// requests would; this is not scored.

#include "draft21_contribution_support.h"

#include "moq/interop/wire/draft21/publish.h"
#include "moq/interop/wire/draft21/token.h"

#include <map>
#include <set>

namespace moq::interop::scenarios::d21c {
namespace {

constexpr std::uint64_t kTargetGroup = 0;
constexpr std::uint64_t kTargetObject = 1;
constexpr std::uint64_t kFetchHeaderType = 0x5;
constexpr std::uint64_t kPublish = 0x1d;
constexpr std::uint64_t kPublishSkipped = 0xf;
constexpr std::uint64_t kSubscribeTracks = 0x51;

// ---- helpers ---------------------------------------------------------------
Bytes location_pair(std::uint64_t group, std::uint64_t object) {
    Bytes result;
    put_vi(result, group);
    put_vi(result, object);
    return result;
}

// LOCATION_FILTER (0x21, Section 9.20.10): Start Group/Object, then End Group
// delta and End Object. End Object 0 is open to the end of the End Group.
Param bounded_filter(std::uint64_t group, std::uint64_t first, std::uint64_t last) {
    auto value = location_pair(group, first);
    put_vi(value, 0);
    put_vi(value, last);
    return param_lp(0x21, value);
}

RawProbeDefinition residual_definition() {
    // PUBLISH_NAMESPACE announcements are acknowledged by base_definition().
    return base_definition("");
}

RawProbeWrite request_write(Bytes bytes, bool fin = false) {
    return {RawProbeChannel::NewBidi, std::move(bytes), fin};
}

// Follow-up written on the stream opened by write `base` once its
// SUBSCRIBE_OK is complete.
RawProbeWrite follow_up(Bytes bytes, std::size_t base, bool fin = false) {
    RawProbeWrite write{RawProbeChannel::NewBidi, std::move(bytes), fin, base};
    write.peer_response_ready = subscribe_ok_ready;
    return write;
}

struct Object {
    std::uint64_t group{0};
    std::uint64_t id{0};
    bool data{true};  // false for an Object Status
    transport::StreamId stream{0};
    bool datagram{false};
    Bytes payload;
};

struct SubgroupStream {
    transport::StreamId stream{0};
    std::uint64_t alias{0};
    std::uint64_t group{0};
    std::optional<std::uint64_t> subgroup_id;
    std::vector<Object> objects;
};

// Section 11.3.1: Subgroup header and the Objects received so far. A property
// block is skipped by its length; its contents do not matter to these rows.
std::optional<SubgroupStream> parse_subgroup_stream(transport::StreamId id, const StreamRecord& record) {
    wire::Cursor cursor(record.bytes);
    const auto type = read_vi(cursor);
    if (!type || *type >= 128 || (*type & 0x10u) == 0 || (*type & 0x06u) == 0x06u) return std::nullopt;
    SubgroupStream result;
    result.stream = id;
    const auto alias = read_vi(cursor);
    const auto group = alias ? read_vi(cursor) : std::nullopt;
    if (!alias || !group) return std::nullopt;
    result.alias = *alias;
    result.group = *group;
    const unsigned mode = static_cast<unsigned>((*type & 0x06u) >> 1u);
    if (mode == 2u) {
        const auto subgroup = read_vi(cursor);
        if (!subgroup) return std::nullopt;
        result.subgroup_id = *subgroup;
    } else if (mode == 0u) {
        result.subgroup_id = 0;
    }
    if ((*type & 0x20u) == 0u && !read_n(cursor, 1)) return std::nullopt;
    std::optional<std::uint64_t> previous;
    while (cursor.remaining() != 0) {
        auto local = cursor;
        const auto delta = read_vi(local);
        if (!delta) break;
        if ((*type & 0x01u) != 0u) {
            const auto length = read_vi(local);
            if (!length || *length > 65535 || !read_n(local, static_cast<std::size_t>(*length))) break;
        }
        const auto payload_length = read_vi(local);
        if (!payload_length) break;
        bool data = true;
        Bytes payload;
        if (*payload_length == 0) {
            if (!read_vi(local)) break;
            data = false;
        } else {
            if (*payload_length > kMaximumTotalBytes) break;
            const auto block = read_n(local, static_cast<std::size_t>(*payload_length));
            if (!block) break;
            payload.assign(block->begin(), block->end());
        }
        const std::uint64_t object = previous ? *previous + *delta + 1 : *delta;
        if (mode == 1u && !previous) result.subgroup_id = object;
        previous = object;
        result.objects.push_back({*group, object, data, id, false, std::move(payload)});
        cursor = local;
    }
    return result;
}

std::vector<SubgroupStream> subgroup_streams(const View& view, std::uint64_t alias) {
    std::vector<SubgroupStream> result;
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        auto parsed = parse_subgroup_stream(id, stream);
        if (parsed && parsed->alias == alias) result.push_back(std::move(*parsed));
    }
    return result;
}

// Section 11.2: Object Datagram. Only the fields needed to place the Object.
std::optional<Object> parse_datagram_object(const DatagramRecord& datagram, std::uint64_t alias) {
    wire::Cursor cursor(datagram.data);
    const auto flags = read_vi(cursor);
    const auto track = flags ? read_vi(cursor) : std::nullopt;
    const auto group = track ? read_vi(cursor) : std::nullopt;
    if (!flags || !track || !group || *track != alias) return std::nullopt;
    std::uint64_t object = 0;
    if ((*flags & 0x04u) == 0) {
        const auto value = read_vi(cursor);
        if (!value) return std::nullopt;
        object = *value;
    }
    // Without a Status the remainder of the datagram is the payload (Section 11.2).
    Bytes payload;
    const bool has_payload = (*flags & 0x20u) == 0;
    if (has_payload) {
        if ((*flags & 0x08u) == 0 && !read_n(cursor, 1)) return std::nullopt;
        if ((*flags & 0x01u) != 0) {
            const auto length = read_vi(cursor);
            if (!length || *length > 65535 || !read_n(cursor, static_cast<std::size_t>(*length))) return std::nullopt;
        }
        const auto rest = read_n(cursor, cursor.remaining());
        if (rest) payload.assign(rest->begin(), rest->end());
    }
    return Object{*group, object, has_payload, 0, true, std::move(payload)};
}

std::vector<Object> delivered_objects(const View& view, std::uint64_t alias) {
    std::vector<Object> result;
    for (const auto& stream : subgroup_streams(view, alias))
        for (const auto& object : stream.objects) result.push_back(object);
    for (const auto& datagram : view.datagrams())
        if (const auto object = parse_datagram_object(datagram, alias)) result.push_back(*object);
    return result;
}

// The Track Alias is the first field of SUBSCRIBE_OK (Section 9.7).
std::optional<std::uint64_t> alias_of(const View& view, std::size_t write) {
    const auto frames = view.write_frames(write);
    if (frames.empty() || frames.front().type != kSubscribeOk) return std::nullopt;
    wire::Cursor body(frames.front().body);
    return read_vi(body);
}

bool rejected(const View& view, std::size_t write) {
    const auto frames = view.write_frames(write);
    return !frames.empty() && frames.front().type == kRequestError;
}

Spec spec(const char* scenario, std::vector<RowBinding> rows, Builder build, Judge judge, bool window) {
    Spec result;
    result.scenario = scenario;
    result.rows = std::move(rows);
    result.build = std::move(build);
    result.judge = std::move(judge);
    result.window = window;
    return result;
}

// ---- Section 3.1: one copy per matching subscription (D21-3-1-MUST-041) -----
// Both subscriptions ask for exactly Group 7, Object 9. The publisher chooses
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
Param location_range(std::uint64_t start_group, std::uint64_t start_object,
                     std::uint64_t end_group_delta, std::uint64_t end_object) {
    auto value = location_pair(start_group, start_object);
    put_vi(value, end_group_delta);
    put_vi(value, end_object);
    return param_lp(0x21, value);
}

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
std::optional<int> membership(const Object& object) {
    if (object.group != 0 || !object.data) return std::nullopt;
    if (object.id <= 4) return 0;
    if (object.id <= 9) return 1;
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
                for (const auto& object : stream.objects)
                    if (const auto cell = membership(object)) cells.insert(*cell);
                if (cells.empty()) continue;
                if (stream.subgroup_id) subgroup_ids.insert(*stream.subgroup_id);
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

// LARGEST_OBJECT (parameter 0x9, two vi64 fields) of a SUBSCRIBE_OK.
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
struct TrackName {
    Namespace track_namespace;
    Bytes name;
    bool operator==(const TrackName& other) const {
        return track_namespace == other.track_namespace && name == other.name;
    }
};

std::optional<Namespace> read_namespace(wire::Cursor& cursor) {
    const auto count = read_vi(cursor);
    if (!count || *count > 32) return std::nullopt;
    Namespace result;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto length = read_vi(cursor);
        const auto field = length ? read_n(cursor, static_cast<std::size_t>(*length)) : std::nullopt;
        if (!field || field->empty()) return std::nullopt;
        result.emplace_back(field->begin(), field->end());
    }
    return result;
}

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
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 0u) continue;
        const auto frames = view.frames(record);
        if (frames.empty() || frames.front().type != kPublish) continue;
        wire::Cursor body(frames.front().body);
        if (!read_vi(body)) continue;
        auto name_space = read_namespace(body);
        const auto length = name_space ? read_vi(body) : std::nullopt;
        const auto name = length ? read_n(body, static_cast<std::size_t>(*length)) : std::nullopt;
        if (!name_space || !name) continue;
        result.push_back({{std::move(*name_space), Bytes(name->begin(), name->end())}, record.first_event});
    }
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

// ---- Requests the publisher opens ---------------------------------------------------
// The contexts below send nothing: the runner answers what the publisher opens
// (RawProbeCourtesy) and reads what the publisher puts on the wire.
constexpr std::uint64_t kPublishDone = 0xb;
constexpr std::uint64_t kRequestUpdate = 0x2;
constexpr std::uint64_t kGoaway = 0x10;
constexpr std::uint64_t kPublishNamespace = 0x6;

struct PublishRecord {
    transport::StreamId stream{0};
    std::size_t first_event{0};
    TrackName track;
    std::uint64_t alias{0};
    // The first of the publisher's FIN, its reset or its PUBLISH_DONE on the stream.
    std::optional<std::size_t> terminated;
    // The runner's volunteered answer to this PUBLISH.
    std::optional<RawProbeCourtesyKind> response;
    std::size_t response_event{0};
    std::size_t updates{0};  // REQUEST_UPDATE messages after the PUBLISH
};

std::optional<std::size_t> earliest(std::optional<std::size_t> left, std::optional<std::size_t> right) {
    if (!left) return right;
    if (!right) return left;
    return std::min(*left, *right);
}

std::vector<PublishRecord> publish_records(const View& view) {
    std::vector<PublishRecord> result;
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 0u) continue;
        const auto frames = view.frames(record);
        if (frames.empty() || frames.front().type != kPublish) continue;
        wire::Cursor body(frames.front().body);
        if (!read_vi(body)) continue;
        auto name_space = read_namespace(body);
        const auto length = name_space ? read_vi(body) : std::nullopt;
        const auto name = length ? read_n(body, static_cast<std::size_t>(*length)) : std::nullopt;
        const auto alias = name ? read_vi(body) : std::nullopt;
        if (!name_space || !name || !alias) continue;
        PublishRecord publish;
        publish.stream = id;
        publish.first_event = record.first_event;
        publish.track = {std::move(*name_space), Bytes(name->begin(), name->end())};
        publish.alias = *alias;
        publish.terminated = earliest(record.fin_event, record.reset_event);
        for (std::size_t index = 1; index < frames.size(); ++index) {
            if (frames[index].type == kPublishDone)
                publish.terminated = earliest(publish.terminated, frames[index].event);
            if (frames[index].type == kRequestUpdate) ++publish.updates;
        }
        for (const auto& write : view.courtesy_writes()) {
            if (write.stream_id != id || publish.response) continue;
            if (write.kind != RawProbeCourtesyKind::PublishOk && write.kind != RawProbeCourtesyKind::PublishError)
                continue;
            publish.response = write.kind;
            publish.response_event = write.event_count;
        }
        result.push_back(std::move(publish));
    }
    return result;
}

bool established(const PublishRecord& record) { return record.response == RawProbeCourtesyKind::PublishOk; }

// Both subscriptions were open at once: neither ended before the other began.
bool simultaneous(const PublishRecord& left, const PublishRecord& right) {
    return established(left) && established(right) &&
           (!left.terminated || *left.terminated > right.first_event) &&
           (!right.terminated || *right.terminated > left.first_event);
}

RawProbeDefinition observing(RawProbeCourtesy courtesy) {
    auto definition = base_definition("");
    definition.courtesy = courtesy;
    // Rejecting a PUBLISH can make a publisher give up.
    definition.publisher_exit_is_evidence = courtesy.publish == RawProbePublishResponse::Reject ||
                                            courtesy.publish == RawProbePublishResponse::RejectAfterObject;
    return definition;
}

RawProbeCourtesy accepting_publishes() {
    RawProbeCourtesy result;
    result.publish = RawProbePublishResponse::Accept;
    return result;
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
        const auto parsed = parse_subgroup_stream(id, record);
        if (parsed && parsed->alias == alias) note(record.first_event);
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

// ---- Section 8.9 lines 3336-3337: DELETE only after every USE_ALIAS was answered ----------
// "Senders MUST NOT send DELETE for an alias while any message using USE_ALIAS with that
// alias has not received a response." The runner holds back its answer to each message
// that uses an Alias, then answers it. A DELETE that reaches the runner before the
// answer to an earlier use was even written cannot have waited for it.
enum class TokenAction { Delete, Register, UseAlias };

struct AliasMessage {
    TokenAction action{TokenAction::UseAlias};
    std::uint64_t alias{0};
    transport::StreamId stream{0};
    std::size_t frame_index{0};
    std::size_t event{0};
};

// AUTHORIZATION TOKEN parameters (0x03) of a publisher request message.
std::vector<wire::draft21::Token> tokens_of(std::uint64_t type, const Bytes& body_bytes) {
    std::vector<wire::draft21::Token> result;
    wire::Cursor body(body_bytes);
    if (!read_vi(body)) return result;
    if (type == kPublishNamespace || type == kPublish) {
        auto name_space = read_namespace(body);
        if (!name_space) return result;
        if (type == kPublish) {
            const auto length = read_vi(body);
            if (!length || !read_n(body, static_cast<std::size_t>(*length)) || !read_vi(body)) return result;
        }
    } else if (type != kRequestUpdate) {
        return result;
    }
    const auto count = read_vi(body);
    if (!count) return result;
    std::uint64_t parameter = 0;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto delta = read_vi(body);
        if (!delta) return result;
        parameter += *delta;
        if ((parameter & 1u) == 0u) {
            if (!read_vi(body)) return result;
            continue;
        }
        const auto length = read_vi(body);
        const auto value = length ? read_n(body, static_cast<std::size_t>(*length)) : std::nullopt;
        if (!value) return result;
        if (parameter != 0x03) continue;
        const auto token = wire::draft21::decode_token(*value);
        if (const auto* decoded = std::get_if<wire::draft21::Token>(&token)) result.push_back(*decoded);
    }
    return result;
}

std::vector<AliasMessage> alias_messages(const View& view) {
    std::vector<AliasMessage> result;
    for (const auto& [id, record] : view.streams()) {
        if ((id & 3u) != 0u) continue;
        const auto frames = view.frames(record);
        for (std::size_t index = 0; index < frames.size(); ++index) {
            for (const auto& token : tokens_of(frames[index].type, frames[index].body)) {
                if (!token.alias) continue;
                AliasMessage message{TokenAction::UseAlias, *token.alias, id, index, frames[index].event};
                if (token.alias_type == wire::draft21::TokenAliasType::Delete) message.action = TokenAction::Delete;
                else if (token.alias_type == wire::draft21::TokenAliasType::Register) message.action = TokenAction::Register;
                else if (token.alias_type != wire::draft21::TokenAliasType::UseAlias) continue;
                result.push_back(message);
            }
        }
    }
    return result;
}

// The event at which the runner wrote its answer to frame `index` of `stream`, if it did.
// A stream opened by PUBLISH_NAMESPACE has its first frame answered by the base
// definition's acknowledgement, which is recorded apart from the courtesy responses.
std::optional<std::size_t> answer_event(const View& view, transport::StreamId stream, std::size_t index) {
    const auto* record = view.stream(stream);
    if (record) {
        const auto frames = view.frames(*record);
        if (!frames.empty() && frames.front().type == kPublishNamespace) {
            if (index == 0) return view.auto_reply_event(stream);
            --index;
        }
    }
    std::size_t seen = 0;
    for (const auto& write : view.courtesy_writes()) {
        if (write.stream_id != stream) continue;
        if (seen++ == index) return write.event_count;
    }
    return std::nullopt;
}

Spec pending_alias_delete_spec() {
    return spec("d21-publisher-delete-with-pending-alias-uses",
        {{"D21-8-9-MUST-NOT-281", "d21-no-delete-with-unanswered-alias-use"}},
        [](const Fixture&) {
            auto courtesy = accepting_publishes();
            courtesy.update = RawProbeUpdateResponse::HoldAliasUses;
            auto definition = observing(courtesy);
            // MAX_AUTH_TOKEN_CACHE_SIZE (Option 0x04) of 4096 lets the publisher register tokens.
            definition.setup_bytes = bytes_of({0xaf, 0, 0, 3, 4, 0x90, 0});
            definition.publisher_exit_is_evidence = true;
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto messages = alias_messages(view);
            bool compliant_delete = false;
            for (const auto& retirement : messages) {
                if (retirement.action != TokenAction::Delete) continue;
                bool used = false;
                for (const auto& use : messages) {
                    if (use.action != TokenAction::UseAlias || use.alias != retirement.alias ||
                        use.event >= retirement.event) continue;
                    used = true;
                    const auto answer = answer_event(view, use.stream, use.frame_index);
                    // `answer` counts the events seen when the answer was written, so an answer
                    // is after the DELETE only if more events than the DELETE's index preceded it.
                    if (!answer || *answer > retirement.event) return {true, false};
                }
                compliant_delete = compliant_delete || used;
            }
            if (!view.window_ended()) return {false, std::nullopt};
            return {true, compliant_delete ? std::optional<bool>{true} : std::nullopt};
        },
        true);
}

// ---- Section 5.2 lines 1880-1890: an uncommitted Subgroup is reset (D21-5-2-MUST-130) ------
// "If the Object Forwarding Preference is Subgroup and the value of
// SUBGROUP_DELIVERY_TIMEOUT is not zero, the MOQT implementation MUST start a timer ...
// once it becomes aware that all of the objects on the subgroup have been published
// ... If the timer expires before the underlying transport stream reaches 'all data
// committed' state, the implementation MUST reset the stream."
//
// The subscription asks for SUBGROUP_DELIVERY_TIMEOUT (Parameter 0x06) of 200 ms for
// Group 0 and the runner holds back flow control credit on the publisher's data
// streams: each may carry only 64 bytes, a stream credit that is never raised (RFC 9000
// Section 4.1), so a Subgroup whose first Object is larger than that can never be
// committed. A completed Subgroup then has to be reset when the timer expires, and
// that reset reaches the runner because the stream is still unfinished there. (Merely
// withholding acknowledgements would leave a fully received stream, where a later
// RESET_STREAM is not delivered to the application.) The fixture's Group 0 is complete
// and its first Object is larger than 64 bytes.
constexpr std::uint64_t kSubgroupDeliveryTimeout = 0x06;
constexpr std::uint64_t kSubgroupTimeoutMs = 200;
constexpr std::uint64_t kHeldStreamCredit = 64;

Spec uncommitted_subgroup_spec() {
    return spec("d21-subgroup-completion-withheld-acknowledgments",
        {{"D21-5-2-MUST-130", "d21-uncommitted-subgroup-timeout-reset"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            definition.initial_peer_uni_stream_data = kHeldStreamCredit;
            definition.hold_uni_stream_credit = true;
            auto whole_group = location_pair(0, 0);
            put_vi(whole_group, 0);
            definition.writes.push_back(request_write(subscribe_frame(1, fixture,
                {param_vi(kSubgroupDeliveryTimeout, kSubgroupTimeoutMs), param_u8(0x10, 1),
                 param_lp(0x21, whole_group)})));
            return definition;
        },
        [](const View& view) -> Judgement {
            if (rejected(view, 0)) return {true, std::nullopt};
            const auto alias = alias_of(view, 0);
            if (!alias) return {view.close().has_value(), std::nullopt};
            bool unfinished = false;
            for (const auto& stream : subgroup_streams(view, *alias)) {
                const auto* record = view.stream(stream.stream);
                if (!record || record->fin) continue;
                if (record->reset) return {true, true};
                unfinished = true;
            }
            if (!view.window_ended()) return {false, std::nullopt};
            // A stream still open at the end of the window, long after the timer, was never reset.
            return {true, unfinished ? std::optional<bool>{false} : std::nullopt};
        },
        true);
}

// ---- Section 8.9 lines 3270-3271 and 3288-3292: operator-configured credentials -----------
// The runner cannot mint a credential for a Token Type, so these two scenarios send
// what the operator supplied for a Token Type the publisher is configured to understand
// (RunManager options --invalid-auth-token and --expired-auth-token). Without a
// credential nothing is sent and the context ends unscored.
constexpr std::uint64_t kRequestErrorMalformedToken = 0x4;  // Section 12.3, Table 19
constexpr std::uint64_t kRequestErrorNotSupported = 0x3;
constexpr std::uint64_t kRequestErrorExpiredToken = 0x5;
constexpr std::uint64_t kDuplicateAuthTokenAlias = 0x14;    // Section 12.2, Table 18
constexpr std::uint64_t kAuthorization = 0x03;
constexpr std::uint64_t kTrackStatus = 0xd;

Bytes token_status(std::uint64_t request_id, const Fixture& fixture, const Bytes& token) {
    return request_frame(kTrackStatus, request_id, fixture, true, {param_lp(kAuthorization, token)});
}

std::optional<std::uint64_t> request_error_code(const std::vector<Frame>& frames) {
    if (frames.empty() || frames.front().type != kRequestError) return std::nullopt;
    wire::Cursor body(frames.front().body);
    return read_vi(body);
}

bool answered(const View& view, std::size_t write) {
    if (!view.write_frames(write).empty()) return true;
    const auto* stream = view.write_stream(write);
    return stream && (stream->fin || stream->reset);
}

Spec invalid_token_spec() {
    return spec("d21-request-well-formed-invalid-token",
        {{"D21-8-9-MUST-270", "d21-invalid-token-message-error"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            // A publisher that cannot serve the request may give up; that is what it did.
            definition.publisher_exit_is_evidence = true;
            // USE_VALUE (Alias Type 3): Token Type and Value, no Alias.
            if (fixture.credentials.invalid)
                definition.writes.push_back(request_write(token_status(1, fixture,
                    token_value(3, std::nullopt, fixture.credentials.invalid->token_type,
                                fixture.credentials.invalid->value))));
            return definition;
        },
        [](const View& view) -> Judgement {
            if (view.write_bytes(0).empty()) return {true, std::nullopt};  // no credential configured
            const auto frames = view.write_frames(0);
            if (frames.empty()) return {view.close().has_value(), std::nullopt};
            // "The receiver of a message containing a well-formed Token structure that is
            // otherwise invalid MUST reject that message with an MALFORMED_AUTH_TOKEN error."
            if (frames.front().type == kRequestOk) return {true, false};
            const auto code = request_error_code(frames);
            if (!code) return {true, std::nullopt};
            // NOT_SUPPORTED says the Token Type is not understood: the precondition is unmet.
            if (*code == kRequestErrorNotSupported) return {true, std::nullopt};
            return {true, *code == kRequestErrorMalformedToken};
        },
        false);
}

RawProbeWrite after_response(Bytes bytes, std::size_t previous) {
    RawProbeWrite write = request_write(std::move(bytes), true);
    write.evidence_ready = [previous](const RawProbeGateInput& input) {
        const View view(input.prior_writes, input.events);
        return view.valid() && answered(view, previous);
    };
    return write;
}

// The expired credential is registered under Alias 1 (the message fails, yet "MUST
// register the Token Alias ... even if the message fails"), then used by Alias and
// registered a second time. "If a receiver detects that an authorization token has
// expired, it MUST retain the registered Alias until it is deleted by the sender ...
// Any message that references an expired token with Alias Type USE_ALIAS fails with
// EXPIRED_AUTH_TOKEN", and registering a registered Alias again closes the session
// with DUPLICATE_AUTH_TOKEN_ALIAS.
Spec expired_token_alias_spec() {
    return spec("d21-expired-token-alias-lifetime",
        {{"D21-8-9-MUST-273", "d21-expired-token-alias-retained-until-delete"}},
        [](const Fixture& fixture) {
            auto definition = residual_definition();
            // A publisher that cannot serve the requests may give up; that is what it did.
            definition.publisher_exit_is_evidence = true;
            if (!fixture.credentials.expired) return definition;
            const auto& credential = *fixture.credentials.expired;
            definition.writes.push_back(request_write(token_status(1, fixture,
                token_value(1, 1, credential.token_type, credential.value)), true));
            definition.writes.push_back(after_response(token_status(3, fixture,
                token_value(2, 1, std::nullopt, {})), 0));
            definition.writes.push_back(after_response(token_status(5, fixture,
                token_value(1, 1, credential.token_type, credential.value)), 1));
            return definition;
        },
        [](const View& view) -> Judgement {
            if (view.write_bytes(0).empty()) return {true, std::nullopt};  // no credential configured
            const auto registration = view.write_frames(0);
            // The credential must really be expired: only EXPIRED_AUTH_TOKEN for the
            // registration shows that. A success or any other error (UNAUTHORIZED,
            // NOT_SUPPORTED, ...) leaves the precondition unmet.
            if (!registration.empty()) {
                const auto registration_code = request_error_code(registration);
                if (!registration_code || *registration_code != kRequestErrorExpiredToken) return {true, std::nullopt};
            }
            if (!answered(view, 1)) return {view.close().has_value(), std::nullopt};
            const auto use = view.write_frames(1);
            if (use.empty()) return {true, std::nullopt};
            const auto code = request_error_code(use);
            if (!code) return {true, std::nullopt};
            // An Alias the receiver no longer knows (UNKNOWN_AUTH_TOKEN_ALIAS) means it was dropped.
            if (view.unknown_alias_code() && *code == *view.unknown_alias_code()) return {true, false};
            if (*code != kRequestErrorExpiredToken) return {true, false};
            // Still registered, so registering it again before a DELETE is a duplicate.
            const auto* close = view.close() ? &*view.close() : nullptr;
            if (close && close->application && close->code == kDuplicateAuthTokenAlias) return {true, true};
            if (!view.write_frames(2).empty()) return {true, false};
            return {close != nullptr, std::nullopt};
        },
        false);
}

}  // namespace

std::vector<Spec> residual_specs() {
    std::vector<Spec> result;
    result.push_back(concurrent_subscription_spec("d21-overlapping-subscriptions-shared-alias", true));
    result.push_back(concurrent_subscription_spec("d21-overlapping-subscriptions-distinct-aliases", false));
    result.push_back(filter_conjunction_spec());
    result.push_back(mixed_subgroup_spec());
    result.push_back(failed_fill_spec());
    result.push_back(cancelled_fill_spec());
    result.push_back(skipped_publish_spec());
    result.push_back(distinct_aliases_spec());
    result.push_back(distinct_tracks_spec());
    result.push_back(rejected_publish_spec("d21-reject-publish-before-object-production", false));
    result.push_back(rejected_publish_spec("d21-rejected-subscribe-no-delivery", true));
    result.push_back(update_credit_spec("d21-publisher-update-credit-limit", false));
    result.push_back(update_credit_spec("d21-publisher-update-credit-per-stream", true));
    result.push_back(unlimited_updates_spec());
    result.push_back(client_goaway_spec("d21-publisher-client-goaway-control", true));
    result.push_back(client_goaway_spec("d21-publisher-client-goaway-request", false));
    result.push_back(pending_alias_delete_spec());
    result.push_back(uncommitted_subgroup_spec());
    result.push_back(invalid_token_spec());
    result.push_back(expired_token_alias_spec());
    return result;
}

}  // namespace moq::interop::scenarios::d21c
