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

// The moq CLI's WebTransport stack (web-transport-proto 0.6.2) enables WebTransport with the pre-WT_ENABLED
// identifiers only (L1e Task 4 live smoke).
PeerCapabilities legacy_only_capabilities() {
    PeerCapabilities caps{true, 0, true, true, false};
    caps.legacy_webtransport = true;
    return caps;
}

TEST(WebTransportConnectMoqLite, AdmitsALegacyWebTransportClient) {
    const auto result = validate_connect(lite_request("\"moq-lite-06\""), legacy_only_capabilities(),
                                         lite_endpoint("moq-lite-06"), WebTransportProfile::MoqLite06);
    EXPECT_TRUE(result.accepted()) << result.evidence;
    auto no_settings = legacy_only_capabilities();
    no_settings.settings_received = false;
    EXPECT_FALSE(validate_connect(lite_request("\"moq-lite-06\""), no_settings, lite_endpoint("moq-lite-06"),
                                  WebTransportProfile::MoqLite06).accepted());
}

TEST(WebTransportConnectMoqLite, MoqTransportProfilesIgnoreLegacyWebTransport) {
    const struct { WebTransportProfile profile; const char* protocol; } cases[] = {
        {WebTransportProfile::Draft18Wt15, "moqt-18"},
        {WebTransportProfile::Draft21Wt16, "moqt-21"},
        {WebTransportProfile::Draft22Wt16, "moqt-22"}};
    for (const auto& c : cases) {
        const auto offer = std::string("\"") + c.protocol + "\"";
        auto caps = legacy_only_capabilities();
        caps.reset_stream_at = true;
        const auto result = validate_connect(lite_request(offer), caps, lite_endpoint(c.protocol), c.profile);
        EXPECT_FALSE(result.accepted()) << c.protocol;
        EXPECT_EQ(result.http_status, 400) << c.protocol;
        // With SETTINGS_WT_ENABLED the flag changes nothing either.
        auto both = capabilities();
        both.legacy_webtransport = true;
        EXPECT_EQ(validate_connect(lite_request(offer), both, lite_endpoint(c.protocol), c.profile).http_status,
                  validate_connect(lite_request(offer), capabilities(), lite_endpoint(c.protocol), c.profile)
                      .http_status) << c.protocol;
    }
}

TEST(WebTransportConnectMoqLite, AcceptsTheLegacyUpgradeTokenOnLiteOnly) {
    auto legacy = lite_request("\"moq-lite-06\"");
    legacy.protocol = "webtransport";
    const auto lite = validate_connect(legacy, legacy_only_capabilities(), lite_endpoint("moq-lite-06"),
                                       WebTransportProfile::MoqLite06);
    EXPECT_TRUE(lite.accepted()) << lite.evidence;
    for (const char* token : {"webtransport-h4", "WebTransport", "", "connect-udp"}) {
        auto other = legacy;
        other.protocol = token;
        EXPECT_FALSE(validate_connect(other, legacy_only_capabilities(), lite_endpoint("moq-lite-06"),
                                      WebTransportProfile::MoqLite06).accepted()) << token;
    }
    const struct { WebTransportProfile profile; const char* protocol; } cases[] = {
        {WebTransportProfile::Draft18Wt15, "moqt-18"},
        {WebTransportProfile::Draft21Wt16, "moqt-21"},
        {WebTransportProfile::Draft22Wt16, "moqt-22"}};
    for (const auto& c : cases) {
        auto request = lite_request(std::string("\"") + c.protocol + "\"");
        request.protocol = "webtransport";
        const auto result = validate_connect(request, capabilities(), lite_endpoint(c.protocol), c.profile);
        EXPECT_FALSE(result.accepted()) << c.protocol;
        EXPECT_EQ(result.http_status, 400) << c.protocol;
    }
}

std::vector<std::uint8_t> control_stream(const std::vector<std::uint8_t>& components) {
    std::vector<std::uint8_t> bytes{0x00, 0x04, static_cast<std::uint8_t>(components.size())};
    bytes.insert(bytes.end(), components.begin(), components.end());
    return bytes;
}

// Encoded components: 0x33 (H3_DATAGRAM), 0x80ffd277 (deprecated datagram), 0xc0000000c671706a (MAX_SESSIONS),
// 0xab603742 (ENABLE_WEBTRANSPORT), 0xac7cf000 (WT_ENABLED), 0x08 (ENABLE_CONNECT_PROTOCOL).
const std::vector<std::uint8_t> kDatagram{0x33, 0x01};
const std::vector<std::uint8_t> kDatagramDeprecated{0x80, 0xff, 0xd2, 0x77, 0x01};
const std::vector<std::uint8_t> kMaxSessions1{0xc0, 0x00, 0x00, 0x00, 0xc6, 0x71, 0x70, 0x6a, 0x01};
const std::vector<std::uint8_t> kMaxSessions0{0xc0, 0x00, 0x00, 0x00, 0xc6, 0x71, 0x70, 0x6a, 0x00};
const std::vector<std::uint8_t> kEnable1{0xab, 0x60, 0x37, 0x42, 0x01};
const std::vector<std::uint8_t> kWtEnabled1{0xac, 0x7c, 0xf0, 0x00, 0x01};
const std::vector<std::uint8_t> kConnect{0x08, 0x01};

std::vector<std::uint8_t> join(std::initializer_list<std::vector<std::uint8_t>> parts) {
    std::vector<std::uint8_t> out;
    for (const auto& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

TEST(WebTransportLegacySettings, RecognizesTheMoqCliSettings) {
    EXPECT_EQ(legacy_webtransport_settings(control_stream(join({kMaxSessions1, kConnect, kDatagramDeprecated,
                                                                 kDatagram, kEnable1}))),
              std::optional<bool>{true});
    EXPECT_EQ(legacy_webtransport_settings(control_stream(join({kDatagramDeprecated, kEnable1}))),
              std::optional<bool>{true});
}

TEST(WebTransportLegacySettings, RefusesWithoutDatagramOrWithZeroValues) {
    EXPECT_EQ(legacy_webtransport_settings(control_stream(join({kMaxSessions1}))), std::optional<bool>{false});
    // A present MAX_SESSIONS decides, as in web-transport-proto: 0 disables even with ENABLE = 1.
    EXPECT_EQ(legacy_webtransport_settings(control_stream(join({kDatagram, kMaxSessions0, kEnable1}))),
              std::optional<bool>{false});
    // SETTINGS_WT_ENABLED alone is not a legacy identifier (h3zero decodes it).
    EXPECT_EQ(legacy_webtransport_settings(control_stream(join({kDatagram, kWtEnabled1}))),
              std::optional<bool>{false});
}

TEST(WebTransportLegacySettings, IncompleteAndOtherStreams) {
    const auto full = control_stream(join({kDatagram, kMaxSessions1}));
    for (std::size_t size = 0; size < full.size(); ++size)
        EXPECT_EQ(legacy_webtransport_settings(std::span(full).first(size)), std::nullopt) << size;
    EXPECT_EQ(legacy_webtransport_settings(std::vector<std::uint8_t>{0x02}), std::optional<bool>{false});
    EXPECT_EQ(legacy_webtransport_settings(std::vector<std::uint8_t>{0x00, 0x07, 0x00}),
              std::optional<bool>{false});
    // A component cut by the frame length is malformed.
    EXPECT_EQ(legacy_webtransport_settings(std::vector<std::uint8_t>{0x00, 0x04, 0x03, 0x33, 0x01, 0x80}),
              std::optional<bool>{false});
}

TEST(WebTransportClientSettingsSniff, DecidesFromTheControlStreamAcrossChunks) {
    const auto full = control_stream(join({kDatagram, kMaxSessions1}));
    ClientSettingsSniff sniff;
    sniff.feed(2, std::span(full).first(3));
    EXPECT_EQ(sniff.legacy(), std::nullopt);
    sniff.feed(6, std::vector<std::uint8_t>{0x02, 0x00});  // a QPACK encoder stream
    sniff.feed(2, std::span(full).subspan(3));
    EXPECT_EQ(sniff.legacy(), std::optional<bool>{true});
    EXPECT_EQ(sniff.tracked_streams(), 0u);
    sniff.feed(10, std::vector<std::uint8_t>{0x54, 0x00});  // after the decision nothing is kept
    EXPECT_EQ(sniff.tracked_streams(), 0u);
}

// A control stream whose SETTINGS frame never completes within the bound decides "no legacy WebTransport" and
// stops tracking every stream (other_streams no longer grows with each new client stream).
TEST(WebTransportClientSettingsSniff, ACappedIncompleteControlStreamDecidesFalseAndStopsTracking) {
    ClientSettingsSniff sniff;
    sniff.feed(2, std::vector<std::uint8_t>{0x00, 0x04, 0x44, 0x00});  // SETTINGS of length 0x400: never complete
    sniff.feed(6, std::vector<std::uint8_t>{0x54});
    EXPECT_EQ(sniff.tracked_streams(), 2u);
    const std::vector<std::uint8_t> padding(ClientSettingsSniff::kMaximumBytes, 0x21);
    sniff.feed(2, padding);
    EXPECT_EQ(sniff.legacy(), std::optional<bool>{false});
    EXPECT_EQ(sniff.tracked_streams(), 0u);
    for (std::uint64_t id = 14; id < 14 + 4 * 64; id += 4) sniff.feed(id, std::vector<std::uint8_t>{0x54});
    EXPECT_EQ(sniff.tracked_streams(), 0u);
}

TEST(WebTransportClientSettingsSniff, IgnoresEmptyChunks) {
    ClientSettingsSniff sniff;
    sniff.feed(2, std::span<const std::uint8_t>{});
    EXPECT_EQ(sniff.tracked_streams(), 0u);
    EXPECT_EQ(sniff.legacy(), std::nullopt);
}

}  // namespace
