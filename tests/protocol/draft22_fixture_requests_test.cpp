// The draft 21 family SUBSCRIBE / TRACK_STATUS probes that a draft 22 run shares (wire draft 22 through
// scenarios::current_wire_draft()) and that named their own track, namespace () and track "x". A publisher
// may refuse the empty namespace before it reads the parameter the probe is about (imquic: 0x3 "Invalid
// number of namespaces"), so on the draft 22 wire they name the run's track fixture. Draft 21 is frozen: on
// wire draft 21 every close and request profile stays byte-identical to the pins below.
#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace moq::interop::scenarios {
namespace {

using Bytes = std::vector<std::byte>;

std::string hex(const Bytes& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(digits[value >> 4u]);
        result.push_back(digits[value & 15u]);
    }
    return result;
}

// Every write of a definition: channel, reused stream, fin, bytes.
std::string shape(const RawProbeDefinition& definition) {
    std::string result = hex(definition.setup_bytes);
    for (const auto& write : definition.writes) {
        result += " " + std::to_string(static_cast<int>(write.channel)) + ":" +
                  (write.reuse_write_stream ? std::to_string(*write.reuse_write_stream) : std::string("-")) +
                  (write.fin ? ":f:" : ":o:") + hex(write.bytes);
    }
    return result;
}

// Everything a profile list states as data, for a whole-list fingerprint.
std::string full(const RawProbeDefinition& definition) {
    std::string result = definition.id + "|" + shape(definition) + "|" +
        std::to_string(definition.start_after_peer_setup) + "|" + std::to_string(definition.deadline.count()) +
        "|" + std::to_string(static_cast<bool>(definition.peer_setup_ready)) +
        std::to_string(static_cast<bool>(definition.response_ready)) +
        std::to_string(static_cast<bool>(definition.peer_request_ready));
    if (definition.liveness)
        result += "|live" + std::to_string(definition.liveness->draft) + "/" +
                  std::to_string(definition.liveness->delay.count()) + "/" +
                  std::to_string(definition.liveness->grace.count()) + "/" +
                  std::to_string(definition.liveness->request_id) + "/" + hex(definition.liveness->request);
    return result;
}

std::uint64_t fnv(const std::string& text, std::uint64_t hash = 14695981039346656037ull) {
    for (const char c : text) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

std::uint64_t fingerprint(const std::vector<Draft21CloseProbe>& probes) {
    std::uint64_t hash = fnv("close");
    for (const auto& probe : probes)
        hash = fnv(probe.requirement_id + "|" + probe.evaluator_id + "|" +
                   (probe.expected_close ? std::to_string(*probe.expected_close) : std::string("none")) + "|" +
                   full(probe.definition) + "\n", hash);
    return hash;
}

std::uint64_t fingerprint(const std::vector<RequestProbeProfile>& profiles) {
    std::uint64_t hash = fnv("request");
    for (const auto& profile : profiles)
        hash = fnv(std::to_string(profile.draft) + "|" + profile.requirement_id + "|" + profile.evaluator_id + "|" +
                   std::to_string(profile.expected_error) + "|" + std::to_string(profile.namespace_scoped) + "|" +
                   std::to_string(profile.compatibility_error) + "|" + full(profile.definition) + "\n", hash);
    return hash;
}

template <class Profiles>
std::map<std::string, std::string> shapes(const Profiles& profiles) {
    std::map<std::string, std::string> result;
    for (const auto& profile : profiles) result[profile.definition.id] = shape(profile.definition);
    return result;
}

// Pinned from the definitions before the draft 22 fixture change (HEAD 9afa0da): the close probes whose
// SUBSCRIBE / TRACK_STATUS names (), "x".
const std::map<std::string, std::string> kCloseNamePins{
    {"d21-parameter-type-delta-overflow", "af000000 1:-:o:03001001000178020200ffffffffffffffffff"},
    {"d21-request-undecodable-authorization-token", "af000000 1:-:o:0300080100017801030103"},
    {"d21-unknown-message-parameter", "af000000 1:-:o:03000701000178017e00"},
    {"d21-unexpected-duplicate-message-parameter", "af000000 1:-:o:030009010001780210000000"},
    {"d21-parameter-invalid-message-scope", "af000000 1:-:o:03000701000178010801"},
    {"d21-group-order-zero", "af000000 1:-:o:03000701000178012200"},
    {"d21-location-filter-end-group-overflow", "af000000 1:-:o:0300120100017801210bffffffffffffffffff0001"},
    {"d21-forward-value-two", "af000000 1:-:o:03000701000178011002"},
    {"d21-include-properties-value-two", "af000000 1:-:o:03000701000178013502"},
    {"d21-fill-forbidden-nested-authorization", "af000000 1:-:o:03000b0100017801230403020300"},
    {"d21-fill-forbidden-track-property-filter", "af000000 1:-:o:03000c010001780123052903000000"},
    {"d21-fill-recursive-parameter", "af000000 1:-:o:030009010001780123022300"},
    {"d21-update-on-track-status", "af000000 1:-:o:0d000501000178000200020300"},
    {"d21-token-duplicate-registration", "af000000 1:-:o:03000a01000178010303010000 1:-:o:03000a03000178010303010000"},
    {"d21-request-token-cache-overflow", "af000000 1:-:o:03000b0100017801030401000078"},
    {"d21-unknown-request-stream-message", "af000000 1:-:o:03000501000178007e0000"},
    {"d21-request-message-truncated-at-fin", "af000000 1:-:f:030007010001780110"},
    {"d21-group-order-above-two", "af000000 1:-:o:03000701000178012203"},
    {"d21-fill-invalid-group-order", "af000000 1:-:o:030009010001780123022203"},
    {"d21-fill-location-filter-end-group-overflow", "af000000 1:-:o:0300140100017801230d210bffffffffffffffffff0001"},
    {"d21-forward-value-255", "af000000 1:-:o:030007010001780110ff"},
    {"d21-include-properties-value-255", "af000000 1:-:o:030007010001780135ff"},
    {"d21-request-alias-registration-with-default-zero-cache", "af000000 1:-:o:03000b010001780103040100809d"},
    {"d21-fill-timeout-outside-fill-or-fetch", "af000000 1:-:o:03000701000178010a00"},
};
// The request profiles whose SUBSCRIBE names (), "x".
const std::map<std::string, std::string> kRequestNamePins{
    {"d21-range-filter-start-delta-overflow", "af000000 1:-:o:0300130100017801260c00ffffffffffffffffff0001"},
    {"d21-range-filter-end-delta-overflow", "af000000 1:-:o:0300120100017801260b00ffffffffffffffffff01"},
    {"d21-duplicate-range-filter-key-in-request", "af000000 1:-:o:03000d01000178022602000000020000"},
    {"d21-priority-filter-start-above-255", "af000000 1:-:o:03000a01000178012703008100"},
    {"d21-priority-filter-end-above-255", "af000000 1:-:o:03000b010001780127040080ff01"},
    {"d21-object-property-filter-odd-property-type", "af000000 1:-:o:03000a01000178012803000100"},
    {"d21-request-unknown-token-alias", "af000000 1:-:o:030009010001780103020200"},
};
// Whole lists (every field above, of every profile), before the change.
constexpr std::uint64_t kCloseFingerprint = 13478458653519975156ull;
constexpr std::uint64_t kRequestFingerprint = 9664250555963211302ull;

template <class Profiles>
std::map<std::string, std::string> named(const Profiles& profiles, const std::map<std::string, std::string>& pins) {
    std::map<std::string, std::string> result;
    for (const auto& [id, pin] : shapes(profiles))
        if (pins.contains(id)) result[id] = pin;
    return result;
}

TEST(Draft22FixtureRequests, Draft21CloseDefinitionsArePinned) {
    ASSERT_EQ(current_wire_draft(), 21u);
    const auto probes = draft21_close_probes();
    EXPECT_EQ(named(probes, kCloseNamePins), kCloseNamePins);
    EXPECT_EQ(fingerprint(probes), kCloseFingerprint);
}

TEST(Draft22FixtureRequests, Draft21RequestDefinitionsArePinned) {
    ASSERT_EQ(current_wire_draft(), 21u);
    const auto profiles = draft21_request_profiles();
    EXPECT_EQ(named(profiles, kRequestNamePins), kRequestNamePins);
    EXPECT_EQ(fingerprint(profiles), kRequestFingerprint);
}

// --- the run's names on the draft 22 wire ---------------------------------------------------------------

using Namespace = std::vector<Bytes>;

Bytes text(std::string_view value) {
    Bytes result;
    for (const char c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}

const Namespace kNamespace{text("media")};
const Bytes kName = text("vide_1");
// Track Namespace (media) and Track Name vide_1, as draft 22 encodes them after the Request ID.
constexpr std::string_view kFixtureNames = "01056d6564696106766964655f31";
constexpr std::chrono::milliseconds kDeadline{1000};

// The pinned shape with the fixture's names in place of (), "x" in every write that names a track: the
// 3 bytes 00 01 78 after the type, 16-bit length and one-byte Request ID become kFixtureNames (11 bytes
// more, added to the length). Nothing else changes.
std::string renamed(const std::string& pin) {
    std::string result;
    std::size_t start = 0;
    while (start <= pin.size()) {
        const auto end = std::min(pin.find(' ', start), pin.size());
        auto token = pin.substr(start, end - start);
        const auto colon = token.rfind(':');
        if (colon != std::string::npos && token.compare(colon + 1 + 8, 6, "000178") == 0) {
            auto bytes = token.substr(colon + 1);
            const auto length = std::stoul(bytes.substr(2, 4), nullptr, 16) + 11;
            char buffer[5];
            std::snprintf(buffer, sizeof buffer, "%04lx", length);
            bytes = bytes.substr(0, 2) + buffer + bytes.substr(6, 2) + std::string(kFixtureNames) + bytes.substr(14);
            token = token.substr(0, colon + 1) + bytes;
        }
        result += (result.empty() ? "" : " ") + token;
        start = end + 1;
    }
    return result;
}

Bytes from_hex(std::string_view value) {
    Bytes result;
    for (std::size_t i = 0; i + 1 < value.size(); i += 2)
        result.push_back(static_cast<std::byte>(std::stoul(std::string(value.substr(i, 2)), nullptr, 16)));
    return result;
}

template <class Profiles>
const auto* find(const Profiles& profiles, std::string_view id) {
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                    [&](const auto& profile) { return profile.definition.id == id; });
    return found == profiles.end() ? nullptr : &*found;
}

// A transcript in which every write of `definition` was accepted in order on the streams the controller
// would use; `responses` holds what the publisher answered on a write's stream right after that write.
RawProbeTranscript delivered(const RawProbeDefinition& definition, const std::map<std::size_t, Bytes>& responses = {}) {
    RawProbeTranscript t;
    t.scenario_id = definition.id;
    t.events = {transport::ConnectionEstablishedEvent{},
                transport::StreamDataEvent{2, definition.setup_bytes, false}};
    t.setup = {{RawProbeChannel::NewUni, definition.setup_bytes, false}, 3, 4, false, 1};
    t.transport_established = t.peer_setup_received = t.complete = t.stimulus_delivered = true;
    std::uint64_t next = 1;
    for (std::size_t i = 0; i < definition.writes.size(); ++i) {
        const auto& write = definition.writes[i];
        const auto stream = write.reuse_write_stream ? *t.writes[*write.reuse_write_stream].stream_id : next;
        if (!write.reuse_write_stream) next += 4;
        t.writes.push_back({write, stream, write.bytes.size(), write.fin, t.events.size()});
        if (const auto found = responses.find(i); found != responses.end())
            t.events.push_back(transport::StreamDataEvent{stream, found->second, false});
    }
    t.delivery_event_count = t.events.size();
    return t;
}

RawProbeTranscript closed(const RawProbeDefinition& definition, std::uint64_t code) {
    auto t = delivered(definition);
    t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, code, {}});
    return t;
}

// Degenerate track fixtures (the run manager test's list), and one too large for a probe frame.
std::vector<std::pair<std::string, std::pair<Namespace, Bytes>>> degenerate_fixtures() {
    return {
        {"empty namespace", {{}, text("t")}},
        {"empty namespace and name", {{}, {}}},
        {"empty name", {{text("n")}, {}}},
        {"empty field", {{Bytes{}}, text("t")}},
        {"33 fields", {Namespace(33, text("n")), text("t")}},
        {"field over 4096 bytes", {{Bytes(4097, std::byte{'n'})}, text("t")}},
        {"name over 4096 bytes", {{text("n")}, Bytes(4097, std::byte{'t'})}},
        {"largest accepted", {Namespace(32, Bytes(120, std::byte{'n'})), Bytes(256, std::byte{'t'})}},
    };
}
const std::pair<Namespace, Bytes> kOversized{Namespace(32, Bytes(4096, std::byte{'n'})), text("t")};

// The two END_GROUP overflow probes are not built on the draft 22 wire (draft 22 cannot encode them).
bool wire22_close_probe(const std::string& id) {
    return id != "d21-location-filter-end-group-overflow" && id != "d21-fill-location-filter-end-group-overflow";
}

TEST(Draft22FixtureRequests, Wire21CloseProbesIgnoreTheFixture) {
    const auto probes = draft21_close_probes(kDeadline, {}, {std::byte{'x'}}, kNamespace, kName);
    EXPECT_EQ(fingerprint(probes), kCloseFingerprint);
}

TEST(Draft22FixtureRequests, Wire22CloseProbesNameTheRunsTrack) {
    const ScopedWireDraft wire(22);
    const auto built = draft21_close_probes(kDeadline, {}, {std::byte{'x'}}, kNamespace, kName);
    const auto own = draft21_close_probes();
    std::size_t renamed_count = 0;
    for (const auto& probe : built) {
        SCOPED_TRACE(probe.definition.id);
        const auto pin = kCloseNamePins.find(probe.definition.id);
        if (pin == kCloseNamePins.end()) {
            // Every other probe is exactly what it is without a fixture.
            EXPECT_EQ(full(probe.definition), full(find(own, probe.definition.id)->definition));
            continue;
        }
        ++renamed_count;
        EXPECT_EQ(shape(probe.definition), renamed(pin->second));
        const auto first = hex(probe.definition.writes.front().bytes);
        EXPECT_NE(first.find(kFixtureNames), std::string::npos);
        EXPECT_NE(first.substr(8, 6), "000178");
        // Without a fixture (or with an empty track name) the probe keeps its own names, as before.
        EXPECT_EQ(shape(find(own, probe.definition.id)->definition), pin->second);
    }
    EXPECT_EQ(renamed_count, kCloseNamePins.size() - 2);
    const auto nameless = draft21_close_probes(kDeadline, {}, {std::byte{'x'}}, kNamespace, {});
    for (const auto& probe : nameless)
        EXPECT_EQ(full(probe.definition), full(find(own, probe.definition.id)->definition)) << probe.definition.id;
}

TEST(Draft22FixtureRequests, Wire22CloseProofRebuildsTheRunsTrack) {
    const ScopedWireDraft wire(22);
    const auto built = draft21_close_probes(kDeadline, {}, {std::byte{'x'}}, kNamespace, kName);
    // The evaluator holds the fixture-free profiles, as evaluate_draft21_raw_probes does.
    const auto judged = draft21_close_probes();
    for (const auto& [id, pin] : kCloseNamePins) {
        if (!wire22_close_probe(id)) continue;
        SCOPED_TRACE(id);
        const auto* definition = find(built, id);
        const auto* profile = find(judged, id);
        ASSERT_NE(definition, nullptr);
        ASSERT_NE(profile, nullptr);
        const auto code = profile->expected_close.value_or(3);
        // The verdict is the one the probe's own names would get: only the names differ.
        const auto own = evaluate_draft21_close_probe(closed(profile->definition, code), *profile);
        const auto named = evaluate_draft21_close_probe(closed(definition->definition, code), *profile);
        EXPECT_EQ(named, own);
        // A first write the builder would not have produced proves nothing.
        auto altered = closed(definition->definition, code);
        altered.writes[0].write.bytes.push_back(std::byte{0});
        altered.writes[0].accepted += 1;
        EXPECT_FALSE(evaluate_draft21_close_probe(altered, *profile).has_value());
    }
    // The probes whose setup gate this transcript meets reach a verdict on the run's names.
    for (const auto* id : {"d21-group-order-zero", "d21-parameter-type-delta-overflow", "d21-update-on-track-status",
                           "d21-unknown-request-stream-message", "d21-request-message-truncated-at-fin",
                           "d21-request-undecodable-authorization-token"}) {
        SCOPED_TRACE(id);
        const auto* profile = find(judged, id);
        const auto code = profile->expected_close.value_or(3);
        EXPECT_EQ(evaluate_draft21_close_probe(closed(find(built, id)->definition, code), *profile), true);
        EXPECT_EQ(evaluate_draft21_close_probe(closed(find(built, id)->definition, 0), *profile).value_or(false),
                  false);
        auto renamed_track = closed(find(built, id)->definition, code);
        renamed_track.writes[0].write.bytes[6] = std::byte{'M'};
        EXPECT_EQ(evaluate_draft21_close_probe(renamed_track, *profile), true)
            << "any track the run named is rebuilt from the delivered request";
    }
}

TEST(Draft22FixtureRequests, Wire21CloseProofIsUnchanged) {
    std::vector<Draft21CloseProbe> built;
    {
        const ScopedWireDraft wire(22);
        built = draft21_close_probes(kDeadline, {}, {std::byte{'x'}}, kNamespace, kName);
    }
    const auto judged = draft21_close_probes();
    const auto* profile = find(judged, "d21-group-order-zero");
    EXPECT_FALSE(evaluate_draft21_close_probe(closed(find(built, "d21-group-order-zero")->definition, 3), *profile)
                     .has_value());
    EXPECT_EQ(evaluate_draft21_close_probe(closed(profile->definition, 3), *profile), true);
}

TEST(Draft22FixtureRequests, Wire22CloseProbesBuildDegenerateFixturesWithoutCrashing) {
    const ScopedWireDraft wire(22);
    const auto judged = draft21_close_probes();
    for (const auto& [name, fixture] : degenerate_fixtures()) {
        SCOPED_TRACE(name);
        std::vector<Draft21CloseProbe> built;
        ASSERT_NO_THROW(built = draft21_close_probes(kDeadline, {}, {std::byte{'x'}}, fixture.first, fixture.second));
        const auto* definition = find(built, "d21-group-order-zero");
        const auto* profile = find(judged, "d21-group-order-zero");
        EXPECT_EQ(evaluate_draft21_close_probe(closed(definition->definition, 3), *profile), true);
    }
    EXPECT_THROW(draft21_close_probes(kDeadline, {}, {std::byte{'x'}}, kOversized.first, kOversized.second),
                 std::invalid_argument);
}

// --- request profiles --------------------------------------------------------------------------------------

// A publisher SETUP offering MAX_FILTER_RANGES 2 (the range and priority filter probes wait for it).
const Bytes kRangesSetup = from_hex("af0000020602");

// `definition` delivered after kRangesSetup, answered on its request stream by `response` (with FIN).
RawProbeTranscript answered(const RawProbeDefinition& definition, const Bytes& response) {
    auto t = delivered(definition);
    t.events[1] = transport::StreamDataEvent{2, kRangesSetup, false};
    t.events.push_back(transport::StreamDataEvent{*t.writes.front().stream_id, response, true});
    t.unknown_auth_token_alias_compatibility_code = 0x17;
    return t;
}

// REQUEST_ERROR with `code`, no retry, empty reason.
Bytes request_error(unsigned code) { return {std::byte{5}, std::byte{0}, std::byte{3}, static_cast<std::byte>(code),
                                             std::byte{0}, std::byte{0}}; }

TEST(Draft22FixtureRequests, Wire21RequestProfilesIgnoreTheFixture) {
    EXPECT_EQ(fingerprint(draft21_request_profiles(kDeadline, kNamespace, kName)), kRequestFingerprint);
}

TEST(Draft22FixtureRequests, Wire22RequestProfilesNameTheRunsTrack) {
    const ScopedWireDraft wire(22);
    const auto built = draft21_request_profiles(kDeadline, kNamespace, kName);
    const auto own = draft21_request_profiles();
    std::size_t renamed_count = 0;
    for (const auto& profile : built) {
        SCOPED_TRACE(profile.definition.id);
        const auto pin = kRequestNamePins.find(profile.definition.id);
        if (pin == kRequestNamePins.end()) {
            // The reserved-namespace requests keep their names: the name is what they test.
            EXPECT_EQ(full(profile.definition), full(find(own, profile.definition.id)->definition));
            continue;
        }
        ++renamed_count;
        EXPECT_EQ(shape(profile.definition), renamed(pin->second));
        EXPECT_NE(hex(profile.definition.writes.front().bytes).find(kFixtureNames), std::string::npos);
        EXPECT_EQ(shape(find(own, profile.definition.id)->definition), pin->second);
    }
    EXPECT_EQ(renamed_count, kRequestNamePins.size());
    for (const auto& profile : draft21_request_profiles(kDeadline, kNamespace, {}))
        EXPECT_EQ(full(profile.definition), full(find(own, profile.definition.id)->definition))
            << profile.definition.id;
}

TEST(Draft22FixtureRequests, Wire22RequestProofRebuildsTheRunsTrack) {
    const ScopedWireDraft wire(22);
    const auto built = draft21_request_profiles(kDeadline, kNamespace, kName);
    const auto judged = draft21_request_profiles();
    for (const auto& [id, pin] : kRequestNamePins) {
        SCOPED_TRACE(id);
        const auto* definition = find(built, id);
        const auto* profile = find(judged, id);
        ASSERT_NE(definition, nullptr);
        ASSERT_NE(profile, nullptr);
        const auto response = request_error(static_cast<unsigned>(
            profile->compatibility_error ? 0x17 : profile->expected_error));
        EXPECT_EQ(evaluate_draft21_request_profile(answered(profile->definition, response), *profile), true);
        EXPECT_EQ(evaluate_draft21_request_profile(answered(definition->definition, response), *profile), true);
        EXPECT_EQ(evaluate_draft21_request_profile(answered(definition->definition, request_error(0x10)), *profile),
                  false);
        auto altered = answered(definition->definition, response);
        altered.writes[0].write.bytes.push_back(std::byte{0});
        altered.writes[0].accepted += 1;
        EXPECT_FALSE(evaluate_draft21_request_profile(altered, *profile).has_value());
        auto renamed_track = answered(definition->definition, response);
        renamed_track.writes[0].write.bytes[6] = std::byte{'M'};
        EXPECT_EQ(evaluate_draft21_request_profile(renamed_track, *profile), true);
    }
    // The reserved-namespace requests are proved against their own bytes, as before.
    const auto* reserved = find(judged, "d21-request-single-period-namespace");
    EXPECT_EQ(evaluate_draft21_request_profile(answered(reserved->definition, request_error(0x10)), *reserved), true);
}

TEST(Draft22FixtureRequests, Wire21RequestProofIsUnchanged) {
    std::vector<RequestProbeProfile> built;
    {
        const ScopedWireDraft wire(22);
        built = draft21_request_profiles(kDeadline, kNamespace, kName);
    }
    const auto judged = draft21_request_profiles();
    for (const auto& [id, pin] : kRequestNamePins) {
        SCOPED_TRACE(id);
        const auto* profile = find(judged, id);
        const auto response = request_error(static_cast<unsigned>(
            profile->compatibility_error ? 0x17 : profile->expected_error));
        EXPECT_FALSE(evaluate_draft21_request_profile(answered(find(built, id)->definition, response), *profile)
                         .has_value());
        const auto own = answered(profile->definition, response);
        EXPECT_EQ(evaluate_draft21_request_profile(own, *profile), evaluate_raw_probe_request_error(own, *profile));
        EXPECT_EQ(evaluate_draft21_request_profile(own, *profile), true);
    }
}

TEST(Draft22FixtureRequests, Wire22RequestProfilesBuildDegenerateFixturesWithoutCrashing) {
    const ScopedWireDraft wire(22);
    const auto judged = draft21_request_profiles();
    const auto* profile = find(judged, "d21-priority-filter-start-above-255");
    for (const auto& [name, fixture] : degenerate_fixtures()) {
        SCOPED_TRACE(name);
        std::vector<RequestProbeProfile> built;
        ASSERT_NO_THROW(built = draft21_request_profiles(kDeadline, fixture.first, fixture.second));
        const auto* definition = find(built, "d21-priority-filter-start-above-255");
        EXPECT_EQ(evaluate_draft21_request_profile(answered(definition->definition, request_error(0x36)), *profile),
                  true);
    }
    EXPECT_THROW(draft21_request_profiles(kDeadline, kOversized.first, kOversized.second), std::invalid_argument);
}

}  // namespace
}  // namespace moq::interop::scenarios
