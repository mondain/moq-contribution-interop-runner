#include "transport/webtransport_connect.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

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

TEST(WebTransportConnect, Draft22ProfileRequiresMoqt22Endpoint) {
    auto req = request(21);
    req.headers[1].second = "\"moqt-22\"";
    RunEndpoint ep22 = endpoint(21);
    ep22.moqt_protocol = "moqt-22";
    const auto ok = validate_connect(req, capabilities(), ep22, WebTransportProfile::Draft22Wt16);
    EXPECT_TRUE(ok.accepted()) << ok.evidence;
    EXPECT_EQ(ok.selected_protocol, "moqt-22");
    for (const char* other : {"moqt-21", "moqt-18"}) {
        RunEndpoint ep = endpoint(21);
        ep.moqt_protocol = other;
        const auto result = validate_connect(req, capabilities(), ep,
                                             WebTransportProfile::Draft22Wt16);
        EXPECT_EQ(result.http_status, 500) << other;
        EXPECT_EQ(result.evidence, "run endpoint draft mismatch") << other;
    }
    for (auto other : {WebTransportProfile::Draft18Wt15, WebTransportProfile::Draft21Wt16}) {
        const auto result = validate_connect(req, capabilities(), ep22, other);
        EXPECT_EQ(result.http_status, 500);
        EXPECT_EQ(result.evidence, "run endpoint draft mismatch");
    }
}

TEST(WebTransportConnect, Draft22ProfileDecidesLikeDraft21ForEveryOtherInput) {
    const auto compare = [](const H3Request& req, const PeerCapabilities& caps,
                            RunEndpoint ep) {
        ep.moqt_protocol = "moqt-21";
        const auto a = validate_connect(req, caps, ep, WebTransportProfile::Draft21Wt16);
        ep.moqt_protocol = "moqt-22";
        auto req22 = req;
        for (auto& header : req22.headers) {
            if (header.first != "wt-available-protocols") continue;
            const auto pos = header.second.find("moqt-21");
            if (pos != std::string::npos) header.second.replace(pos, 7, "moqt-22");
        }
        const auto b = validate_connect(req22, caps, ep, WebTransportProfile::Draft22Wt16);
        EXPECT_EQ(a.http_status, b.http_status) << a.evidence << " / " << b.evidence;
        EXPECT_EQ(a.accepted(), b.accepted());
        if (a.accepted()) EXPECT_EQ(b.selected_protocol, "moqt-22");
        else EXPECT_EQ(a.evidence, b.evidence);
    };
    const auto good = request(21);
    const auto caps = capabilities();
    const auto ep = endpoint(21);
    compare(good, caps, ep);
    auto req = good;
    req.headers[1].second = "\"other\";q=1, \"moqt-21\";v=?1";
    compare(req, caps, ep);
    const std::vector<std::function<void(H3Request&)>> changes = {
        [](H3Request& r) { r.method = "GET"; },
        [](H3Request& r) { r.protocol = "webtransport"; },
        [](H3Request& r) { r.scheme = "http"; },
        [](H3Request& r) { r.authority = "other.test"; },
        [](H3Request& r) { r.path = "/other"; },
        [](H3Request& r) { r.headers[0].second = "https://other.test"; },
        [](H3Request& r) { r.headers[1].second = "\"moqt-17\""; },
        [](H3Request& r) { r.headers[1].second = "\"moqt-18\""; },
        [](H3Request& r) { r.headers.clear(); },
        [](H3Request& r) { r.headers.push_back(r.headers[1]); },
        [](H3Request& r) { r.headers.push_back(r.headers[0]); },
        [](H3Request& r) { r.headers[1].second += " trailing"; },
        [](H3Request& r) { r.headers[1].second = "moqt-21"; },
        [](H3Request& r) { r.headers[1].second = "\"moqt-21\", 9"; },
        [](H3Request& r) { r.headers[1].second = "\"moqt-21\";p=@"; },
        [](H3Request& r) { r.headers.erase(r.headers.begin()); },
        [](H3Request& r) { r.headers.push_back({"wt-protocol", "x"}); },
    };
    for (const auto& change : changes) {
        auto r = good;
        change(r);
        compare(r, caps, ep);
    }
    auto policy = ep;
    policy.require_origin = true;
    auto no_origin = good;
    no_origin.headers.erase(no_origin.headers.begin());
    compare(no_origin, caps, policy);
    const std::vector<std::function<void(PeerCapabilities&)>> cap_changes = {
        [](PeerCapabilities& c) { c.settings_received = false; },
        [](PeerCapabilities& c) { c.wt_enabled_value = 0; },
        [](PeerCapabilities& c) { c.wt_enabled_value = 2; },
        [](PeerCapabilities& c) { c.h3_datagram = false; },
        [](PeerCapabilities& c) { c.quic_datagram = false; },
        [](PeerCapabilities& c) { c.reset_stream_at = false; },
    };
    for (const auto& change : cap_changes) {
        auto c = caps;
        change(c);
        compare(good, c, ep);
    }
}

H3Request lite_request(const std::string& offers) {
    return {"CONNECT", "webtransport-h3", "https", "runner.test:4433", "/moq",
            {{"origin", "https://publisher.test"}, {"wt-available-protocols", offers}}};
}

RunEndpoint lite_endpoint(const std::string& protocol) {
    return {"runner.test:4433", "/moq", {"https://publisher.test"}, protocol};
}

TEST(WebTransportConnectMoqLite, AnswersTheOfferedMoqLiteProtocol) {
    const auto result = validate_connect(lite_request("\"other\", \"moq-lite-06\""), capabilities(),
                                         lite_endpoint("moq-lite-06"), WebTransportProfile::MoqLite06);
    EXPECT_TRUE(result.accepted()) << result.evidence;
    EXPECT_EQ(result.selected_protocol, "moq-lite-06");
}

TEST(WebTransportConnectMoqLite, RefusesUnknownAndMoqTransportOffers) {
    for (const char* offers : {"\"moq-lite-07\"", "\"moqt-22\"", "\"moqt-21\""}) {
        const auto result = validate_connect(lite_request(offers), capabilities(),
                                             lite_endpoint("moq-lite-06"), WebTransportProfile::MoqLite06);
        EXPECT_FALSE(result.accepted()) << offers;
        EXPECT_TRUE(result.selected_protocol.empty());
    }
}

TEST(WebTransportConnectMoqLite, ProfileAndEndpointMustAgree) {
    EXPECT_FALSE(validate_connect(lite_request("\"moq-lite-06\""), capabilities(),
                                  lite_endpoint("moqt-22"), WebTransportProfile::MoqLite06).accepted());
    EXPECT_FALSE(validate_connect(lite_request("\"moq-lite-06\""), capabilities(),
                                  lite_endpoint("moq-lite-06"), WebTransportProfile::Draft22Wt16).accepted());
}

TEST(WebTransportConnectMoqLite, MoqTransportProfilesStillAnswerOnlyTheirOwnProtocol) {
    const auto d22 = validate_connect(lite_request("\"moqt-22\", \"moq-lite-06\""), capabilities(),
                                      lite_endpoint("moqt-22"), WebTransportProfile::Draft22Wt16);
    EXPECT_TRUE(d22.accepted());
    EXPECT_EQ(d22.selected_protocol, "moqt-22");
    EXPECT_FALSE(validate_connect(lite_request("\"moq-lite-06\""), capabilities(),
                                  lite_endpoint("moqt-22"), WebTransportProfile::Draft22Wt16).accepted());
}

// Draft section 4.2: the lite WebTransport binding uses native WebTransport streams only;
// datagrams are optional there, so only SETTINGS_WT_ENABLED is a capability requirement.
PeerCapabilities wt_only_capabilities() {
    return {true, 1, false, false, false};
}

TEST(WebTransportConnectMoqLite, AcceptsAConnectWithoutDatagramOrResetStreamAtSettings) {
    const auto result = validate_connect(lite_request("\"moq-lite-06\""), wt_only_capabilities(),
                                         lite_endpoint("moq-lite-06"), WebTransportProfile::MoqLite06);
    EXPECT_TRUE(result.accepted()) << result.evidence;
    EXPECT_EQ(result.selected_protocol, "moq-lite-06");
}

TEST(WebTransportConnectMoqLite, StillRequiresWebTransportSettings) {
    for (int variant = 0; variant < 3; ++variant) {
        auto caps = wt_only_capabilities();
        if (variant == 0) caps.settings_received = false;
        if (variant == 1) caps.wt_enabled_value = 0;
        if (variant == 2) caps.wt_enabled_value = 2;
        const auto result = validate_connect(lite_request("\"moq-lite-06\""), caps,
                                             lite_endpoint("moq-lite-06"), WebTransportProfile::MoqLite06);
        EXPECT_FALSE(result.accepted()) << variant;
        EXPECT_EQ(result.http_status, 400);
    }
}

TEST(WebTransportConnectMoqLite, MoqTransportProfilesStillRefuseTheSameConnect) {
    const struct { WebTransportProfile profile; const char* protocol; } cases[] = {
        {WebTransportProfile::Draft18Wt15, "moqt-18"},
        {WebTransportProfile::Draft21Wt16, "moqt-21"},
        {WebTransportProfile::Draft22Wt16, "moqt-22"}};
    for (const auto& c : cases) {
        const auto offer = std::string("\"") + c.protocol + "\"";
        EXPECT_FALSE(validate_connect(lite_request(offer), wt_only_capabilities(),
                                      lite_endpoint(c.protocol), c.profile).accepted()) << c.protocol;
        EXPECT_TRUE(validate_connect(lite_request(offer), capabilities(),
                                     lite_endpoint(c.protocol), c.profile).accepted()) << c.protocol;
    }
}

TEST(WebTransportConnectMoqLite, MixedOffersResolveToTheConfiguredProtocol) {
    const auto lite = validate_connect(lite_request("\"moqt-22\", \"moq-lite-06\""), wt_only_capabilities(),
                                       lite_endpoint("moq-lite-06"), WebTransportProfile::MoqLite06);
    EXPECT_TRUE(lite.accepted()) << lite.evidence;
    EXPECT_EQ(lite.selected_protocol, "moq-lite-06");
    const auto d22 = validate_connect(lite_request("\"moq-lite-06\", \"moqt-22\""), capabilities(),
                                      lite_endpoint("moqt-22"), WebTransportProfile::Draft22Wt16);
    EXPECT_TRUE(d22.accepted()) << d22.evidence;
    EXPECT_EQ(d22.selected_protocol, "moqt-22");
}

TEST(WebTransportConnectMoqLite, ProtocolMapsToTheProfileBothWays) {
    EXPECT_EQ(profile_for_application_protocol("moq-lite-06"), WebTransportProfile::MoqLite06);
    EXPECT_EQ(profile_for_application_protocol("moqt-22"), WebTransportProfile::Draft22Wt16);
    EXPECT_EQ(application_protocol_for(WebTransportProfile::MoqLite06), "moq-lite-06");
    EXPECT_EQ(application_protocol_for(WebTransportProfile::Draft18Wt15), "moqt-18");
    EXPECT_THROW(profile_for_application_protocol("moq-lite-07"), std::logic_error);
}

}  // namespace
