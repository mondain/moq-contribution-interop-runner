#pragma once

#include "moq/interop/scenarios/draft18_contribution.h"
#include "moq/interop/wire/draft18/messages.h"
#include "contribution_harness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string_view>

namespace moq::interop::test {

namespace d18 = wire::draft18;
using Bytes = std::vector<std::byte>;
using scenarios::Draft18ContributionProbe;

inline Bytes text(std::string_view value) {
    Bytes result;
    for (const auto c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}
inline Bytes encode(const d18::Message& message) {
    wire::ByteWriter out(65546);
    EXPECT_TRUE(d18::encode_message(message, out).has_value());
    return {out.bytes().begin(), out.bytes().end()};
}
inline Bytes setup_with(d18::KeyValuePairs options) { return encode(d18::SetupMessage{std::move(options)}); }
// Hand-assembled SETUP so that duplicate option types are not rejected by the
// typed encoder or decoder: 0x2F00, 16-bit length, then raw key-value pairs.
inline Bytes raw_setup(const Bytes& payload) {
    Bytes result = bytes_of({0xaf, 0x00, static_cast<unsigned>(payload.size() >> 8),
                             static_cast<unsigned>(payload.size() & 255)});
    return concat(std::move(result), payload);
}
inline Bytes ok(d18::Parameters parameters = {}) { return encode(d18::RequestOkMessage{std::move(parameters), {}}); }
inline Bytes error(std::uint64_t code) {
    return encode(d18::RequestErrorMessage{code, 0, {}, std::nullopt});
}
inline Bytes subscribe_ok(std::uint64_t alias = 5, d18::Parameters parameters = {}) {
    return encode(d18::SubscribeOkMessage{alias, std::move(parameters), {}});
}
inline d18::Parameter largest(std::uint64_t group, std::uint64_t object) {
    return {0x09, d18::Location{group, object}};
}

inline Bytes vi(std::uint64_t value) {
    wire::ByteWriter out(9);
    EXPECT_TRUE(wire::write_vi64(value, out));
    return {out.bytes().begin(), out.bytes().end()};
}
// SUBGROUP_HEADER with explicit priority and no Subgroup ID field (id 0): type 0x10..
inline Bytes subgroup_header(std::uint64_t alias, std::uint64_t group, std::uint64_t type = 0x10) {
    auto out = vi(type);
    out = concat(std::move(out), vi(alias));
    out = concat(std::move(out), vi(group));
    if ((type & 0x06u) == 0x04u) out = concat(std::move(out), vi(0));
    if ((type & 0x20u) == 0u) out.push_back(std::byte{128});
    return out;
}
// One Subgroup object without properties: Object ID delta, length, [status], payload.
inline Bytes subgroup_object(std::uint64_t delta, const Bytes& payload, std::optional<std::uint64_t> status = std::nullopt) {
    auto out = vi(delta);
    out = concat(std::move(out), vi(payload.size()));
    if (payload.empty() && status) out = concat(std::move(out), vi(*status));
    return concat(std::move(out), payload);
}

inline const Draft18ContributionProbe& probe(const std::vector<Draft18ContributionProbe>& probes,
                                             std::string_view scenario, std::string_view requirement) {
    const auto found = std::find_if(probes.begin(), probes.end(), [&](const auto& p) {
        return p.definition.id == scenario && p.requirement_id == requirement;
    });
    EXPECT_NE(found, probes.end()) << scenario << " " << requirement;
    return *found;
}

// Decodes the request message in `bytes` as a draft-18 request-stream message.
inline std::optional<d18::Message> decode_request(const Bytes& bytes) {
    wire::Cursor cursor(bytes);
    auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    if (auto* message = std::get_if<d18::Message>(&decoded)) return std::move(*message);
    return std::nullopt;
}

}  // namespace moq::interop::test
