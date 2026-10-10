#include "lite_ref_options.h"

#include <array>
#include <charconv>
#include <utility>

namespace moq::interop::lite_ref {
namespace {

using test::lite::LiteDefect;
namespace l06 = wire::moqlite06;

constexpr std::array<std::pair<LiteDefect, std::string_view>, 19> kDefects{{
    {LiteDefect::NoSetupStream, "no-setup-stream"},
    {LiteDefect::SilentOnAnnounce, "silent-on-announce"},
    {LiteDefect::IgnoreUnknownStreams, "ignore-unknown-streams"},
    {LiteDefect::CloseOnInvalidSubscribe, "close-on-invalid-subscribe"},
    {LiteDefect::OffsetGroupStart, "offset-group-start"},
    {LiteDefect::FetchTruncatesOnShortRange, "fetch-truncates-on-short-range"},
    {LiteDefect::FetchIgnoresUnknownGroup, "fetch-ignores-unknown-group"},
    {LiteDefect::TrackInfoChangesBetweenRequests, "track-info-changes-between-requests"},
    {LiteDefect::TrackInfoZeroTimescale, "track-info-zero-timescale"},
    {LiteDefect::ProbeResetsOnTarget, "probe-resets-on-target"},
    {LiteDefect::ProbeNoneNotReset, "probe-none-not-reset"},
    {LiteDefect::GoawayOversizeLogged, "goaway-oversize-logged"},
    {LiteDefect::GoawayDuplicateIgnored, "goaway-duplicate-ignored"},
    {LiteDefect::GoawayClosesSessionOnFirst, "goaway-closes-session-on-first"},
    {LiteDefect::OpensStreamsAfterGoaway, "opens-streams-after-goaway"},
    {LiteDefect::DatagramOversize, "datagram-oversize"},
    {LiteDefect::DatagramUnknownSubscribeId, "datagram-unknown-subscribe-id"},
    {LiteDefect::DatagramDiffersFromStream, "datagram-differs-from-stream"},
    {LiteDefect::DatagramOnly, "datagram-only"},
}};

// The conforming configuration the live and in-process conformance runs use (tests/support/lite_conformance.h), on the
// fixture the adapters pin. Probe level Report; four frames per group so FETCH has the ranges it needs.
test::lite::ConformingLitePublisherConfig reference_config() {
    test::lite::ConformingLitePublisherConfig config;
    config.broadcast = std::string(kFixtureBroadcast);
    config.track = std::string(kFixtureTrack);
    config.hop_id = 7;
    config.setup_parameters = {{l06::kParamHop, std::vector<std::byte>{std::byte{7}}},
                               {l06::kParamCost, std::vector<std::byte>{std::byte{0}}},
                               {l06::kParamProbe, std::vector<std::byte>{std::byte{1}}}};
    config.frames_per_group = 4;
    config.fetch_frames_per_group = 4;
    config.fetch_last_group = 1000;
    return config;
}

bool set_probe_level(test::lite::ConformingLitePublisherConfig& config, std::string_view level) {
    std::byte value{};
    if (level == "none") value = std::byte{0};
    else if (level == "report") value = std::byte{1};
    else if (level == "increase") value = std::byte{2};
    else return false;
    std::erase_if(config.setup_parameters, [](const l06::SetupParameter& p) { return p.id == l06::kParamProbe; });
    if (level != "none") config.setup_parameters.push_back({l06::kParamProbe, std::vector<std::byte>{value}});
    return true;
}

// scheme://host:port/path?query -> binding, path, query.
bool split_endpoint(std::string_view url, test::lite::ConformingLitePublisherConfig& config) {
    scenarios::LiteBinding binding;
    if (url.starts_with("moql://")) {
        binding = scenarios::LiteBinding::NativeQuic;
        url.remove_prefix(7);
    } else if (url.starts_with("https://")) {
        binding = scenarios::LiteBinding::WebTransport;
        url.remove_prefix(8);
    } else {
        return false;
    }
    const auto slash = url.find('/');
    if (slash == std::string_view::npos || slash == 0) return false;
    auto rest = url.substr(slash);
    const auto question = rest.find('?');
    config.binding = binding;
    config.session_url_path = std::string(rest.substr(0, question));
    config.session_url_query = question == std::string_view::npos ? "" : std::string(rest.substr(question + 1));
    return true;
}

}  // namespace

std::string_view defect_name(LiteDefect defect) {
    if (defect == LiteDefect::None) return "none";
    for (const auto& [value, name] : kDefects)
        if (value == defect) return name;
    return "none";
}

std::optional<LiteDefect> defect_from_name(std::string_view name) {
    if (name == "none") return LiteDefect::None;
    for (const auto& [value, text] : kDefects)
        if (text == name) return value;
    return std::nullopt;
}

std::vector<std::string_view> all_defect_names() {
    std::vector<std::string_view> names;
    for (const auto& [value, name] : kDefects) names.push_back(name);
    return names;
}

std::string usage() {
    std::string text =
        "usage: moq-interop-lite-ref-publisher --connect URL [--defect NAME] [--datagrams] "
        "[--probe-level none|report|increase] [--frames-per-group N] [--groups N] [--group-interval-polls N] [--retract-after-polls N]\n"
        "  URL is moql://HOST:PORT/PATH?QUERY (native QUIC) or https://HOST:PORT/PATH?QUERY (WebTransport).\n"
        "  defects:";
    for (const auto name : all_defect_names()) text += " " + std::string(name);
    return text + "\n";
}

ParseResult parse_options(std::span<const std::string_view> args) {
    Options options;
    options.publisher = reference_config();
    options.defect = "none";
    bool have_connect = false;
    const auto fail = [](std::string message) { return ParseResult{std::nullopt, std::move(message)}; };
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto flag = args[i];
        const auto value = [&]() -> std::optional<std::string_view> {
            return i + 1 < args.size() ? std::optional<std::string_view>{args[++i]} : std::nullopt;
        };
        if (flag == "--help") {
            options.help = true;
        } else if (flag == "--datagrams") {
            options.publisher.datagrams = true;
        } else if (flag == "--connect") {
            const auto v = value();
            if (!v) return fail("--connect needs a URL");
            if (!split_endpoint(*v, options.publisher))
                return fail("--connect must be moql://HOST:PORT/PATH or https://HOST:PORT/PATH, got '" + std::string(*v) + "'");
            options.connect = std::string(*v);
            have_connect = true;
        } else if (flag == "--defect") {
            const auto v = value();
            if (!v) return fail("--defect needs a name");
            const auto defect = defect_from_name(*v);
            if (!defect) return fail("unknown defect '" + std::string(*v) + "'");
            options.publisher.defect = *defect;
            options.defect = std::string(*v);
        } else if (flag == "--probe-level") {
            const auto v = value();
            if (!v || !set_probe_level(options.publisher, *v)) return fail("--probe-level must be none, report or increase");
        } else if (flag == "--groups" || flag == "--group-interval-polls" || flag == "--retract-after-polls") {
            const auto v = value();
            std::size_t count = 0;
            if (!v || std::from_chars(v->data(), v->data() + v->size(), count).ec != std::errc{} || count == 0)
                return fail(std::string(flag) + " needs a positive number");
            if (flag == "--groups") options.publisher.groups_per_subscription = count;
            else if (flag == "--group-interval-polls") options.publisher.group_period_polls = count;
            else options.publisher.retract_after_polls = count;
        } else if (flag == "--frames-per-group") {
            const auto v = value();
            std::size_t count = 0;
            if (!v || std::from_chars(v->data(), v->data() + v->size(), count).ec != std::errc{} || count == 0)
                return fail("--frames-per-group needs a positive number");
            options.publisher.frames_per_group = count;
        } else {
            return fail("unknown argument '" + std::string(flag) + "'");
        }
    }
    if (options.help) return {std::move(options), {}};
    if (!have_connect) return fail("--connect is required");
    return {std::move(options), {}};
}

}  // namespace moq::interop::lite_ref
