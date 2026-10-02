// Draft-21 contribution profiles, slice B. Each scenario names the evidence a
// verdict rests on; evidence that is absent or ambiguous leaves the row
// unscored (no judgement result), never a pass.
//
// Rows (requirement id: scenarios):
//   D21-9-9-MUST-365      PUBLISH_DONE Stream Count 0 without data streams
//   D21-9-4-1-MUST-340    empty Track Name in namespace-scoped Redirects
//   D21-9-10-MUST-371/372 PUBLISH_STATE_NOTIFY content
//   D21-9-20-18-MUST-456  LARGEST_OBJECT after publication
//   D21-9-15-MUST-386, D21-9-18-MUST-394  authorized-only discovery
//   D21-9-20-3-MUST-NOT-407  token not copied from SUBSCRIBE_TRACKS to PUBLISH
//   D21-11-5-1-MUST-565, D21-11-5-2-MUST-570  zero padding emitted by the publisher
//   D21-11-3-2-MUST-543   early Subgroup termination is a reset
//   D21-10-7-MUST-489/490 property filters search both property lists
//   D21-13-MUST-593/594   unknown GREASE values (SETUP options, REQUEST_ERROR
//                         code, token type, Stream Reset code)

#include "draft21_contribution_support.h"

#include "moq/interop/wire/draft21/key_values.h"
#include "moq/interop/wire/draft21/publish.h"
#include "moq/interop/wire/draft21/request_error.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

namespace moq::interop::scenarios::d21c {
namespace {
namespace d21 = wire::draft21;

constexpr std::uint64_t kTokenParameter = 0x03;
constexpr std::uint64_t kLargestObjectParameter = 0x09;
constexpr std::uint64_t kForwardParameter = 0x10;
constexpr std::uint64_t kPriorityParameter = 0x20;
constexpr std::uint64_t kGroupOrderParameter = 0x22;
constexpr std::uint64_t kObjectPropertyFilter = 0x28;
constexpr std::uint64_t kImmutablePropertiesType = 0x0b;
constexpr std::uint64_t kPublish = 0x1d;
constexpr std::uint64_t kRedirect = 0x34;
constexpr std::uint64_t kGreaseValue = 0x9d;
constexpr std::uint64_t kPaddingStreamType = 0x132b3e28;
constexpr std::uint64_t kPaddingDatagramType = 0x132b3e29;
constexpr std::uint64_t kMaxFilterRanges = 0x06;  // Section 9.1.6

// ---- Message Parameter blocks --------------------------------------------------
struct Value {
    std::uint64_t type{0};
    std::optional<std::uint64_t> number;
    std::optional<std::pair<std::uint64_t, std::uint64_t>> location;
    Bytes bytes;
};
struct Block {
    bool ok{false};
    std::vector<Value> values;
};

Block parse_block(wire::Cursor& body) {
    Block block;
    const auto count = read_vi(body);
    if (!count || *count > 64) return block;
    std::uint64_t previous = 0;
    for (std::uint64_t index = 0; index < *count; ++index) {
        const auto delta = read_vi(body);
        if (!delta || *delta > std::numeric_limits<std::uint64_t>::max() - previous) return block;
        Value value;
        value.type = previous + *delta;
        previous = value.type;
        switch (value.type) {
            case 0x10: case 0x20: case 0x22: case 0x35: {
                const auto octet = read_n(body, 1);
                if (!octet) return block;
                value.number = std::to_integer<std::uint64_t>((*octet)[0]);
                break;
            }
            case 0x02: case 0x04: case 0x06: case 0x08: case 0x0a: case 0x32: {
                value.number = read_vi(body);
                if (!value.number) return block;
                break;
            }
            case 0x09: {
                const auto group = read_vi(body);
                const auto object = group ? read_vi(body) : std::nullopt;
                if (!group || !object) return block;
                value.location = {*group, *object};
                break;
            }
            case 0x03: case 0x21: case 0x23: case 0x25: case 0x26: case 0x27:
            case 0x28: case 0x29: case 0x34: {
                const auto length = read_vi(body);
                if (!length || *length > 65535) return block;
                const auto bytes = read_n(body, static_cast<std::size_t>(*length));
                if (!bytes) return block;
                value.bytes.assign(bytes->begin(), bytes->end());
                break;
            }
            default:
                // An unknown parameter's value cannot be skipped.
                return block;
        }
        block.values.push_back(std::move(value));
    }
    block.ok = true;
    return block;
}

const Value* find_value(const Block& block, std::uint64_t type) {
    for (const auto& value : block.values)
        if (value.type == type) return &value;
    return nullptr;
}

// Parameters of a SUBSCRIBE_OK, REQUEST_OK or PUBLISH_STATE_NOTIFY frame.
std::optional<Block> block_of(const Frame& message) {
    wire::Cursor body(message.body);
    if (message.type == kSubscribeOk) {
        if (!read_vi(body)) return std::nullopt;
    } else if (message.type != kRequestOk && message.type != kPublishStateNotify) {
        return std::nullopt;
    }
    auto block = parse_block(body);
    if (!block.ok) return std::nullopt;
    return block;
}

// ---- Object parsing -------------------------------------------------------------
struct Properties {
    d21::KeyValues top;
    std::vector<d21::KeyValues> immutable;
};

std::optional<Properties> parse_properties(std::span<const std::byte> block) {
    wire::Cursor cursor(block);
    auto decoded = d21::decode_key_values_to_end(cursor);
    auto* entries = std::get_if<d21::KeyValues>(&decoded);
    if (!entries) return std::nullopt;
    Properties result;
    result.top = *entries;
    for (const auto& entry : result.top) {
        if (entry.type != kImmutablePropertiesType) continue;
        const auto* raw = std::get_if<Bytes>(&entry.value);
        if (!raw) return std::nullopt;
        wire::Cursor nested(*raw);
        auto inner = d21::decode_key_values_to_end(nested);
        auto* values = std::get_if<d21::KeyValues>(&inner);
        if (!values) return std::nullopt;
        result.immutable.push_back(*values);
    }
    return result;
}

struct Obj {
    std::uint64_t id{0};
    std::optional<std::uint64_t> status;
    Properties properties;
    bool has_properties{false};
};

struct Subgroup {
    bool header{false};
    bool invalid{false};
    std::uint64_t type{0};
    std::uint64_t alias{0};
    std::uint64_t group{0};
    std::optional<std::uint64_t> subgroup;
    std::size_t header_length{0};
    std::vector<Obj> objects;
    bool terminal{false};  // an End of Group or End of Track Object
    bool partial_body{false};  // bytes follow the header that are not yet a whole Object
};

Subgroup parse_subgroup(std::span<const std::byte> data) {
    Subgroup result;
    wire::Cursor cursor(data);
    const auto type = read_vi(cursor);
    if (!type) return result;
    result.type = *type;
    if (*type >= 128 || (*type & 0x10u) == 0 || (*type & 0x06u) == 0x06u) {
        result.invalid = true;
        return result;
    }
    const auto alias = read_vi(cursor);
    const auto group = alias ? read_vi(cursor) : std::nullopt;
    if (!alias || !group) return result;
    const unsigned mode = static_cast<unsigned>((*type & 0x06u) >> 1u);
    std::optional<std::uint64_t> subgroup;
    if (mode == 2) {
        subgroup = read_vi(cursor);
        if (!subgroup) return result;
    } else if (mode == 0) {
        subgroup = 0;
    }
    if ((*type & 0x20u) == 0 && !read_n(cursor, 1)) return result;
    result.header = true;
    result.alias = *alias;
    result.group = *group;
    result.header_length = cursor.offset();
    std::optional<std::uint64_t> previous;
    while (cursor.remaining() != 0) {
        auto local = cursor;
        const auto delta = read_vi(local);
        if (!delta) { result.partial_body = true; break; }
        Obj record;
        if ((*type & 0x01u) != 0) {
            const auto length = read_vi(local);
            if (!length) { result.partial_body = true; break; }
            if (*length > 65535) { result.invalid = true; break; }
            const auto block = read_n(local, static_cast<std::size_t>(*length));
            if (!block) { result.partial_body = true; break; }
            auto properties = parse_properties(*block);
            if (!properties) { result.invalid = true; break; }
            record.properties = std::move(*properties);
            record.has_properties = true;
        }
        const auto payload_length = read_vi(local);
        if (!payload_length) { result.partial_body = true; break; }
        if (*payload_length == 0) {
            const auto status = read_vi(local);
            if (!status) { result.partial_body = true; break; }
            record.status = *status;
        } else {
            if (*payload_length > kMaximumTotalBytes) { result.invalid = true; break; }
            if (!read_n(local, static_cast<std::size_t>(*payload_length))) { result.partial_body = true; break; }
        }
        record.id = previous ? *previous + *delta + 1 : *delta;
        if (mode == 1 && !previous) subgroup = record.id;
        previous = record.id;
        if (record.status && (*record.status == 3 || *record.status == 4)) result.terminal = true;
        result.objects.push_back(std::move(record));
        cursor = local;
    }
    result.subgroup = subgroup;
    return result;
}

struct DatagramObject {
    bool valid{false};
    std::uint64_t alias{0};
    std::uint64_t group{0};
};

DatagramObject parse_datagram_object(std::span<const std::byte> data) {
    DatagramObject result;
    wire::Cursor cursor(data);
    const auto flags = read_vi(cursor);
    if (!flags || *flags >= 128 || (*flags & 0x10u) != 0) return result;
    const auto alias = read_vi(cursor);
    const auto group = alias ? read_vi(cursor) : std::nullopt;
    if (!alias || !group) return result;
    result = {true, *alias, *group};
    return result;
}

// ---- views over the transcript ----------------------------------------------------
std::optional<std::uint64_t> subscribe_alias(const View& view, std::size_t write) {
    const auto frames = view.write_frames(write);
    if (frames.empty() || frames.front().type != kSubscribeOk) return std::nullopt;
    wire::Cursor body(frames.front().body);
    return read_vi(body);
}

// Event index at which the first byte of a body of this stream's Objects arrived.
std::size_t body_event(const StreamRecord& record, std::size_t header_length) {
    for (const auto& [event, cumulative] : record.chunks)
        if (cumulative > header_length) return event;
    return record.first_event;
}

// First observed Object (stream body byte or datagram) of the subscription.
std::optional<std::size_t> first_object_event(const View& view, std::uint64_t alias) {
    std::optional<std::size_t> earliest;
    const auto note = [&](std::size_t event) {
        if (!earliest || event < *earliest) earliest = event;
    };
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        const auto parsed = parse_subgroup(stream.bytes);
        if (!parsed.header || parsed.alias != alias || stream.bytes.size() <= parsed.header_length) continue;
        note(body_event(stream, parsed.header_length));
    }
    for (const auto& datagram : view.datagrams()) {
        const auto parsed = parse_datagram_object(datagram.data);
        if (parsed.valid && parsed.alias == alias) note(datagram.event);
    }
    return earliest;
}

bool object_gate(const RawProbeGateInput& input) {
    const View view(input.prior_writes, input.events);
    if (!view.valid()) return false;
    const auto alias = subscribe_alias(view, 0);
    return alias && first_object_event(view, *alias);
}

bool application_close(const View& view) { return view.close().has_value(); }

RawProbeWrite request_write(Bytes bytes, bool fin = false) {
    return {RawProbeChannel::NewBidi, std::move(bytes), fin};
}

// Follow-up on the stream opened by write `base`, once its SUBSCRIBE_OK is complete.
RawProbeWrite update_write(Bytes bytes, std::size_t base) {
    RawProbeWrite write{RawProbeChannel::NewBidi, std::move(bytes), false, base};
    write.peer_response_ready = subscribe_ok_ready;
    return write;
}

Spec spec(const char* scenario, std::vector<RowBinding> rows, Builder build, Judge judge) {
    Spec result;
    result.scenario = scenario;
    result.rows = std::move(rows);
    result.build = std::move(build);
    result.judge = std::move(judge);
    return result;
}

Judgement unresolved(const View& view) { return {application_close(view), std::nullopt}; }

// A complete SUBSCRIBE_OK, REQUEST_OK or REQUEST_ERROR on write `index`'s stream.
bool answered(const View& view, std::size_t index) {
    const auto frames = view.write_frames(index);
    return !frames.empty() && (frames.front().type == kSubscribeOk || frames.front().type == kRequestOk ||
                               frames.front().type == kRequestError);
}

// A REQUEST_UPDATE that cannot succeed: it uses an authorization token Alias
// that was never registered. The publisher must end the subscription.
Bytes failing_update(std::uint64_t request_id) {
    return request_update_frame(request_id, {param_lp(kTokenParameter, bytes_of({2, 0}))});
}

Bytes build_subscribe(const Fixture& fixture, std::uint64_t id, unsigned forward) {
    return subscribe_frame(id, fixture, {param_u8(kForwardParameter, forward)});
}

// ---- Section 9.20.18: LARGEST_OBJECT after publication ---------------------------
// The first Object byte is observed before any follow-up is sent, so the
// publisher has published on the Track (Section 3.1.3) when it answers a second
// SUBSCRIBE, a REQUEST_UPDATE or a TRACK_STATUS.
std::optional<Frame> update_response(const View& view) {
    const auto frames = view.write_frames(0);
    // Frame 0 is SUBSCRIBE_OK; the update's response is the next REQUEST_OK/ERROR.
    for (std::size_t index = 1; index < frames.size(); ++index)
        if (frames[index].type == kRequestOk || frames[index].type == kRequestError) return frames[index];
    return std::nullopt;
}

Spec largest_after_publication() {
    return spec("d21-largest-object-required-after-publication",
        {{"D21-9-20-18-MUST-456", "d21-published-track-largest-object-present"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(build_subscribe(fixture, 1, 1)));
            RawProbeWrite second{RawProbeChannel::NewBidi, build_subscribe(fixture, 3, 0), false};
            second.evidence_ready = object_gate;
            definition.writes.push_back(std::move(second));
            auto update = update_write(request_update_frame(5, {param_u8(kPriorityParameter, 100)}), 0);
            update.evidence_ready = object_gate;
            definition.writes.push_back(std::move(update));
            // The status request goes last: it is the one an implementation
            // without TRACK_STATUS support may answer by ending the session.
            RawProbeWrite status{RawProbeChannel::NewBidi, track_status_frame(7, fixture), true};
            status.evidence_ready = [](const RawProbeGateInput& input) {
                const View view(input.prior_writes, input.events);
                if (!view.valid() || !object_gate(input)) return false;
                const auto second_frames = view.write_frames(1);
                return !second_frames.empty() && update_response(view).has_value();
            };
            definition.writes.push_back(std::move(status));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto alias = subscribe_alias(view, 0);
            if (!alias || !first_object_event(view, *alias)) return unresolved(view);
            std::size_t answered_count = 0;
            std::size_t with_largest = 0;
            // false when an acceptance lacks LARGEST_OBJECT.
            const auto check = [&](const Frame& message) {
                if (message.type == kRequestError) { ++answered_count; return true; }
                if (message.type != kSubscribeOk && message.type != kRequestOk) return true;
                const auto block = block_of(message);
                if (!block) return true;
                ++answered_count;
                if (find_value(*block, kLargestObjectParameter)) { ++with_largest; return true; }
                return false;
            };
            const auto second = view.write_frames(1);
            if (!second.empty() && !check(second.front())) return {true, false};
            const auto update = update_response(view);
            if (update && !check(*update)) return {true, false};
            const auto status = view.write_frames(3);
            const bool status_answered = !status.empty();
            if (status_answered && !check(status.front())) return {true, false};
            const bool ready = (!second.empty() && update) && (status_answered || application_close(view));
            if (ready || (application_close(view) && answered_count >= 1))
                return {true, with_largest >= 1 ? std::optional<bool>{true} : std::nullopt};
            return {false, std::nullopt};
        });
}

// Control context: a subscription whose Forward State is 0 and which has seen
// no Object on the wire. Nothing is owed (LARGEST_OBJECT is omitted when no
// Object was published), so a well-formed SUBSCRIBE_OK only shows that the
// publisher answered; absence of the parameter is never a violation here.
Spec largest_before_publication() {
    return spec("d21-largest-object-before-publication",
        {{"D21-9-20-18-MUST-456", "d21-published-track-largest-object-present"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(build_subscribe(fixture, 1, 0)));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto frames = view.write_frames(0);
            if (frames.empty()) return unresolved(view);
            if (frames.front().type != kSubscribeOk) return {true, std::nullopt};
            return {true, block_of(frames.front()) ? std::optional<bool>{true} : std::nullopt};
        });
}

// ---- Section 9.9: PUBLISH_DONE Stream Count --------------------------------------
struct Done {
    std::uint64_t status{0};
    std::uint64_t streams{0};
};

std::optional<Done> publish_done(const std::vector<Frame>& frames) {
    for (const auto& message : frames) {
        if (message.type != kPublishDone) continue;
        wire::Cursor body(message.body);
        const auto status = read_vi(body);
        const auto count = status ? read_vi(body) : std::nullopt;
        if (!status || !count) return std::nullopt;
        return Done{*status, *count};
    }
    return std::nullopt;
}

// Data streams the publisher opened: Subgroup and FETCH streams. SETUP and
// padding streams are not subscription data streams.
std::size_t data_streams(const View& view) {
    std::size_t count = 0;
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        wire::Cursor cursor(stream.bytes);
        const auto type = read_vi(cursor);
        if (!type) continue;
        if (*type == 5 || (*type < 128 && (*type & 0x10u) != 0)) ++count;
    }
    return count;
}

std::size_t datagram_objects(const View& view) {
    std::size_t count = 0;
    for (const auto& datagram : view.datagrams())
        if (parse_datagram_object(datagram.data).valid) ++count;
    return count;
}

bool done_seen_gate(const RawProbeGateInput& input) {
    const View view(input.prior_writes, input.events);
    return view.valid() && publish_done(view.write_frames(0)).has_value();
}

// A request on a fresh stream, sent once PUBLISH_DONE has arrived. Its answer
// is a later round trip, so data streams that PUBLISH_DONE was meant to count
// have had the chance to arrive before the trace is judged.
RawProbeWrite settle_write(const Fixture& fixture) {
    RawProbeWrite settle{RawProbeChannel::NewBidi, build_subscribe(fixture, 5, 0), false};
    settle.evidence_ready = done_seen_gate;
    return settle;
}

Judgement done_judgement(const View& view, bool need_datagram_objects) {
    const auto done = publish_done(view.write_frames(0));
    if (!done) return unresolved(view);
    const auto* stream = view.write_stream(0);
    const bool settled = (stream && stream->fin) || application_close(view) || answered(view, 2);
    if (!settled) return {false, std::nullopt};
    const bool datagrams = datagram_objects(view) != 0;
    // The precondition is the absence of any data stream for the subscription.
    if (data_streams(view) != 0 || datagrams != need_datagram_objects) return {true, std::nullopt};
    return {true, done->streams == 0};
}

Spec done_without_streams() {
    return spec("d21-publish-done-without-data-streams",
        {{"D21-9-9-MUST-365", "d21-publish-done-zero-stream-count"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            // No Object can match a start group this far in the future, so the
            // publisher opens no data stream for the subscription.
            Bytes filter;
            put_vi(filter, 1000000);
            put_vi(filter, 0);
            definition.writes.push_back(request_write(subscribe_frame(
                1, fixture, {param_u8(kForwardParameter, 1), param_lp(0x21, filter)})));
            // A publisher that does not end the subscription by itself must
            // end it for this REQUEST_UPDATE, which cannot succeed.
            definition.writes.push_back(update_write(failing_update(3), 0));
            definition.writes.push_back(settle_write(fixture));
            return definition;
        },
        [](const View& view) { return done_judgement(view, false); });
}

Spec done_datagram_only() {
    return spec("d21-publish-done-datagram-only",
        {{"D21-9-9-MUST-365", "d21-publish-done-zero-stream-count"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(build_subscribe(fixture, 1, 1)));
            auto update = update_write(failing_update(3), 0);
            // Only once an Object arrived as a datagram and no stream carried data.
            update.evidence_ready = [](const RawProbeGateInput& input) {
                const View view(input.prior_writes, input.events);
                return view.valid() && datagram_objects(view) != 0 && data_streams(view) == 0;
            };
            definition.writes.push_back(std::move(update));
            definition.writes.push_back(settle_write(fixture));
            return definition;
        },
        [](const View& view) { return done_judgement(view, true); });
}

// ---- Section 9.4.1: namespace-scoped Redirect --------------------------------------
Judgement redirect_judgement(const View& view) {
    const auto frames = view.write_frames(0);
    if (frames.empty()) return unresolved(view);
    if (frames.front().type != kRequestError) return {true, std::nullopt};
    const auto whole = frame(frames.front().type, frames.front().body);
    wire::Cursor cursor(whole);
    // Decode without the namespace rule so a non-empty Track Name is observable.
    auto decoded = d21::decode_request_error(cursor, false, false);
    const auto* error = std::get_if<d21::RequestErrorMessage>(&decoded);
    if (!error) return {true, std::nullopt};
    if (error->error_code != kRedirect || !error->redirect) return {true, std::nullopt};
    return {true, error->redirect->track_name.empty()};
}

Spec redirect_spec(const char* scenario, std::uint64_t type) {
    return spec(scenario, {{"D21-9-4-1-MUST-340", "d21-namespace-redirect-emitted-empty-track-name"}},
        [type](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(
                discovery_frame(type, 1, fixture.track_namespace, {})));
            return definition;
        },
        redirect_judgement);
}

// ---- Section 9.10: PUBLISH_STATE_NOTIFY ----------------------------------------------
struct Notify {
    Block block;
    std::size_t event{0};
    std::size_t index{0};  // position in the request stream
};

std::vector<Notify> notifications(const View& view) {
    std::vector<Notify> result;
    const auto frames = view.write_frames(0);
    for (std::size_t index = 0; index < frames.size(); ++index) {
        if (frames[index].type != kPublishStateNotify) continue;
        const auto block = block_of(frames[index]);
        if (block) result.push_back({*block, frames[index].event, index});
    }
    return result;
}

Builder notify_build(bool with_update) {
    return [with_update](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.writes.push_back(request_write(subscribe_frame(1, fixture,
            {param_u8(kForwardParameter, 1), param_u8(kPriorityParameter, 100),
             param_u8(kGroupOrderParameter, 1)})));
        if (with_update)
            definition.writes.push_back(update_write(
                request_update_frame(3, {param_u8(kForwardParameter, 0)}), 0));
        return definition;
    };
}

Spec notify_known_largest() {
    return spec("d21-publish-state-notify-known-largest-object",
        {{"D21-9-10-MUST-372", "d21-notify-known-largest-object-present"}},
        notify_build(false), [](const View& view) -> Judgement {
            const auto alias = subscribe_alias(view, 0);
            const auto first = alias ? first_object_event(view, *alias) : std::nullopt;
            if (!first) return unresolved(view);
            bool compared = false;
            for (const auto& notify : notifications(view)) {
                // An Object received before the notification was received is
                // known to the publisher that sent the notification.
                if (notify.event < *first) continue;
                compared = true;
                if (!find_value(notify.block, kLargestObjectParameter)) return {true, false};
            }
            return compared ? Judgement{true, true} : unresolved(view);
        });
}

Spec notify_before_first_object() {
    return spec("d21-publish-state-notify-before-first-object",
        {{"D21-9-10-MUST-372", "d21-notify-known-largest-object-present"}},
        notify_build(false), [](const View& view) -> Judgement {
            const auto alias = subscribe_alias(view, 0);
            if (!alias) return unresolved(view);
            const auto first = first_object_event(view, *alias);
            for (const auto& notify : notifications(view)) {
                // The parameter is owed only when known, which the wire cannot
                // show before any Object: a well-formed notification is
                // compatible, so only its presence is recorded.
                if (!first || notify.event < *first) return {true, true};
            }
            return unresolved(view);
        });
}

// Subscriber-controlled values this subscription requested explicitly.
bool differs_from_requested(const Block& block, std::uint64_t forward) {
    const auto differs = [&](std::uint64_t type, std::uint64_t requested) {
        const auto* value = find_value(block, type);
        return value && value->number && *value->number != requested;
    };
    return differs(kForwardParameter, forward) || differs(kPriorityParameter, 100) ||
           differs(kGroupOrderParameter, 1);
}

Spec notify_preserves_control() {
    return spec("d21-publish-state-notify-preserves-subscriber-control",
        {{"D21-9-10-MUST-NOT-371", "d21-notify-no-unsolicited-subscriber-value-change"}},
        notify_build(false), [](const View& view) -> Judgement {
            const auto notices = notifications(view);
            for (const auto& notify : notices)
                if (differs_from_requested(notify.block, 1)) return {true, false};
            return notices.empty() ? unresolved(view) : Judgement{true, true};
        });
}

Spec notify_requested_forward() {
    return spec("d21-publish-state-notify-requested-forward-change",
        {{"D21-9-10-MUST-NOT-371", "d21-notify-no-unsolicited-subscriber-value-change"}},
        notify_build(true), [](const View& view) -> Judgement {
            const auto frames = view.write_frames(0);
            // The update is in effect once its REQUEST_OK is on the stream.
            std::optional<std::size_t> acknowledged;
            for (std::size_t index = 1; index < frames.size(); ++index)
                if (frames[index].type == kRequestOk) { acknowledged = index; break; }
            if (!acknowledged) return unresolved(view);
            bool compared = false;
            for (const auto& notify : notifications(view)) {
                if (notify.index < *acknowledged) continue;
                compared = true;
                if (differs_from_requested(notify.block, 0)) return {true, false};
            }
            return compared ? Judgement{true, true} : unresolved(view);
        });
}

// ---- Section 11.5: padding emitted by the publisher ---------------------------------
Spec padding_stream_spec() {
    return spec("d21-padding-stream-emission",
        {{"D21-11-5-1-MUST-565", "d21-padding-stream-zero-bytes"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(build_subscribe(fixture, 1, 1)));
            return definition;
        },
        [](const View& view) -> Judgement {
            bool complete = false;
            for (const auto& [id, stream] : view.streams()) {
                if ((id & 3u) != 2u) continue;
                wire::Cursor cursor(stream.bytes);
                const auto type = read_vi(cursor);
                if (!type || *type != kPaddingStreamType) continue;
                const auto rest = std::span<const std::byte>(stream.bytes).subspan(cursor.offset());
                if (std::any_of(rest.begin(), rest.end(), [](std::byte b) { return b != std::byte{0}; }))
                    return {true, false};
                if (stream.fin) complete = true;
            }
            return complete ? Judgement{true, true} : unresolved(view);
        });
}

Spec padding_datagram_spec() {
    return spec("d21-padding-datagram-emission",
        {{"D21-11-5-2-MUST-570", "d21-padding-datagram-zero-bytes"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(build_subscribe(fixture, 1, 1)));
            return definition;
        },
        [](const View& view) -> Judgement {
            bool seen = false;
            for (const auto& datagram : view.datagrams()) {
                wire::Cursor cursor(datagram.data);
                const auto type = read_vi(cursor);
                if (!type || *type != kPaddingDatagramType) continue;
                const auto rest = std::span<const std::byte>(datagram.data).subspan(cursor.offset());
                if (std::any_of(rest.begin(), rest.end(), [](std::byte b) { return b != std::byte{0}; }))
                    return {true, false};
                seen = true;
            }
            return seen ? Judgement{true, true} : unresolved(view);
        });
}

// ---- Sections 9.15, 9.18, 9.20.3: discovery authorization -----------------------------
Bytes text_bytes(const std::string& value) {
    Bytes result;
    for (const char c : value) result.push_back(static_cast<std::byte>(static_cast<unsigned char>(c)));
    return result;
}

std::string denied_value(const Fixture& fixture) {
    return fixture.denied_token.empty() ? std::string(kDefaultDeniedToken) : fixture.denied_token;
}

// Token Type 0 is negotiated out of band; the operator configures the
// publisher's policy to refuse this value (Section 8.9).
Param denied_credential(const Fixture& fixture) {
    return param_lp(kTokenParameter, token_value(3, std::nullopt, 0, text_bytes(denied_value(fixture))));
}

Judgement authorization_judgement(const View& view) {
    const auto frames = view.write_frames(0);
    if (frames.empty()) return unresolved(view);
    // Without a controllable policy the publisher may legitimately grant
    // every discovery request: nothing can be concluded.
    if (!view.denied_token()) return {true, std::nullopt};
    if (frames.front().type == kRequestError) return {true, true};
    if (frames.front().type == kRequestOk) return {true, false};
    return {true, std::nullopt};
}

Spec discovery_authorization(const char* scenario, std::uint64_t type, const char* requirement,
                             const char* evaluator) {
    return spec(scenario, {{requirement, evaluator}},
        [type](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(discovery_frame(
                type, 1, fixture.track_namespace, {denied_credential(fixture)})));
            return definition;
        },
        authorization_judgement);
}

constexpr const char* kSubscriberCredential = "interop-subscriber-credential";

struct ResolvedToken {
    std::uint64_t type{0};
    Bytes value;
};

// Resolves the tokens of a PUBLISH, following aliases the publisher registered.
std::vector<ResolvedToken> resolved_tokens(const d21::PublishMessage& message,
                                           std::map<std::uint64_t, ResolvedToken>& aliases) {
    std::vector<ResolvedToken> result;
    for (const auto& parameter : message.parameters) {
        const auto* token = std::get_if<d21::Token>(&parameter.value);
        if (!token || parameter.type != kTokenParameter) continue;
        switch (token->alias_type) {
            case d21::TokenAliasType::Register:
                if (token->alias && token->token_type) {
                    aliases[*token->alias] = {*token->token_type, token->value};
                    result.push_back({*token->token_type, token->value});
                }
                break;
            case d21::TokenAliasType::UseValue:
                if (token->token_type) result.push_back({*token->token_type, token->value});
                break;
            case d21::TokenAliasType::UseAlias:
                // A Section 8.9 alias space is per direction: only an alias the
                // publisher registered resolves to a credential.
                if (token->alias && aliases.contains(*token->alias)) result.push_back(aliases[*token->alias]);
                break;
            case d21::TokenAliasType::Delete:
                if (token->alias) aliases.erase(*token->alias);
                break;
        }
    }
    return result;
}

Spec token_not_copied() {
    return spec("d21-track-discovery-does-not-copy-authorization",
        {{"D21-9-20-3-MUST-NOT-407", "d21-discovery-authorization-not-copied"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(discovery_frame(
                0x51, 1, fixture.track_namespace,
                {param_lp(kTokenParameter, token_value(3, std::nullopt, 0, text_bytes(kSubscriberCredential)))})));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto frames = view.write_frames(0);
            const Bytes credential = text_bytes(kSubscriberCredential);
            std::map<std::uint64_t, ResolvedToken> aliases;
            bool observed = false;
            for (const auto& [id, stream] : view.streams()) {
                if ((id & 3u) != 0u) continue;  // publisher-opened request streams
                wire::Cursor cursor(stream.bytes);
                auto decoded = d21::decode_publish(cursor);
                const auto* publish = std::get_if<d21::PublishMessage>(&decoded);
                if (!publish) continue;
                observed = true;
                for (const auto& token : resolved_tokens(*publish, aliases))
                    if (token.type == 0 && token.value == credential) return {true, false};
            }
            // Only a PUBLISH that follows an accepted SUBSCRIBE_TRACKS is its result.
            if (observed && !frames.empty() && frames.front().type == kRequestOk) return {true, true};
            if (!frames.empty() && frames.front().type == kRequestError) return {true, std::nullopt};
            return unresolved(view);
        });
}

// ---- Section 11.3.2: early Subgroup termination -------------------------------------
Spec early_reset() {
    return spec("d21-subgroup-early-handoff-reset",
        {{"D21-11-3-2-MUST-543", "d21-early-subgroup-reset"}},
        [](const Fixture& fixture) {
            auto definition = base_definition("");
            definition.writes.push_back(request_write(build_subscribe(fixture, 1, 1)));
            // Forward State 0 makes the publisher stop sending partway through a
            // Subgroup; wait for a stream that is open.
            auto pause = update_write(request_update_frame(3, {param_u8(kForwardParameter, 0)}), 0);
            pause.evidence_ready = [](const RawProbeGateInput& input) {
                const View view(input.prior_writes, input.events);
                const auto alias = view.valid() ? subscribe_alias(view, 0) : std::nullopt;
                if (!alias) return false;
                for (const auto& [id, stream] : view.streams()) {
                    if ((id & 3u) != 2u || stream.fin || stream.reset) continue;
                    const auto parsed = parse_subgroup(stream.bytes);
                    if (parsed.header && parsed.alias == *alias && !parsed.terminal) return true;
                }
                return false;
            };
            definition.writes.push_back(std::move(pause));
            // Resume once the pause was acknowledged, so Objects that follow the
            // paused ones show whether the earlier stream was complete.
            RawProbeWrite resume{RawProbeChannel::NewBidi,
                                 request_update_frame(5, {param_u8(kForwardParameter, 1)}), false, 0};
            resume.evidence_ready = [](const RawProbeGateInput& input) {
                const View view(input.prior_writes, input.events);
                if (!view.valid()) return false;
                const auto frames = view.write_frames(0);
                return frames.size() > 1 && frames[1].type == kRequestOk;
            };
            definition.writes.push_back(std::move(resume));
            return definition;
        },
        [](const View& view) -> Judgement {
            const auto alias = subscribe_alias(view, 0);
            const auto pause = view.write_event(1);
            if (!alias || !pause) return unresolved(view);
            struct Stream {
                transport::StreamId id;
                Subgroup parsed;
                const StreamRecord* record;
            };
            std::vector<Stream> streams;
            for (const auto& [id, record] : view.streams()) {
                if ((id & 3u) != 2u) continue;
                auto parsed = parse_subgroup(record.bytes);
                if (parsed.header && parsed.alias == *alias) streams.push_back({id, std::move(parsed), &record});
            }
            std::optional<bool> verdict;
            for (const auto& open : streams) {
                // Streams that were open, and not complete, when Forward State 0 was sent.
                if (open.record->first_event >= *pause || open.parsed.terminal) continue;
                if ((open.record->fin_event && *open.record->fin_event < *pause) ||
                    (open.record->reset_event && *open.record->reset_event < *pause)) continue;
                if (open.record->reset) { verdict = true; continue; }
                if (!open.record->fin || open.parsed.objects.empty()) continue;
                // A FIN is a promise of completeness. Objects of the same Group and
                // Subgroup past the last one on this stream disprove it.
                const auto last = open.parsed.objects.back().id;
                for (const auto& other : streams) {
                    if (other.id == open.id || other.parsed.group != open.parsed.group ||
                        other.parsed.subgroup != open.parsed.subgroup) continue;
                    for (const auto& object : other.parsed.objects)
                        if (object.id > last) return {true, false};
                }
            }
            if (verdict) return {true, verdict};
            return unresolved(view);
        });
}

// ---- Sections 3.3.2 and 10.7: property filters --------------------------------------
// An even-typed Property carrying one integer value, found in exactly one list.
struct Candidate {
    std::uint64_t type{0};
    std::uint64_t value{0};
};

std::optional<Candidate> mutable_only_candidate(const Obj& object, bool in_immutable) {
    if (!object.has_properties) return std::nullopt;
    std::map<std::uint64_t, std::vector<std::uint64_t>> top;
    std::map<std::uint64_t, std::vector<std::uint64_t>> inner;
    for (const auto& entry : object.properties.top)
        if (const auto* number = std::get_if<std::uint64_t>(&entry.value); number && entry.type % 2 == 0)
            top[entry.type].push_back(*number);
    for (const auto& list : object.properties.immutable)
        for (const auto& entry : list)
            if (const auto* number = std::get_if<std::uint64_t>(&entry.value); number && entry.type % 2 == 0)
                inner[entry.type].push_back(*number);
    const auto& wanted = in_immutable ? inner : top;
    const auto& other = in_immutable ? top : inner;
    for (const auto& [type, values] : wanted) {
        if (type == kImmutablePropertiesType || other.contains(type) || values.size() != 1) continue;
        return Candidate{type, values.front()};
    }
    return std::nullopt;
}

// First Object of the unfiltered subscription (write 0) offering a candidate.
std::optional<Candidate> find_candidate(const View& view, bool in_immutable) {
    const auto alias = subscribe_alias(view, 0);
    if (!alias) return std::nullopt;
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u) continue;
        const auto parsed = parse_subgroup(stream.bytes);
        if (!parsed.header || parsed.alias != *alias) continue;
        for (const auto& object : parsed.objects)
            if (const auto candidate = mutable_only_candidate(object, in_immutable)) return candidate;
    }
    return std::nullopt;
}

Bytes property_filter_value(const Candidate& candidate) {
    Bytes value;
    value.push_back(std::byte{0});  // SetID 0
    put_vi(value, candidate.type);
    put_vi(value, candidate.value);  // Range Start
    put_vi(value, 0);                // Range End delta: a single value
    return value;
}

Spec property_filter_spec(const char* scenario, const char* requirement, const char* evaluator,
                          bool in_immutable) {
    return spec(scenario, {{requirement, evaluator}},
        [in_immutable](const Fixture& fixture) {
            auto definition = base_definition("");
            // Range Filters need a non-zero MAX_FILTER_RANGES (Section 3.3.2).
            definition.peer_setup_ready = [](auto input) {
                if (!setup_decodes(input)) return false;
                const auto limit = setup_numeric_option(input, kMaxFilterRanges);
                return limit && *limit >= 1;
            };
            definition.writes.push_back(request_write(subscribe_frame(1, fixture,
                {param_u8(kForwardParameter, 1)})));
            RawProbeWrite filtered{RawProbeChannel::NewBidi, {}, false};
            filtered.prepare_bytes = [fixture, in_immutable](const RawProbeGateInput& input) -> std::optional<Bytes> {
                const View view(input.prior_writes, input.events);
                if (!view.valid()) return std::nullopt;
                const auto candidate = find_candidate(view, in_immutable);
                if (!candidate) return std::nullopt;
                return subscribe_frame(3, fixture, {param_u8(kForwardParameter, 1),
                    param_lp(kObjectPropertyFilter, property_filter_value(*candidate))});
            };
            definition.writes.push_back(std::move(filtered));
            return definition;
        },
        [in_immutable](const View& view) -> Judgement {
            const auto candidate = find_candidate(view, in_immutable);
            const auto filtered = subscribe_alias(view, 1);
            if (!candidate || !filtered) {
                const auto frames = view.write_frames(1);
                if (!frames.empty() && frames.front().type == kRequestError) return {true, std::nullopt};
                return unresolved(view);
            }
            // Objects of the filtered subscription that carry the property value
            // in the list this scenario places it.
            std::size_t matching = 0;
            for (const auto& [id, stream] : view.streams()) {
                if ((id & 3u) != 2u) continue;
                const auto parsed = parse_subgroup(stream.bytes);
                if (!parsed.header || parsed.alias != *filtered) continue;
                for (const auto& object : parsed.objects) {
                    const auto found = mutable_only_candidate(object, in_immutable);
                    if (found && found->type == candidate->type && found->value == candidate->value) ++matching;
                }
            }
            if (matching != 0) return {true, true};
            // Objects with that value keep arriving on the unfiltered
            // subscription after the filter was accepted, yet none passes it.
            const auto accepted = view.write_event(1);
            const auto unfiltered = subscribe_alias(view, 0);
            std::size_t later = 0;
            for (const auto& [id, stream] : view.streams()) {
                if ((id & 3u) != 2u || !accepted || stream.first_event < *accepted) continue;
                const auto parsed = parse_subgroup(stream.bytes);
                if (!parsed.header || !unfiltered || parsed.alias != *unfiltered) continue;
                for (const auto& object : parsed.objects) {
                    const auto found = mutable_only_candidate(object, in_immutable);
                    if (found && found->type == candidate->type && found->value == candidate->value) ++later;
                }
            }
            if (later >= 2 && view.close()) return {true, false};
            return unresolved(view);
        });
}

// ---- Section 13: unknown GREASE values ------------------------------------------------

// Continued usability proves the unknown value was handled without ending the
// session: the follow-up request on a fresh stream is answered.
Judgement grease_handled(const View& view, std::size_t follow_up) {
    if (answered(view, follow_up)) return {true, true};
    return unresolved(view);
}

// An application close other than NO_ERROR after the stimulus and before any
// answer is a session closed because of it (the only departure from ordinary
// traffic is the unknown value). A close with no code, or a transport close,
// proves nothing.
Judgement grease_not_closed(const View& view, std::size_t follow_up, std::optional<std::size_t> after) {
    if (answered(view, follow_up)) return {true, true};
    const auto& close = view.close();
    if (!close) return {false, std::nullopt};
    if (close->application && close->code != 0 && (!after || close->event >= *after)) return {true, false};
    return {true, std::nullopt};
}

Bytes grease_token_request(const Fixture& fixture) {
    return subscribe_frame(1, fixture, {param_u8(kForwardParameter, 1),
        param_lp(kTokenParameter, token_value(3, std::nullopt, kGreaseValue, text_bytes("interop")))});
}

Spec grease_token_type_spec() {
    Spec spec;
    spec.scenario = "d21-grease-auth-token-type";
    spec.rows = {{"D21-13-MUST-593", "d21-grease-publisher-context-handling",
                  [](const View& view) { return grease_handled(view, 1); }},
                 {"D21-13-MUST-NOT-594", "d21-grease-no-unknown-value-session-close",
                  [](const View& view) { return grease_not_closed(view, 1, std::nullopt); }}};
    spec.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.writes.push_back(request_write(grease_token_request(fixture)));
        // A fresh request proves the session still serves requests.
        definition.writes.push_back(request_write(build_subscribe(fixture, 3, 0)));
        return definition;
    };
    spec.judge = [](const View& view) { return grease_handled(view, 1); };
    return spec;
}

constexpr std::uint64_t kUnknownResetCode = kGreaseValue;  // Stream Reset Error Codes, 0x7f * N + 0x9D

std::optional<transport::StreamId> open_subgroup_stream(const RawProbeGateInput& input) {
    const View view(input.prior_writes, input.events);
    if (!view.valid()) return std::nullopt;
    const auto alias = subscribe_alias(view, 0);
    if (!alias) return std::nullopt;
    for (const auto& [id, stream] : view.streams()) {
        if ((id & 3u) != 2u || stream.fin || stream.reset) continue;
        const auto parsed = parse_subgroup(stream.bytes);
        if (parsed.header && parsed.alias == *alias) return id;
    }
    return std::nullopt;
}

Spec grease_stop_sending_spec() {
    Spec spec;
    spec.scenario = "d21-grease-stop-sending";
    spec.rows = {{"D21-13-MUST-593", "d21-unknown-stop-sending-graceful-handling",
                  [](const View& view) { return grease_handled(view, 2); }},
                 {"D21-13-MUST-NOT-594", "d21-unknown-stop-sending-preserves-session",
                  [](const View& view) { return grease_not_closed(view, 2, view.write_event(1)); }}};
    spec.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        definition.writes.push_back(request_write(build_subscribe(fixture, 1, 1)));
        // The subscriber cancels one of the publisher's open Subgroup streams
        // with a Stream Reset Error Code the publisher cannot know.
        RawProbeWrite stop{RawProbeChannel::NewUni, {}, false};
        stop.operation = RawProbeOperation::StopSending;
        stop.application_error = kUnknownResetCode;
        stop.select_peer_stream = open_subgroup_stream;
        definition.writes.push_back(std::move(stop));
        definition.writes.push_back(request_write(build_subscribe(fixture, 3, 0)));
        return definition;
    };
    spec.judge = [](const View& view) {
        // Nothing is concluded unless the STOP_SENDING was actually delivered.
        if (!view.write_event(1)) return unresolved(view);
        return grease_handled(view, 2);
    };
    return spec;
}

// ---- Section 9.2: GOAWAY New Session URI --------------------------------------------
Bytes goaway_frame(const std::string& uri) {
    Bytes body;
    put_lp(body, text_bytes(uri));
    put_vi(body, 0);  // Timeout 0: migrate as quickly as possible
    return frame(0x10, body);
}

struct UriParts {
    std::string authority;
    std::string path;
};

std::optional<UriParts> split_uri(const std::string& uri) {
    const auto scheme = uri.find("://");
    if (scheme == std::string::npos) return std::nullopt;
    const auto start = scheme + 3;
    const auto slash = uri.find('/', start);
    if (slash == std::string::npos) return UriParts{uri.substr(start), "/"};
    return UriParts{uri.substr(start, slash - start), uri.substr(slash)};
}

std::optional<std::string> option_text(const d21::SetupMessage& setup, std::uint64_t type) {
    for (const auto& option : setup.options) {
        if (option.type != type) continue;
        const auto* bytes = std::get_if<Bytes>(&option.value);
        if (!bytes) return std::nullopt;
        std::string value;
        for (const auto byte : *bytes) value.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
        return value;
    }
    return std::nullopt;
}

Spec goaway_alternate_uri() {
    Spec spec;
    spec.scenario = "d21-publisher-goaway-alternate-uri";
    spec.rows = {{"D21-9-2-MUST-329", "d21-client-migrates-to-provided-uri"}};
    spec.build = [](const Fixture& fixture) {
        auto definition = base_definition("");
        // An established subscription is open when the GOAWAY arrives.
        definition.writes.push_back(request_write(build_subscribe(fixture, 1, 0)));
        definition.writes.push_back({RawProbeChannel::Control, goaway_frame("moqt://unbound.invalid/moq"), false});
        definition.alternate_listener = true;
        definition.bind_alternate_uri = [](RawProbeDefinition& bound, const std::string& uri) {
            bound.writes[1].bytes = goaway_frame(uri);
        };
        return definition;
    };
    spec.judge = [](const View& view) -> Judgement {
        const auto uri = view.alternate_uri();
        if (!uri || !view.write_event(1)) return {false, std::nullopt};
        // Only a session on the second listener shows the New Session URI was used.
        bool established = false;
        for (const auto& event : view.alternate_events())
            if (std::holds_alternative<transport::ConnectionEstablishedEvent>(event)) established = true;
        const auto setup = established ? peer_setup_from_events(view.alternate_events()) : std::nullopt;
        if (!setup) return {false, std::nullopt};
        // A native QUIC client builds AUTHORITY and PATH from the URI it connects to
        // (Sections 9.1.1 and 9.1.2); an option that contradicts the URI shows another
        // one was used.
        const auto parts = split_uri(*uri);
        if (!parts) return {true, std::nullopt};
        const auto authority = option_text(*setup, 5);
        const auto path = option_text(*setup, 1);
        if ((authority && *authority != parts->authority) || (path && *path != parts->path))
            return {true, false};
        return {true, true};
    };
    return spec;
}

}  // namespace

std::vector<Spec> d21b_specs() {
    std::vector<Spec> result;
    result.push_back(largest_after_publication());
    result.push_back(largest_before_publication());
    result.push_back(done_without_streams());
    result.push_back(done_datagram_only());
    result.push_back(redirect_spec("d21-publisher-namespace-redirect", 0x50));
    result.push_back(redirect_spec("d21-publisher-subscribe-tracks-redirect", 0x51));
    result.push_back(notify_known_largest());
    result.push_back(notify_before_first_object());
    result.push_back(notify_preserves_control());
    result.push_back(notify_requested_forward());
    result.push_back(padding_stream_spec());
    result.push_back(padding_datagram_spec());
    result.push_back(discovery_authorization("d21-namespace-discovery-authorization", 0x50,
        "D21-9-15-MUST-386", "d21-namespace-discovery-authorized-only"));
    result.push_back(discovery_authorization("d21-track-discovery-authorization", 0x51,
        "D21-9-18-MUST-394", "d21-track-discovery-authorized-only"));
    result.push_back(token_not_copied());
    result.push_back(early_reset());
    result.push_back(property_filter_spec("d21-filter-mutable-property", "D21-10-7-MUST-489",
        "d21-filter-finds-mutable-property", false));
    result.push_back(property_filter_spec("d21-filter-immutable-property", "D21-10-7-MUST-490",
        "d21-filter-finds-immutable-property", true));
    result.push_back(grease_token_type_spec());
    result.push_back(grease_stop_sending_spec());
    result.push_back(goaway_alternate_uri());
    return result;
}

// The SETUP-option and REQUEST_ERROR GREASE contexts are defined with the
// session profiles; slice B adds the Section 13 rows that need them.
void d21b_attach_rows(std::vector<Spec>& specs) {
    for (auto& spec : specs) {
        if (spec.scenario == "d21-grease-setup-options") {
            spec.rows.push_back({"D21-13-MUST-593", "d21-grease-publisher-context-handling",
                                 [](const View& view) { return grease_handled(view, 0); }});
            spec.rows.push_back({"D21-13-MUST-NOT-594", "d21-grease-no-unknown-value-session-close",
                                 [](const View& view) { return grease_not_closed(view, 0, std::nullopt); }});
        } else if (spec.scenario == "d21-grease-request-error") {
            spec.rows.push_back({"D21-13-MUST-593", "d21-grease-publisher-context-handling",
                                 [](const View& view) { return grease_handled(view, 1); }});
            spec.rows.push_back({"D21-13-MUST-NOT-594", "d21-grease-no-unknown-value-session-close",
                                 [](const View& view) { return grease_not_closed(view, 1, std::nullopt); }});
        }
    }
}

}  // namespace moq::interop::scenarios::d21c
