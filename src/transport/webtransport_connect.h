#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace moq::interop::transport {

enum class WebTransportProfile { Draft18Wt15, Draft21Wt16, Draft22Wt16 };

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
};

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
