#include "moq/interop/scenarios/fetch_probe.h"
#include "inline_filter_sites_testing.h"
#include "moq/interop/scenarios/location_filter_param.h"
#include "moq/interop/scenarios/draft18_response.h"
#include "moq/interop/wire/draft21/publish_done.h"
#include "moq/interop/wire/draft21/request_error.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"

#include <algorithm>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace moq::interop::scenarios {
namespace {
namespace d18 = wire::draft18;
namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
constexpr std::size_t kMaximumFrame = 65546;
constexpr std::size_t kMaximumEvents = kRawProbeMaximumEvents;
constexpr std::size_t kMaximumHeader = 18;
constexpr std::uint64_t kRequestId = 1;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
struct Fixture { Namespace track_namespace; Bytes track_name; };
bool valid_fixture(const Fixture& fixture) {
    if (fixture.track_namespace.size() > 32) return false;
    std::size_t total = fixture.track_name.size();
    if (total > 4096) return false;
    for (const auto& field : fixture.track_namespace) {
        if (field.empty() || field.size() > 4096 - total) return false;
        total += field.size();
    }
    if (fixture.track_namespace.empty()) return true;
    return fixture.track_namespace.front() != bytes({'.'}) &&
        (!fixture.track_name.empty() || fixture.track_namespace.front() != bytes({'.','s','e','s','s','i','o','n'}));
}
Bytes encode_fetch(unsigned draft, const Fixture& fixture) {
    if (!valid_fixture(fixture)) throw std::invalid_argument("invalid FETCH track fixture");
    wire::ByteWriter output(kMaximumFrame);
    if (draft == 18) {
        const d18::FetchMessage message{kRequestId,
            d18::StandaloneFetch{{fixture.track_namespace},{fixture.track_name},
                {0,0},{std::numeric_limits<std::uint64_t>::max(),0}}, {}};
        if (!d18::encode_message(d18::Message{message},output).has_value())
            throw std::invalid_argument("unencodable draft18 FETCH fixture");
    } else {
        wire::ByteWriter body(65535);
        bool success = wire::write_vi64(kRequestId,body) &&
            wire::write_vi64(fixture.track_namespace.size(),body);
        for (const auto& field : fixture.track_namespace)
            success = success && wire::write_length_prefixed_bytes(field,body);
        success = success && wire::write_length_prefixed_bytes(fixture.track_name,body);
        // Section9.20.10: explicit absolute start0/0, inclusive complete final
        // group. Three vi64 fields distinguish this from Next Object.
        const auto filter = filter_param_value({0, 0, std::numeric_limits<std::uint64_t>::max()});
        success = success &&
            wire::write_vi64(1,body) && wire::write_vi64(0x21,body) &&
            body.append_bytes(filter) &&
            wire::write_vi64(0x16,output) &&
            output.append_byte(static_cast<std::byte>(body.size() >> 8u)) &&
            output.append_byte(static_cast<std::byte>(body.size() & 255u)) &&
            output.append_bytes(body.bytes());
        if (!success) throw std::invalid_argument("unencodable draft21 FETCH fixture");
    }
    return {output.bytes().begin(),output.bytes().end()};
}
std::optional<Fixture> decode_fixture(unsigned draft, std::span<const std::byte> input) {
    if (input.size() > kMaximumFrame) return std::nullopt;
    Fixture fixture;
    wire::Cursor cursor(input);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* fetch = message ? std::get_if<d18::FetchMessage>(message) : nullptr;
        const auto* standalone = fetch ? std::get_if<d18::StandaloneFetch>(&fetch->fetch) : nullptr;
        if (!standalone || cursor.remaining() != 0 || fetch->request_id != kRequestId) return std::nullopt;
        fixture = {standalone->track_namespace.fields,standalone->track_name.bytes};
    } else {
        const auto decoded = d21::decode_request_frame(cursor,true);
        const auto* frame = std::get_if<d21::RequestFrame>(&decoded);
        if (!frame || frame->type.type != 0x16 || cursor.remaining() != 0) return std::nullopt;
        wire::Cursor body(frame->body);
        const auto id = wire::read_vi64(body);
        const auto* request = std::get_if<std::uint64_t>(&id);
        const auto count = wire::read_vi64(body);
        const auto* fields = std::get_if<std::uint64_t>(&count);
        if (!request || *request != kRequestId || !fields || *fields > 32) return std::nullopt;
        std::size_t total = 0;
        for (std::uint64_t i = 0; i < *fields; ++i) {
            const auto field = wire::read_length_prefixed_bytes(body,4096 - total);
            const auto* value = std::get_if<std::span<const std::byte>>(&field);
            if (!value || value->empty()) return std::nullopt;
            total += value->size();
            fixture.track_namespace.emplace_back(value->begin(),value->end());
        }
        const auto name = wire::read_length_prefixed_bytes(body,4096 - total);
        const auto* value = std::get_if<std::span<const std::byte>>(&name);
        if (!value) return std::nullopt;
        fixture.track_name.assign(value->begin(),value->end());
    }
    if (!valid_fixture(fixture)) return std::nullopt;
    // Namespace/name alone are configurable. Rebuilding rejects extra
    // parameters, altered range/ID, noncanonical frames, and trailing bytes.
    const auto expected = encode_fetch(draft,fixture);
    if (input.size() != expected.size() || !std::equal(input.begin(),input.end(),expected.begin()))
        return std::nullopt;
    return fixture;
}
bool setup_ready(unsigned draft, std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Control,cursor,{});
        const auto* message = std::get_if<d18::Message>(&decoded);
        return message && std::holds_alternative<d18::SetupMessage>(*message);
    }
    return std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor));
}
bool valid_ok(unsigned draft, std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* ok = message ? std::get_if<d18::FetchOkMessage>(message) : nullptr;
        return ok && ok->end_of_track <= 1 && cursor.remaining() == 0 &&
            draft18_track_properties_valid(ok->track_properties);
    }
    const auto decoded = d21::decode_successful_response(cursor,d21::ResponseContext::Fetch);
    return std::holds_alternative<d21::SuccessfulResponse>(decoded) && cursor.remaining() == 0;
}
bool session_terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
        std::holds_alternative<transport::LocalCloseEvent>(event) ||
        std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
        std::holds_alternative<transport::TransportErrorEvent>(event) ||
        std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}
struct HeaderEvidence { Bytes prefix; std::size_t first; bool closed{false}; };
struct OpenFetch { transport::StreamId request; transport::StreamId data; };
std::optional<OpenFetch> open_fetch(unsigned draft, const RawProbeGateInput& input, bool expected_opener_fin) {
    if (input.prior_writes.size() != 1 || input.events.size() > kMaximumEvents) return std::nullopt;
    const auto& opener = input.prior_writes.front();
    if (!opener.stream_id || (*opener.stream_id & 3u) != 1u || !opener.delivery_event_count ||
        *opener.delivery_event_count > input.events.size() || opener.write.fin != expected_opener_fin ||
        opener.fin_accepted != expected_opener_fin ||
        opener.accepted != opener.write.bytes.size() || opener.operation_accepted ||
        opener.write.operation != RawProbeOperation::Write || opener.write.channel != RawProbeChannel::NewBidi ||
        !decode_fixture(draft,opener.write.bytes)) return std::nullopt;
    Bytes response;
    std::map<transport::StreamId,HeaderEvidence> streams;
    for (std::size_t i = 0; i < input.events.size(); ++i) {
        const auto& event = input.events[i];
        if (session_terminal(event)) return std::nullopt;
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event)) {
            if (data->stream_id == *opener.stream_id) {
                if (i < *opener.delivery_event_count || data->fin ||
                    data->data.size() > kMaximumFrame - response.size()) return std::nullopt;
                response.insert(response.end(),data->data.begin(),data->data.end());
            } else if ((data->stream_id & 3u) == 2u) {
                if (!streams.contains(data->stream_id)) {
                    if (streams.size() >= 64) return std::nullopt;
                    streams.emplace(data->stream_id,HeaderEvidence{{},i,false});
                }
                auto& header = streams.at(data->stream_id);
                if (header.closed && !data->data.empty()) return std::nullopt;
                const auto count = std::min(data->data.size(),kMaximumHeader - header.prefix.size());
                header.prefix.insert(header.prefix.end(),data->data.begin(),
                    data->data.begin() + static_cast<std::ptrdiff_t>(count));
                header.closed |= data->fin;
            }
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event)) {
            if (reset->stream_id == *opener.stream_id) return std::nullopt;
            if ((reset->stream_id & 3u) == 2u) {
                if (!streams.contains(reset->stream_id)) {
                    if (streams.size() >= 64) return std::nullopt;
                    streams.emplace(reset->stream_id,HeaderEvidence{{},i,true});
                } else streams.at(reset->stream_id).closed = true;
            }
        } else if (const auto* stop = std::get_if<transport::PeerStopSendingEvent>(&event)) {
            if (stop->stream_id == *opener.stream_id) return std::nullopt;
        }
    }
    if (!valid_ok(draft,response)) return std::nullopt;
    std::optional<transport::StreamId> associated;
    for (const auto& [id,header] : streams) {
        wire::Cursor cursor(header.prefix);
        const auto type = wire::read_vi64(cursor);
        const auto* value = std::get_if<std::uint64_t>(&type);
        if (!value || *value != 5) continue;
        const auto request = wire::read_vi64(cursor);
        const auto* request_id = std::get_if<std::uint64_t>(&request);
        if (!request_id) return std::nullopt;
        if (*request_id != kRequestId) continue;
        if (associated || header.closed || header.first < *opener.delivery_event_count) return std::nullopt;
        associated = id;
    }
    if (!associated) return std::nullopt;
    return OpenFetch{*opener.stream_id,*associated};
}
bool valid_error(unsigned draft, std::span<const std::byte> response) {
    wire::Cursor cursor(response);
    if (draft == 18) {
        const auto decoded = d18::decode_message(d18::StreamRole::Request,cursor,{});
        const auto* message = std::get_if<d18::Message>(&decoded);
        const auto* error = message ? std::get_if<d18::RequestErrorMessage>(message) : nullptr;
        return error && cursor.remaining() == 0 && draft18_request_error_valid(*error);
    }
    const auto decoded = d21::decode_request_error(cursor,true,false);
    const auto* error = std::get_if<d21::RequestErrorMessage>(&decoded);
    return error && cursor.remaining() == 0 && d21::valid_reason_phrase(error->reason);
}
struct Cleanup {
    std::optional<bool> request_reset;
    std::optional<bool> data_reset;
    bool rejection{false};
    bool invalid{false};
};
Cleanup observe(const RawProbeTranscript& transcript, unsigned draft) {
    Cleanup result;
    if (transcript.writes.size() != 2 || !transcript.delivery_event_count ||
        *transcript.delivery_event_count > transcript.events.size() || transcript.events.size() > kMaximumEvents)
        return result;
    const auto marker = *transcript.delivery_event_count;
    const auto opened = open_fetch(draft,{std::span(transcript.writes).first(1),
        std::span(transcript.events).first(marker)},
        transcript.writes.back().write.operation == RawProbeOperation::StopSending);
    if (!opened) return result;
    Bytes response;
    bool session_closed = false;
    for (std::size_t i = marker; i < transcript.events.size(); ++i) {
        const auto& event = transcript.events[i];
        if (session_terminal(event)) { session_closed = true; continue; }
        if (const auto* data = std::get_if<transport::StreamDataEvent>(&event);
            data && (data->stream_id == opened->request || data->stream_id == opened->data)) {
            auto& terminal = data->stream_id == opened->request ? result.request_reset : result.data_reset;
            if (session_closed || terminal) { result.invalid = true; continue; }
            if (data->stream_id == opened->request) {
                if (data->data.size() > kMaximumFrame - response.size()) { result.invalid = true; continue; }
                response.insert(response.end(),data->data.begin(),data->data.end());
            }
            if (data->fin) terminal = false;
        } else if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
                   reset && (reset->stream_id == opened->request || reset->stream_id == opened->data)) {
            auto& terminal = reset->stream_id == opened->request ? result.request_reset : result.data_reset;
            if (session_closed || terminal) { result.invalid = true; continue; }
            terminal = true;
        }
    }
    // ERROR and reset travel on different QUIC streams. Their arrival order
    // cannot establish causality; both must follow the accepted local UPDATE.
    result.rejection = valid_error(draft,response);
    return result;
}
std::vector<FetchProbe> profiles(unsigned draft, std::chrono::milliseconds deadline, const Fixture& fixture) {
    if (deadline.count() <= 0) throw std::invalid_argument("invalid FETCH probe deadline");
    const auto initial = encode_fetch(draft,fixture);
    const auto definition = [&](const char* id, bool failed_update) {
        RawProbeWrite final{RawProbeChannel::NewBidi,
            failed_update ? bytes({2,0,6,3,1,3,2,2,0}) : Bytes{},failed_update,0,{}};
        if (!failed_update) { final.operation = RawProbeOperation::StopSending; final.application_error = 1; }
        final.evidence_ready = [draft,failed_update](const auto& input) {
            return open_fetch(draft,input,!failed_update).has_value();
        };
        RawProbeDefinition result{id,bytes({0xaf,0,0,0}),
            {{RawProbeChannel::NewBidi,initial,!failed_update},std::move(final)},true,
            [draft](auto input) { return setup_ready(draft,input); },deadline,
            [draft,failed_update](const auto& transcript) {
                const auto cleanup = observe(transcript,draft);
                return !cleanup.invalid && (failed_update ? cleanup.rejection && cleanup.data_reset.has_value()
                    : cleanup.request_reset == std::optional<bool>{false} ||
                      cleanup.data_reset == std::optional<bool>{false} ||
                      (cleanup.request_reset.has_value() && cleanup.data_reset.has_value()));
            },{}};
        return result;
    };
    if (draft == 18) return {
        {"D18-5-2-MUST-003","fetch-request-stream-reset",18,FetchProbeExpectation::CancelRequestReset,
            definition("cancel-fetch-request-with-open-data-stream",false)},
        {"D18-5-2-MUST-004","fetch-data-stream-reset",18,FetchProbeExpectation::CancelDataReset,
            definition("cancel-fetch-request-with-open-data-stream",false)},
        {"D18-10-9-1-MUST-002","fetch-data-stream-reset",18,FetchProbeExpectation::FailedUpdateDataReset,
            definition("reject-request-update-for-open-fetch",true)}};
    return {
        {"D21-3-2-1-MUST-055","d21-fetch-cancel-resets-bidi-request-stream",21,FetchProbeExpectation::CancelRequestReset,
            definition("d21-cancel-fetch-with-open-request-and-data-streams",false)},
        {"D21-3-2-1-MUST-056","d21-fetch-cancel-resets-unidirectional-data-stream",21,FetchProbeExpectation::CancelDataReset,
            definition("d21-cancel-fetch-with-open-request-and-data-streams",false)},
        {"D21-9-5-1-MUST-347","d21-failed-fetch-update-resets-data-stream",21,FetchProbeExpectation::FailedUpdateDataReset,
            definition("d21-failed-fetch-update-data-reset",true)}};
}
}  // namespace

SiteBytes fetch_probe_fetch_for_test(const SiteNamespace& track_namespace, const SiteBytes& track_name) {
    return encode_fetch(21, Fixture{track_namespace, track_name});
}

std::vector<FetchProbe> draft18_fetch_probes(std::chrono::milliseconds deadline,
    Namespace track_namespace, Bytes track_name) {
    return profiles(18,deadline,{std::move(track_namespace),std::move(track_name)});
}
std::vector<FetchProbe> draft21_fetch_probes(std::chrono::milliseconds deadline,
    Namespace track_namespace, Bytes track_name) {
    return profiles(21,deadline,{std::move(track_namespace),std::move(track_name)});
}
std::optional<bool> evaluate_fetch_probe(const RawProbeTranscript& transcript, const FetchProbe& profile) {
    if ((profile.draft != 18 && profile.draft != 21) || transcript.writes.size() != 2 ||
        profile.definition.deadline.count() <= 0) return std::nullopt;
    const auto fixture = decode_fixture(profile.draft,transcript.writes.front().write.bytes);
    if (!fixture) return std::nullopt;
    const auto candidates = profiles(profile.draft,profile.definition.deadline,*fixture);
    const auto expected = std::find_if(candidates.begin(),candidates.end(),[&](const auto& candidate) {
        return candidate.requirement_id == profile.requirement_id && candidate.evaluator_id == profile.evaluator_id &&
            candidate.expectation == profile.expectation && candidate.definition.id == profile.definition.id;
    });
    if (expected == candidates.end() || !raw_probe_stimulus_valid(transcript,expected->definition)) return std::nullopt;
    const auto cleanup = observe(transcript,profile.draft);
    if (cleanup.invalid) return std::nullopt;
    if (profile.expectation == FetchProbeExpectation::FailedUpdateDataReset && !cleanup.rejection) return std::nullopt;
    const auto result = profile.expectation == FetchProbeExpectation::CancelRequestReset
        ? cleanup.request_reset : cleanup.data_reset;
    // FIN can precede publisher receipt of cancellation or UPDATE. QUIC can
    // also suppress a subsequent RESET after complete data was received.
    // FIN-only evidence therefore cannot establish a missing reset.
    if (result == std::optional<bool>{false}) return std::nullopt;
    return result;
}
}  // namespace moq::interop::scenarios
