#include "moq/interop/scenarios/lite06_common.h"

#include "moq/interop/scenarios/lite06_setup.h"

#include <algorithm>
#include <variant>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"
#include "moq/interop/wire/moqlite06/varint.h"

namespace moq::interop::scenarios {
namespace lite06 {
namespace {

namespace l06 = wire::moqlite06;

template <class T>
const T* value_of(const wire::DecodeResult<T>& result) {
    return std::get_if<T>(&result);
}

enum class Lenient { Complete, Incomplete, Malformed };

// Reads one SETUP body (after STREAM_TYPE) leniently: intact framing, repeated ids kept.
Lenient read_setup(wire::Cursor& cursor, l06::SetupMessage& out) {
    const auto framed = l06::read_framed_body(cursor);
    if (std::holds_alternative<wire::NeedMore>(framed)) return Lenient::Incomplete;
    const auto* body = value_of(framed);
    if (!body) return Lenient::Malformed;
    wire::Cursor fields(*body);
    const auto count = l06::read_varint(fields);
    const auto* number = value_of(count);
    if (!number) return Lenient::Malformed;
    // Each parameter takes at least two bytes, so a larger count cannot be honest (and must not drive a loop).
    if (*number > body->size()) return Lenient::Malformed;
    l06::SetupMessage message;
    for (std::uint64_t i = 0; i < *number; ++i) {
        const auto id = l06::read_varint(fields);
        const auto* id_value = value_of(id);
        if (!id_value) return Lenient::Malformed;
        const auto length = l06::read_varint(fields);
        const auto* length_value = value_of(length);
        if (!length_value || *length_value > fields.remaining()) return Lenient::Malformed;
        const auto bytes = wire::read_bytes(fields, static_cast<std::size_t>(*length_value));
        const auto* span = value_of(bytes);
        if (!span) return Lenient::Malformed;
        message.parameters.push_back({*id_value, std::vector<std::byte>(span->begin(), span->end())});
    }
    if (fields.remaining() != 0) return Lenient::Malformed;
    out = std::move(message);
    return Lenient::Complete;
}

}  // namespace

bool is_session_code(std::uint64_t code) {
    return std::find(std::begin(kSessionErrorCodes), std::end(kSessionErrorCodes), code) !=
           std::end(kSessionErrorCodes);
}

bool is_stream_code(std::uint64_t code) {
    return std::find(std::begin(kStreamErrorCodes), std::end(kStreamErrorCodes), code) != std::end(kStreamErrorCodes);
}

const LiteStepRecord* proved_stimulus(const LiteTranscript& transcript, std::string_view label) {
    const LiteStepRecord* record = nullptr;
    if (label == kRunnerSetupLabel) {
        record = &transcript.runner_setup;
    } else {
        for (const auto& step : transcript.steps) {
            if (step.label == label) {
                record = &step;
                break;
            }
        }
    }
    if (!record || !transcript.established || !record->delivered()) return nullptr;
    if (record->accepted != record->bytes.size()) return nullptr;
    if (record->fin && !record->fin_accepted) return nullptr;
    return record;
}

const LiteStepRecord* proved_stimulus(const LiteTranscript& transcript, std::string_view label,
                                      std::span<const std::byte> expected) {
    const auto* record = proved_stimulus(transcript, label);
    if (!record || !std::equal(record->bytes.begin(), record->bytes.end(), expected.begin(), expected.end()))
        return nullptr;
    return record;
}

std::vector<const session::LiteStreamRecord*> peer_streams(const LiteTranscript& transcript) {
    std::vector<const session::LiteStreamRecord*> out;
    for (const auto& record : transcript.streams)
        if (record.origin == session::LiteOrigin::Peer) out.push_back(&record);
    return out;
}

const session::LiteStreamRecord* find_stream(const LiteTranscript& transcript, std::uint64_t stream_id) {
    for (const auto& record : transcript.streams)
        if (record.stream_id == stream_id) return &record;
    return nullptr;
}

std::optional<PeerSetup> peer_setup_message(const LiteTranscript& transcript) {
    PeerSetup setup;
    for (const auto* record : peer_streams(transcript)) {
        if (record->bidirectional || record->stream_type != std::optional<std::uint64_t>{0x1}) continue;
        if (++setup.setup_streams == 1) setup.stream = record;
    }
    if (!setup.stream) return std::nullopt;
    setup.reset = setup.stream->reset_seen;
    // The bytes as received: transport events carry only what the peer sent.
    std::vector<std::byte> raw;
    for (const auto& event : transcript.events) {
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || data->stream_id != setup.stream->stream_id) continue;
        raw.insert(raw.end(), data->data.begin(), data->data.end());
        setup.fin = setup.fin || data->fin;
    }
    wire::Cursor cursor(raw);
    const auto type = l06::read_stream_type(cursor);
    if (!value_of(type)) {
        // The recorder classified the stream from these bytes, so they cannot be short; treat as malformed.
        setup.malformed = true;
        return setup;
    }
    setup.bytes.assign(raw.begin() + static_cast<std::ptrdiff_t>(cursor.offset()), raw.end());
    l06::SetupMessage message;
    switch (read_setup(cursor, message)) {
        case Lenient::Complete:
            setup.message = std::move(message);
            setup.trailing = cursor.remaining() != 0;
            break;
        case Lenient::Incomplete:
            break;
        case Lenient::Malformed:
            setup.malformed = true;
            break;
    }
    return setup;
}

std::optional<std::uint64_t> session_close_code(const LiteTranscript& transcript) {
    if (!transcript.peer_close || transcript.peer_close->space != transport::CloseErrorSpace::Application)
        return std::nullopt;
    return transcript.peer_close->code;
}

std::optional<std::uint64_t> stream_reset_code(const LiteTranscript& transcript, std::uint64_t stream_id) {
    const auto* record = find_stream(transcript, stream_id);
    if (!record) return std::nullopt;
    return record->reset_code;
}

std::vector<std::byte> raw_setup_stream(const std::vector<l06::SetupParameter>& parameters) {
    wire::ByteWriter body(std::size_t{1} << 20);
    bool ok = l06::write_varint(parameters.size(), body);
    for (const auto& parameter : parameters) {
        ok = ok && l06::write_varint(parameter.id, body) && l06::write_varint(parameter.value.size(), body) &&
             body.append_bytes(parameter.value);
    }
    wire::ByteWriter out(std::size_t{1} << 21);
    ok = ok && l06::write_stream_type(static_cast<std::uint64_t>(l06::UniStreamType::Setup), out) &&
         l06::write_framed_message(body.bytes(), out);
    if (!ok) return {};
    return {out.bytes().begin(), out.bytes().end()};
}

std::optional<bool> judge_close_probe(const LiteTranscript& transcript, std::string_view scenario_id,
                                      const LiteStepRecord* stimulus) {
    // The peer's close is the observation (it may come before the allowance step ran), so plain judgeable().
    if (transcript.scenario_id != scenario_id || !judgeable(transcript) || !stimulus) return std::nullopt;
    if (transcript.runner_closed) return std::nullopt;
    if (transcript.peer_close) {
        // A delivered stimulus executed before the close was seen (the engine stops stepping at a peer close).
        return transcript.peer_close->space == transport::CloseErrorSpace::Application &&
               transcript.peer_close->code == kProtocolViolation;
    }
    // No close: a Fail only once the allowance step has run (time-bounded); a stream-level reaction alone
    // (reset or STOP_SENDING of the offending stream) lands here too.
    for (const auto& step : transcript.steps)
        if (step.label == kAllowanceLabel) return step.executed() ? std::optional<bool>{false} : std::nullopt;
    return std::nullopt;
}

}  // namespace lite06

std::optional<bool> evaluate_l06_errors_code_space(const LiteTranscript& transcript) {
    // Bound (catalog row 027) to its own scenario and the four MUST-level close probes only.
    const auto& id = transcript.scenario_id;
    const bool bound = id == kL06ErrorsCodeSpace || id == kL06SetupDuplicateParameter ||
                       id == kL06SetupDuplicateStream || id == kL06SetupServerPath || id == kL06SetupServerRole;
    if (!bound || !judgeable(transcript) || !lite06::proved_stimulus(transcript, lite06::kRunnerSetupLabel))
        return std::nullopt;
    // Stream half: the peer's RESET_STREAM and STOP_SENDING codes (record fields of the peer's direction only).
    bool stream_half = false;
    for (const auto& record : transcript.streams) {
        for (const auto& code : {record.reset_code, record.stop_sending_code}) {
            if (!code) continue;
            if (lite06::is_session_code(*code) && !lite06::is_stream_code(*code)) return false;
            if (lite06::is_stream_code(*code)) stream_half = true;
        }
    }
    // Session half: an application-space close by the peer.
    bool session_half = false;
    if (const auto code = lite06::session_close_code(transcript)) {
        if (lite06::is_stream_code(*code) && !lite06::is_session_code(*code)) return false;
        session_half = lite06::is_session_code(*code);  // an unregistered value is not judged here
    }
    if (transcript.scenario_id == kL06ErrorsCodeSpace)
        return stream_half && session_half ? std::optional<bool>{true} : std::nullopt;
    return session_half ? std::optional<bool>{true} : std::nullopt;
}

}  // namespace moq::interop::scenarios
