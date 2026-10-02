#include "moq/interop/scenarios/discovery_overlap.h"
#include "moq/interop/scenarios/draft18_response.h"
#include "moq/interop/wire/draft21/request_error.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"
#include <algorithm>
#include <set>
#include <stdexcept>

namespace moq::interop::scenarios {
namespace {
namespace d18 = wire::draft18;
namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
constexpr std::size_t kMaximumFrame = 65546;
constexpr std::size_t kMaximumEvents = 4096;
Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
bool valid_namespace(const Namespace& fields) {
    if (fields.empty() || fields.size() > 31) return false;
    std::size_t size = 0;
    for (const auto& field : fields) {
        if (field.empty() || field.size() > 4094 - size) return false;
        size += field.size();
    }
    return fields.front() != bytes({'.'});
}
Bytes frame(std::uint64_t type, const wire::ByteWriter& body) {
    wire::ByteWriter output(kMaximumFrame);
    if (!wire::write_vi64(type,output) ||
        !output.append_byte(static_cast<std::byte>(body.size() >> 8u)) ||
        !output.append_byte(static_cast<std::byte>(body.size() & 255u)) ||
        !output.append_bytes(body.bytes())) throw std::invalid_argument("unencodable discovery frame");
    return {output.bytes().begin(),output.bytes().end()};
}
bool write_namespace(const Namespace& fields, wire::ByteWriter& output) {
    if (!wire::write_vi64(fields.size(),output)) return false;
    for (const auto& field : fields)
        if (!wire::write_length_prefixed_bytes(field,output)) return false;
    return true;
}
Bytes request(std::uint64_t type, std::uint64_t id, const Namespace& fields) {
    wire::ByteWriter body(65535);
    if (!wire::write_vi64(id,body) || !write_namespace(fields,body) || !wire::write_vi64(0,body))
        throw std::invalid_argument("unencodable discovery request");
    return frame(type,body);
}
Bytes update(std::uint64_t id, const Namespace& fields) {
    wire::ByteWriter prefix(65535), body(65535);
    if (!write_namespace(fields,prefix) || !wire::write_vi64(id,body) ||
        !wire::write_vi64(1,body) || !wire::write_vi64(0x34,body) ||
        !wire::write_length_prefixed_bytes(prefix.bytes(),body))
        throw std::invalid_argument("unencodable discovery update");
    return frame(2,body);
}
std::optional<Namespace> recover_namespace(std::span<const std::byte> input) {
    if (input.size() > kMaximumFrame) return {};
    wire::Cursor cursor(input);
    const auto decoded = d21::decode_request_frame(cursor,true);
    const auto* parsed = std::get_if<d21::RequestFrame>(&decoded);
    if (!parsed || (parsed->type.type != 0x50 && parsed->type.type != 0x51) || cursor.remaining()) return {};
    wire::Cursor body(parsed->body);
    const auto id = wire::read_vi64(body), count = wire::read_vi64(body);
    const auto* request_id = std::get_if<std::uint64_t>(&id);
    const auto* fields = std::get_if<std::uint64_t>(&count);
    if (!request_id || *request_id != 1 || !fields || *fields == 0 || *fields > 31) return {};
    Namespace result;
    for (std::uint64_t i = 0; i < *fields; ++i) {
        const auto decoded_field = wire::read_length_prefixed_bytes(body,4096);
        const auto* field = std::get_if<std::span<const std::byte>>(&decoded_field);
        if (!field) return {};
        result.emplace_back(field->begin(),field->end());
    }
    if (!valid_namespace(result) || request(parsed->type.type,1,result) != Bytes(input.begin(),input.end())) return {};
    return result;
}
bool terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
        std::holds_alternative<transport::LocalCloseEvent>(event) ||
        std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
        std::holds_alternative<transport::TransportErrorEvent>(event) ||
        std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}
bool setup_ready(unsigned draft, std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    if (draft == 21) return std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor));
    const auto decoded = d18::decode_message(d18::StreamRole::Control,cursor,{});
    const auto* message = std::get_if<d18::Message>(&decoded);
    return message && std::holds_alternative<d18::SetupMessage>(*message);
}
bool namespace_tail(wire::Cursor& cursor) {
    std::vector<Namespace> active;
    while (cursor.remaining()) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<d18::Message>(&decoded);
        if (!message) return false;
        if (const auto* announced = std::get_if<d18::NamespaceMessage>(message))
            active.push_back(announced->track_namespace_suffix.fields);
        else if (const auto* done = std::get_if<d18::NamespaceDoneMessage>(message)) {
            const auto found = std::find(active.begin(),active.end(),done->track_namespace_suffix.fields);
            if (found == active.end()) return false;
            active.erase(found);
        } else return false;
    }
    return true;
}
// Only a complete, scoped response may establish an active discovery request.
std::optional<bool> response(unsigned draft, std::uint64_t type, bool updating,
                             std::span<const std::byte> input, bool notifications = false) {
    wire::Cursor cursor(input);
    if (draft == 21) {
        const auto context = updating ? d21::ResponseContext::RequestUpdate : type == 0x50
            ? d21::ResponseContext::SubscribeNamespace : d21::ResponseContext::SubscribeTracks;
        if (std::holds_alternative<d21::SuccessfulResponse>(d21::decode_successful_response(cursor,context)))
            return cursor.remaining() == 0 || (notifications && type == 0x50 && namespace_tail(cursor)) ? std::optional<bool>{false} : std::nullopt;
        cursor = wire::Cursor(input);
        const auto decoded = d21::decode_request_error(cursor,true,true);
        if (const auto* error = std::get_if<d21::RequestErrorMessage>(&decoded); error && cursor.remaining() == 0) return error->error_code == 0x30;
        return {};
    }
    const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
    const auto* message = std::get_if<d18::Message>(&decoded);
    if (!message) return {};
    if (const auto* error = std::get_if<d18::RequestErrorMessage>(message)) {
        if (error->redirect && !error->redirect->track_name.bytes.empty()) return {};
        if (cursor.remaining()) return {};
        return error->error_code == 0x30;
    }
    const auto* ok = std::get_if<d18::RequestOkMessage>(message);
    if (!ok || !draft18_track_properties_valid(ok->track_properties)) return {};
    const auto context = updating ? d18::ParameterContext::RequestUpdateOk : type == 0x50
        ? d18::ParameterContext::SubscribeNamespaceOk : d18::ParameterContext::SubscribeTracksOk;
    if ((updating || type == 0x50) && !ok->track_properties.entries.empty()) return {};
    for (const auto& parameter : ok->parameters)
        if (d18::validate_parameter_scope(parameter.type,context) != d18::ParameterScopeResult::Allowed) return {};
    if (cursor.remaining() && !(notifications && type == 0x50 && namespace_tail(cursor))) return {};
    return false;
}
bool typed_ok(unsigned draft, std::uint64_t type, std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded_type = wire::read_vi64(cursor);
    const auto* value = std::get_if<std::uint64_t>(&decoded_type);
    return value && *value == 7 && response(draft,type,false,input,true) == false;
}
struct Observation { Bytes bytes; bool closed{false}; bool invalid{false}; };
Observation stream_response(std::span<const transport::TransportEvent> events,
    transport::StreamId id, std::size_t marker, bool active) {
    Observation result;
    bool session_closed = false;
    for (std::size_t i = 0; i < events.size(); ++i) {
        const auto& event = events[i];
        if (terminal(event)) { session_closed = true; if (active) result.invalid = true; continue; }
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event); data && data->stream_id == id) {
            if (i < marker) { if (data->fin) { result.closed = true; if (active) result.invalid = true; } continue; }
            if (result.closed || session_closed || data->data.size() > kMaximumFrame - result.bytes.size()) {
                result.invalid = true; continue;
            }
            result.bytes.insert(result.bytes.end(),data->data.begin(),data->data.end());
            if (data->fin) { result.closed = true; if (active) result.invalid = true; }
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event); reset && reset->stream_id == id) {
            result.closed = true; if (active) result.invalid = true;
        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event); stop && stop->stream_id == id) {
            if (active) result.invalid = true;
        }
    }
    return result;
}
struct Layout {
    std::size_t initial;
    std::vector<std::uint64_t> types;
    std::vector<std::size_t> targets;
    bool updating;
};
bool established(unsigned draft, const Layout& layout, const RawProbeGateInput& input, std::size_t challenge = 0) {
    if (input.prior_writes.size() < layout.initial || input.events.size() > kMaximumEvents) return false;
    std::set<transport::StreamId> streams;
    for (std::size_t i = 0; i < layout.initial; ++i) {
        if (layout.updating && std::find(layout.targets.begin(),layout.targets.begin() + static_cast<std::ptrdiff_t>(challenge),i) != layout.targets.begin() + static_cast<std::ptrdiff_t>(challenge)) continue;
        const auto& write = input.prior_writes[i];
        if (!write.stream_id || (*write.stream_id & 3u) != 1 ||
            !streams.insert(*write.stream_id).second || write.write.fin || write.fin_accepted ||
            !write.delivery_event_count || *write.delivery_event_count > input.events.size() ||
            write.accepted != write.write.bytes.size()) return false;
        const auto observed = stream_response(input.events,*write.stream_id,*write.delivery_event_count,true);
        if (observed.invalid || !typed_ok(draft,layout.types[i],observed.bytes)) return false;
    }
    return true;
}
std::optional<bool> observe(unsigned draft, const Layout& layout, const RawProbeTranscript& transcript, std::optional<std::uint64_t> target_type = {}) {
    if (transcript.events.size() > kMaximumEvents ||
        transcript.writes.size() != layout.initial + layout.targets.size()) return {};
    bool all = true;
    bool failed = false;
    for (std::size_t i = 0; i < layout.targets.size(); ++i) {
        if (target_type && layout.types[layout.targets[i]] != *target_type) continue;
        const auto& write = transcript.writes[layout.initial + i];
        if (!write.stream_id || !write.delivery_event_count || *write.delivery_event_count > transcript.events.size()) return {};
        const auto observed = stream_response(transcript.events,*write.stream_id,*write.delivery_event_count,false);
        if (observed.invalid) return {};
        const auto verdict = response(draft,layout.types[layout.targets[i]],layout.updating,observed.bytes,true);
        if (verdict == false) { if (target_type) return false; failed = true; }
        if (!verdict) all = false;
    }
    return all ? std::optional<bool>{!failed} : std::nullopt;
}
RawProbeDefinition definition(unsigned draft, const std::string& id, const Namespace& a,
                             std::chrono::milliseconds deadline, bool updating, bool independent, std::uint64_t type) {
    Namespace b = a;
    // A different first field is disjoint even under the drafts' common-prefix wording.
    b.front().push_back(std::byte{'b'});
    Namespace ancestor = a; ancestor.pop_back();
    Namespace descendant = a; descendant.push_back(bytes({'c'}));
    Layout layout{independent ? (updating ? 4u : 2u) : (updating ? 2u : 1u),{}, {},updating};
    RawProbeDefinition result{id,bytes({0xaf,0,0,0}),{},true,
        [draft](auto input) { return setup_ready(draft,input); },deadline};
    std::uint64_t next_id = 1;
    const auto add_initial = [&](std::uint64_t request_type, const Namespace& prefix) {
        result.writes.push_back({RawProbeChannel::NewBidi,request(request_type,next_id,prefix),false});
        next_id += 2;
        layout.types.push_back(request_type);
    };
    if (independent) {
        add_initial(0x50,a);
        if (updating) add_initial(0x50,b);
        add_initial(0x51,a);
        if (updating) add_initial(0x51,b);
    } else {
        add_initial(type,a);
        if (updating) add_initial(type,b);
    }
    const auto add_challenge = [&](std::size_t target, const Namespace& prefix) {
        RawProbeWrite write{RawProbeChannel::NewBidi,
            updating ? update(next_id,prefix) : request(layout.types[target],next_id,prefix),false};
        next_id += 2;
        if (updating) write.reuse_write_stream = target;
        layout.targets.push_back(target);
        result.writes.push_back(std::move(write));
    };
    if (updating) { add_challenge(1,a); if (independent) add_challenge(3,a); }
    else if (independent) { add_challenge(0,a); add_challenge(1,a); }
    else { add_challenge(0,a); add_challenge(0,ancestor); add_challenge(0,descendant); }
    for (std::size_t i = layout.initial; i < result.writes.size(); ++i)
        result.writes[i].evidence_ready = [draft,layout,challenge = i - layout.initial](const auto& input) { return established(draft,layout,input,challenge); };
    result.response_ready = [draft,layout](const auto& transcript) { return observe(draft,layout,transcript).has_value(); };
    return result;
}
std::vector<DiscoveryOverlapProbe> profiles(unsigned draft, std::chrono::milliseconds deadline, Namespace a) {
    if (a.empty()) a = {bytes({'a'})};
    if (!valid_namespace(a) || deadline.count() <= 0) throw std::invalid_argument("invalid discovery overlap fixture/deadline");
    struct Row { const char* requirement; const char* evaluator; const char* scenario; bool updating; bool independent; std::uint64_t type; };
    const std::vector<Row> rows = draft == 18 ? std::vector<Row>{
        {"D18-10-2-14-MUST-001","request-error-prefix-overlap","update-namespace-subscription-prefix-to-overlap-active-namespace-subscription",true,false,0x50},
        {"D18-10-2-14-MUST-002","request-error-prefix-overlap","update-track-subscription-prefix-to-overlap-active-track-subscription",true,false,0x51},
        {"D18-10-18-MUST-003","request-error-prefix-overlap","receive-overlapping-subscribe-namespace-in-same-session",false,false,0x50},
        {"D18-10-19-MUST-003","request-error-prefix-overlap","receive-overlapping-subscribe-tracks-in-same-session",false,false,0x51}} : std::vector<Row>{
        {"D21-9-15-MUST-385","d21-namespace-overlap-request-error","d21-subscribe-namespace-overlap",false,false,0x50},
        {"D21-9-15-MUST-385","d21-namespace-overlap-request-error","d21-discovery-independent-overlap-spaces",false,true,0x50},
        {"D21-9-18-MUST-393","d21-track-discovery-overlap-request-error","d21-subscribe-tracks-overlap",false,false,0x51},
        {"D21-9-18-MUST-393","d21-track-discovery-overlap-request-error","d21-discovery-independent-overlap-spaces",false,true,0x51},
        {"D21-9-20-21-MUST-470","d21-namespace-prefix-update-overlap-error","d21-namespace-prefix-update-overlap",true,false,0x50},
        {"D21-9-20-21-MUST-470","d21-namespace-prefix-update-overlap-error","d21-discovery-update-independent-overlap-spaces",true,true,0x50},
        {"D21-9-20-21-MUST-471","d21-track-prefix-update-overlap-error","d21-track-prefix-update-overlap",true,false,0x51},
        {"D21-9-20-21-MUST-471","d21-track-prefix-update-overlap-error","d21-discovery-update-independent-overlap-spaces",true,true,0x51}};
    std::vector<DiscoveryOverlapProbe> result;
    for (const auto& row : rows) result.push_back({row.requirement,row.evaluator,draft,
        definition(draft,row.scenario,a,deadline,row.updating,row.independent,row.type)});
    return result;
}
}  // namespace
bool discovery_overlap_namespace_valid(const Namespace& fields) { return fields.empty() || valid_namespace(fields); }
std::vector<DiscoveryOverlapProbe> draft18_discovery_overlap_probes(std::chrono::milliseconds deadline, Namespace a) {
    return profiles(18,deadline,std::move(a));
}
std::vector<DiscoveryOverlapProbe> draft21_discovery_overlap_probes(std::chrono::milliseconds deadline, Namespace a) {
    return profiles(21,deadline,std::move(a));
}
std::optional<bool> evaluate_discovery_overlap_probe(const RawProbeTranscript& transcript, const DiscoveryOverlapProbe& profile) {
    if ((profile.draft != 18 && profile.draft != 21) || profile.definition.deadline.count() <= 0 || transcript.writes.empty()) return {};
    const auto a = recover_namespace(transcript.writes.front().write.bytes);
    if (!a) return {};
    for (const auto& expected : profiles(profile.draft,profile.definition.deadline,*a)) {
        if (expected.requirement_id != profile.requirement_id || expected.evaluator_id != profile.evaluator_id ||
            expected.definition.id != profile.definition.id) continue;
        if (!raw_probe_stimulus_valid(transcript,expected.definition)) return {};
        const bool updating = expected.definition.writes.back().reuse_write_stream.has_value();
        const auto first_challenge = std::find_if(expected.definition.writes.begin(),expected.definition.writes.end(),
            [](const auto& write) { return static_cast<bool>(write.evidence_ready); });
        Layout layout{static_cast<std::size_t>(first_challenge - expected.definition.writes.begin()),{}, {},updating};
        for (std::size_t i = 0; i < layout.initial; ++i) {
            wire::Cursor cursor(expected.definition.writes[i].bytes);
            layout.types.push_back(std::get<std::uint64_t>(wire::read_vi64(cursor)));
        }
        for (std::size_t i = layout.initial; i < expected.definition.writes.size(); ++i)
            layout.targets.push_back(updating ? *expected.definition.writes[i].reuse_write_stream :
                (layout.initial == 2 ? i - layout.initial : 0));
        const auto type = profile.requirement_id == "D18-10-2-14-MUST-001" ||
            profile.requirement_id == "D18-10-18-MUST-003" || profile.requirement_id == "D21-9-15-MUST-385" ||
            profile.requirement_id == "D21-9-20-21-MUST-470" ? 0x50u : 0x51u;
        return observe(profile.draft,layout,transcript,type);
    }
    return {};
}
}  // namespace moq::interop::scenarios
