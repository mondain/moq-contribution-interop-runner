#include "draft18_contribution_internal.h"

namespace moq::interop::scenarios::contribution {
namespace {

constexpr std::uint64_t kOptionPath = 0x01;
constexpr std::uint64_t kOptionAuthority = 0x05;
constexpr std::string_view kQueryScenario = "connect-publisher-to-native-uri-with-query";
constexpr std::string_view kQuery = "interop=1";

struct UriParts {
    std::string authority;
    std::string path;
    std::optional<std::string> query;
};

// RFC 3986 split of a moqt:// URI into authority, path-abempty and query.
std::optional<UriParts> split_uri(const std::string& uri) {
    const auto scheme = uri.find("://");
    if (scheme == std::string::npos) return std::nullopt;
    UriParts parts;
    auto rest = std::string_view(uri).substr(scheme + 3);
    if (const auto fragment = rest.find('#'); fragment != std::string_view::npos) rest = rest.substr(0, fragment);
    const auto end_of_authority = rest.find_first_of("/?");
    parts.authority = std::string(rest.substr(0, end_of_authority));
    if (end_of_authority == std::string_view::npos) return parts;
    rest = rest.substr(end_of_authority);
    const auto query = rest.find('?');
    parts.path = std::string(rest.substr(0, query));
    if (query != std::string_view::npos) parts.query = std::string(rest.substr(query + 1));
    return parts;
}

// The single value of a Setup Option, or nothing if it is absent or repeated.
std::optional<std::string> option_value(const d18::KeyValuePairs& options, std::uint64_t type) {
    std::optional<std::string> found;
    for (const auto& option : options) {
        if (option.type != type) continue;
        const auto* bytes = std::get_if<d18::ByteValue>(&option.value);
        if (!bytes || found) return std::nullopt;
        found = std::string(reinterpret_cast<const char*>(bytes->bytes.data()), bytes->bytes.size());
    }
    return found;
}

enum class Component { Authority, Path, PathWithQuery };

// Sections 10.3.1.1 and 10.3.1.2: a native QUIC client repeats the authority,
// path-abempty and (after a question mark) query of the URI it connected to.
std::optional<bool> setup_matches_uri(const RawProbeTranscript& transcript, bool webtransport,
                                      Component component) {
    if (webtransport || !bounded(transcript) || !transcript.connection_uri) return std::nullopt;
    const auto uri = split_uri(*transcript.connection_uri);
    const auto control = peer_control(transcript);
    if (!uri || !control) return std::nullopt;
    if (component == Component::Authority)
        return option_value(control->setup_options, kOptionAuthority) == std::optional<std::string>{uri->authority};
    std::string expected = uri->path;
    if (component == Component::PathWithQuery) {
        if (!uri->query) return std::nullopt;
        expected += "?" + *uri->query;
    } else if (uri->query) {
        return std::nullopt;  // the query-bearing URI belongs to the query scenario
    }
    return option_value(control->setup_options, kOptionPath) == std::optional<std::string>{expected};
}

}  // namespace

std::vector<Draft18ContributionProbe> uri_probes(std::chrono::milliseconds deadline) {
    std::vector<Draft18ContributionProbe> result;
    const auto add = [&](const char* requirement, const char* evaluator, const char* id, Component component) {
        result.push_back(make_probe(requirement, evaluator,
            RawProbeDefinition{id, setup_message({}), {}, true, setup_ready, deadline,
                [](const RawProbeTranscript& transcript) { return transcript.peer_setup_received; }, {}},
            [component](const RawProbeTranscript& transcript, bool webtransport) {
                return setup_matches_uri(transcript, webtransport, component);
            }));
    };
    add("D18-10-3-1-1-MUST-004", "setup-authority-matches-connection-uri",
        "connect-publisher-to-native-uri-with-authority", Component::Authority);
    add("D18-10-3-1-2-MUST-004", "setup-path-matches-uri-path-component",
        "connect-publisher-to-native-uri-with-path", Component::Path);
    add("D18-10-3-1-2-MUST-005", "setup-path-includes-question-mark-and-query",
        "connect-publisher-to-native-uri-with-query", Component::PathWithQuery);
    return result;
}

}  // namespace moq::interop::scenarios::contribution

namespace moq::interop::scenarios {
std::string_view draft18_contribution_connection_query(std::string_view id) {
    return id == contribution::kQueryScenario ? contribution::kQuery : std::string_view{};
}
}  // namespace moq::interop::scenarios
