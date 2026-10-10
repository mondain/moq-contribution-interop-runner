#include "transport/webtransport_connect.h"

#include "moq/interop/app/draft_traits.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace moq::interop::transport {
namespace {

constexpr std::size_t kMaxFieldLength = 4096;
constexpr std::size_t kMaxOffers = 32;

bool is_lower_alpha(char c) { return c >= 'a' && c <= 'z'; }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_key_start(char c) { return is_lower_alpha(c) || c == '*'; }
bool is_key_tail(char c) {
    return is_key_start(c) || is_digit(c) || c == '_' || c == '-' || c == '.';
}
bool is_token_start(char c) {
    return is_lower_alpha(c) || (c >= 'A' && c <= 'Z') || c == '*';
}
bool is_token_tail(char c) {
    return is_token_start(c) || is_digit(c) || c == '_' || c == '-' ||
           c == '.' || c == ':' || c == '/' || c == '!' || c == '#' ||
           c == '$' || c == '%' || c == '&' || c == '\'' || c == '+' ||
           c == '^' || c == '`' || c == '|' || c == '~';
}
bool is_base64(char c) {
    return is_digit(c) || is_lower_alpha(c) || (c >= 'A' && c <= 'Z') ||
           c == '+' || c == '/';
}

class SfParser {
public:
    explicit SfParser(std::string_view input) : input_(input) {}

    bool parse_list(std::vector<std::string>& values) {
        if (input_.empty() || input_.size() > kMaxFieldLength) return false;
        skip_ows();
        while (pos_ < input_.size() && values.size() < kMaxOffers) {
            std::string value;
            if (!parse_string(value) || !parse_params()) return false;
            values.push_back(std::move(value));
            skip_ows();
            if (pos_ == input_.size()) return true;
            if (!consume(',')) return false;
            skip_ows();
            if (pos_ == input_.size()) return false;
        }
        return false;
    }

private:
    bool consume(char c) {
        if (pos_ >= input_.size() || input_[pos_] != c) return false;
        ++pos_;
        return true;
    }
    void skip_ows() {
        while (pos_ < input_.size() && (input_[pos_] == ' ' || input_[pos_] == '\t')) ++pos_;
    }
    bool parse_string(std::string& out) {
        if (!consume('"')) return false;
        while (pos_ < input_.size()) {
            const char c = input_[pos_++];
            if (c == '"') return true;
            if (c == '\\') {
                if (pos_ == input_.size()) return false;
                const char escaped = input_[pos_++];
                if (escaped != '"' && escaped != '\\') return false;
                out.push_back(escaped);
            } else {
                if (c < 0x20 || c > 0x7e) return false;
                out.push_back(c);
            }
        }
        return false;
    }
    bool parse_key() {
        if (pos_ == input_.size() || !is_key_start(input_[pos_])) return false;
        ++pos_;
        while (pos_ < input_.size() && is_key_tail(input_[pos_])) ++pos_;
        return true;
    }
    bool parse_bare_item() {
        if (pos_ == input_.size()) return false;
        if (input_[pos_] == '"') {
            std::string ignored;
            return parse_string(ignored);
        }
        if (consume('?')) return consume('0') || consume('1');
        if (consume(':')) {
            while (pos_ < input_.size() && is_base64(input_[pos_])) ++pos_;
            if (consume('=')) consume('=');
            return consume(':');
        }
        if (input_[pos_] == '-' || is_digit(input_[pos_])) {
            consume('-');
            const auto start = pos_;
            while (pos_ < input_.size() && is_digit(input_[pos_])) ++pos_;
            if (start == pos_ || pos_ - start > 15) return false;
            if (consume('.')) {
                const auto fraction = pos_;
                while (pos_ < input_.size() && is_digit(input_[pos_])) ++pos_;
                if (fraction == pos_ || pos_ - fraction > 3 || fraction - start - 1 > 12)
                    return false;
            }
            return true;
        }
        if (!is_token_start(input_[pos_])) return false;
        ++pos_;
        while (pos_ < input_.size() && is_token_tail(input_[pos_])) ++pos_;
        return true;
    }
    bool parse_params() {
        while (consume(';')) {
            if (!parse_key()) return false;
            if (consume('=') && !parse_bare_item()) return false;
        }
        return true;
    }
    std::string_view input_;
    std::size_t pos_ = 0;
};

ConnectDecision reject(int status, std::string_view reason) {
    return {status, {}, std::string(reason)};
}

std::string_view required_protocol_for(WebTransportProfile profile) {
    return application_protocol_for(profile);
}

}  // namespace

std::string_view application_protocol_for(WebTransportProfile profile) {
    switch (profile) {
        case WebTransportProfile::Draft18Wt15: return app::alpn(app::DraftVersion::Draft18);
        case WebTransportProfile::Draft21Wt16: return app::alpn(app::DraftVersion::Draft21);
        case WebTransportProfile::Draft22Wt16: return app::alpn(app::DraftVersion::Draft22);
        case WebTransportProfile::MoqLite06: return app::alpn(app::DraftVersion::MoqLite06);
    }
    throw std::logic_error("unreachable WebTransportProfile");
}

WebTransportProfile profile_for_application_protocol(std::string_view protocol) {
    if (protocol == app::alpn(app::DraftVersion::Draft18)) return WebTransportProfile::Draft18Wt15;
    if (protocol == app::alpn(app::DraftVersion::Draft21)) return WebTransportProfile::Draft21Wt16;
    if (protocol == app::alpn(app::DraftVersion::Draft22)) return WebTransportProfile::Draft22Wt16;
    if (protocol == app::alpn(app::DraftVersion::MoqLite06)) return WebTransportProfile::MoqLite06;
    throw std::logic_error("unsupported WebTransport application protocol");
}

ConnectDecision validate_connect(const H3Request& request,
                                 const PeerCapabilities& caps,
                                 const RunEndpoint& endpoint,
                                 WebTransportProfile profile) {
    const std::string_view required_protocol = required_protocol_for(profile);
    if (endpoint.moqt_protocol != required_protocol)
        return reject(500, "run endpoint draft mismatch");
    // moq-lite runs on native WebTransport streams (draft section 4.2); datagrams are
    // optional there, so the lite profile needs only SETTINGS_WT_ENABLED.
    const bool needs_datagrams = profile != WebTransportProfile::MoqLite06;
    // The lite profile also admits a client that enabled WebTransport with a legacy identifier (PeerCapabilities).
    const bool wt_enabled = caps.wt_enabled_value == 1 ||
        (profile == WebTransportProfile::MoqLite06 && caps.legacy_webtransport);
    if (!caps.settings_received || !wt_enabled ||
        (needs_datagrams &&
         (!caps.h3_datagram || !caps.quic_datagram || !caps.reset_stream_at)))
        return reject(400, "required WebTransport capability missing");
    // The upgrade token is "webtransport-h3"; the lite profile also takes "webtransport", the token of
    // draft-ietf-webtrans-http3 before -09, which the moq CLI's stack (web-transport-proto 0.6.2) sends.
    const bool upgrade_token = request.protocol == "webtransport-h3" ||
        (profile == WebTransportProfile::MoqLite06 && request.protocol == "webtransport");
    if (request.method != "CONNECT" || !upgrade_token || request.scheme != "https")
        return reject(400, "invalid WebTransport CONNECT pseudo-header");
    if (request.authority.empty() || request.authority != endpoint.authority ||
        request.path.empty() || request.path != endpoint.path)
        return reject(404, "unknown WebTransport endpoint");

    std::string_view origin;
    std::string_view offered;
    bool seen_origin = false;
    bool seen_offered = false;
    for (const auto& [name, value] : request.headers) {
        if (name == "origin") {
            if (seen_origin) return reject(400, "duplicate Origin header");
            seen_origin = true;
            origin = value;
        } else if (name == "wt-available-protocols") {
            if (seen_offered) return reject(400, "duplicate WT-Available-Protocols header");
            seen_offered = true;
            offered = value;
        } else if (name == "wt-protocol") {
            return reject(400, "WT-Protocol is a response-only header");
        }
    }
    if ((seen_origin &&
         std::find(endpoint.allowed_origins.begin(), endpoint.allowed_origins.end(),
                   origin) == endpoint.allowed_origins.end()) ||
        (!seen_origin && endpoint.require_origin))
        return reject(403, "Origin not allowed");
    if (!seen_offered) return reject(400, "WT-Available-Protocols missing");
    std::vector<std::string> values;
    if (!SfParser(offered).parse_list(values))
        return reject(400, "WT-Available-Protocols malformed");
    if (std::find(values.begin(), values.end(), required_protocol) == values.end())
        return reject(400, "required MOQT protocol not offered");
    return {200, std::string(required_protocol), "WebTransport CONNECT accepted"};
}

namespace {

// The QUIC variable-length integer at bytes[at] (advancing `at`), or nullopt when the bytes end inside it.
std::optional<std::uint64_t> read_varint(std::span<const std::uint8_t> bytes, std::size_t& at) {
    if (at >= bytes.size()) return std::nullopt;
    const std::size_t width = std::size_t{1} << (bytes[at] >> 6u);
    if (bytes.size() - at < width) return std::nullopt;
    std::uint64_t value = bytes[at] & 0x3fu;
    for (std::size_t index = 1; index < width; ++index) value = (value << 8u) | bytes[at + index];
    at += width;
    return value;
}

}  // namespace

std::optional<bool> legacy_webtransport_settings(std::span<const std::uint8_t> prefix) {
    std::size_t at = 0;
    const auto stream_type = read_varint(prefix, at);
    if (!stream_type) return std::nullopt;
    if (*stream_type != 0x00) return false;
    const auto frame_type = read_varint(prefix, at);
    if (!frame_type) return std::nullopt;
    if (*frame_type != 0x04) return false;  // RFC 9114 6.2.1: SETTINGS is the control stream's first frame
    const auto length = read_varint(prefix, at);
    if (!length) return std::nullopt;
    if (prefix.size() - at < *length) return std::nullopt;
    const auto frame = prefix.subspan(at, static_cast<std::size_t>(*length));
    std::size_t cursor = 0;
    std::optional<std::uint64_t> datagram, datagram_deprecated, max_sessions, enable;
    while (cursor < frame.size()) {
        const auto id = read_varint(frame, cursor);
        const auto value = id ? read_varint(frame, cursor) : std::nullopt;
        if (!value) return false;
        const std::uint64_t pair[2]{*id, *value};
        switch (pair[0]) {
            case 0x33: datagram = pair[1]; break;
            case 0xffd277: datagram_deprecated = pair[1]; break;
            case 0xc671706a: max_sessions = pair[1]; break;
            case 0x2b603742: enable = pair[1]; break;
            default: break;
        }
    }
    if ((datagram ? datagram : datagram_deprecated) != std::optional<std::uint64_t>{1}) return false;
    if (max_sessions) return *max_sessions != 0;
    return enable == std::optional<std::uint64_t>{1};
}

void ClientSettingsSniff::feed(std::uint64_t stream_id, std::span<const std::uint8_t> bytes) {
    if (legacy_.has_value() || bytes.empty() || other_streams_.contains(stream_id)) return;
    auto& seen = streams_[stream_id];
    seen.insert(seen.end(), bytes.begin(), bytes.begin() +
                static_cast<std::ptrdiff_t>(std::min(bytes.size(), kMaximumBytes - seen.size())));
    if (seen.front() != 0x00) {  // a QPACK or other stream, not the control stream
        streams_.erase(stream_id);
        other_streams_.insert(stream_id);
        return;
    }
    if (const auto legacy = legacy_webtransport_settings(seen))
        decide(*legacy);
    else if (seen.size() >= kMaximumBytes)
        decide(false);
}

void ClientSettingsSniff::decide(bool legacy) {
    legacy_ = legacy;
    streams_.clear();
    other_streams_.clear();
}

}  // namespace moq::interop::transport
