// Draft-21 contribution profiles that observe Object delivery by the
// publisher under test: subscription delivery and Forward State, payload
// identity across Message Parameters, Prior Group/Object ID Gap properties,
// Object Forwarding Preference, datagram and Subgroup header flags, and
// Subgroup stream termination.

#include "draft21_contribution_support.h"

#include "moq/interop/wire/draft21/key_values.h"

#include <map>
#include <set>

namespace moq::interop::scenarios::d21c {
namespace {
namespace d21 = wire::draft21;
using namespace shared;

constexpr std::uint64_t kTargetGroup = 7;
constexpr std::uint64_t kTargetObject = 9;
constexpr std::uint64_t kPriorGroupGap = 0x3c;
constexpr std::uint64_t kPriorObjectGap = 0x3e;

// ---- object parsing ----------------------------------------------------------
// Every occurrence of `type`, in the mutable list and inside Immutable Properties.
std::vector<std::uint64_t> property_values(const Properties& properties, std::uint64_t type) {
    std::vector<std::uint64_t> values;
    const auto collect = [&](const d21::KeyValues& list) {
        for (const auto& entry : list) {
            if (entry.type != type) continue;
            if (const auto* value = std::get_if<std::uint64_t>(&entry.value)) values.push_back(*value);
        }
    };
    collect(properties.top);
    for (const auto& list : properties.immutable) collect(list);
    return values;
}

struct FetchParse {
    bool header{false};
    bool invalid{false};
    std::uint64_t request_id{0};
    std::optional<std::uint64_t> flags;
    std::optional<ObjectRecord> first;
    bool range_marker{false};
};

FetchParse parse_fetch(std::span<const std::byte> data) {
    FetchParse result;
    wire::Cursor cursor(data);
    const auto type = read_vi(cursor);
    if (!type) return result;
    if (*type != 5) { result.invalid = true; return result; }
    const auto request = read_vi(cursor);
    if (!request) return result;
    result.header = true;
    result.request_id = *request;
    const auto flags = read_vi(cursor);
    if (!flags) return result;
    result.flags = *flags;
    if (*flags == 0x8c || *flags == 0x10c || *flags == 0x20c) { result.range_marker = true; return result; }
    // The first Object must carry absolute Group and Object IDs and a priority.
    if (*flags > 0x7f || (*flags & 0x1cu) != 0x1cu ||
        ((*flags & 0x40u) == 0 && ((*flags & 3u) == 1 || (*flags & 3u) == 2))) {
        result.invalid = true;
        return result;
    }
    ObjectRecord record;
    const auto group = read_vi(cursor);
    if (!group) return result;
    record.group = *group;
    if ((*flags & 0x40u) == 0 && (*flags & 3u) == 3) {
        const auto subgroup = read_vi(cursor);
        if (!subgroup) return result;
        record.subgroup = *subgroup;
    }
    const auto object = read_vi(cursor);
    if (!object) return result;
    record.object = *object;
    if (!read_n(cursor, 1)) return result;
    if ((*flags & 0x20u) != 0) {
        const auto length = read_vi(cursor);
        if (!length) return result;
        if (*length > 65535) { result.invalid = true; return result; }
        const auto block = read_n(cursor, static_cast<std::size_t>(*length));
        if (!block) return result;
        auto properties = parse_properties(*block);
        if (!properties) { result.invalid = true; return result; }
        record.properties = std::move(*properties);
        record.has_properties = true;
    }
    const auto payload_length = read_vi(cursor);
    if (!payload_length) return result;
    if (*payload_length > kMaximumTotalBytes) { result.invalid = true; return result; }
    const auto payload = read_n(cursor, static_cast<std::size_t>(*payload_length));
    if (!payload) return result;
    record.payload.assign(payload->begin(), payload->end());
    result.first = std::move(record);
    return result;
}

struct DatagramParse {
    bool valid{false};
    std::uint64_t flags{0};
    std::uint64_t alias{0};
    ObjectRecord record;
};

DatagramParse parse_datagram(std::span<const std::byte> data) {
    DatagramParse result;
    wire::Cursor cursor(data);
    const auto flags = read_vi(cursor);
    if (!flags) return result;
    result.flags = *flags;
    const auto alias = read_vi(cursor);
    const auto group = alias ? read_vi(cursor) : std::nullopt;
    if (!alias || !group) return result;
    result.alias = *alias;
    result.record.group = *group;
    if ((*flags & 0x04u) == 0) {
        const auto object = read_vi(cursor);
        if (!object) return result;
        result.record.object = *object;
    }
    if ((*flags & 0x08u) == 0 && !read_n(cursor, 1)) return result;
    if ((*flags & 0x01u) != 0) {
        const auto length = read_vi(cursor);
        if (!length || *length > 65535) return result;
        const auto block = read_n(cursor, static_cast<std::size_t>(*length));
        if (!block) return result;
        auto properties = parse_properties(*block);
        if (!properties) return result;
        result.record.properties = std::move(*properties);
        result.record.has_properties = true;
    }
    if ((*flags & 0x20u) != 0) {
        const auto status = read_vi(cursor);
        if (!status) return result;
        result.record.status = *status;
    } else {
        const auto payload = read_n(cursor, cursor.remaining());
        if (!payload) return result;
        result.record.payload.assign(payload->begin(), payload->end());
    }
    result.valid = true;
    return result;
}

// ---- subscription views ------------------------------------------------------
std::optional<std::uint64_t> alias_of(const View& view, std::size_t write) {
    const auto* record = view.write_stream(write);
    const auto frames = view.write_frames(write);
    if (!record || frames.empty() || frames.front().type != kSubscribeOk) return std::nullopt;
    wire::Cursor body(frames.front().body);
    return read_vi(body);
}

struct Delivery {
    bool datagram{false};
    ObjectRecord record;
    std::size_t event{0};
};

struct Delivered {
    std::vector<Delivery> objects;
    std::vector<std::size_t> stream_events;  // first event of each alias stream
};

Delivered subscription_deliveries(const View& view, std::uint64_t alias) {
    Delivered result;
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        const auto parsed = parse_subgroup(stream.bytes);
        if (!parsed.header || parsed.alias != alias) continue;
        result.stream_events.push_back(stream.first_event);
        for (const auto& record : parsed.objects) result.objects.push_back({false, record, stream.first_event});
    }
    for (const auto& datagram : view.datagrams()) {
        const auto parsed = parse_datagram(datagram.data);
        if (!parsed.valid || parsed.alias != alias) continue;
        result.stream_events.push_back(datagram.event);
        result.objects.push_back({true, parsed.record, datagram.event});
    }
    return result;
}

const Delivery* find_delivery(const Delivered& delivered, std::uint64_t group, std::uint64_t object) {
    for (const auto& item : delivered.objects)
        if (item.record.group == group && item.record.object == object) return &item;
    return nullptr;
}

bool target_delivered(const View& view, std::size_t write) {
    const auto alias = alias_of(view, write);
    return alias && find_delivery(subscription_deliveries(view, *alias), kTargetGroup, kTargetObject);
}

struct FetchStream {
    const StreamRecord* record{nullptr};
    FetchParse parse;
};

FetchStream fetch_stream(const View& view, std::uint64_t request_id) {
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        auto parsed = parse_fetch(stream.bytes);
        if (parsed.header && parsed.request_id == request_id) return {&stream, std::move(parsed)};
    }
    return {};
}

bool request_rejected(const View& view, std::size_t write) {
    const auto frames = view.write_frames(write);
    return !frames.empty() && frames.front().type == kRequestError;
}

// ---- request builders ---------------------------------------------------------
Bytes location_pair(std::uint64_t group, std::uint64_t object) {
    Bytes result;
    put_vi(result, group);
    put_vi(result, object);
    return result;
}

Param start_filter() { return param_lp(0x21, location_pair(kTargetGroup, kTargetObject)); }

Param target_range_filter() {
    auto value = location_pair(kTargetGroup, kTargetObject);
    put_vi(value, 0);              // End Group delta
    put_vi(value, kTargetObject);  // End Object
    return param_lp(0x21, value);
}

RawProbeWrite update_write(Bytes bytes, std::size_t base) {
    RawProbeWrite write{RawProbeChannel::NewBidi, std::move(bytes), false, base};
    write.peer_response_ready = subscribe_ok_ready;
    return write;
}

// ---- Section 9.6 / 3.1: eligible Objects are delivered --------------------------
Spec delivery_spec() {
    return spec("d21-successful-subscribe-object-delivery",
        {{"D21-9-6-MUST-355", "d21-successful-subscription-delivers-eligible-objects"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(subscribe_frame(1, fixture, {param_u8(0x10, 1)})));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto alias = alias_of(view, 0);
            if (alias && !subscription_deliveries(view, *alias).objects.empty()) return {true, true};
            // An idle source or a filtered track is not a delivery failure.
            return {view.close().has_value(), std::nullopt};
        });
}

Spec forward_zero_spec() {
    return spec("d21-successful-subscribe-forward-zero",
        {{"D21-9-6-MUST-355", "d21-successful-subscription-delivers-eligible-objects"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(subscribe_frame(1, fixture, {param_u8(0x10, 0)})));
            definition.writes.push_back(update_write(request_update_frame(3, {param_u8(0x10, 1)}), 0));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto alias = alias_of(view, 0);
            const auto marker = view.write_event(1);
            if (!alias || !marker) return {view.close().has_value(), std::nullopt};
            const auto delivered = subscription_deliveries(view, *alias);
            // Objects must not flow while the Forward State is 0.
            for (const auto event : delivered.stream_events)
                if (event < *marker) return {true, false};
            for (const auto& item : delivered.objects)
                if (item.event >= *marker) return {true, true};
            return {view.close().has_value(), std::nullopt};
        });
}

// ---- Section 9.20: Message Parameters leave Object payloads alone -----------------
std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<Bytes>> payloads_by_location(
    const Delivered& delivered) {
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::vector<Bytes>> result;
    for (const auto& item : delivered.objects)
        if (!item.record.status || *item.record.status == 0)
            result[{item.record.group, item.record.object}].push_back(item.record.payload);
    return result;
}

Spec subscribe_payload_spec() {
    return spec("d21-subscribe-parameters-preserve-payload",
        {{"D21-9-20-MUST-NOT-402", "d21-subscribe-object-payload-identity"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            // Two concurrent subscriptions to one track differ only in delivery parameters.
            definition.writes.push_back(request_write(subscribe_frame(1, fixture,
                {param_u8(0x10, 1), param_u8(0x20, 0), param_u8(0x22, 1)})));
            definition.writes.push_back(request_write(subscribe_frame(3, fixture,
                {param_u8(0x10, 1), param_u8(0x20, 255), param_u8(0x22, 2)})));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto first = alias_of(view, 0);
            const auto second = alias_of(view, 1);
            if (!first || !second) return {view.close().has_value(), std::nullopt};
            bool compared = false;
            const auto first_objects = payloads_by_location(subscription_deliveries(view, *first));
            if (*first == *second) {
                // A shared Track Alias carries each Object once per subscription.
                for (const auto& [location, copies] : first_objects) {
                    if (copies.size() < 2) continue;
                    compared = true;
                    for (const auto& copy : copies)
                        if (copy != copies.front()) return {true, false};
                }
            } else {
                const auto second_objects = payloads_by_location(subscription_deliveries(view, *second));
                for (const auto& [location, copies] : first_objects) {
                    const auto other = second_objects.find(location);
                    if (other == second_objects.end()) continue;
                    compared = true;
                    for (const auto& copy : copies)
                        for (const auto& peer : other->second)
                            if (copy != peer) return {true, false};
                }
            }
            if (compared) return {true, true};
            return {view.close().has_value(), std::nullopt};
        });
}

Spec fetch_payload_spec() {
    return spec("d21-fetch-parameters-preserve-payload",
        {{"D21-9-20-MUST-NOT-403", "d21-fetch-object-payload-identity"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(fetch_frame(1, fixture,
                {param_u8(0x20, 0), target_range_filter()}), true));
            definition.writes.push_back(request_write(fetch_frame(3, fixture,
                {param_u8(0x20, 255), param_u8(0x22, 2), target_range_filter()}), true));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto first = fetch_stream(view, 1);
            const auto second = fetch_stream(view, 3);
            if (first.parse.first && second.parse.first) {
                const auto& left = *first.parse.first;
                const auto& right = *second.parse.first;
                const bool same_object = left.group == right.group && left.object == right.object;
                if (!same_object) return {true, std::nullopt};
                return {true, left.payload == right.payload};
            }
            const bool rejected = request_rejected(view, 0) || request_rejected(view, 1);
            return {rejected || view.close().has_value(), std::nullopt};
        });
}

// ---- Sections 10.8 and 10.9: Prior Group/Object ID Gap -----------------------------
// A subscription delivers Object 7/9, then FETCH retrieves the same Object.
RawProbeWrite fetch_after_delivery(Bytes bytes) {
    RawProbeWrite write{RawProbeChannel::NewBidi, std::move(bytes), true};
    write.evidence_ready = [](const RawProbeGateInput& input) {
        const View view(input.prior_writes, input.events);
        return view.valid() && target_delivered(view, 0);
    };
    return write;
}

struct Repeat {
    std::vector<std::uint64_t> delivered;
    std::vector<std::uint64_t> fetched;
};

std::optional<Repeat> repeated_gap(const View& view, std::uint64_t type) {
    const auto alias = alias_of(view, 0);
    if (!alias) return std::nullopt;
    const auto delivered = subscription_deliveries(view, *alias);
    const auto* delivery = find_delivery(delivered, kTargetGroup, kTargetObject);
    const auto fetched = fetch_stream(view, 3);
    if (!delivery || !fetched.parse.first || fetched.parse.first->group != kTargetGroup ||
        fetched.parse.first->object != kTargetObject)
        return std::nullopt;
    return Repeat{property_values(delivery->record.properties, type),
                  property_values(fetched.parse.first->properties, type)};
}

Judgement repeat_ready(const View& view, const std::optional<Repeat>& repeat, std::optional<bool> result) {
    if (repeat) return {true, result};
    return {request_rejected(view, 1) || view.close().has_value(), std::nullopt};
}

std::vector<Spec> gap_specs(const char* scenario_prefix, std::uint64_t type, const char* stable_row,
                            const char* stable_evaluator, const char* preserved_row,
                            const char* preserved_evaluator, const char* count_row,
                            const char* count_evaluator) {
    std::vector<Spec> result;
    const std::string prefix = scenario_prefix;
    const auto repeat_build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.writes.push_back(request_write(subscribe_frame(1, fixture,
            {param_u8(0x10, 1), start_filter()})));
        definition.writes.push_back(fetch_after_delivery(fetch_frame(3, fixture, {target_range_filter()})));
        return definition;
    };
    Spec repeat;
    repeat.scenario = prefix + "-repeat";
    repeat.build = repeat_build;
    // The original publisher must not change an existing gap value ...
    repeat.rows.push_back({stable_row, stable_evaluator, [type](const View& view) -> Judgement {
        const auto repeat_values = repeated_gap(view, type);
        std::optional<bool> verdict;
        if (repeat_values && repeat_values->delivered.size() == 1 && repeat_values->fetched.size() == 1)
            verdict = repeat_values->delivered.front() == repeat_values->fetched.front();
        return repeat_ready(view, repeat_values, verdict);
    }});
    // ... nor remove it; omitting it initially stays legal.
    repeat.rows.push_back({preserved_row, preserved_evaluator, [type](const View& view) -> Judgement {
        const auto repeat_values = repeated_gap(view, type);
        std::optional<bool> verdict;
        if (repeat_values && repeat_values->delivered.size() == 1)
            verdict = repeat_values->fetched.size() >= 1;
        return repeat_ready(view, repeat_values, verdict);
    }});
    result.push_back(std::move(repeat));

    Spec singleton;
    singleton.scenario = prefix + "-singleton";
    singleton.rows = {{count_row, count_evaluator}};
    singleton.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.writes.push_back(request_write(fetch_frame(1, fixture, {target_range_filter()}), true));
        return definition;
    };
    singleton.judge = [type](const View& view) -> Judgement {
        const auto fetched = fetch_stream(view, 1);
        if (fetched.parse.first && fetched.parse.first->group == kTargetGroup &&
            fetched.parse.first->object == kTargetObject)
            return {true, property_values(fetched.parse.first->properties, type).size() <= 1};
        return {request_rejected(view, 0) || view.close().has_value(), std::nullopt};
    };
    result.push_back(std::move(singleton));
    return result;
}

// ---- Sections 11.1.1 and 11.4.1.1: forwarding preference ----------------------------
struct PreferenceObservation {
    bool via_datagram{false};
    bool fetch_datagram_flag{false};
};

std::optional<PreferenceObservation> preference(const View& view) {
    const auto alias = alias_of(view, 0);
    if (!alias) return std::nullopt;
    const auto delivered = subscription_deliveries(view, *alias);
    const auto* delivery = find_delivery(delivered, kTargetGroup, kTargetObject);
    const auto fetched = fetch_stream(view, 3);
    if (!delivery || !fetched.parse.first || !fetched.parse.flags || fetched.parse.first->group != kTargetGroup ||
        fetched.parse.first->object != kTargetObject)
        return std::nullopt;
    return PreferenceObservation{delivery->datagram, (*fetched.parse.flags & 0x40u) != 0};
}

Builder preference_build() {
    return [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.writes.push_back(request_write(subscribe_frame(1, fixture,
            {param_u8(0x10, 1), start_filter()})));
        definition.writes.push_back(fetch_after_delivery(fetch_frame(3, fixture, {target_range_filter()})));
        return definition;
    };
}

Judgement preference_ready(const View& view, const std::optional<PreferenceObservation>& seen,
                           std::optional<bool> result) {
    if (seen) return {true, result};
    return {request_rejected(view, 1) || view.close().has_value(), std::nullopt};
}

// ---- Section 11.2.1 and 11.3.1: header flags -------------------------------------------
Spec datagram_flags_spec() {
    return spec("d21-object-datagram-flags",
        {{"D21-11-2-1-MUST-522", "d21-object-datagram-reserved-bit-zero"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(subscribe_frame(1, fixture, {param_u8(0x10, 1)})));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto alias = alias_of(view, 0);
            if (!alias) return {view.close().has_value(), std::nullopt};
            bool seen = false;
            for (const auto& datagram : view.datagrams()) {
                const auto parsed = parse_datagram(datagram.data);
                if (!parsed.valid || parsed.alias != *alias) continue;
                // The decoded Type Flags value, not its length-prefix bits.
                if ((parsed.flags & 0x10u) != 0) return {true, false};
                seen = true;
            }
            return seen ? Judgement{true, true} : Judgement{view.close().has_value(), std::nullopt};
        });
}

constexpr std::uint64_t kPaddingStreamType = 0x132b3e28;

Spec subgroup_header_spec() {
    return spec("d21-subgroup-header-flags",
        {{"D21-11-3-1-MUST-534", "d21-subgroup-header-bit-four"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(subscribe_frame(1, fixture, {param_u8(0x10, 1)})));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto alias = alias_of(view, 0);
            if (!alias) return {view.close().has_value(), std::nullopt};
            bool seen = false;
            for (const auto& [id, stream] : view.streams()) {
                if ((id & 3u) != 2u) continue;
                wire::Cursor cursor(stream.bytes);
                const auto type = read_vi(cursor);
                if (!type) continue;
                // SETUP, FETCH_HEADER and padding streams are not Subgroup streams.
                if (*type >= 128 && *type != kPaddingStreamType) continue;
                if (*type == 5 || *type == kPaddingStreamType) continue;
                if ((*type & 0x10u) == 0) return {true, false};
                const auto parsed = parse_subgroup(stream.bytes);
                if (parsed.header && parsed.alias == *alias) seen = true;
            }
            return seen ? Judgement{true, true} : Judgement{view.close().has_value(), std::nullopt};
        });
}

// ---- Section 11.3.2: closing Subgroup streams ---------------------------------------------
Judgement fin_judgement(const View& view) {
    const auto alias = alias_of(view, 0);
    if (!alias) return {view.close().has_value(), std::nullopt};
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        const auto parsed = parse_subgroup(stream.bytes);
        if (!parsed.header || parsed.alias != *alias) continue;
        // The header's END_OF_GROUP flag or an End of Group/Track status says the
        // sender has handed every Object of this Subgroup to the stream.
        const bool complete = parsed.terminal_status || ((parsed.type & 0x08u) != 0 && stream.fin);
        if (!complete) continue;
        if (stream.fin) return {true, true};
        if (stream.reset && parsed.terminal_status) return {true, false};
    }
    return {view.close().has_value(), std::nullopt};
}

Spec fin_spec(const char* scenario, bool start_location) {
    return spec(scenario, {{"D21-11-3-2-MUST-536", "d21-complete-subgroup-fin"}},
        [start_location](const Fixture& fixture) {
            auto definition = base_definition("");
            std::vector<Param> params{param_u8(0x10, 1)};
            // Objects before the Start Location are excluded from the obligation.
            if (start_location) params.push_back(start_filter());
            definition.writes.push_back(request_write(subscribe_frame(1, fixture, params)));
            return definition;
        },
        fin_judgement);
}

// Streams already open (header parsed, not closed) once SUBSCRIBE_OK is complete.
std::set<transport::StreamId> open_subscription_streams(const View& view, std::uint64_t alias) {
    std::set<transport::StreamId> result;
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u || stream.fin || stream.reset) continue;
        const auto parsed = parse_subgroup(stream.bytes);
        if (parsed.header && parsed.alias == alias) result.insert(id);
    }
    return result;
}

Spec premature_close_spec() {
    return spec("d21-subgroup-premature-close-reset",
        {{"D21-11-3-2-MUST-537", "d21-incomplete-subgroup-reset"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(subscribe_frame(1, fixture, {param_u8(0x10, 1)})));
            // Forward State 0 omits further Subgroup Objects; wait for an open stream.
            auto update = update_write(request_update_frame(3, {param_u8(0x10, 0)}), 0);
            update.evidence_ready = [](const RawProbeGateInput& input) {
                const View view(input.prior_writes, input.events);
                const auto alias = alias_of(view, 0);
                return view.valid() && alias && !open_subscription_streams(view, *alias).empty();
            };
            definition.writes.push_back(std::move(update));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto alias = alias_of(view, 0);
            const auto marker = view.write_event(1);
            if (!alias || !marker) return {view.close().has_value(), std::nullopt};
            // Streams open when the update was sent.
            std::set<transport::StreamId> targets;
            for (const auto& [id, stream] : view.streams()) {
                if ((id & 3u) != 2u || stream.first_event >= *marker) continue;
                const auto parsed = parse_subgroup(stream.bytes);
                if (!parsed.header || parsed.alias != *alias) continue;
                if ((stream.fin_event && *stream.fin_event < *marker) ||
                    (stream.reset_event && *stream.reset_event < *marker)) continue;
                targets.insert(id);
            }
            if (targets.empty()) return {view.close().has_value(), std::nullopt};
            bool any_reset = false;
            for (const auto id : targets) {
                const auto* stream = view.stream(id);
                if (stream && stream->reset) any_reset = true;
                // A clean FIN may already have been committed: ambiguous.
                if (stream && stream->fin) return {true, std::nullopt};
            }
            return any_reset ? Judgement{true, true} : Judgement{view.close().has_value(), std::nullopt};
        });
}

}  // namespace

std::vector<Spec> object_specs() {
    std::vector<Spec> result;
    result.push_back(delivery_spec());
    result.push_back(forward_zero_spec());
    result.push_back(subscribe_payload_spec());
    result.push_back(fetch_payload_spec());
    for (auto& entry : gap_specs("d21-prior-group-gap", kPriorGroupGap,
             "D21-10-8-MUST-NOT-495", "d21-prior-group-gap-stable",
             "D21-10-8-MUST-NOT-496", "d21-prior-group-gap-preserved",
             "D21-10-8-MUST-NOT-497", "d21-prior-group-gap-count"))
        result.push_back(std::move(entry));
    for (auto& entry : gap_specs("d21-prior-object-gap", kPriorObjectGap,
             "D21-10-9-MUST-NOT-500", "d21-prior-object-gap-stable",
             "D21-10-9-MUST-NOT-501", "d21-prior-object-gap-preserved",
             "D21-10-9-MUST-NOT-502", "d21-prior-object-gap-count"))
        result.push_back(std::move(entry));

    // Datagram preference is observable only through the FETCH flag for the
    // same Object; both rows share one stimulus.
    result.push_back(spec("d21-subscription-forwarding-preference",
        {{"D21-11-1-1-MUST-505", "d21-object-forwarding-preference-preserved"}},
        preference_build(), [](const View& view) -> Judgement {
            const auto seen = preference(view);
            std::optional<bool> verdict;
            if (seen) {
                if (seen->via_datagram == seen->fetch_datagram_flag) verdict = true;
                // FETCH names Datagram but the subscription used a stream.
                else if (seen->fetch_datagram_flag) verdict = false;
            }
            return preference_ready(view, seen, verdict);
        }));
    result.push_back(spec("d21-fetch-datagram-preference",
        {{"D21-11-4-1-1-MUST-557", "d21-fetch-datagram-flag"}},
        preference_build(), [](const View& view) -> Judgement {
            const auto seen = preference(view);
            std::optional<bool> verdict;
            // Only an Object known to be Datagram preference exercises the rule.
            if (seen && seen->via_datagram) verdict = seen->fetch_datagram_flag;
            return preference_ready(view, seen, verdict);
        }));

    result.push_back(datagram_flags_spec());
    result.push_back(subgroup_header_spec());
    result.push_back(fin_spec("d21-complete-subgroup-fin", false));
    result.push_back(fin_spec("d21-subgroup-start-location-fin", true));
    result.push_back(premature_close_spec());
    return result;
}

}  // namespace moq::interop::scenarios::d21c
