#include "moq/interop/scenarios/draft18_request.h"
#include "moq/interop/wire/draft18/messages.h"

#include <stdexcept>
#include <string_view>
#include <utility>

namespace moq::interop::scenarios {
namespace {
std::vector<std::byte> bytes(std::initializer_list<unsigned> values) {
    std::vector<std::byte> result;
    for (auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
void vi(std::vector<std::byte>& output, std::uint64_t value) {
    wire::ByteWriter writer(16);
    if (!wire::write_vi64(value,writer)) throw std::logic_error("varint failed");
    output.insert(output.end(),writer.bytes().begin(),writer.bytes().end());
}
bool setup_ready(std::span<const std::byte> input) {
    wire::Cursor cursor(input);
    const auto decoded = wire::draft18::decode_message(wire::draft18::StreamRole::Control,cursor,{});
    const auto* message = std::get_if<wire::draft18::Message>(&decoded);
    return message && std::holds_alternative<wire::draft18::SetupMessage>(*message);
}
RequestProbeProfile profile(std::string requirement, std::string scenario,
                            std::string evaluator, std::uint64_t expected_error,
                            std::uint64_t type, const std::vector<std::byte>& payload,
                            std::chrono::milliseconds deadline) {
    if (payload.size()>65535) throw std::logic_error("request probe payload too large");
    std::vector<std::byte> framed;
    vi(framed,type);
    framed.push_back(static_cast<std::byte>(payload.size()>>8));
    framed.push_back(static_cast<std::byte>(payload.size()&255));
    framed.insert(framed.end(),payload.begin(),payload.end());
    RawProbeDefinition definition;
    definition.id = std::move(scenario);
    definition.setup_bytes = bytes({0xaf,0,0,0});
    definition.writes.push_back({RawProbeChannel::NewBidi,std::move(framed),false});
    definition.peer_setup_ready = setup_ready;
    // A publisher that announces its namespace first waits for the acknowledgement
    // before it reads other requests (section 10.15); the answer is not the stimulus.
    definition.acknowledge_publisher_namespace = true;
    definition.deadline = deadline;
    definition.response_ready = request_probe_response_ready;
    return {18,std::move(requirement),std::move(evaluator),expected_error,false,std::move(definition)};
}
}  // namespace
std::vector<RequestProbeProfile> draft18_request_profiles(std::chrono::milliseconds deadline) {
    auto session_request = bytes({1,1,8,'.','s','e','s','s','i','o','n'});
    constexpr std::string_view unknown_track = "interop-unknown-78937ae9d2abc4e0b6c1";
    vi(session_request,unknown_track.size());
    for (auto c : unknown_track) session_request.push_back(static_cast<std::byte>(c));
    session_request.push_back(std::byte{0});
    std::vector<RequestProbeProfile> result;
    result.push_back(profile("D18-3-2-1-MUST-002","request-track-in-single-period-namespace",
        "request-rejected-does-not-exist",0x10,3,bytes({1,1,1,'.',1,'x',0}),deadline));
    result.push_back(profile("D18-3-2-2-MUST-001","request-empty-track-name-in-session-namespace",
        "request-rejected-does-not-exist",0x10,3,bytes({1,1,8,'.','s','e','s','s','i','o','n',0,0}),deadline));
    result.push_back(profile("D18-3-2-2-MUST-002","request-unrecognized-session-level-name",
        "request-error-does-not-exist-for-session-name",0x10,3,session_request,deadline));
    // Section 10.2.2 mandates message rejection with UNKNOWN_AUTH_TOKEN_ALIAS.
    // Draft 18 omits that name from its REQUEST_ERROR registry; 0x17 is its
    // only assigned code, in Section 3.5. Scoring requires an explicitly
    // configured compatibility request code; 0x17 below is a fixture value.
    result.push_back(profile("D18-10-2-2-MUST-007","receive-use-alias-for-unregistered-token",
        "request-error-unknown-auth-token-alias",0x17,3,bytes({1,1,1,'n',1,'x',1,3,2,2,7}),deadline));
    result.push_back(profile("D18-10-2-2-MUST-007","receive-delete-for-unregistered-token",
        "request-error-unknown-auth-token-alias",0x17,3,bytes({1,1,1,'n',1,'x',1,3,2,0,7}),deadline));
    result.push_back(profile("D18-10-12-2-MUST-004","receive-joining-fetch-with-unrelated-or-wrong-state-request-id",
        "request-error-invalid-joining-request-id",0x32,0x16,bytes({1,2,3,0,0}),deadline));
    for (auto& entry : result) {
        if (entry.requirement_id == "D18-10-2-2-MUST-007")
            entry.compatibility_error = true;
    }
    return result;
}
}  // namespace moq::interop::scenarios
