#include "draft18_contribution_internal.h"
#include "moq/interop/scenarios/fetch_first_object.h"

#include <set>

namespace moq::interop::scenarios {
namespace contribution {
namespace {
std::optional<Fixture> fixture_from_transcript(const RawProbeTranscript& transcript) {
    if (transcript.writes.empty()) return std::nullopt;
    const auto& first = transcript.writes.front().write.bytes;
    if (first.size() > kMaximumFrame) return std::nullopt;
    wire::Cursor cursor(first);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto* message = std::get_if<d18::Message>(&decoded);
    if (!message) return std::nullopt;
    if (const auto* subscribe = std::get_if<d18::SubscribeMessage>(message))
        return Fixture{subscribe->track_namespace.fields, subscribe->track_name.bytes};
    if (const auto* discovery = std::get_if<d18::SubscribeNamespaceMessage>(message))
        return Fixture{discovery->track_namespace_prefix.fields, {std::byte{'x'}}};
    if (const auto* tracks = std::get_if<d18::SubscribeTracksMessage>(message))
        return Fixture{tracks->track_namespace_prefix.fields, {std::byte{'x'}}};
    if (const auto* fetch = std::get_if<d18::FetchMessage>(message))
        if (const auto* standalone = std::get_if<d18::StandaloneFetch>(&fetch->fetch))
            return Fixture{standalone->track_namespace.fields, standalone->track_name.bytes};
    return std::nullopt;
}
}  // namespace
}  // namespace contribution

std::vector<Draft18ContributionProbe> draft18_contribution_probes(
    std::chrono::milliseconds deadline, std::vector<std::vector<std::byte>> track_namespace,
    std::vector<std::byte> track_name, Draft18TokenCredentials credentials) {
    if (deadline.count() <= 0)
        throw std::invalid_argument("invalid draft-18 contribution deadline");
    const contribution::Fixture fixture{std::move(track_namespace), std::move(track_name)};
    std::vector<Draft18ContributionProbe> result;
    const auto append = [&](std::vector<Draft18ContributionProbe> probes) {
        for (auto& probe : probes) result.push_back(std::move(probe));
    };
    append(contribution::setup_probes(deadline));
    append(contribution::subscription_probes(deadline, fixture));
    append(contribution::publisher_initiated_probes(deadline));
    append(contribution::object_probes(deadline, fixture));
    append(contribution::goaway_probes(deadline));
    append(contribution::uri_probes(deadline));
    append(contribution::closure_probes(deadline, fixture));
    append(contribution::exchange_probes(deadline, fixture));
    append(contribution::origination_probes(deadline, fixture));
    append(contribution::token_probes(deadline, fixture, credentials));
    return result;
}

bool draft18_contribution_fixture_valid(
    const std::vector<std::vector<std::byte>>& track_namespace,
    const std::vector<std::byte>& track_name) {
    return fetch_first_object_fixture_valid(track_namespace, track_name);
}

bool draft18_contribution_scenario(std::string_view id) {
    static const std::set<std::string, std::less<>> known = [] {
        std::set<std::string, std::less<>> ids;
        for (const auto& probe : draft18_contribution_probes()) ids.insert(probe.definition.id);
        return ids;
    }();
    return known.contains(id);
}

bool draft18_contribution_requires_track(std::string_view id) {
    static const std::set<std::string, std::less<>> tracked = [] {
        std::set<std::string, std::less<>> ids;
        for (const auto& probe : draft18_contribution_probes())
            if (probe.requires_track) ids.insert(probe.definition.id);
        return ids;
    }();
    return tracked.contains(id);
}

std::optional<bool> evaluate_draft18_contribution_probe(
    const RawProbeTranscript& transcript, const Draft18ContributionProbe& profile,
    bool webtransport) {
    contribution::Fixture fixture{{}, {std::byte{'x'}}};
    // A context that sends nothing carries no request to recover the fixture from,
    // and its stimulus does not depend on the fixture.
    const bool sends_nothing = transcript.writes.empty();
    if (profile.requires_track && !sends_nothing) {
        auto recovered = contribution::fixture_from_transcript(transcript);
        if (!recovered || !draft18_contribution_fixture_valid(recovered->track_namespace,
                                                              recovered->track_name))
            return std::nullopt;
        fixture = std::move(*recovered);
    }
    const auto candidates = draft18_contribution_probes(
        profile.definition.deadline, fixture.track_namespace, fixture.track_name,
        contribution::token_credentials(transcript));
    const auto expected = std::find_if(candidates.begin(), candidates.end(), [&](const auto& c) {
        return c.requirement_id == profile.requirement_id &&
               c.evaluator_id == profile.evaluator_id &&
               c.definition.id == profile.definition.id;
    });
    if (expected == candidates.end() || !raw_probe_stimulus_valid(transcript, expected->definition))
        return std::nullopt;
    return expected->observe(transcript, webtransport);
}

}  // namespace moq::interop::scenarios
