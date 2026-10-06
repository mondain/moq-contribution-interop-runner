#include "moq/interop/scenarios/wire_draft.h"

#include "moq/interop/wire/draft22/publish.h"

#include <utility>
#include <variant>

namespace moq::interop::scenarios {
namespace {

thread_local unsigned t_wire_draft = 21;
// The first adapter refusal on this thread since the last take (or the enclosing ScopedWireDraft).
thread_local std::optional<std::string> t_adapter_refusal;

wire::DecodeError unrepresentable(std::size_t offset) {
    note_adapter_refusal(kUnrepresentableLocationFilterDetail);
    return {wire::DecodeErrorCode::ProtocolViolation, offset, std::string(kUnrepresentableLocationFilterDetail)};
}

// The varint fields of a draft 22 filter in draft 21's Length-delimited payload form.
bool filter_payload(const wire::draft22::LocationFilter& filter, std::vector<std::byte>& payload) {
    using Type = wire::draft22::LocationFilterType;
    wire::ByteWriter writer(64);
    // At most four varints of at most 9 bytes (36 bytes) into a 64-byte writer: the writes cannot fail.
    const auto put =[&](std::uint64_t value) { return wire::write_vi64(value, writer); };
    switch (filter.type) {
        case Type::None: break;
        case Type::RelativeGroup: put(filter.start_group); break;
        case Type::Absolute:
            if (filter.start_group == 0 && filter.start_object == 0) return false;
            put(filter.start_group);
            put(filter.start_object);
            break;
        case Type::AbsoluteBounded:
            put(filter.start_group);
            put(filter.start_object);
            put(filter.end_group_delta.value_or(0));
            break;
        case Type::AbsoluteRange:
            put(filter.start_group);
            put(filter.start_object);
            put(filter.end_group_delta.value_or(0));
            put(filter.end_object.value_or(0));
            break;
        case Type::NextObject:
            put(0);
            put(0);
            break;
    }
    const auto bytes = writer.bytes();
    payload.assign(bytes.begin(), bytes.end());
    return true;
}

}  // namespace

unsigned current_wire_draft() noexcept { return t_wire_draft; }

// Moving an optional<string> and assigning nullopt do not throw, so both stay noexcept.
ScopedWireDraft::ScopedWireDraft(unsigned draft) noexcept
    : previous_(t_wire_draft), previous_refusal_(std::exchange(t_adapter_refusal, std::nullopt)) {
    t_wire_draft = draft;
}

ScopedWireDraft::~ScopedWireDraft() {
    t_wire_draft = previous_;
    // The enclosing scope's refusal came first; otherwise this scope's refusal (if any) stays recorded.
    if (previous_refusal_) t_adapter_refusal = std::move(previous_refusal_);
}

void note_adapter_refusal(std::string_view detail) {
    if (!t_adapter_refusal) t_adapter_refusal = std::string(detail);
}

std::optional<std::string> take_adapter_refusal() { return std::exchange(t_adapter_refusal, std::nullopt); }

wire::DecodeResult<wire::draft21::PublishMessage> decode_publish_for_wire(wire::Cursor& input) {
    if (t_wire_draft != 22) return wire::draft21::decode_publish(input);
    wire::Cursor working = input;
    const auto decoded = wire::draft22::decode_publish(working);
    if (const auto* need = std::get_if<wire::NeedMore>(&decoded)) return *need;
    if (const auto* error = std::get_if<wire::DecodeError>(&decoded)) return *error;
    const auto& source = std::get<wire::draft22::PublishMessage>(decoded);
    wire::draft21::PublishMessage result{source.request_id, source.track_namespace, source.track_name,
                                         source.track_alias, {}, source.track_properties};
    for (const auto& parameter : source.parameters) {
        wire::draft21::PublishParameter converted{parameter.type, std::uint8_t{0}};
        if (const auto* filter = std::get_if<wire::draft22::LocationFilter>(&parameter.value)) {
            std::vector<std::byte> payload;
            if (!filter_payload(*filter, payload)) return unrepresentable(input.offset());
            converted.value = std::move(payload);
        } else if (const auto* octet = std::get_if<std::uint8_t>(&parameter.value)) {
            converted.value = *octet;
        } else if (const auto* integer = std::get_if<std::uint64_t>(&parameter.value)) {
            converted.value = *integer;
        } else if (const auto* location = std::get_if<wire::draft21::Location>(&parameter.value)) {
            converted.value = *location;
        } else {
            converted.value = std::get<wire::draft21::Token>(parameter.value);
        }
        result.parameters.push_back(std::move(converted));
    }
    input = working;
    return result;
}

}  // namespace moq::interop::scenarios
