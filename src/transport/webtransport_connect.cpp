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
    if (!caps.settings_received || caps.wt_enabled_value != 1 ||
        (needs_datagrams &&
         (!caps.h3_datagram || !caps.quic_datagram || !caps.reset_stream_at)))
        return reject(400, "required WebTransport capability missing");
    if (request.method != "CONNECT" || request.protocol != "webtransport-h3" ||
        request.scheme != "https")
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

}  // namespace moq::interop::transport
