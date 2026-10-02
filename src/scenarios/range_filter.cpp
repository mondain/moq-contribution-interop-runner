#include "moq/interop/scenarios/range_filter.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/request_error.h"
#include "moq/interop/wire/draft21/successful_response.h"
#include "moq/interop/wire/draft21/publish_done.h"

#include <map>
#include <algorithm>
#include "moq/interop/wire/draft21/request_frame.h"
#include <stdexcept>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
constexpr std::size_t bound = 65546;
struct Fixture { Namespace track_namespace; Bytes name; };
bool valid_fixture(const Fixture& f) {
    if (f.track_namespace.size()>32 || f.name.empty() || f.name.size()>4096) return false;
    std::size_t total=f.name.size();
    for(const auto& field:f.track_namespace) {
        if(field.empty() || field.size()>4096-total) return false;
        total+=field.size();
    }
    if(f.track_namespace.empty())return true;
    return f.track_namespace.front()!=Bytes{std::byte{'.'}} &&
        f.track_namespace.front()!=Bytes{std::byte{'.'},std::byte{'s'},std::byte{'e'},std::byte{'s'},std::byte{'s'},std::byte{'i'},std::byte{'o'},std::byte{'n'}};
}
std::optional<Fixture> fixture(std::span<const std::byte> bytes) {
    if(bytes.size()>65546)return {};
    wire::Cursor cursor(bytes);
    const auto decoded=wire::draft21::decode_request_frame(cursor,true);
    const auto* frame=std::get_if<wire::draft21::RequestFrame>(&decoded);
    if(!frame || frame->type.type!=3 || cursor.remaining())return {};
    wire::Cursor body(frame->body);
    const auto request=wire::read_vi64(body);
    const auto count=wire::read_vi64(body);
    const auto* id=std::get_if<std::uint64_t>(&request);
    const auto* fields=std::get_if<std::uint64_t>(&count);
    if(!id || *id!=1 || !fields || *fields>32)return {};
    Fixture result;
    std::size_t total=0;
    for(std::uint64_t i=0;i<*fields;++i) {
        const auto field=wire::read_length_prefixed_bytes(body,4096-total);
        const auto* value=std::get_if<std::span<const std::byte>>(&field);
        if(!value || value->empty())return {};
        total+=value->size();
        result.track_namespace.emplace_back(value->begin(),value->end());
    }
    const auto name=wire::read_length_prefixed_bytes(body,4096-total);
    const auto* value=std::get_if<std::span<const std::byte>>(&name);
    if(!value)return {};
    result.name.assign(value->begin(),value->end());
    if(!valid_fixture(result))return {};
    // Only track identity is recovered. The regenerated profile proves every
    // remaining byte, delivery marker, and gate against the actual prefix.
    return result;
}
void integer(Bytes& out, std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("range probe vi64");
    out.insert(out.end(), writer.bytes().begin(), writer.bytes().end());
}
Bytes frame(unsigned type, Bytes body) {
    if (body.size() > 65535) throw std::logic_error("range probe frame capacity");
    Bytes out{std::byte(type), std::byte(body.size() >> 8), std::byte(body.size() & 255)};
    out.insert(out.end(), body.begin(), body.end());
    return out;
}
std::optional<std::uint64_t> capacity(std::span<const std::byte> input, bool require_omitted = false) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft21::decode_setup(cursor);
    const auto* setup = std::get_if<wire::draft21::SetupMessage>(&decoded);
    if (!setup) return {};
    for (const auto& option : setup->options)
        if (option.type == 6) return require_omitted ? std::nullopt : std::optional{std::get<std::uint64_t>(option.value)};
    return 0;
}
std::optional<std::uint64_t> peer_capacity(const RawProbeGateInput& input, bool require_omitted = false) {
    std::map<transport::StreamId, Bytes> candidates;
    std::optional<std::uint64_t> result;
    std::optional<transport::StreamId> selected;
    std::size_t total = 0;
    for (const auto& event : input.events) {
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || (data->stream_id & 3u) != 2u) continue;
        if (data->data.size() > bound - total) return {};
        total += data->data.size();
        auto& bytes = candidates[data->stream_id];
        bytes.insert(bytes.end(), data->data.begin(), data->data.end());
        const auto decoded = capacity(bytes, require_omitted);
        if (decoded) {
            if (selected && *selected != data->stream_id) return {};
            selected = data->stream_id;
            result = decoded;
        }
    }
    return result;
}
Bytes subscribe(const Namespace& ns, const Bytes& name, const Bytes& params, unsigned count) {
    Bytes body;
    integer(body, 1); integer(body, ns.size());
    for (const auto& part : ns) { integer(body, part.size()); body.insert(body.end(), part.begin(), part.end()); }
    integer(body, name.size()); body.insert(body.end(), name.begin(), name.end());
    integer(body, count); body.insert(body.end(), params.begin(), params.end());
    return frame(3, std::move(body));
}
void filter(Bytes& params, unsigned delta, unsigned set, std::uint64_t ranges) {
    integer(params, delta); integer(params, 1 + 2 * ranges); integer(params, set);
    for (std::uint64_t i = 0; i < ranges; ++i) { integer(params, 1); integer(params, 0); }
}
std::optional<Bytes> excess(const Namespace& ns, const Bytes& name, std::uint64_t cap) {
    // Even the minimal range pairs exceed the uint16 frame bound beyond this.
    if (cap > 32760) return {};
    Bytes params;
    if (cap == 0) filter(params, 0x26, 0, 1);
    else { filter(params, 0x26, 0, cap); filter(params, 0, 1, 1); }
    try { return subscribe(ns, name, params, cap ? 2 : 1); }
    catch (const std::logic_error&) { return {}; }
}
struct Responses { Bytes bytes; bool invalid{false}; };
Responses responses(const RawProbeTranscript& t, std::size_t after, transport::StreamId stream) {
    Responses out;
    for (std::size_t i = after; i < t.events.size(); ++i) {
        if (const auto* d = std::get_if<transport::StreamDataEvent>(&t.events[i]); d && d->stream_id == stream) {
            if (d->data.size() > bound - out.bytes.size()) { out.invalid = true; return out; }
            out.bytes.insert(out.bytes.end(), d->data.begin(), d->data.end());
            if(d->fin)break;
        } else if(const auto* reset=std::get_if<transport::PeerResetEvent>(&t.events[i]);reset && reset->stream_id==stream) {
            break;
        } else if(std::holds_alternative<transport::PeerCloseEvent>(t.events[i]) ||
                  std::holds_alternative<transport::LocalCloseEvent>(t.events[i]) ||
                  std::holds_alternative<transport::IdleTimeoutEvent>(t.events[i])) {
            break;
        }
    }
    return out;
}
bool initial_ok(std::span<const std::byte> bytes) {
    wire::Cursor cursor(bytes);
    const auto value = wire::draft21::decode_successful_response(cursor, wire::draft21::ResponseContext::Subscribe);
    return std::holds_alternative<wire::draft21::SuccessfulResponse>(value) && cursor.remaining() == 0;
}
std::optional<bool> rejection(const RawProbeTranscript& t) {
    if (t.writes.empty() || !t.writes.back().delivery_event_count || !t.writes.back().stream_id) return {};
    auto received = responses(t, *t.writes.back().delivery_event_count, *t.writes.back().stream_id);
    if (received.invalid) return {};
    wire::Cursor cursor(received.bytes);
    std::optional<bool> result;
    while (cursor.remaining()) {
        wire::Cursor peek(std::span<const std::byte>(received.bytes).subspan(cursor.offset()));
        const auto type = wire::read_vi64(peek);
        const auto* number = std::get_if<std::uint64_t>(&type);
        if (!number) return {};
        if (*number == 5) {
            const auto decoded = wire::draft21::decode_request_error(cursor, true, false);
            const auto* error = std::get_if<wire::draft21::RequestErrorMessage>(&decoded);
            if (!error) return {};
            if (result) return {};
            result = error->error_code == 0x36;
            continue;
        }
        if (*number == 4 || *number == 7) {
            const auto decoded = wire::draft21::decode_successful_response(cursor,
                *number == 7 ? wire::draft21::ResponseContext::RequestUpdate : wire::draft21::ResponseContext::Subscribe);
            if (!std::holds_alternative<wire::draft21::SuccessfulResponse>(decoded)) return {};
            if (result) return {};
            result = false;
            continue;
        }
        if (*number == 0xb) {
            const auto decoded = wire::draft21::decode_publish_done(cursor);
            if (!std::holds_alternative<wire::draft21::PublishDoneMessage>(decoded)) return {};
            continue;
        }
        return {};
    }
    return result;
}
}  // namespace

std::vector<RangeFilterProbe> draft21_range_filter_probes(
    std::chrono::milliseconds deadline, Namespace ns, Bytes name) {
    if(!valid_fixture({ns,name}))
        throw std::invalid_argument("range probe requires ordinary named track");
    std::vector<RangeFilterProbe> result;
    RawProbeDefinition duplicate;
    duplicate.id = "d21-duplicate-range-filter-key-in-update";
    duplicate.setup_bytes = {std::byte{0xaf}, std::byte{0}, std::byte{0}, std::byte{0}};
    duplicate.deadline = deadline;
    duplicate.peer_setup_ready = [](auto bytes) { const auto cap = capacity(bytes); return cap && *cap >= 2; };
    duplicate.writes.push_back({RawProbeChannel::NewBidi, subscribe(ns, name, {}, 0), false});
    Bytes params; filter(params, 0x26, 0, 1); filter(params, 0, 0, 1);
    Bytes update{std::byte{3}, std::byte{2}};
    update.insert(update.end(), params.begin(), params.end());
    RawProbeWrite write{RawProbeChannel::NewBidi, frame(2, std::move(update)), true};
    write.reuse_write_stream = 0;
    write.peer_response_ready = initial_ok;
    duplicate.writes.push_back(std::move(write));
    duplicate.response_ready = [](const auto& t) { return rejection(t).has_value(); };
    result.push_back({"D21-3-3-2-MUST-064", "d21-duplicate-range-filter-invalid-filter", 21, std::move(duplicate)});
    for (const bool zero : {false, true}) {
        RawProbeDefinition definition;
        definition.id = zero ? "d21-range-filter-with-zero-negotiated-limit" : "d21-range-filter-total-exceeds-negotiated-limit";
        definition.setup_bytes = {std::byte{0xaf}, std::byte{0}, std::byte{0}, std::byte{0}};
        definition.deadline = deadline;
        definition.peer_setup_ready = [zero, ns, name](auto bytes) {
            const auto cap = capacity(bytes);
            return cap && ((*cap == 0) == zero) && excess(ns, name, *cap).has_value();
        };
        RawProbeWrite dynamic{RawProbeChannel::NewBidi, {}, true};
        dynamic.prepare_bytes = [ns, name, zero](const RawProbeGateInput& input) -> std::optional<Bytes> {
            const auto cap = peer_capacity(input);
            if (!cap || ((*cap == 0) != zero)) return {};
            return excess(ns, name, *cap);
        };
        definition.writes.push_back(std::move(dynamic));
        definition.response_ready = [](const auto& t) { return rejection(t).has_value(); };
        result.push_back({"D21-3-3-2-MUST-065", "d21-excess-filter-ranges-invalid-filter", 21, std::move(definition)});
    }
    // Section 9.1.6 has three independently required catalogue contexts.
    // Reuse the encoding helpers while retaining their own actual gates.
    for (const bool zero : {false, true}) {
        RawProbeDefinition definition = result[zero ? 2 : 1].definition;
        definition.id = zero ? "d21-range-filter-default-zero-limit" : "d21-range-filter-total-limit";
        if (zero) {
            definition.peer_setup_ready = [](auto bytes) { return capacity(bytes, true).has_value(); };
            definition.writes[0].prepare_bytes = [ns, name](const RawProbeGateInput& input) -> std::optional<Bytes> {
                const auto cap = peer_capacity(input, true);
                return cap ? excess(ns, name, *cap) : std::nullopt;
            };
        }
        result.push_back({"D21-9-1-6-MUST-315", "d21-range-filter-limit-invalid-filter", 21, std::move(definition)});
    }
    RawProbeDefinition retained;
    retained.id = "d21-range-filter-update-total-limit";
    retained.setup_bytes = {std::byte{0xaf}, std::byte{0}, std::byte{0}, std::byte{0}};
    retained.deadline = deadline;
    const auto initial = [ns, name](std::uint64_t cap) -> std::optional<Bytes> {
        if (!cap || cap > 32760) return {};
        Bytes parameters;
        filter(parameters, 0x26, 0, cap);
        try { return subscribe(ns, name, parameters, 1); }
        catch (const std::logic_error&) { return {}; }
    };
    retained.peer_setup_ready = [initial](auto bytes) {
        const auto cap = capacity(bytes);
        return cap && initial(*cap).has_value();
    };
    RawProbeWrite opening{RawProbeChannel::NewBidi, {}, false};
    opening.prepare_bytes = [initial](const RawProbeGateInput& input) -> std::optional<Bytes> {
        const auto cap = peer_capacity(input);
        return cap ? initial(*cap) : std::nullopt;
    };
    retained.writes.push_back(std::move(opening));
    RawProbeWrite addition{RawProbeChannel::NewBidi,
        {std::byte{2},std::byte{0},std::byte{7},std::byte{3},std::byte{1},std::byte{0x26},std::byte{3},std::byte{1},std::byte{1},std::byte{0}}, true};
    addition.reuse_write_stream = 0;
    addition.peer_response_ready = initial_ok;
    retained.writes.push_back(std::move(addition));
    retained.response_ready = [](const auto& t) { return rejection(t).has_value(); };
    result.push_back({"D21-9-1-6-MUST-315", "d21-range-filter-limit-invalid-filter", 21, std::move(retained)});
    return result;
}
std::optional<bool> evaluate_range_filter_probe(const RawProbeTranscript& t, const RangeFilterProbe& p) {
    if(p.draft!=21 || p.definition.deadline.count()<=0 || t.writes.empty())return {};
    const auto actual=fixture(t.writes.front().write.bytes);
    if(!actual)return {};
    const auto candidates=draft21_range_filter_probes(p.definition.deadline,actual->track_namespace,actual->name);
    const auto expected=std::find_if(candidates.begin(),candidates.end(),[&](const auto& c) {
        return c.requirement_id==p.requirement_id && c.evaluator_id==p.evaluator_id &&
               c.draft==p.draft && c.definition.id==p.definition.id;
    });
    if(expected==candidates.end() || !raw_probe_stimulus_valid(t,expected->definition))return {};
    return rejection(t);
}
}  // namespace moq::interop::scenarios
