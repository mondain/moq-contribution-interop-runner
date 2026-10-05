#include "moq/interop/scenarios/location_filter_param.h"

#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/draft22/location_filter.h"

#include <span>
#include <stdexcept>
#include <string>

namespace moq::interop::scenarios {
namespace {

constexpr std::size_t kCapacity = 64;  // at most 5 varints of at most 9 bytes

std::string describe(const FilterFields& fields) {
    std::string text = "{";
    for (std::size_t index = 0; index < fields.size(); ++index) {
        if (index != 0) text += ",";
        text += std::to_string(fields[index]);
    }
    return text + "}";
}

void put(wire::ByteWriter& writer, std::uint64_t value) {
    if (!wire::write_vi64(value, writer)) throw std::logic_error("LOCATION_FILTER varint capacity");
}

std::vector<std::byte> to_vector(const wire::ByteWriter& writer) {
    const auto bytes = writer.bytes();
    return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> draft21_form(const FilterFields& fields) {
    wire::ByteWriter payload(kCapacity);
    for (const auto field : fields) put(payload, field);
    wire::ByteWriter out(kCapacity);
    put(out, payload.size());
    if (!out.append_bytes(payload.bytes())) throw std::logic_error("LOCATION_FILTER capacity");
    return to_vector(out);
}

std::vector<std::byte> draft22_form(const FilterFields& fields) {
    using Type = wire::draft22::LocationFilterType;
    wire::draft22::LocationFilter filter;
    switch (fields.size()) {
        case 0: filter.type = Type::None; break;
        case 2:
            if (fields[0] == 0 && fields[1] == 0) {
                filter.type = Type::NextObject;
            } else {
                filter.type = Type::Absolute;
                filter.start_group = fields[0];
                filter.start_object = fields[1];
            }
            break;
        case 3:
            filter.type = Type::AbsoluteBounded;
            filter.start_group = fields[0];
            filter.start_object = fields[1];
            filter.end_group_delta = fields[2];
            break;
        case 4:
            filter.type = Type::AbsoluteRange;
            filter.start_group = fields[0];
            filter.start_object = fields[1];
            filter.end_group_delta = fields[2];
            filter.end_object = fields[3];
            break;
        default:
            throw std::logic_error("LOCATION_FILTER fields " + describe(fields) + " have no draft 22 form");
    }
    wire::ByteWriter out(kCapacity);
    if (wire::draft22::encode_location_filter(filter, out).has_value()) {
        throw std::logic_error("draft 22 refuses LOCATION_FILTER fields " + describe(fields));
    }
    return to_vector(out);
}

}  // namespace

std::vector<std::byte> filter_param_value(const FilterFields& fields) {
    return current_wire_draft() == 22 ? draft22_form(fields) : draft21_form(fields);
}

std::vector<std::byte> nested_filter_param_value(const FilterFields& fields) {
    return filter_param_value(fields);
}

}  // namespace moq::interop::scenarios
