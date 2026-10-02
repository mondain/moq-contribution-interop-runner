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

#include <map>
#include <set>

namespace moq::interop::scenarios::d21c {
namespace {

constexpr std::uint64_t kTargetGroup = 0;
constexpr std::uint64_t kTargetObject = 1;
constexpr std::uint64_t kFetchHeaderType = 0x5;

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
    auto definition = base_definition("");
    definition.acknowledge_publisher_namespaces = true;
    return definition;
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
        if (*payload_length == 0) {
            if (!read_vi(local)) break;
            data = false;
        } else if (*payload_length > kMaximumTotalBytes ||
                   !read_n(local, static_cast<std::size_t>(*payload_length))) {
            break;
        }
        const std::uint64_t object = previous ? *previous + *delta + 1 : *delta;
        if (mode == 1u && !previous) result.subgroup_id = object;
        previous = object;
        result.objects.push_back({*group, object, data, id, false});
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
    return Object{*group, object, (*flags & 0x20u) == 0, 0, true};
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
            std::set<int> cells_seen;
            for (const auto& stream : subgroup_streams(view, *alias)) {
                std::set<int> cells;
                for (const auto& object : stream.objects)
                    if (const auto cell = membership(object)) cells.insert(*cell);
                // A stream names exactly one Subgroup; Objects of two Subgroups on it mix them.
                if (cells.size() > 1) return {true, false};
                cells_seen.insert(cells.begin(), cells.end());
            }
            if (!view.window_ended()) return {false, std::nullopt};
            // Both Subgroups must have been observed for the split to be exercised.
            return {true, cells_seen.size() == 2 ? std::optional<bool>{true} : std::nullopt};
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

// The plain subscription (Request ID 1) has delivered an Object.
bool warmed_up(const View& view) {
    const auto alias = alias_of(view, 0);
    return view.valid() && alias && !delivered_objects(view, *alias).empty();
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

}  // namespace

std::vector<Spec> residual_specs() {
    std::vector<Spec> result;
    result.push_back(concurrent_subscription_spec("d21-overlapping-subscriptions-shared-alias", true));
    result.push_back(concurrent_subscription_spec("d21-overlapping-subscriptions-distinct-aliases", false));
    result.push_back(filter_conjunction_spec());
    result.push_back(mixed_subgroup_spec());
    result.push_back(failed_fill_spec());
    result.push_back(cancelled_fill_spec());
    return result;
}

}  // namespace moq::interop::scenarios::d21c
