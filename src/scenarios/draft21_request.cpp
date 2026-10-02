#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/wire/draft21/setup.h"

#include <limits>
#include <stdexcept>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;

Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}

void integer(Bytes& output, std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("vi64 capacity");
    output.insert(output.end(), writer.bytes().begin(), writer.bytes().end());
}

Bytes frame(unsigned type, const Bytes& body) {
    if (body.size() > 65535) throw std::logic_error("probe frame exceeds uint16");
    auto result = bytes({type, static_cast<unsigned>(body.size() >> 8u),
                         static_cast<unsigned>(body.size() & 255u)});
    result.insert(result.end(), body.begin(), body.end());
    return result;
}

Bytes subscribe(const Bytes& parameters, unsigned count = 1) {
    auto body = bytes({1, 0, 1, 'x', count});
    body.insert(body.end(), parameters.begin(), parameters.end());
    return frame(3, body);
}

bool setup_ready(std::span<const std::byte> input, unsigned minimum_ranges) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft21::decode_setup(cursor);
    const auto* setup = std::get_if<wire::draft21::SetupMessage>(&decoded);
    if (!setup) return false;
    if (minimum_ranges == 0) return true;
    for (const auto& option : setup->options) {
        if (option.type == 6) {
            const auto* capacity = std::get_if<std::uint64_t>(&option.value);
            return capacity && *capacity >= minimum_ranges;
        }
    }
    return false;
}
}  // namespace

std::vector<RequestProbeProfile> draft21_request_profiles(
    std::chrono::milliseconds deadline) {
    std::vector<RequestProbeProfile> result;
    const auto add = [&](const char* requirement, const char* scenario,
                         const char* evaluator, std::uint64_t error,
                         Bytes payload, bool namespace_scoped = false,
                         unsigned minimum_ranges = 0) {
        RawProbeDefinition definition{scenario, bytes({0xaf, 0, 0, 0}),
            {{RawProbeChannel::NewBidi, std::move(payload), false}}, true,
            [minimum_ranges](std::span<const std::byte> input) {
                return setup_ready(input, minimum_ranges);
            }, deadline, request_probe_response_ready};
        result.push_back({21, requirement, evaluator, error,
                          namespace_scoped, std::move(definition)});
    };

    // Sections 2.4.2 and 6.5: these reserved requests bypass the Application.
    add("D21-2-4-2-MUST-031", "d21-request-single-period-namespace",
        "d21-single-period-request-does-not-exist", 0x10,
        frame(3, bytes({1, 1, 1, '.', 1, 'x', 0})));
    add("D21-6-5-MUST-170", "d21-session-namespace-empty-track-request",
        "d21-session-empty-track-does-not-exist", 0x10,
        frame(3, bytes({1, 1, 8, '.', 's', 'e', 's', 's', 'i', 'o', 'n', 0, 0})));
    add("D21-6-5-MUST-171", "d21-session-namespace-unknown-track-request",
        "d21-session-unknown-track-does-not-exist", 0x10,
        frame(3, bytes({1, 1, 8, '.', 's', 'e', 's', 's', 'i', 'o', 'n', 1, 'x', 0})));
    add("D21-6-5-MUST-172", "d21-session-namespace-unknown-namespace-request",
        "d21-session-unknown-namespace-does-not-exist", 0x10,
        frame(0x50, bytes({1, 2, 8, '.', 's', 'e', 's', 's', 'i', 'o', 'n',
                           1, 'x', 0})), true);

    // Section 8.6: Range Start adds to the previous End, and Range End
    // adds to its Start. OBJECTID_FILTER has no narrower value bound.
    auto start_overflow = bytes({0x26, 12, 0});
    integer(start_overflow, std::numeric_limits<std::uint64_t>::max());
    start_overflow.insert(start_overflow.end(), {std::byte{0}, std::byte{1}});
    add("D21-8-6-MUST-249", "d21-range-filter-start-delta-overflow",
        "d21-range-delta-overflow-invalid-filter", 0x36,
        subscribe(start_overflow), false, 2);
    auto end_overflow = bytes({0x26, 11, 0});
    integer(end_overflow, std::numeric_limits<std::uint64_t>::max());
    end_overflow.push_back(std::byte{1});
    add("D21-8-6-MUST-249", "d21-range-filter-end-delta-overflow",
        "d21-range-delta-overflow-invalid-filter", 0x36,
        subscribe(end_overflow), false, 1);
    // Section 3.3.2: repeated Type is legal; repeated (Type, SetID) is not.
    add("D21-3-3-2-MUST-064", "d21-duplicate-range-filter-key-in-request",
        "d21-duplicate-range-filter-invalid-filter", 0x36,
        subscribe(bytes({0x26, 2, 0, 0, 0, 2, 0, 0}), 2), false, 2);
    // Sections 9.20.13-15: complete filters with invalid field values.
    add("D21-9-20-13-MUST-433", "d21-priority-filter-start-above-255",
        "d21-priority-filter-invalid-filter", 0x36,
        subscribe(bytes({0x27, 3, 0, 0x81, 0})), false, 1);
    add("D21-9-20-13-MUST-433", "d21-priority-filter-end-above-255",
        "d21-priority-filter-invalid-filter", 0x36,
        subscribe(bytes({0x27, 4, 0, 0x80, 0xff, 1})), false, 1);
    add("D21-9-20-14-MUST-435", "d21-object-property-filter-odd-property-type",
        "d21-object-property-filter-invalid-filter", 0x36,
        subscribe(bytes({0x28, 3, 0, 1, 0})), false, 1);
    add("D21-9-20-15-MUST-437", "d21-track-property-filter-odd-property-type",
        "d21-track-property-filter-invalid-filter", 0x36,
        frame(0x51, bytes({1, 0, 1, 0x29, 3, 0, 1, 0})), true, 1);

    // Section 8.9: the server registers no Alias in SETUP or earlier requests.
    add("D21-8-9-MUST-269", "d21-request-unknown-token-alias",
        "d21-unknown-token-alias-message-error", 0x17,
        subscribe(bytes({3, 2, 2, 0})));
    for (auto& entry : result) {
        if (entry.requirement_id == "D21-8-9-MUST-269")
            entry.compatibility_error = true;
    }
    return result;
}

}  // namespace moq::interop::scenarios
