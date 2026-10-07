#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/scenarios/draft22_run_names.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft21/setup.h"

#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

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

Bytes named_subscribe(const Bytes& names, const Bytes& parameters, unsigned count) {
    // Request ID 1, the track's names (zero namespace fields and name x on wire 21), parameter count.
    auto body = bytes({1});
    body.insert(body.end(), names.begin(), names.end());
    body.push_back(static_cast<std::byte>(count));
    body.insert(body.end(), parameters.begin(), parameters.end());
    if (body.size() > 65535) throw std::invalid_argument("track names do not fit a probe request");
    return frame(3, body);
}

// The profiles whose SUBSCRIBE names a track only because the message needs one: (), "x" on wire 21, the
// run's track on wire 22 (draft21_request_profiles' request names).
bool run_names_profile(std::string_view id) {
    static const std::set<std::string_view> ids{
        "d21-range-filter-start-delta-overflow", "d21-range-filter-end-delta-overflow",
        "d21-duplicate-range-filter-key-in-request", "d21-priority-filter-start-above-255",
        "d21-priority-filter-end-above-255", "d21-object-property-filter-odd-property-type",
        "d21-request-unknown-token-alias"};
    return ids.contains(id);
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
    std::chrono::milliseconds deadline, std::vector<Bytes> request_namespace, Bytes request_name) {
    // Draft 22 runs share these profiles. A publisher may refuse an empty namespace before it reads the filter
    // or token a profile is about, so on the draft 22 wire their SUBSCRIBE names the run's track (draft 21 is
    // frozen: its bytes stay). Names too large to send throw std::invalid_argument.
    const auto names = encode_probe_track_names(
        probe_track_names(std::move(request_namespace), std::move(request_name)));
    const auto subscribe = [&names](const Bytes& parameters, unsigned count = 1) {
        return named_subscribe(names, parameters, count);
    };
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

std::optional<bool> evaluate_draft21_request_profile(
    const RawProbeTranscript& transcript, const RequestProbeProfile& profile) {
    if (current_wire_draft() != 22 || !run_names_profile(profile.definition.id))
        return evaluate_raw_probe_request_error(transcript, profile);
    // On the draft 22 wire the SUBSCRIBE named the run's track: rebuild the definition for the names the
    // delivered request carries (only an exact rebuild is accepted) and prove the stimulus against it.
    if (transcript.writes.empty()) return std::nullopt;
    const auto& first = transcript.writes.front().write.bytes;
    const auto names = recover_probe_track_names(first);
    if (!names) return std::nullopt;
    std::vector<RequestProbeProfile> rebuilt;
    try {
        rebuilt = draft21_request_profiles(profile.definition.deadline, names->track_namespace, names->track_name);
    } catch (const std::invalid_argument&) {
        return std::nullopt;
    }
    const auto found = std::find_if(rebuilt.begin(), rebuilt.end(), [&](const auto& candidate) {
        return candidate.definition.id == profile.definition.id &&
            candidate.requirement_id == profile.requirement_id && candidate.evaluator_id == profile.evaluator_id;
    });
    if (found == rebuilt.end() || found->definition.writes.empty() ||
        found->definition.writes.front().bytes != first) return std::nullopt;
    return evaluate_raw_probe_request_error(transcript, *found);
}

}  // namespace moq::interop::scenarios
