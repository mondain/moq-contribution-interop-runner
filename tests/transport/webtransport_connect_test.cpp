#include "transport/webtransport_connect.h"

#include <gtest/gtest.h>

using namespace moq::interop::transport;

namespace {

H3Request request(int draft) {
    return {"CONNECT", "webtransport-h3", "https", "runner.test:4433", "/moq",
            {{"origin", "https://publisher.test"},
             {"wt-available-protocols", draft == 18 ? "\"moqt-18\"" : "\"moqt-21\""}}};
}

RunEndpoint endpoint(int draft) {
    return {"runner.test:4433", "/moq", {"https://publisher.test"},
            draft == 18 ? "moqt-18" : "moqt-21"};
}

PeerCapabilities capabilities() {
    return {true, 1, true, true, true};
}

WebTransportProfile profile(int draft) {
    return draft == 18 ? WebTransportProfile::Draft18Wt15 :
                         WebTransportProfile::Draft21Wt16;
}

TEST(WebTransportConnect, AcceptsExactProfilesAndMultipleOffers) {
    for (int draft : {18, 21}) {
        auto req = request(draft);
        req.headers[1].second = "\"other\";q=1, \"moqt-" + std::to_string(draft) + "\";v=?1";
        const auto result = validate_connect(req, capabilities(), endpoint(draft), profile(draft));
        EXPECT_TRUE(result.accepted()) << result.evidence;
        EXPECT_EQ(result.http_status, 200);
        EXPECT_EQ(result.selected_protocol, "moqt-" + std::to_string(draft));
    }
}

TEST(WebTransportConnect, RejectsWrongRequestAndProtocol) {
    for (int draft : {18, 21}) {
        const auto good = request(draft);
        const auto ep = endpoint(draft);
        const auto caps = capabilities();
        auto reject = [&](auto change) {
            auto req = good;
            change(req);
            const auto result = validate_connect(req, caps, ep, profile(draft));
            EXPECT_FALSE(result.accepted()) << result.evidence;
            EXPECT_GE(result.http_status, 400);
            EXPECT_TRUE(result.selected_protocol.empty());
        };
        reject([](auto& r) { r.method = "GET"; });
        reject([](auto& r) { r.protocol = "webtransport"; });
        reject([](auto& r) { r.scheme = "http"; });
        reject([](auto& r) { r.authority = "other.test"; });
        reject([](auto& r) { r.path = "/other"; });
        reject([](auto& r) { r.headers[0].second = "https://other.test"; });
        reject([](auto& r) { r.headers[1].second = "\"moqt-17\""; });
        reject([draft](auto& r) { r.headers[1].second = draft == 18 ? "\"moqt-21\"" : "\"moqt-18\""; });
    }
}

TEST(WebTransportConnect, RejectsAbsentDuplicateAndMalformedFields) {
    for (int draft : {18, 21}) {
        const auto ep = endpoint(draft);
        const auto caps = capabilities();
        auto reject = [&](auto change) {
            auto req = request(draft);
            change(req);
            EXPECT_FALSE(validate_connect(req, caps, ep, profile(draft)).accepted());
        };
        reject([](auto& r) { r.headers.clear(); });
        reject([](auto& r) { r.headers.push_back(r.headers[1]); });
        reject([](auto& r) { r.headers.push_back(r.headers[0]); });
        reject([](auto& r) { r.headers[1].second += " trailing"; });
        reject([draft](auto& r) { r.headers[1].second = "moqt-" + std::to_string(draft); });
        reject([draft](auto& r) { r.headers[1].second = "\"moqt-" + std::to_string(draft) + "\", 9"; });
        reject([draft](auto& r) { r.headers[1].second = "\"moqt-" + std::to_string(draft) + "\";p=@"; });
        reject([draft](auto& r) { r.headers[1].second = "\"moqt-" + std::to_string(draft) + "\"\nInjected: true"; });
    }
}

TEST(WebTransportConnect, RequiresDraftCapabilityAndDatagrams) {
    for (int draft : {18, 21}) {
        auto caps = capabilities();
        const auto req = request(draft);
        const auto ep = endpoint(draft);
        auto reject = [&]() {
            const auto result = validate_connect(req, caps, ep, profile(draft));
            EXPECT_FALSE(result.accepted()) << result.evidence;
            EXPECT_GE(result.http_status, 400);
        };
        caps.settings_received = false; reject(); caps = capabilities();
        caps.wt_enabled_value = 0; reject(); caps = capabilities();
        caps.wt_enabled_value = 2; reject(); caps = capabilities();
        caps.h3_datagram = false; reject(); caps = capabilities();
        caps.quic_datagram = false; reject(); caps = capabilities();
        caps.reset_stream_at = false; reject();
    }
}

TEST(WebTransportConnect, NeverReflectsUnsafeInputInEvidence) {
    auto req = request(21);
    req.path = "/bad\r\nX-Injected: yes";
    const auto result = validate_connect(req, capabilities(), endpoint(21), profile(21));
    EXPECT_FALSE(result.accepted());
    EXPECT_LE(result.evidence.size(), 160u);
    EXPECT_EQ(result.evidence.find('\n'), std::string::npos);
    EXPECT_EQ(result.evidence.find("X-Injected"), std::string::npos);
}

TEST(WebTransportConnect, OriginIsOptionalForNonBrowserClientButCheckedIfPresent) {
    auto req = request(21);
    req.headers.erase(req.headers.begin());
    EXPECT_TRUE(validate_connect(req, capabilities(), endpoint(21),
                                 profile(21)).accepted());
    auto policy = endpoint(21);
    policy.require_origin = true;
    EXPECT_FALSE(validate_connect(req, capabilities(), policy,
                                  profile(21)).accepted());
}

}  // namespace
