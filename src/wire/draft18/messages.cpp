#include "moq/interop/wire/draft18/messages.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <utility>

namespace moq::interop::wire::draft18 {
namespace {

constexpr std::uint64_t kSetupMessageType = 0x2f00;
constexpr std::uint64_t kGoawayMessageType = 0x10;
constexpr std::uint64_t kRequestUpdateMessageType = 0x02;
constexpr std::uint64_t kSubscribeMessageType = 0x03;
constexpr std::uint64_t kSubscribeOkMessageType = 0x04;
constexpr std::uint64_t kRequestErrorMessageType = 0x05;
constexpr std::uint64_t kRequestOkMessageType = 0x07;
constexpr std::uint64_t kNamespaceMessageType = 0x08;
constexpr std::uint64_t kPublishDoneMessageType = 0x0b;
constexpr std::uint64_t kNamespaceDoneMessageType = 0x0e;
constexpr std::uint64_t kPublishBlockedMessageType = 0x0f;
constexpr std::uint64_t kPublishMessageType = 0x1d;
constexpr std::uint64_t kFetchMessageType = 0x16;
constexpr std::uint64_t kFetchOkMessageType = 0x18;
constexpr std::uint64_t kTrackStatusMessageType = 0x0d;
constexpr std::uint64_t kPublishNamespaceMessageType = 0x06;
constexpr std::uint64_t kSubscribeNamespaceMessageType = 0x50;
constexpr std::uint64_t kSubscribeTracksMessageType = 0x51;
constexpr std::size_t kMaximumMessagePayload = 65'535;
constexpr std::size_t kMaximumFrameSize = kMaximumMessagePayload + 11;
constexpr std::size_t kMaximumFullTrackName = 4'096;
constexpr std::size_t kMaximumReasonPhrase = 1'024;
constexpr std::size_t kMaximumNewSessionUri = 8'192;

DecodeError invalid(std::size_t offset, std::string detail) {
    return {DecodeErrorCode::InvalidValue, offset, std::move(detail)};
}

DecodeError protocol_violation(std::size_t offset, std::string detail) {
    return {DecodeErrorCode::ProtocolViolation, offset, std::move(detail)};
}

DecodeError key_value_formatting_error(std::size_t offset,
                                       std::string detail) {
    return {DecodeErrorCode::KeyValueFormattingError, offset,
            std::move(detail)};
}

template <class T>
DraftDecodeResult<T> bounded_payload_error(const NeedMore& need,
                                           std::string detail) {
    return invalid(need.offset, std::move(detail));
}

std::vector<std::byte> copy_bytes(std::span<const std::byte> bytes) {
    return {bytes.begin(), bytes.end()};
}

bool is_known_setup_option(std::uint64_t type) {
    return type == 0x01 || type == 0x03 || type == 0x04 || type == 0x05 ||
           type == 0x07;
}

bool is_repeatable_setup_option(std::uint64_t type) { return type == 0x03; }

std::optional<DecodeError> validate_setup_options(
    std::span<const KeyValuePair> options, std::size_t offset) {
    std::optional<std::uint64_t> previous;
    for (const auto& option : options) {
        if (previous && option.type == *previous &&
            is_known_setup_option(option.type) &&
            !is_repeatable_setup_option(option.type)) {
            return invalid(offset, "duplicate non-repeatable SETUP option");
        }
        previous = option.type;
    }
    return std::nullopt;
}

EncodeResult encode_key_value_pairs_to(std::span<const KeyValuePair> entries,
                                       ByteWriter& output) {
    std::uint64_t previous_type = 0;
    bool first = true;
    for (const auto& entry : entries) {
        if (!first && entry.type < previous_type) {
            return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                         "KVP types are not ordered");
        }
        const auto delta = first ? entry.type : entry.type - previous_type;
        if (!write_vi64(delta, output)) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "KVP type exceeds output capacity");
        }

        if ((entry.type & 1u) == 0u) {
            const auto* integer = std::get_if<VarIntValue>(&entry.value);
            if (integer == nullptr) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "even KVP type requires vi64 value");
            }
            if (!write_vi64(integer->value, output)) {
                return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                             "KVP integer exceeds output capacity");
            }
        } else {
            const auto* bytes = std::get_if<ByteValue>(&entry.value);
            if (bytes == nullptr) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "odd KVP type requires byte value");
            }
            if (bytes->bytes.size() > kMaximumMessagePayload) {
                return EncodeResult::failure(EncodeErrorCode::PayloadTooLarge,
                                             "KVP byte value exceeds draft limit");
            }
            if (!write_length_prefixed_bytes(bytes->bytes, output)) {
                return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                             "KVP bytes exceed output capacity");
            }
        }
        previous_type = entry.type;
        first = false;
    }
    return EncodeResult::success();
}

EncodeResult append_staged(const ByteWriter& staged, ByteWriter& output,
                           std::string detail) {
    if (!output.append_bytes(staged.bytes())) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     std::move(detail));
    }
    return EncodeResult::success();
}

std::size_t namespace_value_length(const TrackNamespace& name_space) {
    std::size_t result = 0;
    for (const auto& field : name_space.fields) {
        if (field.size() > kMaximumFullTrackName - result) {
            return kMaximumFullTrackName + 1;
        }
        result += field.size();
    }
    return result;
}

bool is_known_parameter(std::uint64_t type) {
    switch (type) {
        case 0x02:
        case 0x03:
        case 0x04:
        case 0x06:
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x10:
        case 0x20:
        case 0x21:
        case 0x22:
        case 0x32:
        case 0x34:
            return true;
        default:
            return false;
    }
}

bool context_is(ParameterContext context,
                std::initializer_list<ParameterContext> allowed) {
    return std::find(allowed.begin(), allowed.end(), context) != allowed.end();
}

bool is_opening_request_type(std::uint64_t type) {
    return type == kSubscribeMessageType || type == kPublishMessageType ||
           type == kFetchMessageType || type == kTrackStatusMessageType ||
           type == kPublishNamespaceMessageType ||
           type == kSubscribeNamespaceMessageType ||
           type == kSubscribeTracksMessageType;
}

bool is_continuation_request_type(std::uint64_t type) {
    switch (type) {
        case kRequestUpdateMessageType:
        case kSubscribeOkMessageType:
        case kRequestErrorMessageType:
        case kRequestOkMessageType:
        case kNamespaceMessageType:
        case kPublishDoneMessageType:
        case kNamespaceDoneMessageType:
        case kPublishBlockedMessageType:
        case kFetchOkMessageType:
            return true;
        default:
            return false;
    }
}

struct TrackOpeningFields {
    std::uint64_t request_id;
    TrackNamespace track_namespace;
    TrackName track_name;
    Parameters parameters;
};

DraftDecodeResult<TrackOpeningFields> decode_track_opening_fields(
    Cursor& input, ParameterContext context, const Limits& limits,
    bool includes_alias, std::optional<std::uint64_t>& alias) {
    Cursor working = input;
    const auto request_id = read_vi64(working);
    if (const auto* value = std::get_if<NeedMore>(&request_id)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&request_id)) return *value;
    const auto name_space = decode_track_namespace(working, limits);
    if (const auto* value = std::get_if<NeedMore>(&name_space)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&name_space)) return *value;
    const auto decoded_namespace = std::get<TrackNamespace>(name_space);
    const auto name = decode_track_name(working, decoded_namespace, limits);
    if (const auto* value = std::get_if<NeedMore>(&name)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&name)) return *value;
    if (includes_alias) {
        const auto decoded_alias = read_vi64(working);
        if (const auto* value = std::get_if<NeedMore>(&decoded_alias)) return *value;
        if (const auto* value = std::get_if<DecodeError>(&decoded_alias)) return *value;
        alias = std::get<std::uint64_t>(decoded_alias);
    }
    const auto count = read_vi64(working);
    if (const auto* value = std::get_if<NeedMore>(&count)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&count)) return *value;
    const auto parameters = decode_parameters(
        working, std::get<std::uint64_t>(count), context, limits);
    if (const auto* value = std::get_if<NeedMore>(&parameters)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&parameters)) return *value;
    if (const auto* value = std::get_if<DraftAmbiguity>(&parameters)) return *value;
    input = working;
    return TrackOpeningFields{std::get<std::uint64_t>(request_id),
                              decoded_namespace, std::get<TrackName>(name),
                              std::get<Parameters>(parameters)};
}

struct NamespaceOpeningFields {
    std::uint64_t request_id;
    TrackNamespace track_namespace;
    Parameters parameters;
};

DraftDecodeResult<NamespaceOpeningFields> decode_namespace_opening_fields(
    Cursor& input, ParameterContext context, const Limits& limits) {
    Cursor working = input;
    const auto request_id = read_vi64(working);
    if (const auto* value = std::get_if<NeedMore>(&request_id)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&request_id)) return *value;
    const auto name_space = decode_track_namespace(working, limits);
    if (const auto* value = std::get_if<NeedMore>(&name_space)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&name_space)) return *value;
    const auto count = read_vi64(working);
    if (const auto* value = std::get_if<NeedMore>(&count)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&count)) return *value;
    const auto parameters = decode_parameters(
        working, std::get<std::uint64_t>(count), context, limits);
    if (const auto* value = std::get_if<NeedMore>(&parameters)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&parameters)) return *value;
    if (const auto* value = std::get_if<DraftAmbiguity>(&parameters)) return *value;
    input = working;
    return NamespaceOpeningFields{std::get<std::uint64_t>(request_id),
                                  std::get<TrackNamespace>(name_space),
                                  std::get<Parameters>(parameters)};
}

DraftDecodeResult<FetchMessage> decode_fetch_payload(Cursor& input,
                                                     const Limits& limits) {
    Cursor working = input;
    const auto request_id = read_vi64(working);
    if (const auto* value = std::get_if<NeedMore>(&request_id)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&request_id)) return *value;
    const auto fetch_type = read_vi64(working);
    if (const auto* value = std::get_if<NeedMore>(&fetch_type)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&fetch_type)) return *value;
    const auto type = std::get<std::uint64_t>(fetch_type);
    Fetch fetch;
    if (type == 1) {
        const auto name_space = decode_track_namespace(working, limits);
        if (const auto* value = std::get_if<NeedMore>(&name_space)) return *value;
        if (const auto* value = std::get_if<DecodeError>(&name_space)) return *value;
        const auto decoded_namespace = std::get<TrackNamespace>(name_space);
        const auto name = decode_track_name(working, decoded_namespace, limits);
        if (const auto* value = std::get_if<NeedMore>(&name)) return *value;
        if (const auto* value = std::get_if<DecodeError>(&name)) return *value;
        const auto start = decode_location(working);
        if (const auto* value = std::get_if<NeedMore>(&start)) return *value;
        if (const auto* value = std::get_if<DecodeError>(&start)) return *value;
        const auto end = decode_location(working);
        if (const auto* value = std::get_if<NeedMore>(&end)) return *value;
        if (const auto* value = std::get_if<DecodeError>(&end)) return *value;
        const auto start_value = std::get<Location>(start);
        const auto end_value = std::get<Location>(end);
        if (end_value.group < start_value.group ||
            (end_value.group == start_value.group &&
             end_value.object < start_value.object)) {
            return protocol_violation(input.offset(),
                                      "standalone FETCH range is reversed");
        }
        fetch = StandaloneFetch{decoded_namespace, std::get<TrackName>(name),
                                start_value, end_value};
    } else if (type == 2 || type == 3) {
        const auto joining_request_id = read_vi64(working);
        if (const auto* value = std::get_if<NeedMore>(&joining_request_id)) return *value;
        if (const auto* value = std::get_if<DecodeError>(&joining_request_id)) return *value;
        const auto joining_start = read_vi64(working);
        if (const auto* value = std::get_if<NeedMore>(&joining_start)) return *value;
        if (const auto* value = std::get_if<DecodeError>(&joining_start)) return *value;
        if (type == 2) {
            fetch = RelativeJoiningFetch{
                std::get<std::uint64_t>(joining_request_id),
                std::get<std::uint64_t>(joining_start)};
        } else {
            fetch = AbsoluteJoiningFetch{
                std::get<std::uint64_t>(joining_request_id),
                std::get<std::uint64_t>(joining_start)};
        }
    } else {
        return protocol_violation(input.offset(), "unknown FETCH type");
    }
    const auto count = read_vi64(working);
    if (const auto* value = std::get_if<NeedMore>(&count)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&count)) return *value;
    const auto parameters = decode_parameters(
        working, std::get<std::uint64_t>(count), ParameterContext::Fetch, limits);
    if (const auto* value = std::get_if<NeedMore>(&parameters)) return *value;
    if (const auto* value = std::get_if<DecodeError>(&parameters)) return *value;
    if (const auto* value = std::get_if<DraftAmbiguity>(&parameters)) return *value;
    input = working;
    return FetchMessage{std::get<std::uint64_t>(request_id), std::move(fetch),
                        std::get<Parameters>(parameters)};
}

EncodeResult encode_track_opening_fields(std::uint64_t request_id,
                                         const TrackNamespace& name_space,
                                         const TrackName& name,
                                         std::optional<std::uint64_t> alias,
                                         const Parameters& parameters,
                                         ParameterContext context,
                                         ByteWriter& output) {
    if (!write_vi64(request_id, output)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "request ID exceeds payload capacity");
    }
    auto result = encode_track_namespace(name_space, output);
    if (!result.has_value()) return result;
    result = encode_track_name(name, name_space, output);
    if (!result.has_value()) return result;
    if (alias && !write_vi64(*alias, output)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "track alias exceeds payload capacity");
    }
    if (!write_vi64(parameters.size(), output)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "parameter count exceeds payload capacity");
    }
    return encode_parameters(parameters, context, output);
}

EncodeResult encode_namespace_opening_fields(
    std::uint64_t request_id, const TrackNamespace& name_space,
    const Parameters& parameters, ParameterContext context, ByteWriter& output) {
    if (!write_vi64(request_id, output)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "request ID exceeds payload capacity");
    }
    auto result = encode_track_namespace(name_space, output);
    if (!result.has_value()) return result;
    if (!write_vi64(parameters.size(), output)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "parameter count exceeds payload capacity");
    }
    return encode_parameters(parameters, context, output);
}

DraftDecodeResult<ReasonPhrase> decode_reason_phrase(Cursor& input) {
    Cursor working = input;
    const auto length_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&length_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;
    const auto length = std::get<std::uint64_t>(length_result);
    if (length > kMaximumReasonPhrase) {
        return protocol_violation(input.offset(),
                                  "reason phrase exceeds 1024 bytes");
    }
    const auto bytes_result =
        read_bytes(working, static_cast<std::size_t>(length));
    if (const auto* need = std::get_if<NeedMore>(&bytes_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&bytes_result)) return *error;
    input = working;
    return ReasonPhrase{copy_bytes(
        std::get<std::span<const std::byte>>(bytes_result))};
}

EncodeResult encode_reason_phrase(const ReasonPhrase& reason,
                                  ByteWriter& output) {
    if (reason.bytes.size() > kMaximumReasonPhrase) {
        return EncodeResult::failure(EncodeErrorCode::PayloadTooLarge,
                                     "reason phrase exceeds 1024 bytes");
    }
    ByteWriter staged(kMaximumReasonPhrase + 2);
    if (!write_length_prefixed_bytes(reason.bytes, staged)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "reason phrase exceeds staging capacity");
    }
    return append_staged(staged, output,
                         "reason phrase exceeds output capacity");
}

DraftDecodeResult<Redirect> decode_redirect(Cursor& input,
                                            const Limits& limits) {
    Cursor working = input;
    const auto uri_result =
        read_length_prefixed_bytes(working, kMaximumMessagePayload);
    if (const auto* need = std::get_if<NeedMore>(&uri_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&uri_result)) {
        if (error->code == DecodeErrorCode::LengthExceedsLimit) {
            return protocol_violation(error->offset,
                                      "redirect URI exceeds framed payload limit");
        }
        return *error;
    }
    const auto name_space = decode_track_namespace(working, limits);
    if (const auto* need = std::get_if<NeedMore>(&name_space)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&name_space)) return *error;
    const auto decoded_namespace = std::get<TrackNamespace>(name_space);
    const auto name = decode_track_name(working, decoded_namespace, limits);
    if (const auto* need = std::get_if<NeedMore>(&name)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&name)) return *error;
    input = working;
    return Redirect{
        copy_bytes(std::get<std::span<const std::byte>>(uri_result)),
        decoded_namespace, std::get<TrackName>(name)};
}

EncodeResult encode_redirect(const Redirect& redirect, ByteWriter& output) {
    ByteWriter staged(kMaximumMessagePayload);
    if (!write_length_prefixed_bytes(redirect.connect_uri, staged)) {
        return EncodeResult::failure(EncodeErrorCode::PayloadTooLarge,
                                     "redirect URI exceeds payload capacity");
    }
    auto result = encode_track_namespace(redirect.track_namespace, staged);
    if (result.has_value()) {
        result = encode_track_name(redirect.track_name,
                                   redirect.track_namespace, staged);
    }
    if (!result.has_value()) return result;
    return append_staged(staged, output, "redirect exceeds output capacity");
}

DraftDecodeResult<Parameters> decode_counted_parameters(
    Cursor& input, ParameterContext context, const Limits& limits) {
    Cursor working = input;
    const auto count_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&count_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&count_result)) return *error;
    const auto parameters = decode_parameters(
        working, std::get<std::uint64_t>(count_result), context, limits);
    if (const auto* need = std::get_if<NeedMore>(&parameters)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&parameters)) return *error;
    if (const auto* ambiguity = std::get_if<DraftAmbiguity>(&parameters)) {
        return *ambiguity;
    }
    input = working;
    return std::get<Parameters>(parameters);
}

EncodeResult encode_counted_parameters(const Parameters& parameters,
                                       ParameterContext context,
                                       ByteWriter& output) {
    if (!write_vi64(parameters.size(), output)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "parameter count exceeds payload capacity");
    }
    return encode_parameters(parameters, context, output);
}

template <class T>
std::optional<MessageDecodeResult> bounded_message_failure(
    const DraftDecodeResult<T>& result) {
    if (const auto* value = std::get_if<NeedMore>(&result)) {
        return protocol_violation(value->offset,
                                  "message field exceeds framed payload");
    }
    if (const auto* value = std::get_if<DecodeError>(&result)) return *value;
    if (const auto* value = std::get_if<DraftAmbiguity>(&result)) return *value;
    return std::nullopt;
}

}  // namespace

EncodeResult::EncodeResult(State state, EncodeError error,
                           DraftAmbiguity ambiguity)
    : state_(state), error_(std::move(error)), ambiguity_(std::move(ambiguity)) {}

EncodeResult EncodeResult::success() { return EncodeResult(State::Success); }

EncodeResult EncodeResult::failure(EncodeErrorCode code, std::string detail) {
    return EncodeResult(State::Error, EncodeError{code, std::move(detail)});
}

EncodeResult EncodeResult::ambiguity(std::size_t offset, std::string detail) {
    return EncodeResult(State::Ambiguity, {},
                        DraftAmbiguity{offset, std::move(detail)});
}

bool EncodeResult::has_value() const noexcept { return state_ == State::Success; }

bool EncodeResult::is_ambiguity() const noexcept {
    return state_ == State::Ambiguity;
}

const EncodeError* EncodeResult::error() const noexcept {
    return state_ == State::Error ? &error_ : nullptr;
}

const DraftAmbiguity* EncodeResult::draft_ambiguity() const noexcept {
    return state_ == State::Ambiguity ? &ambiguity_ : nullptr;
}

KeyValueDecodeResult decode_key_value_pairs(Cursor& input,
                                            std::size_t payload_length,
                                            const Limits& limits) {
    Cursor working = input;
    const auto payload_result = read_bytes(working, payload_length);
    if (const auto* need = std::get_if<NeedMore>(&payload_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&payload_result)) return *error;

    const auto payload = std::get<std::span<const std::byte>>(payload_result);
    Cursor bounded(payload, input.offset());
    KeyValuePairs entries;
    std::uint64_t previous_type = 0;
    while (bounded.remaining() != 0) {
        const auto entry_offset = bounded.offset();
        const auto delta_result = read_vi64(bounded);
        if (const auto* need = std::get_if<NeedMore>(&delta_result)) {
            return bounded_payload_error<KeyValuePairs>(
                *need, "KVP type is truncated within bounded payload");
        }
        if (const auto* error = std::get_if<DecodeError>(&delta_result)) return *error;
        const auto delta = std::get<std::uint64_t>(delta_result);
        if (delta > std::numeric_limits<std::uint64_t>::max() - previous_type) {
            return protocol_violation(entry_offset,
                                      "KVP resolved type overflows uint64");
        }
        const auto type = previous_type + delta;

        if ((type & 1u) == 0u) {
            const auto value_start = bounded.remaining();
            const auto value_result = read_vi64(bounded);
            if (const auto* need = std::get_if<NeedMore>(&value_result)) {
                return bounded_payload_error<KeyValuePairs>(
                    *need, "KVP integer is truncated within bounded payload");
            }
            if (const auto* error = std::get_if<DecodeError>(&value_result)) return *error;
            const auto consumed = value_start - bounded.remaining();
            const auto raw_offset = payload.size() - value_start;
            entries.push_back(KeyValuePair{
                type,
                VarIntValue{std::get<std::uint64_t>(value_result),
                            copy_bytes(payload.subspan(raw_offset, consumed))}});
        } else {
            const auto length_result = read_vi64(bounded);
            if (const auto* need = std::get_if<NeedMore>(&length_result)) {
                return bounded_payload_error<KeyValuePairs>(
                    *need, "KVP length is truncated within bounded payload");
            }
            if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;
            const auto declared = std::get<std::uint64_t>(length_result);
            if (declared > kMaximumMessagePayload) {
                return protocol_violation(
                    entry_offset, "KVP byte value exceeds draft limit");
            }
            if (declared > limits.maximum_odd_value_length) {
                return DecodeError{DecodeErrorCode::LengthExceedsLimit, entry_offset,
                                   "KVP byte value exceeds configured limit"};
            }
            const auto value_result =
                read_bytes(bounded, static_cast<std::size_t>(declared));
            if (const auto* need = std::get_if<NeedMore>(&value_result)) {
                return bounded_payload_error<KeyValuePairs>(
                    *need, "KVP bytes are truncated within bounded payload");
            }
            if (const auto* error = std::get_if<DecodeError>(&value_result)) return *error;
            entries.push_back(KeyValuePair{
                type, ByteValue{copy_bytes(
                          std::get<std::span<const std::byte>>(value_result))}});
        }
        previous_type = type;
    }

    input = working;
    return entries;
}

EncodeResult encode_key_value_pairs(std::span<const KeyValuePair> entries,
                                    ByteWriter& output) {
    ByteWriter staged(kMaximumMessagePayload);
    const auto result = encode_key_value_pairs_to(entries, staged);
    if (!result.has_value()) return result;
    if (!output.append_bytes(staged.bytes())) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "encoded KVPs exceed output capacity");
    }
    return EncodeResult::success();
}

TrackPropertiesDecodeResult decode_track_properties(Cursor& input,
                                                     std::size_t payload_length,
                                                     const Limits& limits) {
    Cursor working = input;
    const auto result = decode_key_value_pairs(working, payload_length, limits);
    if (const auto* value = std::get_if<KeyValuePairs>(&result)) {
        input = working;
        return TrackProperties{std::move(*value)};
    }
    if (const auto* need = std::get_if<NeedMore>(&result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&result)) {
        if (error->code == DecodeErrorCode::InvalidValue) {
            return key_value_formatting_error(
                error->offset, "track property KVP is malformed");
        }
        return *error;
    }
    return std::get<DraftAmbiguity>(result);
}

bool is_mandatory_track_property(const KeyValuePair& property) noexcept {
    return property.type >= 0x4000 && property.type <= 0x7fff;
}

LocationDecodeResult decode_location(Cursor& input) {
    Cursor working = input;
    const auto group = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&group)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&group)) return *error;
    const auto object = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&object)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&object)) return *error;
    input = working;
    return Location{std::get<std::uint64_t>(group),
                    std::get<std::uint64_t>(object)};
}

EncodeResult encode_location(const Location& location, ByteWriter& output) {
    ByteWriter staged(18);
    if (!write_vi64(location.group, staged) ||
        !write_vi64(location.object, staged)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "location exceeds staging capacity");
    }
    return append_staged(staged, output, "location exceeds output capacity");
}

TrackNamespaceDecodeResult decode_track_namespace(Cursor& input,
                                                   const Limits&) {
    Cursor working = input;
    const auto count_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&count_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&count_result)) return *error;
    const auto count = std::get<std::uint64_t>(count_result);
    if (count > 32) {
        return protocol_violation(input.offset(),
                                  "track namespace has more than 32 fields");
    }
    TrackNamespace result;
    result.fields.reserve(static_cast<std::size_t>(count));
    std::size_t total = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto length_result = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&length_result)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;
        const auto length = std::get<std::uint64_t>(length_result);
        if (length == 0) {
            return protocol_violation(working.offset(),
                                      "track namespace field is empty");
        }
        if (length > kMaximumFullTrackName - total) {
            return protocol_violation(working.offset(),
                                      "track namespace exceeds 4096 bytes");
        }
        const auto field_result = read_bytes(working, static_cast<std::size_t>(length));
        if (const auto* need = std::get_if<NeedMore>(&field_result)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&field_result)) return *error;
        const auto field = std::get<std::span<const std::byte>>(field_result);
        result.fields.emplace_back(field.begin(), field.end());
        total += static_cast<std::size_t>(length);
    }
    input = working;
    return result;
}

EncodeResult encode_track_namespace(const TrackNamespace& name_space,
                                    ByteWriter& output) {
    if (name_space.fields.size() > 32) {
        return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                     "track namespace has more than 32 fields");
    }
    if (namespace_value_length(name_space) > kMaximumFullTrackName) {
        return EncodeResult::failure(EncodeErrorCode::PayloadTooLarge,
                                     "track namespace exceeds 4096 bytes");
    }
    ByteWriter staged(kMaximumMessagePayload);
    if (!write_vi64(name_space.fields.size(), staged)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "namespace count exceeds staging capacity");
    }
    for (const auto& field : name_space.fields) {
        if (field.empty()) {
            return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                         "track namespace field is empty");
        }
        if (!write_length_prefixed_bytes(field, staged)) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "namespace exceeds staging capacity");
        }
    }
    return append_staged(staged, output, "namespace exceeds output capacity");
}

TrackNameDecodeResult decode_track_name(Cursor& input,
                                        const TrackNamespace& name_space,
                                        const Limits&) {
    const auto namespace_length = namespace_value_length(name_space);
    if (namespace_length > kMaximumFullTrackName) {
        return protocol_violation(input.offset(),
                                  "track namespace exceeds 4096 bytes");
    }
    Cursor working = input;
    const auto name_result = read_length_prefixed_bytes(
        working, kMaximumFullTrackName - namespace_length);
    if (const auto* need = std::get_if<NeedMore>(&name_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&name_result)) {
        if (error->code == DecodeErrorCode::LengthExceedsLimit) {
            return protocol_violation(error->offset,
                                      "full track name exceeds 4096 bytes");
        }
        return *error;
    }
    const auto name = std::get<std::span<const std::byte>>(name_result);
    input = working;
    return TrackName{copy_bytes(name)};
}

EncodeResult encode_track_name(const TrackName& name,
                               const TrackNamespace& name_space,
                               ByteWriter& output) {
    const auto namespace_length = namespace_value_length(name_space);
    if (namespace_length > kMaximumFullTrackName ||
        name.bytes.size() > kMaximumFullTrackName - namespace_length) {
        return EncodeResult::failure(EncodeErrorCode::PayloadTooLarge,
                                     "full track name exceeds 4096 bytes");
    }
    ByteWriter staged(kMaximumFullTrackName + 3);
    if (!write_length_prefixed_bytes(name.bytes, staged)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "track name exceeds staging capacity");
    }
    return append_staged(staged, output, "track name exceeds output capacity");
}

SubscriptionFilterDecodeResult decode_subscription_filter(
    Cursor& input, std::size_t encoded_length) {
    Cursor working = input;
    const auto bytes_result = read_bytes(working, encoded_length);
    if (const auto* need = std::get_if<NeedMore>(&bytes_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&bytes_result)) return *error;
    const auto encoded = std::get<std::span<const std::byte>>(bytes_result);
    Cursor bounded(encoded, input.offset());
    const auto type_result = read_vi64(bounded);
    if (const auto* need = std::get_if<NeedMore>(&type_result)) {
        return protocol_violation(need->offset,
                                  "subscription filter type is missing");
    }
    if (const auto* error = std::get_if<DecodeError>(&type_result)) return *error;
    const auto raw_type = std::get<std::uint64_t>(type_result);
    if (raw_type < 1 || raw_type > 4) {
        return protocol_violation(input.offset(),
                                  "unknown subscription filter type");
    }
    SubscriptionFilter result{static_cast<SubscriptionFilterType>(raw_type),
                              std::nullopt, std::nullopt};
    if (raw_type == 3 || raw_type == 4) {
        const auto location = decode_location(bounded);
        if (const auto* need = std::get_if<NeedMore>(&location)) {
            return protocol_violation(
                need->offset, "subscription filter location is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&location)) return *error;
        result.start = std::get<Location>(location);
    }
    if (raw_type == 4) {
        const auto delta = read_vi64(bounded);
        if (const auto* need = std::get_if<NeedMore>(&delta)) {
            return protocol_violation(
                need->offset, "subscription filter delta is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&delta)) return *error;
        result.end_group_delta = std::get<std::uint64_t>(delta);
        if (*result.end_group_delta >
            std::numeric_limits<std::uint64_t>::max() - result.start->group) {
            return protocol_violation(input.offset(),
                                      "subscription filter range overflows");
        }
    }
    if (bounded.remaining() != 0) {
        return protocol_violation(bounded.offset(),
                                  "subscription filter has trailing bytes");
    }
    input = working;
    return result;
}

EncodeResult encode_subscription_filter(const SubscriptionFilter& filter,
                                        ByteWriter& output) {
    const auto raw_type = static_cast<std::uint64_t>(filter.type);
    if (raw_type < 1 || raw_type > 4) {
        return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                     "unknown subscription filter type");
    }
    const bool needs_start = raw_type == 3 || raw_type == 4;
    const bool needs_delta = raw_type == 4;
    if (filter.start.has_value() != needs_start ||
        filter.end_group_delta.has_value() != needs_delta) {
        return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                     "subscription filter fields do not match type");
    }
    if (needs_delta && *filter.end_group_delta >
                           std::numeric_limits<std::uint64_t>::max() -
                               filter.start->group) {
        return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                     "subscription filter range overflows");
    }
    ByteWriter staged(36);
    if (!write_vi64(raw_type, staged)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "filter type exceeds staging capacity");
    }
    if (needs_start && !encode_location(*filter.start, staged).has_value()) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "filter location exceeds staging capacity");
    }
    if (needs_delta && !write_vi64(*filter.end_group_delta, staged)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "filter delta exceeds staging capacity");
    }
    return append_staged(staged, output, "filter exceeds output capacity");
}

TokenDecodeResult decode_token(Cursor& input, std::size_t encoded_length) {
    Cursor working = input;
    const auto bytes_result = read_bytes(working, encoded_length);
    if (const auto* need = std::get_if<NeedMore>(&bytes_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&bytes_result)) return *error;
    const auto encoded = std::get<std::span<const std::byte>>(bytes_result);
    Cursor bounded(encoded, input.offset());
    const auto alias_type_result = read_vi64(bounded);
    if (const auto* need = std::get_if<NeedMore>(&alias_type_result)) {
        return key_value_formatting_error(need->offset,
                                          "token alias type is missing");
    }
    if (const auto* error = std::get_if<DecodeError>(&alias_type_result)) return *error;
    const auto raw_type = std::get<std::uint64_t>(alias_type_result);
    if (raw_type > 3) {
        return key_value_formatting_error(input.offset(),
                                          "unknown token alias type");
    }
    Token result;
    result.alias_type = static_cast<TokenAliasType>(raw_type);

    if (raw_type != 3) {
        const auto alias = read_vi64(bounded);
        if (const auto* need = std::get_if<NeedMore>(&alias)) {
            return key_value_formatting_error(need->offset,
                                              "token alias is missing");
        }
        if (const auto* error = std::get_if<DecodeError>(&alias)) return *error;
        result.alias = std::get<std::uint64_t>(alias);
    }
    if (raw_type == 1 || raw_type == 3) {
        const auto token_type = read_vi64(bounded);
        if (const auto* need = std::get_if<NeedMore>(&token_type)) {
            return key_value_formatting_error(need->offset,
                                              "token type is missing");
        }
        if (const auto* error = std::get_if<DecodeError>(&token_type)) return *error;
        result.token_type = std::get<std::uint64_t>(token_type);
        const auto value = read_bytes(bounded, bounded.remaining());
        result.token_value = copy_bytes(
            std::get<std::span<const std::byte>>(value));
    } else if (bounded.remaining() != 0) {
        return key_value_formatting_error(
            bounded.offset(), "alias-only token has trailing bytes");
    }
    input = working;
    return result;
}

EncodeResult encode_token(const Token& token, ByteWriter& output) {
    const auto raw_type = static_cast<std::uint64_t>(token.alias_type);
    if (raw_type > 3) {
        return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                     "unknown token alias type");
    }
    const bool needs_alias = raw_type != 3;
    const bool needs_value = raw_type == 1 || raw_type == 3;
    if (token.alias.has_value() != needs_alias ||
        token.token_type.has_value() != needs_value ||
        (!needs_value && !token.token_value.empty())) {
        return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                     "token fields do not match alias type");
    }
    ByteWriter staged(kMaximumMessagePayload);
    if (!write_vi64(raw_type, staged) ||
        (needs_alias && !write_vi64(*token.alias, staged)) ||
        (needs_value && !write_vi64(*token.token_type, staged)) ||
        (needs_value && !staged.append_bytes(token.token_value))) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "token exceeds staging capacity");
    }
    return append_staged(staged, output, "token exceeds output capacity");
}

ParameterScopeResult validate_parameter_scope(std::uint64_t type,
                                              ParameterContext context) noexcept {
    if (context == ParameterContext::Unresolved) {
        return ParameterScopeResult::Unresolved;
    }
    bool allowed = false;
    switch (type) {
        case 0x02:
        case 0x06:
            allowed = context_is(context, {ParameterContext::Subscribe,
                                           ParameterContext::PublishOk,
                                           ParameterContext::RequestUpdateSubscription});
            break;
        case 0x03:
            allowed = context_is(context, {
                ParameterContext::Publish, ParameterContext::Subscribe,
                ParameterContext::RequestUpdateSubscription,
                ParameterContext::RequestUpdateFetch,
                ParameterContext::RequestUpdatePublishNamespace,
                ParameterContext::RequestUpdateSubscribeNamespace,
                ParameterContext::RequestUpdateSubscribeTracks,
                ParameterContext::SubscribeNamespace,
                ParameterContext::SubscribeTracks,
                ParameterContext::PublishNamespace,
                ParameterContext::TrackStatus, ParameterContext::Fetch});
            break;
        case 0x04:
            allowed = context == ParameterContext::Subscribe;
            break;
        case 0x08:
            allowed = context_is(context, {ParameterContext::SubscribeOk,
                                           ParameterContext::Publish,
                                           ParameterContext::PublishOk,
                                           ParameterContext::RequestUpdateOk});
            break;
        case 0x09:
            allowed = context_is(context, {ParameterContext::SubscribeOk,
                                           ParameterContext::Publish,
                                           ParameterContext::RequestUpdateOk,
                                           ParameterContext::TrackStatusOk});
            break;
        case 0x0a:
            allowed = context == ParameterContext::Fetch;
            break;
        case 0x10:
            allowed = context_is(context, {ParameterContext::Subscribe,
                                           ParameterContext::RequestUpdateSubscription,
                                           ParameterContext::Publish,
                                           ParameterContext::PublishOk,
                                           ParameterContext::SubscribeTracks});
            break;
        case 0x20:
            allowed = context_is(context, {ParameterContext::Subscribe,
                                           ParameterContext::Fetch,
                                           ParameterContext::RequestUpdateSubscription,
                                           ParameterContext::RequestUpdateFetch,
                                           ParameterContext::PublishOk});
            break;
        case 0x21:
            allowed = context_is(context, {ParameterContext::Subscribe,
                                           ParameterContext::PublishOk,
                                           ParameterContext::RequestUpdateSubscription});
            break;
        case 0x22:
            allowed = context_is(context, {ParameterContext::Subscribe,
                                           ParameterContext::PublishOk,
                                           ParameterContext::Fetch});
            break;
        case 0x32:
            allowed = context_is(context, {ParameterContext::PublishOk,
                                           ParameterContext::Subscribe,
                                           ParameterContext::RequestUpdateSubscription});
            break;
        case 0x34:
            allowed = context_is(context, {
                ParameterContext::RequestUpdateSubscribeNamespace,
                ParameterContext::RequestUpdateSubscribeTracks});
            break;
        default:
            return ParameterScopeResult::Forbidden;
    }
    return allowed ? ParameterScopeResult::Allowed
                   : ParameterScopeResult::Forbidden;
}

ParametersDecodeResult decode_parameters(Cursor& input, std::uint64_t count,
                                         ParameterContext context,
                                         const Limits& limits) {
    if (count > limits.maximum_parameter_count) {
        return invalid(input.offset(), "parameter count exceeds configured limit");
    }
    if (count > input.remaining()) {
        return NeedMore{input.offset(), static_cast<std::size_t>(count),
                        input.remaining()};
    }
    Cursor working = input;
    Parameters parameters;
    parameters.reserve(static_cast<std::size_t>(count));
    std::uint64_t previous_type = 0;
    for (std::uint64_t index = 0; index < count; ++index) {
        const auto entry_offset = working.offset();
        const auto delta_result = read_vi64(working);
        if (const auto* need = std::get_if<NeedMore>(&delta_result)) return *need;
        if (const auto* error = std::get_if<DecodeError>(&delta_result)) return *error;
        const auto delta = std::get<std::uint64_t>(delta_result);
        if (delta > std::numeric_limits<std::uint64_t>::max() - previous_type) {
            return protocol_violation(
                entry_offset, "parameter resolved type overflows uint64");
        }
        const auto type = previous_type + delta;
        if (!is_known_parameter(type)) {
            return protocol_violation(entry_offset,
                                      "unknown message parameter");
        }
        if (index != 0 && type == previous_type && type != 0x03) {
            return protocol_violation(entry_offset,
                                      "duplicate non-repeatable parameter");
        }
        const auto scope = validate_parameter_scope(type, context);
        if (scope == ParameterScopeResult::Forbidden) {
            return protocol_violation(
                entry_offset, "message parameter is forbidden in context");
        }
        if (type == 0x04 || type == 0x0a) {
            return DraftAmbiguity{entry_offset,
                                  "draft does not specify timeout parameter encoding"};
        }

        ParameterValue value;
        if (type == 0x02 || type == 0x06 || type == 0x08 || type == 0x32) {
            const auto decoded = read_vi64(working);
            if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
            if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
            value = VarIntParameterValue{std::get<std::uint64_t>(decoded)};
        } else if (type == 0x03) {
            Cursor length_cursor = working;
            const auto length_result = read_vi64(length_cursor);
            if (const auto* need = std::get_if<NeedMore>(&length_result)) return *need;
            if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;
            const auto length = std::get<std::uint64_t>(length_result);
            if (length > kMaximumMessagePayload) {
                return protocol_violation(
                    entry_offset, "authorization token exceeds draft limit");
            }
            if (length > limits.maximum_odd_value_length) {
                return DecodeError{DecodeErrorCode::LengthExceedsLimit, entry_offset,
                                   "authorization token exceeds configured limit"};
            }
            working = length_cursor;
            const auto decoded =
                decode_token(working, static_cast<std::size_t>(length));
            if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
            if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
            if (const auto* ambiguity = std::get_if<DraftAmbiguity>(&decoded)) {
                return *ambiguity;
            }
            value = std::get<Token>(decoded);
        } else if (type == 0x09) {
            const auto decoded = decode_location(working);
            if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
            if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
            value = std::get<Location>(decoded);
        } else if (type == 0x10 || type == 0x20 || type == 0x22) {
            const auto decoded = read_bytes(working, 1);
            if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
            if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
            const auto byte = std::to_integer<std::uint8_t>(
                std::get<std::span<const std::byte>>(decoded)[0]);
            if ((type == 0x10 && byte > 1) ||
                (type == 0x22 && (byte < 1 || byte > 2))) {
                return protocol_violation(entry_offset,
                                          "parameter enum value is invalid");
            }
            value = Uint8ParameterValue{byte};
        } else if (type == 0x21) {
            Cursor length_cursor = working;
            const auto length_result = read_vi64(length_cursor);
            if (const auto* need = std::get_if<NeedMore>(&length_result)) return *need;
            if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;
            const auto length = std::get<std::uint64_t>(length_result);
            if (length > kMaximumMessagePayload) {
                return protocol_violation(
                    entry_offset, "subscription filter exceeds draft limit");
            }
            if (length > limits.maximum_odd_value_length) {
                return DecodeError{DecodeErrorCode::LengthExceedsLimit, entry_offset,
                                   "subscription filter exceeds configured limit"};
            }
            working = length_cursor;
            const auto decoded = decode_subscription_filter(
                working, static_cast<std::size_t>(length));
            if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
            if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
            value = std::get<SubscriptionFilter>(decoded);
        } else {
            const auto decoded = decode_track_namespace(working, limits);
            if (const auto* need = std::get_if<NeedMore>(&decoded)) return *need;
            if (const auto* error = std::get_if<DecodeError>(&decoded)) return *error;
            value = std::get<TrackNamespace>(decoded);
        }
        parameters.push_back(Parameter{type, std::move(value)});
        previous_type = type;
    }
    input = working;
    return parameters;
}

EncodeResult encode_parameters(std::span<const Parameter> parameters,
                               ParameterContext context, ByteWriter& output) {
    ByteWriter staged(kMaximumMessagePayload);
    std::uint64_t previous_type = 0;
    bool first = true;
    for (const auto& parameter : parameters) {
        if (!is_known_parameter(parameter.type)) {
            return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                         "unknown message parameter");
        }
        if (!first && parameter.type < previous_type) {
            return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                         "parameter types are not ordered");
        }
        if (!first && parameter.type == previous_type && parameter.type != 0x03) {
            return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                         "duplicate non-repeatable parameter");
        }
        if (validate_parameter_scope(parameter.type, context) ==
            ParameterScopeResult::Forbidden) {
            return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                         "message parameter is forbidden in context");
        }
        if (parameter.type == 0x04 || parameter.type == 0x0a) {
            return EncodeResult::ambiguity(
                0, "draft does not specify timeout parameter encoding");
        }
        const auto delta = first ? parameter.type : parameter.type - previous_type;
        if (!write_vi64(delta, staged)) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "parameter type exceeds staging capacity");
        }

        if (parameter.type == 0x02 || parameter.type == 0x06 ||
            parameter.type == 0x08 || parameter.type == 0x32) {
            const auto* value = std::get_if<VarIntParameterValue>(&parameter.value);
            if (value == nullptr || !write_vi64(value->value, staged)) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "parameter requires vi64 value");
            }
        } else if (parameter.type == 0x03) {
            const auto* value = std::get_if<Token>(&parameter.value);
            if (value == nullptr) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "authorization parameter requires token");
            }
            ByteWriter token(kMaximumMessagePayload);
            const auto result = encode_token(*value, token);
            if (!result.has_value() ||
                !write_length_prefixed_bytes(token.bytes(), staged)) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "authorization token is invalid");
            }
        } else if (parameter.type == 0x09) {
            const auto* value = std::get_if<Location>(&parameter.value);
            if (value == nullptr || !encode_location(*value, staged).has_value()) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "largest object requires location");
            }
        } else if (parameter.type == 0x10 || parameter.type == 0x20 ||
                   parameter.type == 0x22) {
            const auto* value = std::get_if<Uint8ParameterValue>(&parameter.value);
            if (value == nullptr || (parameter.type == 0x10 && value->value > 1) ||
                (parameter.type == 0x22 &&
                 (value->value < 1 || value->value > 2)) ||
                !staged.append_byte(static_cast<std::byte>(value->value))) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "uint8 parameter value is invalid");
            }
        } else if (parameter.type == 0x21) {
            const auto* value = std::get_if<SubscriptionFilter>(&parameter.value);
            if (value == nullptr) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "filter parameter requires filter");
            }
            ByteWriter filter(36);
            const auto result = encode_subscription_filter(*value, filter);
            if (!result.has_value() ||
                !write_length_prefixed_bytes(filter.bytes(), staged)) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "subscription filter is invalid");
            }
        } else {
            const auto* value = std::get_if<TrackNamespace>(&parameter.value);
            if (value == nullptr ||
                !encode_track_namespace(*value, staged).has_value()) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "namespace prefix is invalid");
            }
        }
        previous_type = parameter.type;
        first = false;
    }
    return append_staged(staged, output, "parameters exceed output capacity");
}

MessageDecodeResult decode_message(StreamRole role, Cursor& input,
                                   const Limits& limits) {
    Cursor working = input;
    const auto type_result = read_vi64(working);
    if (const auto* need = std::get_if<NeedMore>(&type_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&type_result)) return *error;

    const auto type = std::get<std::uint64_t>(type_result);
    if (type == kSetupMessageType && role != StreamRole::Control) {
        return protocol_violation(input.offset(),
                                  "SETUP is not valid on a request stream");
    }
    if ((is_opening_request_type(type) || is_continuation_request_type(type)) &&
        role != StreamRole::Request) {
        return protocol_violation(input.offset(),
                                  "request message is not valid on control stream");
    }
    if (type != kSetupMessageType && type != kGoawayMessageType &&
        !is_opening_request_type(type) &&
        !is_continuation_request_type(type)) {
        return protocol_violation(
            input.offset(), "unknown, reserved, or removed message type");
    }

    const auto length_result = read_bytes(working, 2);
    if (const auto* need = std::get_if<NeedMore>(&length_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&length_result)) return *error;
    const auto length_bytes = std::get<std::span<const std::byte>>(length_result);
    const auto payload_length =
        (static_cast<std::size_t>(std::to_integer<std::uint8_t>(length_bytes[0])) << 8u) |
        static_cast<std::size_t>(std::to_integer<std::uint8_t>(length_bytes[1]));

    const auto payload_offset = working.offset();
    const auto payload_result = read_bytes(working, payload_length);
    if (const auto* need = std::get_if<NeedMore>(&payload_result)) return *need;
    if (const auto* error = std::get_if<DecodeError>(&payload_result)) return *error;

    const auto payload = std::get<std::span<const std::byte>>(payload_result);
    Cursor payload_cursor(payload, payload_offset);
    Message message;
    if (type == kSetupMessageType) {
        const auto options_result =
            decode_key_value_pairs(payload_cursor, payload.size(), limits);
        if (const auto* need = std::get_if<NeedMore>(&options_result)) {
            return invalid(need->offset, "SETUP option exceeds framed payload");
        }
        if (const auto* error = std::get_if<DecodeError>(&options_result)) {
            if (error->code == DecodeErrorCode::InvalidValue) {
                return key_value_formatting_error(
                    error->offset, "SETUP option KVP is malformed");
            }
            return *error;
        }
        if (const auto* ambiguity = std::get_if<DraftAmbiguity>(&options_result)) {
            return *ambiguity;
        }
        auto options = std::get<KeyValuePairs>(options_result);
        message = SetupMessage{std::move(options)};
    } else if (type == kGoawayMessageType) {
        const auto uri_result =
            read_length_prefixed_bytes(payload_cursor, kMaximumNewSessionUri);
        if (const auto* need = std::get_if<NeedMore>(&uri_result)) {
            return protocol_violation(need->offset,
                                      "GOAWAY URI exceeds framed payload");
        }
        if (const auto* error = std::get_if<DecodeError>(&uri_result)) {
            if (error->code == DecodeErrorCode::LengthExceedsLimit) {
                return protocol_violation(error->offset,
                                          "GOAWAY URI exceeds 8192 bytes");
            }
            return *error;
        }
        const auto timeout_result = read_vi64(payload_cursor);
        if (const auto* need = std::get_if<NeedMore>(&timeout_result)) {
            return protocol_violation(need->offset,
                                      "GOAWAY timeout exceeds framed payload");
        }
        if (const auto* error = std::get_if<DecodeError>(&timeout_result)) {
            return *error;
        }
        std::optional<std::uint64_t> request_id;
        if (role == StreamRole::Control) {
            const auto request_id_result = read_vi64(payload_cursor);
            if (const auto* need = std::get_if<NeedMore>(&request_id_result)) {
                return protocol_violation(
                    need->offset, "GOAWAY request ID exceeds framed payload");
            }
            if (const auto* error =
                    std::get_if<DecodeError>(&request_id_result)) {
                return *error;
            }
            request_id = std::get<std::uint64_t>(request_id_result);
        }
        message = GoawayMessage{
            copy_bytes(std::get<std::span<const std::byte>>(uri_result)),
            std::get<std::uint64_t>(timeout_result), request_id};
    } else if (type == kSubscribeOkMessageType ||
               type == kRequestOkMessageType) {
        std::uint64_t track_alias = 0;
        if (type == kSubscribeOkMessageType) {
            const auto alias_result = read_vi64(payload_cursor);
            if (const auto* need = std::get_if<NeedMore>(&alias_result)) {
                return protocol_violation(
                    need->offset, "SUBSCRIBE_OK alias exceeds framed payload");
            }
            if (const auto* error = std::get_if<DecodeError>(&alias_result)) {
                return *error;
            }
            track_alias = std::get<std::uint64_t>(alias_result);
        }
        const auto context = type == kSubscribeOkMessageType
                                 ? ParameterContext::SubscribeOk
                                 : ParameterContext::Unresolved;
        const auto parameters =
            decode_counted_parameters(payload_cursor, context, limits);
        if (const auto failure = bounded_message_failure(parameters)) {
            return *failure;
        }
        const auto properties = decode_track_properties(
            payload_cursor, payload_cursor.remaining(), limits);
        if (const auto failure = bounded_message_failure(properties)) {
            return *failure;
        }
        if (type == kSubscribeOkMessageType) {
            message = SubscribeOkMessage{
                track_alias, std::get<Parameters>(parameters),
                std::get<TrackProperties>(properties)};
        } else {
            message = RequestOkMessage{
                std::get<Parameters>(parameters),
                std::get<TrackProperties>(properties)};
        }
    } else if (type == kRequestErrorMessageType) {
        const auto code_result = read_vi64(payload_cursor);
        if (const auto* need = std::get_if<NeedMore>(&code_result)) {
            return protocol_violation(need->offset,
                                      "REQUEST_ERROR code is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&code_result)) {
            return *error;
        }
        const auto retry_result = read_vi64(payload_cursor);
        if (const auto* need = std::get_if<NeedMore>(&retry_result)) {
            return protocol_violation(need->offset,
                                      "REQUEST_ERROR retry is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&retry_result)) {
            return *error;
        }
        const auto reason = decode_reason_phrase(payload_cursor);
        if (const auto failure = bounded_message_failure(reason)) return *failure;
        const auto code = std::get<std::uint64_t>(code_result);
        std::optional<Redirect> redirect;
        if (code == 0x34) {
            const auto decoded = decode_redirect(payload_cursor, limits);
            if (const auto failure = bounded_message_failure(decoded)) {
                return *failure;
            }
            redirect = std::get<Redirect>(decoded);
        }
        message = RequestErrorMessage{
            code, std::get<std::uint64_t>(retry_result),
            std::get<ReasonPhrase>(reason), std::move(redirect)};
    } else if (type == kRequestUpdateMessageType) {
        const auto request_id_result = read_vi64(payload_cursor);
        if (const auto* need = std::get_if<NeedMore>(&request_id_result)) {
            return protocol_violation(need->offset,
                                      "REQUEST_UPDATE ID is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&request_id_result)) {
            return *error;
        }
        const auto parameters = decode_counted_parameters(
            payload_cursor, ParameterContext::Unresolved, limits);
        if (const auto failure = bounded_message_failure(parameters)) {
            return *failure;
        }
        message = RequestUpdateMessage{
            std::get<std::uint64_t>(request_id_result),
            std::get<Parameters>(parameters)};
    } else if (type == kPublishDoneMessageType) {
        const auto status_result = read_vi64(payload_cursor);
        if (const auto* need = std::get_if<NeedMore>(&status_result)) {
            return protocol_violation(need->offset,
                                      "PUBLISH_DONE status is truncated");
        }
        if (const auto* error = std::get_if<DecodeError>(&status_result)) {
            return *error;
        }
        const auto stream_count_result = read_vi64(payload_cursor);
        if (const auto* need = std::get_if<NeedMore>(&stream_count_result)) {
            return protocol_violation(need->offset,
                                      "PUBLISH_DONE stream count is truncated");
        }
        if (const auto* error =
                std::get_if<DecodeError>(&stream_count_result)) {
            return *error;
        }
        const auto reason = decode_reason_phrase(payload_cursor);
        if (const auto failure = bounded_message_failure(reason)) return *failure;
        message = PublishDoneMessage{
            std::get<std::uint64_t>(status_result),
            std::get<std::uint64_t>(stream_count_result),
            std::get<ReasonPhrase>(reason)};
    } else if (type == kFetchOkMessageType) {
        const auto end_of_track_result = read_bytes(payload_cursor, 1);
        if (const auto* need = std::get_if<NeedMore>(&end_of_track_result)) {
            return protocol_violation(need->offset,
                                      "FETCH_OK end-of-track is truncated");
        }
        if (const auto* error =
                std::get_if<DecodeError>(&end_of_track_result)) {
            return *error;
        }
        const auto end_location = decode_location(payload_cursor);
        if (const auto failure = bounded_message_failure(end_location)) {
            return *failure;
        }
        const auto parameters = decode_counted_parameters(
            payload_cursor, ParameterContext::FetchOk, limits);
        if (const auto failure = bounded_message_failure(parameters)) {
            return *failure;
        }
        const auto properties = decode_track_properties(
            payload_cursor, payload_cursor.remaining(), limits);
        if (const auto failure = bounded_message_failure(properties)) {
            return *failure;
        }
        message = FetchOkMessage{
            std::to_integer<std::uint8_t>(
                std::get<std::span<const std::byte>>(end_of_track_result)[0]),
            std::get<Location>(end_location), std::get<Parameters>(parameters),
            std::get<TrackProperties>(properties)};
    } else if (type == kNamespaceMessageType ||
               type == kNamespaceDoneMessageType) {
        const auto name_space = decode_track_namespace(payload_cursor, limits);
        if (const auto failure = bounded_message_failure(name_space)) {
            return *failure;
        }
        if (type == kNamespaceMessageType) {
            message = NamespaceMessage{std::get<TrackNamespace>(name_space)};
        } else {
            message = NamespaceDoneMessage{std::get<TrackNamespace>(name_space)};
        }
    } else if (type == kPublishBlockedMessageType) {
        const auto name_space = decode_track_namespace(payload_cursor, limits);
        if (const auto failure = bounded_message_failure(name_space)) {
            return *failure;
        }
        const auto decoded_namespace = std::get<TrackNamespace>(name_space);
        const auto name =
            decode_track_name(payload_cursor, decoded_namespace, limits);
        if (const auto failure = bounded_message_failure(name)) return *failure;
        message = PublishBlockedMessage{decoded_namespace,
                                        std::get<TrackName>(name)};
    } else if (type == kSubscribeMessageType ||
               type == kTrackStatusMessageType ||
               type == kPublishMessageType) {
        std::optional<std::uint64_t> alias;
        const auto context = type == kSubscribeMessageType
                                 ? ParameterContext::Subscribe
                             : type == kTrackStatusMessageType
                                 ? ParameterContext::TrackStatus
                                 : ParameterContext::Publish;
        const auto fields = decode_track_opening_fields(
            payload_cursor, context, limits, type == kPublishMessageType, alias);
        if (const auto failure = bounded_message_failure(fields)) return *failure;
        auto decoded = std::get<TrackOpeningFields>(fields);
        if (type == kSubscribeMessageType) {
            message = SubscribeMessage{
                decoded.request_id, std::move(decoded.track_namespace),
                std::move(decoded.track_name), std::move(decoded.parameters)};
        } else if (type == kTrackStatusMessageType) {
            message = TrackStatusMessage{
                decoded.request_id, std::move(decoded.track_namespace),
                std::move(decoded.track_name), std::move(decoded.parameters)};
        } else {
            const auto properties = decode_track_properties(
                payload_cursor, payload_cursor.remaining(), limits);
            if (const auto failure = bounded_message_failure(properties)) {
                return *failure;
            }
            message = PublishMessage{
                decoded.request_id, std::move(decoded.track_namespace),
                std::move(decoded.track_name), *alias,
                std::move(decoded.parameters),
                std::get<TrackProperties>(properties)};
        }
    } else if (type == kFetchMessageType) {
        const auto fetch = decode_fetch_payload(payload_cursor, limits);
        if (const auto failure = bounded_message_failure(fetch)) return *failure;
        message = std::get<FetchMessage>(fetch);
    } else {
        ParameterContext context = ParameterContext::PublishNamespace;
        if (type == kSubscribeNamespaceMessageType) {
            context = ParameterContext::SubscribeNamespace;
        } else if (type == kSubscribeTracksMessageType) {
            context = ParameterContext::SubscribeTracks;
        }
        const auto fields =
            decode_namespace_opening_fields(payload_cursor, context, limits);
        if (const auto failure = bounded_message_failure(fields)) return *failure;
        auto decoded = std::get<NamespaceOpeningFields>(fields);
        if (type == kPublishNamespaceMessageType) {
            message = PublishNamespaceMessage{
                decoded.request_id, std::move(decoded.track_namespace),
                std::move(decoded.parameters)};
        } else if (type == kSubscribeNamespaceMessageType) {
            message = SubscribeNamespaceMessage{
                decoded.request_id, std::move(decoded.track_namespace),
                std::move(decoded.parameters)};
        } else {
            message = SubscribeTracksMessage{
                decoded.request_id, std::move(decoded.track_namespace),
                std::move(decoded.parameters)};
        }
    }
    if (payload_cursor.remaining() != 0) {
        return protocol_violation(payload_cursor.offset(),
                                  "message has trailing payload bytes");
    }
    input = working;
    return message;
}

EncodeResult encode_message(const Message& message, ByteWriter& output) {
    ByteWriter payload(kMaximumMessagePayload);
    std::uint64_t type = 0;
    EncodeResult payload_result = EncodeResult::success();
    if (const auto* setup = std::get_if<SetupMessage>(&message)) {
        type = kSetupMessageType;
        if (const auto duplicate = validate_setup_options(setup->options, 0)) {
            return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                         duplicate->detail);
        }
        payload_result = encode_key_value_pairs_to(setup->options, payload);
    } else if (const auto* goaway = std::get_if<GoawayMessage>(&message)) {
        type = kGoawayMessageType;
        if (goaway->new_session_uri.size() > kMaximumNewSessionUri) {
            return EncodeResult::failure(EncodeErrorCode::PayloadTooLarge,
                                         "GOAWAY URI exceeds 8192 bytes");
        }
        if (!write_length_prefixed_bytes(goaway->new_session_uri, payload) ||
            !write_vi64(goaway->timeout, payload) ||
            (goaway->request_id.has_value() &&
             !write_vi64(*goaway->request_id, payload))) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "GOAWAY exceeds payload capacity");
        }
    } else if (const auto* subscribe_ok =
                   std::get_if<SubscribeOkMessage>(&message)) {
        type = kSubscribeOkMessageType;
        if (!write_vi64(subscribe_ok->track_alias, payload)) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "SUBSCRIBE_OK alias exceeds capacity");
        }
        payload_result = encode_counted_parameters(
            subscribe_ok->parameters, ParameterContext::SubscribeOk, payload);
        if (payload_result.has_value()) {
            payload_result = encode_key_value_pairs(
                subscribe_ok->track_properties.entries, payload);
        }
    } else if (const auto* request_ok =
                   std::get_if<RequestOkMessage>(&message)) {
        type = kRequestOkMessageType;
        payload_result = encode_counted_parameters(
            request_ok->parameters, ParameterContext::Unresolved, payload);
        if (payload_result.has_value()) {
            payload_result = encode_key_value_pairs(
                request_ok->track_properties.entries, payload);
        }
    } else if (const auto* request_error =
                   std::get_if<RequestErrorMessage>(&message)) {
        type = kRequestErrorMessageType;
        if ((request_error->error_code == 0x34) !=
            request_error->redirect.has_value()) {
            return EncodeResult::failure(
                EncodeErrorCode::InvalidValue,
                "REQUEST_ERROR redirect does not match error code");
        }
        if (!write_vi64(request_error->error_code, payload) ||
            !write_vi64(request_error->retry_interval, payload)) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "REQUEST_ERROR exceeds capacity");
        }
        payload_result =
            encode_reason_phrase(request_error->reason_phrase, payload);
        if (payload_result.has_value() && request_error->redirect) {
            payload_result = encode_redirect(*request_error->redirect, payload);
        }
    } else if (const auto* request_update =
                   std::get_if<RequestUpdateMessage>(&message)) {
        type = kRequestUpdateMessageType;
        if (!write_vi64(request_update->request_id, payload)) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "REQUEST_UPDATE ID exceeds capacity");
        }
        payload_result = encode_counted_parameters(
            request_update->parameters, ParameterContext::Unresolved, payload);
    } else if (const auto* publish_done =
                   std::get_if<PublishDoneMessage>(&message)) {
        type = kPublishDoneMessageType;
        if (!write_vi64(publish_done->status_code, payload) ||
            !write_vi64(publish_done->stream_count, payload)) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "PUBLISH_DONE exceeds capacity");
        }
        payload_result =
            encode_reason_phrase(publish_done->reason_phrase, payload);
    } else if (const auto* fetch_ok = std::get_if<FetchOkMessage>(&message)) {
        type = kFetchOkMessageType;
        if (!payload.append_byte(static_cast<std::byte>(fetch_ok->end_of_track))) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "FETCH_OK flag exceeds capacity");
        }
        payload_result = encode_location(fetch_ok->end_location, payload);
        if (payload_result.has_value()) {
            payload_result = encode_counted_parameters(
                fetch_ok->parameters, ParameterContext::FetchOk, payload);
        }
        if (payload_result.has_value()) {
            payload_result = encode_key_value_pairs(
                fetch_ok->track_properties.entries, payload);
        }
    } else if (const auto* name_space =
                   std::get_if<NamespaceMessage>(&message)) {
        type = kNamespaceMessageType;
        payload_result =
            encode_track_namespace(name_space->track_namespace_suffix, payload);
    } else if (const auto* name_space_done =
                   std::get_if<NamespaceDoneMessage>(&message)) {
        type = kNamespaceDoneMessageType;
        payload_result = encode_track_namespace(
            name_space_done->track_namespace_suffix, payload);
    } else if (const auto* blocked =
                   std::get_if<PublishBlockedMessage>(&message)) {
        type = kPublishBlockedMessageType;
        payload_result =
            encode_track_namespace(blocked->track_namespace_suffix, payload);
        if (payload_result.has_value()) {
            payload_result = encode_track_name(
                blocked->track_name, blocked->track_namespace_suffix, payload);
        }
    } else if (const auto* subscribe = std::get_if<SubscribeMessage>(&message)) {
        type = kSubscribeMessageType;
        payload_result = encode_track_opening_fields(
            subscribe->request_id, subscribe->track_namespace,
            subscribe->track_name, std::nullopt, subscribe->parameters,
            ParameterContext::Subscribe, payload);
    } else if (const auto* publish = std::get_if<PublishMessage>(&message)) {
        type = kPublishMessageType;
        payload_result = encode_track_opening_fields(
            publish->request_id, publish->track_namespace, publish->track_name,
            publish->track_alias, publish->parameters, ParameterContext::Publish,
            payload);
        if (payload_result.has_value()) {
            payload_result =
                encode_key_value_pairs(publish->track_properties.entries, payload);
        }
    } else if (const auto* fetch = std::get_if<FetchMessage>(&message)) {
        type = kFetchMessageType;
        if (!write_vi64(fetch->request_id, payload)) {
            return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                         "FETCH request ID exceeds capacity");
        }
        if (const auto* standalone = std::get_if<StandaloneFetch>(&fetch->fetch)) {
            if (standalone->end.group < standalone->start.group ||
                (standalone->end.group == standalone->start.group &&
                 standalone->end.object < standalone->start.object)) {
                return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                             "standalone FETCH range is reversed");
            }
            if (!write_vi64(1, payload)) {
                return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                             "FETCH type exceeds capacity");
            }
            payload_result = encode_track_namespace(standalone->track_namespace,
                                                    payload);
            if (payload_result.has_value()) {
                payload_result = encode_track_name(
                    standalone->track_name, standalone->track_namespace, payload);
            }
            if (payload_result.has_value()) {
                payload_result = encode_location(standalone->start, payload);
            }
            if (payload_result.has_value()) {
                payload_result = encode_location(standalone->end, payload);
            }
        } else {
            const bool relative =
                std::holds_alternative<RelativeJoiningFetch>(fetch->fetch);
            if (!write_vi64(relative ? 2u : 3u, payload)) {
                return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                             "FETCH type exceeds capacity");
            }
            const auto joining_request_id = relative
                ? std::get<RelativeJoiningFetch>(fetch->fetch).joining_request_id
                : std::get<AbsoluteJoiningFetch>(fetch->fetch).joining_request_id;
            const auto joining_start = relative
                ? std::get<RelativeJoiningFetch>(fetch->fetch).joining_start
                : std::get<AbsoluteJoiningFetch>(fetch->fetch).joining_start;
            if (!write_vi64(joining_request_id, payload) ||
                !write_vi64(joining_start, payload)) {
                return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                             "joining FETCH exceeds capacity");
            }
        }
        if (payload_result.has_value() &&
            !write_vi64(fetch->parameters.size(), payload)) {
            payload_result = EncodeResult::failure(
                EncodeErrorCode::OutputCapacity,
                "FETCH parameter count exceeds capacity");
        }
        if (payload_result.has_value()) {
            payload_result = encode_parameters(fetch->parameters,
                                               ParameterContext::Fetch, payload);
        }
    } else if (const auto* status = std::get_if<TrackStatusMessage>(&message)) {
        type = kTrackStatusMessageType;
        payload_result = encode_track_opening_fields(
            status->request_id, status->track_namespace, status->track_name,
            std::nullopt, status->parameters, ParameterContext::TrackStatus,
            payload);
    } else if (const auto* publish_namespace =
                   std::get_if<PublishNamespaceMessage>(&message)) {
        type = kPublishNamespaceMessageType;
        payload_result = encode_namespace_opening_fields(
            publish_namespace->request_id, publish_namespace->track_namespace,
            publish_namespace->parameters, ParameterContext::PublishNamespace,
            payload);
    } else if (const auto* subscribe_namespace =
                   std::get_if<SubscribeNamespaceMessage>(&message)) {
        type = kSubscribeNamespaceMessageType;
        payload_result = encode_namespace_opening_fields(
            subscribe_namespace->request_id,
            subscribe_namespace->track_namespace_prefix,
            subscribe_namespace->parameters,
            ParameterContext::SubscribeNamespace, payload);
    } else if (const auto* subscribe_tracks =
                   std::get_if<SubscribeTracksMessage>(&message)) {
        type = kSubscribeTracksMessageType;
        payload_result = encode_namespace_opening_fields(
            subscribe_tracks->request_id,
            subscribe_tracks->track_namespace_prefix,
            subscribe_tracks->parameters, ParameterContext::SubscribeTracks,
            payload);
    } else {
        return EncodeResult::failure(EncodeErrorCode::InvalidValue,
                                     "unknown typed message variant");
    }
    if (!payload_result.has_value()) return payload_result;
    if (payload.size() > kMaximumMessagePayload) {
        return EncodeResult::failure(EncodeErrorCode::PayloadTooLarge,
                                     "message payload exceeds uint16");
    }

    ByteWriter frame(kMaximumFrameSize);
    if (!write_vi64(type, frame)) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "message type exceeds frame capacity");
    }
    const std::array<std::byte, 2> length{
        static_cast<std::byte>((payload.size() >> 8u) & 0xffu),
        static_cast<std::byte>(payload.size() & 0xffu),
    };
    if (!frame.append_bytes(length) || !frame.append_bytes(payload.bytes())) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "message exceeds frame capacity");
    }
    if (!output.append_bytes(frame.bytes())) {
        return EncodeResult::failure(EncodeErrorCode::OutputCapacity,
                                     "message exceeds output capacity");
    }
    return EncodeResult::success();
}

}  // namespace moq::interop::wire::draft18
