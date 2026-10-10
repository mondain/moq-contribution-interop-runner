#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace moq::interop::transport {

enum class WebTransportProfile { Draft18Wt15, Draft21Wt16, Draft22Wt16, MoqLite06 };

// The WT-Protocol value a profile negotiates, and the profile an application protocol selects.
// profile_for_application_protocol throws std::logic_error for an unknown protocol.
[[nodiscard]] std::string_view application_protocol_for(WebTransportProfile profile);
[[nodiscard]] WebTransportProfile profile_for_application_protocol(std::string_view protocol);

struct H3Request {
    std::string method;
    std::string protocol;
    std::string scheme;
    std::string authority;
    std::string path;
    std::vector<std::pair<std::string, std::string>> headers;
};

// Only the decoded SETTINGS_WT_ENABLED (0x2c7cf000) value belongs here.
// Legacy settings have different identifiers and must not be mapped to it.
struct PeerCapabilities {
    bool settings_received = false;
    uint64_t wt_enabled_value = 0;
    bool h3_datagram = false;
    bool quic_datagram = false;
    bool reset_stream_at = false;
    // The client's SETTINGS enabled WebTransport with an identifier of draft-ietf-webtrans-http3 before
    // SETTINGS_WT_ENABLED (legacy_webtransport_settings). Honored by the moq-lite profile only, whose draft names
    // no WebTransport draft: the moq CLI's stack (web-transport-proto 0.6.2) sends only those identifiers.
    bool legacy_webtransport = false;
};

// The pre-SETTINGS_WT_ENABLED WebTransport support a client's HTTP/3 control stream announces, from the first bytes
// of a client-initiated unidirectional stream: nullopt while those bytes hold no complete SETTINGS frame yet; false
// when the stream is not a control stream (type 0x00), its first frame is not a well-formed SETTINGS frame, or the
// frame enables WebTransport by no legacy identifier; true when it carries SETTINGS_WEBTRANSPORT_MAX_SESSIONS
// (0xc671706a) non-zero or SETTINGS_ENABLE_WEBTRANSPORT (0x2b603742) = 1, and H3_DATAGRAM (0x33, or 0xffd277) = 1
// (the rule of web-transport-proto 0.6.2, which the moq CLI applies to its peer).
[[nodiscard]] std::optional<bool> legacy_webtransport_settings(std::span<const std::uint8_t> control_stream_prefix);

struct RunEndpoint {
    std::string authority;
    std::string path;
    std::vector<std::string> allowed_origins;
    std::string moqt_protocol;
    bool require_origin = false;
};

struct ConnectDecision {
    int http_status = 400;
    std::string selected_protocol;
    std::string evidence;

    [[nodiscard]] bool accepted() const {
        return http_status >= 200 && http_status < 300 && !selected_protocol.empty();
    }
};

[[nodiscard]] ConnectDecision validate_connect(const H3Request& request,
                                               const PeerCapabilities& capabilities,
                                               const RunEndpoint& endpoint,
                                               WebTransportProfile profile);

}  // namespace moq::interop::transport
