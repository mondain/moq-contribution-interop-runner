// Cross-check of our draft 22 wire codecs against an independent vector set: moq-playa's
// packages/transport/vectors/d22/wire-vectors.txt (itself copied from red5-moq-relay, with
// bytes produced by or verified against moqxr), copied with its origin and licence (Apache
// 2.0) to tests/golden/data/draft22-playa-vectors.txt.
//
// Every vector in the file is either COVERED (decoded through our wire::draft22 codec or the
// draft 21 codec it aliases, its fields checked against what the vector name states, and
// re-encoded to the same bytes where we have an encoder) or ALLOWLISTED with a reason (we have
// no codec for that message or parameter value; the framing we do have is still asserted).
// A vector that is neither fails EveryVectorIsCoveredOrAllowlisted, so a new vector cannot be
// skipped silently. The file has no negative (must-reject) vectors.
//
// Agreement on all 29 vectors is a PARTIAL independent check, not proof that our draft 22 codecs
// are right: the moqxr-sourced vectors (moqxr-encoder, moqxr-decoder-verified) derive from moqxr's
// draft 21 codec, the draft-text vectors were built from the draft 21 text, and draft 22 changed
// only LOCATION_FILTER (Section 9.20.9). Only the converted-from-d21 vectors (the three Next Object
// forms) were re-encoded for draft 22, with moq-playa's own codec.
#include "moq/interop/scenarios/parameter_walk.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/draft22/location_filter.h"
#include "moq/interop/wire/draft22/publish.h"
#include "moq/interop/wire/draft22/publisher_request.h"
#include "moq/interop/wire/draft22/shared.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft22 {
namespace {

using scenarios::ParameterValueKind;
using scenarios::ParameterWalkStatus;
using scenarios::ScopedWireDraft;
using scenarios::WalkedParameter;
using scenarios::walk_message_parameters;
using scenarios::walk_nested_parameters;

const std::filesystem::path kVectorFile = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR) /
                                          "tests/golden/data/draft22-playa-vectors.txt";

struct Vector {
    std::string name;
    std::string kind;    // control, parameter, stream
    std::string source;  // moqxr-encoder, moqxr-decoder-verified, draft-text, converted-from-d21
    std::vector<std::byte> bytes;
};

std::optional<std::vector<std::byte>> from_hex(const std::string& hex) {
    if (hex.size() % 2 != 0) return std::nullopt;
    std::vector<std::byte> result;
    for (std::size_t index = 0; index < hex.size(); index += 2) {
        unsigned value = 0;
        std::istringstream digits(hex.substr(index, 2));
        if (!(digits >> std::hex >> value)) return std::nullopt;
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

// Format: "<name> <kind> <source> <hex>" per line; '#' starts a comment line.
std::vector<Vector> load_vectors() {
    std::ifstream file(kVectorFile);
    std::vector<Vector> result;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream fields(line);
        Vector vector;
        std::string hex;
        std::string extra;
        if (!(fields >> vector.name >> vector.kind >> vector.source >> hex) || (fields >> extra)) {
            ADD_FAILURE() << "malformed vector line: " << line;
            continue;
        }
        const auto bytes = from_hex(hex);
        if (!bytes) {
            ADD_FAILURE() << "bad hex in vector " << vector.name;
            continue;
        }
        vector.bytes = *bytes;
        result.push_back(std::move(vector));
    }
    return result;
}

const std::vector<Vector>& vectors() {
    static const auto loaded = load_vectors();
    return loaded;
}

std::vector<std::byte> text(const std::string& value) {
    std::vector<std::byte> result;
    for (const auto character : value) result.push_back(static_cast<std::byte>(character));
    return result;
}

std::uint64_t vi(Cursor& cursor) {
    const auto decoded = read_vi64(cursor);
    EXPECT_TRUE(std::holds_alternative<std::uint64_t>(decoded));
    return std::holds_alternative<std::uint64_t>(decoded) ? std::get<std::uint64_t>(decoded) : 0;
}

std::vector<std::byte> length_prefixed(Cursor& cursor) {
    const auto decoded = read_length_prefixed_bytes(cursor, 4096);
    EXPECT_TRUE((std::holds_alternative<std::span<const std::byte>>(decoded)));
    if (!std::holds_alternative<std::span<const std::byte>>(decoded)) return {};
    const auto bytes = std::get<std::span<const std::byte>>(decoded);
    return {bytes.begin(), bytes.end()};
}

std::vector<std::vector<std::byte>> tuple(Cursor& cursor) {
    std::vector<std::vector<std::byte>> result;
    const auto count = vi(cursor);
    for (std::uint64_t index = 0; index < count && index < 32; ++index) {
        result.push_back(length_prefixed(cursor));
    }
    return result;
}

// Draft 22 section 9 framing {Type, Length (16), Body}, through our draft 21-family request
// framing (Table 5 is unchanged, pinned by draft22_wire_audit_test). Asserts the type and that
// the frame is exactly the vector.
RequestFrame framed(const Vector& vector, MessageKind kind) {
    Cursor probe(vector.bytes);
    const auto raw_type = read_vi64(probe);
    EXPECT_TRUE(std::holds_alternative<std::uint64_t>(raw_type));
    const auto info = std::holds_alternative<std::uint64_t>(raw_type)
                          ? classify_message_type(std::get<std::uint64_t>(raw_type))
                          : std::nullopt;
    EXPECT_TRUE(info.has_value());
    Cursor input(vector.bytes);
    const auto frame = decode_request_frame(input, info && info->request_starter);
    EXPECT_TRUE(std::holds_alternative<RequestFrame>(frame)) << vector.name;
    if (!std::holds_alternative<RequestFrame>(frame)) return {};
    EXPECT_EQ(std::get<RequestFrame>(frame).type.kind, kind);
    EXPECT_EQ(input.offset(), vector.bytes.size());
    return std::get<RequestFrame>(frame);
}

// Walks `count` parameters with our draft 22 parameter reader and returns them; asserts the
// walk completes.
std::vector<WalkedParameter> walk(Cursor& body, std::uint64_t count) {
    ScopedWireDraft draft(22);
    std::vector<WalkedParameter> result;
    const auto status = walk_message_parameters(
        body, count, [&](const WalkedParameter& parameter) { result.push_back(parameter); });
    EXPECT_EQ(status.status, ParameterWalkStatus::Complete);
    EXPECT_EQ(status.visited, count);
    return result;
}

std::vector<std::uint64_t> types(const std::vector<WalkedParameter>& parameters) {
    std::vector<std::uint64_t> result;
    for (const auto& parameter : parameters) result.push_back(parameter.type);
    return result;
}

// The LOCATION_FILTER value (Type + fields, section 9.20.9) through decode_location_filter: it
// consumes exactly the value, matches `expected`, and re-encodes to the same bytes.
void check_location_filter(std::span<const std::byte> value, const LocationFilter& expected) {
    Cursor input(value);
    const auto decoded = decode_location_filter(input);
    ASSERT_TRUE(std::holds_alternative<LocationFilter>(decoded));
    const auto& filter = std::get<LocationFilter>(decoded);
    EXPECT_EQ(input.offset(), value.size());
    EXPECT_EQ(filter.type, expected.type);
    EXPECT_EQ(filter.start_group, expected.start_group);
    EXPECT_EQ(filter.start_object, expected.start_object);
    EXPECT_EQ(filter.end_group_delta, expected.end_group_delta);
    EXPECT_EQ(filter.end_object, expected.end_object);
    ByteWriter output(64);
    ASSERT_FALSE(encode_location_filter(filter, output).has_value());
    EXPECT_EQ(std::vector<std::byte>(output.bytes().begin(), output.bytes().end()),
              std::vector<std::byte>(value.begin(), value.end()));
}

struct TrackRequest {
    // Owns the body the walked parameters' spans point into (moving a vector keeps its buffer).
    RequestFrame frame;
    std::uint64_t request_id;
    std::vector<std::vector<std::byte>> track_namespace;
    std::optional<std::vector<std::byte>> track_name;
    std::vector<WalkedParameter> parameters;
};

// SUBSCRIBE (9.6) and FETCH (9.11): Request ID, Track Namespace, Track Name, Parameters.
// SUBSCRIBE_TRACKS (9.18): Request ID, Track Namespace Prefix, Parameters.
TrackRequest track_request(RequestFrame frame, bool has_track_name) {
    TrackRequest result{};
    result.frame = std::move(frame);
    Cursor body(result.frame.body);
    result.request_id = vi(body);
    result.track_namespace = tuple(body);
    if (has_track_name) result.track_name = length_prefixed(body);
    const auto count = vi(body);
    result.parameters = walk(body, count);
    EXPECT_EQ(body.remaining(), 0u) << "bytes after the parameters";
    return result;
}

const std::vector<std::vector<std::byte>> kLive{text("live")};

// A SUBSCRIBE whose only parameter is LOCATION_FILTER `expected` on live/video, Request ID 9.
std::function<void(const Vector&)> subscribe_with_filter(LocationFilter expected) {
    return [expected](const Vector& vector) {
        const auto request = track_request(framed(vector, MessageKind::Subscribe), true);
        EXPECT_EQ(request.request_id, 9u);
        EXPECT_EQ(request.track_namespace, kLive);
        EXPECT_EQ(request.track_name, text("video"));
        ASSERT_EQ(types(request.parameters), (std::vector<std::uint64_t>{0x21}));
        EXPECT_EQ(request.parameters[0].kind, ParameterValueKind::LocationFilter);
        check_location_filter(request.parameters[0].value, expected);
    };
}

LocationFilter filter(LocationFilterType type, std::uint64_t start_group = 0,
                      std::uint64_t start_object = 0,
                      std::optional<std::uint64_t> end_group_delta = std::nullopt,
                      std::optional<std::uint64_t> end_object = std::nullopt) {
    return {type, start_group, start_object, end_group_delta, end_object};
}

void check_setup(const Vector& vector, const std::vector<SetupOption>& expected) {
    Cursor input(vector.bytes);
    const auto decoded = decode_control_message(input, true, true);
    ASSERT_TRUE(std::holds_alternative<ControlMessage>(decoded));
    ASSERT_TRUE(std::holds_alternative<SetupMessage>(std::get<ControlMessage>(decoded)));
    EXPECT_EQ(input.offset(), vector.bytes.size());
    const auto& setup = std::get<SetupMessage>(std::get<ControlMessage>(decoded));
    ASSERT_EQ(setup.options.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        EXPECT_EQ(setup.options[index].type, expected[index].type);
        EXPECT_EQ(setup.options[index].value, expected[index].value);
    }
    ByteWriter output(1024);
    ASSERT_FALSE(encode_setup(setup, output).has_value());
    EXPECT_EQ(std::vector<std::byte>(output.bytes().begin(), output.bytes().end()), vector.bytes);
}

SuccessfulResponse response(const Vector& vector, ResponseContext context) {
    Cursor input(vector.bytes);
    const auto decoded = decode_successful_response(input, context);
    EXPECT_TRUE(std::holds_alternative<SuccessfulResponse>(decoded)) << vector.name;
    if (!std::holds_alternative<SuccessfulResponse>(decoded)) return {};
    EXPECT_EQ(input.offset(), vector.bytes.size());
    return std::get<SuccessfulResponse>(decoded);
}

void expect_location_parameter(const ResponseParameter& parameter, std::uint64_t group,
                               std::uint64_t object) {
    EXPECT_EQ(parameter.type, 0x09u);
    ASSERT_TRUE(std::holds_alternative<Location>(parameter.value));
    EXPECT_EQ(std::get<Location>(parameter.value).group, group);
    EXPECT_EQ(std::get<Location>(parameter.value).object, object);
}

FetchPushResult fetch_stream(const std::vector<std::byte>& bytes) {
    FetchDecoder decoder([](std::uint64_t) { return FetchGroupOrder::Ascending; });
    return decoder.push(bytes, false);
}

// Vectors decoded through our codecs, with the fields their names state.
const std::map<std::string, std::function<void(const Vector&)>>& covered() {
    static const std::map<std::string, std::function<void(const Vector&)>> handlers{
        // Section 9.1, Table 11: PATH (0x01) "/moq", AUTHORITY (0x05) "relay.example.com".
        {"client_setup", [](const Vector& v) {
             check_setup(v, {{0x01, text("/moq")}, {0x05, text("relay.example.com")}});
         }},
        // MAX_FILTER_RANGES (0x06) 16, MAX_REQUEST_UPDATES (0x08) 4.
        {"server_setup_filter_ranges_16_request_updates_4", [](const Vector& v) {
             check_setup(v, {{0x06, std::uint64_t{16}}, {0x08, std::uint64_t{4}}});
         }},
        // Section 9.8: Request ID 2, live/video, Track Alias 1, no parameters or properties.
        // Decode only: we have no PUBLISH encoder.
        {"publish", [](const Vector& v) {
             Cursor input(v.bytes);
             const auto decoded = decode_publisher_request_message(input, true);
             ASSERT_TRUE(std::holds_alternative<PublisherRequestMessage>(decoded));
             const auto& message = std::get<PublisherRequestMessage>(decoded);
             ASSERT_TRUE(std::holds_alternative<PublishMessage>(message));
             const auto& publish = std::get<PublishMessage>(message);
             EXPECT_EQ(input.offset(), v.bytes.size());
             EXPECT_EQ(publish.request_id, 2u);
             EXPECT_EQ(publish.track_namespace, kLive);
             EXPECT_EQ(publish.track_name, text("video"));
             EXPECT_EQ(publish.track_alias, 1u);
             EXPECT_TRUE(publish.parameters.empty());
             EXPECT_TRUE(publish.track_properties.empty());
         }},
        // Section 9.7: Track Alias 1, no parameters.
        {"subscribe_ok_no_largest", [](const Vector& v) {
             const auto ok = response(v, ResponseContext::Subscribe);
             EXPECT_EQ(ok.track_alias, 1u);
             EXPECT_TRUE(ok.parameters.empty());
             EXPECT_TRUE(ok.track_properties.empty());
         }},
        {"subscribe_ok_largest_3_7", [](const Vector& v) {
             const auto ok = response(v, ResponseContext::Subscribe);
             EXPECT_EQ(ok.track_alias, 1u);
             ASSERT_EQ(ok.parameters.size(), 1u);
             expect_location_parameter(ok.parameters[0], 3, 7);
         }},
        // Section 9.3: REQUEST_UPDATE_OK carries LARGEST_OBJECT.
        {"request_update_ok_largest_3_7", [](const Vector& v) {
             const auto ok = response(v, ResponseContext::RequestUpdate);
             ASSERT_EQ(ok.parameters.size(), 1u);
             expect_location_parameter(ok.parameters[0], 3, 7);
         }},
        // Section 9.9: Status Code 2, Stream Count 2^64-1 (9-byte vi64), reason "done".
        {"publish_done_track_ended_unknown_count", [](const Vector& v) {
             Cursor input(v.bytes);
             const auto decoded = decode_publish_done(input);
             ASSERT_TRUE(std::holds_alternative<PublishDoneMessage>(decoded));
             EXPECT_EQ(input.offset(), v.bytes.size());
             const auto& done = std::get<PublishDoneMessage>(decoded);
             EXPECT_EQ(done.status_code, 2u);
             EXPECT_EQ(done.stream_count, std::numeric_limits<std::uint64_t>::max());
             EXPECT_EQ(done.reason, text("done"));
         }},
        // Section 11.4.1: FETCH_HEADER type 0x05, Request ID 95.
        {"fetch_header_fill_req_95", [](const Vector& v) {
             const auto result = fetch_stream(v.bytes);
             EXPECT_FALSE(result.error.has_value());
             ASSERT_TRUE(result.header.has_value());
             EXPECT_EQ(result.header->raw_type, 0x05u);
             EXPECT_EQ(result.header->request_id, 95u);
             EXPECT_EQ(result.header->stream_end_offset, v.bytes.size());
             EXPECT_TRUE(result.events.empty());
         }},
        // Section 11.3: SUBGROUP_HEADER type 0x30, Track Alias 1, Group 3, Subgroup 0.
        {"subgroup_header_alias1_g3_sg0", [](const Vector& v) {
             SubgroupDecoder decoder;
             const auto result = decoder.push(v.bytes, false);
             EXPECT_FALSE(result.error.has_value());
             ASSERT_TRUE(result.header.has_value());
             EXPECT_EQ(result.header->raw_type, 0x30u);
             EXPECT_EQ(result.header->track_alias, 1u);
             EXPECT_EQ(result.header->group_id, 3u);
             EXPECT_EQ(result.header->subgroup_id.value_or(0), 0u);
             EXPECT_EQ(result.header->stream_end_offset, v.bytes.size());
             EXPECT_TRUE(result.objects.empty());
         }},
        // Section 9.20.9: one SUBSCRIBE per Location Filter Type.
        {"subscribe_location_filter_empty",
         subscribe_with_filter(filter(LocationFilterType::None))},
        {"subscribe_location_filter_next_group",
         subscribe_with_filter(filter(LocationFilterType::RelativeGroup, 0))},
        {"subscribe_location_filter_relative_3",
         subscribe_with_filter(filter(LocationFilterType::RelativeGroup, 3))},
        {"subscribe_location_filter_next_object",
         subscribe_with_filter(filter(LocationFilterType::NextObject))},
        {"subscribe_location_filter_start_12_5",
         subscribe_with_filter(filter(LocationFilterType::Absolute, 12, 5))},
        {"subscribe_location_filter_range_12_5_delta_3",
         subscribe_with_filter(filter(LocationFilterType::AbsoluteBounded, 12, 5, 3))},
        {"subscribe_location_filter_range_12_5_delta_3_end_object_7",
         subscribe_with_filter(filter(LocationFilterType::AbsoluteRange, 12, 5, 3, 7))},
        // LOCATION_FILTER Next Object, FILL_PARAMETERS holding LOCATION_FILTER {1, 1} with no
        // count (section 9.20.15), INCLUDE_PROPERTIES 0.
        {"subscribe_join_current_group_with_fill_include_properties_0", [](const Vector& v) {
             const auto request = track_request(framed(v, MessageKind::Subscribe), true);
             EXPECT_EQ(request.request_id, 9u);
             EXPECT_EQ(request.track_namespace, kLive);
             EXPECT_EQ(request.track_name, text("video"));
             ASSERT_EQ(types(request.parameters), (std::vector<std::uint64_t>{0x21, 0x23, 0x35}));
             check_location_filter(request.parameters[0].value,
                                   filter(LocationFilterType::NextObject));
             EXPECT_EQ(request.parameters[1].nested, ParameterWalkStatus::Complete);
             std::vector<WalkedParameter> nested;
             {
                 ScopedWireDraft draft(22);
                 const auto status = walk_nested_parameters(
                     request.parameters[1].payload,
                     [&](const WalkedParameter& parameter) { nested.push_back(parameter); });
                 EXPECT_EQ(status.status, ParameterWalkStatus::Complete);
             }
             ASSERT_EQ(types(nested), (std::vector<std::uint64_t>{0x21}));
             check_location_filter(nested[0].value, filter(LocationFilterType::RelativeGroup, 1));
             EXPECT_EQ(request.parameters[2].number, 0u);
         }},
        // Section 9.18: Request ID 0x5d, prefix live, LOCATION_FILTER Next Object,
        // GROUP_ORDER 2 (descending).
        {"subscribe_tracks_next_object_group_order_descending", [](const Vector& v) {
             const auto request = track_request(framed(v, MessageKind::SubscribeTracks), false);
             EXPECT_EQ(request.request_id, 0x5du);
             EXPECT_EQ(request.track_namespace, kLive);
             ASSERT_EQ(types(request.parameters), (std::vector<std::uint64_t>{0x21, 0x22}));
             check_location_filter(request.parameters[0].value,
                                   filter(LocationFilterType::NextObject));
             EXPECT_EQ(request.parameters[1].number, 2u);
         }},
        // Section 9.11: the FETCH range is a LOCATION_FILTER type 4 {12, 5, 3, 9}: groups 12..15,
        // End Object 9.
        {"fetch_range_12_5_to_15_9", [](const Vector& v) {
             const auto request = track_request(framed(v, MessageKind::Fetch), true);
             EXPECT_EQ(request.request_id, 7u);
             EXPECT_EQ(request.track_namespace, kLive);
             EXPECT_EQ(request.track_name, text("video"));
             ASSERT_EQ(types(request.parameters), (std::vector<std::uint64_t>{0x21}));
             check_location_filter(request.parameters[0].value,
                                   filter(LocationFilterType::AbsoluteRange, 12, 5, 3, 9));
         }},
        // Section 9.12: End Of Track 0, End Location {15, 9}, no parameters.
        {"fetch_ok_inclusive_end_15_9", [](const Vector& v) {
             const auto ok = response(v, ResponseContext::Fetch);
             EXPECT_EQ(ok.end_of_track, std::uint8_t{0});
             ASSERT_TRUE(ok.end_location.has_value());
             EXPECT_EQ(ok.end_location->group, 15u);
             EXPECT_EQ(ok.end_location->object, 9u);
             EXPECT_TRUE(ok.parameters.empty());
             EXPECT_TRUE(ok.track_properties.empty());
         }},
        // Section 9.2: no Request ID, empty New Session URI, Timeout 5000 (2-byte vi64).
        {"goaway_no_uri_timeout_5000", [](const Vector& v) {
             Cursor input(v.bytes);
             const auto decoded = decode_control_message(input, false, false);
             ASSERT_TRUE(std::holds_alternative<ControlMessage>(decoded));
             ASSERT_TRUE(std::holds_alternative<GoawayMessage>(std::get<ControlMessage>(decoded)));
             EXPECT_EQ(input.offset(), v.bytes.size());
             const auto& goaway = std::get<GoawayMessage>(std::get<ControlMessage>(decoded));
             EXPECT_TRUE(goaway.new_session_uri.empty());
             EXPECT_EQ(goaway.timeout_ms, 5000u);
             ByteWriter output(64);
             ASSERT_FALSE(encode_goaway(goaway, false, output).has_value());
             EXPECT_EQ(std::vector<std::byte>(output.bytes().begin(), output.bytes().end()),
                       v.bytes);
         }},
        // Section 11.4.1.2: End of Timed-Out Range (Serialization Flags 0x20C), Group 2,
        // Object 4. The vector is the object alone; our decoder needs the stream's
        // FETCH_HEADER first, so a header for Request ID 0 is prepended.
        {"fetch_object_end_of_timed_out_range_g2_o4", [](const Vector& v) {
             std::vector<std::byte> stream{std::byte{0x05}, std::byte{0x00}};
             stream.insert(stream.end(), v.bytes.begin(), v.bytes.end());
             const auto result = fetch_stream(stream);
             EXPECT_FALSE(result.error.has_value());
             ASSERT_EQ(result.events.size(), 1u);
             ASSERT_TRUE(std::holds_alternative<FetchRangeEvent>(result.events[0]));
             const auto& range = std::get<FetchRangeEvent>(result.events[0]);
             EXPECT_EQ(range.kind, FetchRangeKind::TimedOut);
             EXPECT_EQ(range.serialization_flags, 0x20cu);
             EXPECT_EQ(range.group_id, 2u);
             EXPECT_EQ(range.object_id, 4u);
             EXPECT_EQ(range.stream_end_offset, stream.size());
         }},
    };
    return handlers;
}

struct Allowlisted {
    std::string reason;
    // The framing and parameter checks we can still make with what we have.
    std::function<void(const Vector&)> partial_check;
};

// Vectors we cannot decode with a typed codec of ours, and why.
const std::map<std::string, Allowlisted>& allowlisted() {
    static const std::map<std::string, Allowlisted> entries{
        {"publish_namespace",
         {"no PUBLISH_NAMESPACE decoder: draft 22 changed its figure (draft22_wire_audit_test) "
          "and neither our draft 21 nor draft 22 wire library decodes it; framing and parameter "
          "walk only",
          [](const Vector& v) {
              const auto frame = framed(v, MessageKind::PublishNamespace);
              Cursor body(frame.body);
              EXPECT_EQ(vi(body), 0u);
              EXPECT_EQ(tuple(body), kLive);
              EXPECT_TRUE(walk(body, vi(body)).empty());
              EXPECT_EQ(body.remaining(), 0u);
          }}},
        {"request_update_fill_whole_track",
         {"no REQUEST_UPDATE decoder or encoder in the wire library (scenarios build it by "
          "hand); framing and parameter walk only: Request ID 0x5f, FILL_PARAMETERS empty",
          [](const Vector& v) {
              const auto frame = framed(v, MessageKind::RequestUpdate);
              Cursor body(frame.body);
              EXPECT_EQ(vi(body), 0x5fu);
              const auto parameters = walk(body, vi(body));
              ASSERT_EQ(types(parameters), (std::vector<std::uint64_t>{0x23}));
              EXPECT_TRUE(parameters[0].payload.empty());
              EXPECT_EQ(body.remaining(), 0u);
          }}},
        {"publish_ok_expires_30000",
         {"no PUBLISH_OK decoder: decode_successful_response has no PUBLISH context (we only "
          "send PUBLISH_OK, via encode_empty_publish_ok, which cannot carry EXPIRES); framing "
          "and parameter walk only: EXPIRES 30000 (3-byte vi64)",
          [](const Vector& v) {
              const auto frame = framed(v, MessageKind::RequestOk);
              Cursor body(frame.body);
              const auto parameters = walk(body, vi(body));
              ASSERT_EQ(types(parameters), (std::vector<std::uint64_t>{0x08}));
              EXPECT_EQ(parameters[0].number, 30000u);
              EXPECT_EQ(body.remaining(), 0u);
          }}},
        {"publish_state_notify_largest_3_7_forward_0",
         {"no PUBLISH_STATE_NOTIFY decoder; framing and parameter walk only: LARGEST_OBJECT "
          "{3, 7}, FORWARD 0",
          [](const Vector& v) {
              const auto frame = framed(v, MessageKind::PublishStateNotify);
              Cursor body(frame.body);
              const auto parameters = walk(body, vi(body));
              ASSERT_EQ(types(parameters), (std::vector<std::uint64_t>{0x09, 0x10}));
              EXPECT_EQ(parameters[0].location, (std::pair<std::uint64_t, std::uint64_t>{3, 7}));
              EXPECT_EQ(parameters[1].number, 0u);
              EXPECT_EQ(body.remaining(), 0u);
          }}},
        {"publish_skipped_cam2_video",
         {"no PUBLISH_SKIPPED decoder; framing only, plus the suffix and name",
          [](const Vector& v) {
              const auto frame = framed(v, MessageKind::PublishSkipped);
              Cursor body(frame.body);
              EXPECT_EQ(tuple(body), (std::vector<std::vector<std::byte>>{text("cam2")}));
              EXPECT_EQ(length_prefixed(body), text("video"));
              EXPECT_EQ(body.remaining(), 0u);
          }}},
        {"param_subgroup_filter_set0_3to5_10to15",
         {"no SUBGROUP_FILTER value decoder (section 9.20.10); the parameter walk reads it as "
          "length-prefixed only",
          [](const Vector& v) {
              Cursor body(v.bytes);
              const auto parameters = walk(body, 1);
              ASSERT_EQ(types(parameters), (std::vector<std::uint64_t>{0x25}));
              EXPECT_EQ(parameters[0].payload.size(), 5u);
              EXPECT_EQ(body.remaining(), 0u);
          }}},
        {"param_object_property_filter_set1_prop16_from100",
         {"no OBJECT_PROPERTY_FILTER value decoder (section 9.20.13); the parameter walk reads "
          "it as length-prefixed only",
          [](const Vector& v) {
              Cursor body(v.bytes);
              const auto parameters = walk(body, 1);
              ASSERT_EQ(types(parameters), (std::vector<std::uint64_t>{0x28}));
              EXPECT_EQ(parameters[0].payload.size(), 3u);
              EXPECT_EQ(body.remaining(), 0u);
          }}},
    };
    return entries;
}

TEST(Draft22PlayaVectors, LoadsTheCopiedFile) {
    ASSERT_TRUE(std::filesystem::exists(kVectorFile)) << kVectorFile;
    // 29 vectors at moq-playa 952c872; a refreshed copy adds to them.
    EXPECT_GE(vectors().size(), 29u);
    std::set<std::string> names;
    for (const auto& vector : vectors()) {
        EXPECT_TRUE(names.insert(vector.name).second) << "duplicate " << vector.name;
        EXPECT_TRUE(vector.kind == "control" || vector.kind == "parameter" ||
                    vector.kind == "stream")
            << vector.name << " " << vector.kind;
        EXPECT_FALSE(vector.bytes.empty()) << vector.name;
    }
}

TEST(Draft22PlayaVectors, EveryVectorIsCoveredOrAllowlisted) {
    std::set<std::string> in_file;
    for (const auto& vector : vectors()) {
        in_file.insert(vector.name);
        const bool is_covered = covered().contains(vector.name);
        const bool is_allowlisted = allowlisted().contains(vector.name);
        EXPECT_TRUE(is_covered || is_allowlisted)
            << vector.name << " is neither tested nor allowlisted";
        EXPECT_FALSE(is_covered && is_allowlisted) << vector.name << " is both";
    }
    for (const auto& [name, handler] : covered()) {
        EXPECT_TRUE(in_file.contains(name)) << "covered entry " << name << " not in the file";
    }
    for (const auto& [name, entry] : allowlisted()) {
        EXPECT_TRUE(in_file.contains(name)) << "allowlist entry " << name << " not in the file";
        EXPECT_FALSE(entry.reason.empty()) << name;
    }
}

TEST(Draft22PlayaVectors, CoveredVectorsDecodeThroughOurCodecs) {
    std::size_t checked = 0;
    for (const auto& vector : vectors()) {
        const auto handler = covered().find(vector.name);
        if (handler == covered().end()) continue;
        SCOPED_TRACE(vector.name);
        handler->second(vector);
        ++checked;
    }
    EXPECT_EQ(checked, covered().size());
}

TEST(Draft22PlayaVectors, AllowlistedVectorsStillFrameAndWalk) {
    std::size_t checked = 0;
    for (const auto& vector : vectors()) {
        const auto entry = allowlisted().find(vector.name);
        if (entry == allowlisted().end()) continue;
        SCOPED_TRACE(vector.name + ": " + entry->second.reason);
        entry->second.partial_check(vector);
        ++checked;
    }
    EXPECT_EQ(checked, allowlisted().size());
}

}  // namespace
}  // namespace moq::interop::wire::draft22
