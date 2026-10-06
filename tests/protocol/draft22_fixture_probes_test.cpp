// The draft 21 family probes that a draft 22 run shares (wire draft 22 through
// scenarios::current_wire_draft()) and that must name the run's track fixture and
// answer the publisher's PUBLISH there. Draft 21 is frozen: on wire draft 21 these
// definitions stay byte-identical to the pins below.
#include "moq/interop/scenarios/discovery_overlap.h"
#include "moq/interop/scenarios/draft21_response.h"
#include "moq/interop/scenarios/request_goaway.h"
#include "moq/interop/scenarios/wire_draft.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>

namespace moq::interop::scenarios {
namespace {

using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;

Bytes text(std::string_view value) {
    Bytes result;
    for (const char c : value) result.push_back(static_cast<std::byte>(c));
    return result;
}

std::string hex(const Bytes& bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(digits[value >> 4u]);
        result.push_back(digits[value & 15u]);
    }
    return result;
}

// Every write of a definition (channel, reuse, fin, bytes) and its courtesy policy.
std::string shape(const RawProbeDefinition& definition) {
    std::string result = hex(definition.setup_bytes);
    for (const auto& write : definition.writes) {
        result += " " + std::to_string(static_cast<int>(write.channel)) + ":" +
                  (write.reuse_write_stream ? std::to_string(*write.reuse_write_stream) : std::string("-")) +
                  (write.fin ? ":f:" : ":o:") + hex(write.bytes);
    }
    result += " courtesy=" + std::to_string(static_cast<int>(definition.courtesy.publish)) + "/" +
              std::to_string(static_cast<int>(definition.courtesy.update));
    return result;
}

template <class Profiles>
std::map<std::string, std::string> shapes(const Profiles& profiles) {
    std::map<std::string, std::string> result;
    for (const auto& profile : profiles) result[profile.definition.id] = shape(profile.definition);
    return result;
}

// Pinned from the definitions as they were before the draft 22 fixture change (HEAD 68c4c3e).
const std::map<std::string, std::string> kResponsePins{
    {"d21-subscriber-update-on-publish", "af000000 4:-:o:07000100 4:0:o:0200020100 courtesy=0/0"},
    {"d21-failed-subscription-update-cleanup",
     "af000000 1:-:o:0300050100017800 1:0:f:020006030103020200 courtesy=0/0"},
    {"d21-failed-subscribe-namespace-update-close",
     "af000000 1:-:o:500003010000 1:0:f:020006030103020200 courtesy=0/0"},
    {"d21-failed-subscribe-tracks-update-close",
     "af000000 1:-:o:510003010000 1:0:f:020006030103020200 courtesy=0/0"},
};
const std::map<std::string, std::string> kGoawayPins{
    {"d21-duplicate-request-goaway",
     "af000000 1:-:o:5000050101016100 1:0:o:10000300a710 1:0:o:10000300a710 courtesy=0/0"},
    {"d21-goaway-on-distinct-request-streams",
     "af000000 1:-:o:5000050101016100 1:-:o:5000050301016200 1:0:o:10000300a710 1:1:o:10000300a710 "
     "1:-:o:5000050501016300 courtesy=0/0"},
};
// Built for the namespace fixture ("media").
const std::map<std::string, std::string> kOverlapPins{
    {"d21-discovery-independent-overlap-spaces",
     "af000000 1:-:o:5000090101056d6564696100 1:-:o:5100090301056d6564696100 1:-:o:5000090501056d6564696100 "
     "1:-:o:5100090701056d6564696100 courtesy=0/0"},
    {"d21-discovery-update-independent-overlap-spaces",
     "af000000 1:-:o:5000090101056d6564696100 1:-:o:50000a0301066d656469616200 1:-:o:5100090501056d6564696100 "
     "1:-:o:51000a0701066d656469616200 1:1:o:02000b0901340701056d65646961 1:3:o:02000b0b01340701056d65646961 "
     "courtesy=0/0"},
    {"d21-namespace-prefix-update-overlap",
     "af000000 1:-:o:5000090101056d6564696100 1:-:o:50000a0301066d656469616200 "
     "1:1:o:02000b0501340701056d65646961 courtesy=0/0"},
    {"d21-subscribe-namespace-overlap",
     "af000000 1:-:o:5000090101056d6564696100 1:-:o:5000090301056d6564696100 1:-:o:500003050000 "
     "1:-:o:50000b0702056d65646961016300 courtesy=0/0"},
    {"d21-subscribe-tracks-overlap",
     "af000000 1:-:o:5100090101056d6564696100 1:-:o:5100090301056d6564696100 1:-:o:510003050000 "
     "1:-:o:51000b0702056d65646961016300 courtesy=0/0"},
    {"d21-track-prefix-update-overlap",
     "af000000 1:-:o:5100090101056d6564696100 1:-:o:51000a0301066d656469616200 "
     "1:1:o:02000b0501340701056d65646961 courtesy=0/0"},
};

TEST(Draft22FixtureProbes, Draft21ResponseDefinitionsArePinned) {
    EXPECT_EQ(current_wire_draft(), 21u);
    EXPECT_EQ(shapes(draft21_response_probes()), kResponsePins);
}

TEST(Draft22FixtureProbes, Draft21GoawayDefinitionsArePinned) {
    EXPECT_EQ(current_wire_draft(), 21u);
    EXPECT_EQ(shapes(draft21_request_goaway_probes()), kGoawayPins);
}

TEST(Draft22FixtureProbes, Draft21OverlapDefinitionsArePinned) {
    EXPECT_EQ(current_wire_draft(), 21u);
    EXPECT_EQ(shapes(draft21_discovery_overlap_probes(std::chrono::milliseconds{1000}, {text("media")})),
              kOverlapPins);
}

const Namespace kNamespace{text("media")};
const Bytes kName = text("vide_1");

Bytes from_hex(std::string_view value) {
    Bytes result;
    for (std::size_t i = 0; i + 1 < value.size(); i += 2)
        result.push_back(static_cast<std::byte>(std::stoul(std::string(value.substr(i, 2)), nullptr, 16)));
    return result;
}

template <class Profiles>
const auto* find(const Profiles& profiles, std::string_view id) {
    const auto found = std::find_if(profiles.begin(), profiles.end(),
                                    [&](const auto& profile) { return profile.definition.id == id; });
    return found == profiles.end() ? nullptr : &*found;
}

// A transcript in which every write of `definition` was accepted in order on the
// streams the controller would use; `responses` holds what the publisher answered
// on a write's stream right after that write.
RawProbeTranscript delivered(const RawProbeDefinition& definition, const std::map<std::size_t, Bytes>& responses) {
    RawProbeTranscript t;
    t.scenario_id = definition.id;
    t.events = {transport::ConnectionEstablishedEvent{},
                transport::StreamDataEvent{2, definition.setup_bytes, false}};
    t.setup = {{RawProbeChannel::NewUni, definition.setup_bytes, false}, 3, 4, false, 1};
    t.transport_established = t.peer_setup_received = t.complete = t.stimulus_delivered = true;
    std::uint64_t next = 1;
    for (std::size_t i = 0; i < definition.writes.size(); ++i) {
        const auto& write = definition.writes[i];
        const auto stream = write.reuse_write_stream ? *t.writes[*write.reuse_write_stream].stream_id : next;
        if (!write.reuse_write_stream) next += 4;
        t.writes.push_back({write, stream, write.bytes.size(), write.fin, t.events.size()});
        if (const auto found = responses.find(i); found != responses.end())
            t.events.push_back(transport::StreamDataEvent{stream, found->second, false});
    }
    t.delivery_event_count = t.events.size();
    return t;
}

void answer(RawProbeTranscript& t, const Bytes& data, bool fin) {
    t.events.push_back(transport::StreamDataEvent{*t.writes.back().stream_id, data, fin});
}

const Bytes kRequestOk = from_hex("07000100");
const Bytes kSubscribeOk = from_hex("0400020100");
const Bytes kUpdateFailed = from_hex("050003010000" "0b0003080000");

// --- A3: the run's names on the draft 22 wire ---------------------------------------------------------------

TEST(Draft22FixtureProbes, Wire21IgnoresTheFixture) {
    EXPECT_EQ(shapes(draft21_response_probes(std::chrono::milliseconds{1000}, kNamespace, kName)), kResponsePins);
    EXPECT_EQ(shapes(draft21_request_goaway_probes(std::chrono::milliseconds{1000}, kNamespace)), kGoawayPins);
}

TEST(Draft22FixtureProbes, Wire22SubscriptionCleanupSubscribesToTheRunsTrack) {
    const ScopedWireDraft wire(22);
    const auto profiles = draft21_response_probes(std::chrono::milliseconds{1000}, kNamespace, kName);
    const auto* cleanup = find(profiles, "d21-failed-subscription-update-cleanup");
    ASSERT_NE(cleanup, nullptr);
    ASSERT_EQ(cleanup->definition.writes.size(), 2u);
    // SUBSCRIBE Request ID 1, namespace (media), track vide_1, no parameters.
    EXPECT_EQ(hex(cleanup->definition.writes[0].bytes), "0300100101056d6564696106766964655f3100");
    EXPECT_EQ(hex(cleanup->definition.writes[1].bytes), "020006030103020200");
    // The discovery requests use an empty prefix, which every publisher can answer.
    EXPECT_EQ(hex(find(profiles, "d21-failed-subscribe-namespace-update-close")->definition.writes[0].bytes),
              "500003010000");
    EXPECT_EQ(hex(find(profiles, "d21-failed-subscribe-tracks-update-close")->definition.writes[0].bytes),
              "510003010000");
    // Without a fixture the probe keeps its own name, as before.
    const auto bare = draft21_response_probes();
    EXPECT_EQ(hex(find(bare, "d21-failed-subscription-update-cleanup")->definition.writes[0].bytes),
              "0300050100017800");
}

TEST(Draft22FixtureProbes, Wire22SubscriptionCleanupProofRebuildsTheRunsTrack) {
    const ScopedWireDraft wire(22);
    const auto built = draft21_response_probes(std::chrono::milliseconds{1000}, kNamespace, kName);
    // The evaluator holds the fixture-free profile, as evaluate_draft21_raw_probes does.
    const auto judged = draft21_response_probes();
    const auto* definition = find(built, "d21-failed-subscription-update-cleanup");
    const auto* profile = find(judged, "d21-failed-subscription-update-cleanup");
    ASSERT_NE(definition, nullptr);
    ASSERT_NE(profile, nullptr);
    auto t = delivered(definition->definition, {{0, kSubscribeOk}});
    answer(t, kUpdateFailed, true);
    EXPECT_EQ(evaluate_draft21_response_probe(t, *profile), true);
    // A SUBSCRIBE the builder would not have produced proves nothing.
    auto altered = t;
    altered.writes[0].write.bytes.push_back(std::byte{0});
    altered.writes[0].accepted += 1;
    EXPECT_FALSE(evaluate_draft21_response_probe(altered, *profile).has_value());
    auto renamed = t;
    renamed.writes[0].write.bytes[6] = std::byte{'M'};
    EXPECT_EQ(evaluate_draft21_response_probe(renamed, *profile), true)
        << "any track the run named is rebuilt from the delivered SUBSCRIBE";
}

TEST(Draft22FixtureProbes, Wire21SubscriptionCleanupProofIsUnchanged) {
    // On wire 21 the delivered SUBSCRIBE must be the probe's own; a fixture SUBSCRIBE proves nothing.
    std::vector<Draft21ResponseProbe> built;
    {
        const ScopedWireDraft wire(22);
        built = draft21_response_probes(std::chrono::milliseconds{1000}, kNamespace, kName);
    }
    const auto judged = draft21_response_probes();
    auto t = delivered(find(built, "d21-failed-subscription-update-cleanup")->definition, {{0, kSubscribeOk}});
    answer(t, kUpdateFailed, true);
    EXPECT_FALSE(evaluate_draft21_response_probe(t, *find(judged, "d21-failed-subscription-update-cleanup")).has_value());
    auto own = delivered(find(judged, "d21-failed-subscription-update-cleanup")->definition, {{0, kSubscribeOk}});
    answer(own, kUpdateFailed, true);
    EXPECT_EQ(evaluate_draft21_response_probe(own, *find(judged, "d21-failed-subscription-update-cleanup")), true);
}

TEST(Draft22FixtureProbes, Wire22GoawayProbesUseTheRunsNamespace) {
    const ScopedWireDraft wire(22);
    const auto profiles = draft21_request_goaway_probes(std::chrono::milliseconds{1000}, kNamespace);
    const auto* duplicate = find(profiles, "d21-duplicate-request-goaway");
    const auto* distinct = find(profiles, "d21-goaway-on-distinct-request-streams");
    ASSERT_NE(duplicate, nullptr);
    ASSERT_NE(distinct, nullptr);
    EXPECT_EQ(shape(duplicate->definition),
              "af000000 1:-:o:5000090101056d6564696100 1:0:o:10000300a710 1:0:o:10000300a710 courtesy=0/0");
    ASSERT_TRUE(duplicate->definition.liveness.has_value()) << "the duplicate keeps its liveness follow-up";
    // Two requests a single-namespace publisher can both accept without overlapping (Section 9.2 needs two
    // active request streams): SUBSCRIBE_NAMESPACE and SUBSCRIBE_TRACKS have independent overlap spaces
    // (draft 22 Section 12.3, PREFIX_OVERLAP). The PUBLISH that SUBSCRIBE_TRACKS draws is accepted.
    EXPECT_EQ(shape(distinct->definition),
              "af000000 1:-:o:5000090101056d6564696100 1:-:o:5100090301056d6564696100 1:0:o:10000300a710 "
              "1:1:o:10000300a710 1:-:o:5000050501016300 courtesy=1/0");
    // Without a fixture the probes keep their own prefixes.
    EXPECT_EQ(hex(find(draft21_request_goaway_probes(), "d21-duplicate-request-goaway")->definition.writes[0].bytes),
              "5000050101016100");
}

TEST(Draft22FixtureProbes, Wire22GoawayProofRebuildsTheRunsNamespace) {
    const ScopedWireDraft wire(22);
    const auto built = draft21_request_goaway_probes(std::chrono::milliseconds{1000}, kNamespace);
    const auto judged = draft21_request_goaway_probes();
    {
        const auto* definition = find(built, "d21-duplicate-request-goaway");
        auto t = delivered(definition->definition, {{0, kRequestOk}});
        t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
        EXPECT_EQ(evaluate_request_goaway_probe(t, *find(judged, "d21-duplicate-request-goaway")), true);
    }
    {
        const auto* definition = find(built, "d21-goaway-on-distinct-request-streams");
        auto t = delivered(definition->definition, {{0, kRequestOk}, {1, kRequestOk}});
        answer(t, from_hex("050003100000"), false);
        EXPECT_EQ(evaluate_request_goaway_probe(t, *find(judged, "d21-goaway-on-distinct-request-streams")), true);
        auto altered = t;
        altered.writes[1].write.bytes[0] = std::byte{0x50};
        EXPECT_FALSE(evaluate_request_goaway_probe(altered, *find(judged, "d21-goaway-on-distinct-request-streams"))
                         .has_value());
    }
}

// --- A5: the PUBLISH that SUBSCRIBE_TRACKS draws is answered on the draft 22 wire --------------------------

bool sends_subscribe_tracks(const RawProbeDefinition& definition) {
    return std::any_of(definition.writes.begin(), definition.writes.end(), [](const auto& write) {
        return !write.bytes.empty() && write.bytes.front() == std::byte{0x51};
    });
}

TEST(Draft22FixtureProbes, Wire22SubscribeTracksResponseProbeAcceptsThePublish) {
    const ScopedWireDraft wire(22);
    for (const auto& profiles : {draft21_response_probes(), draft21_response_probes(std::chrono::milliseconds{1000},
                                                                                     kNamespace, kName)}) {
        for (const auto& profile : profiles) {
            SCOPED_TRACE(profile.definition.id);
            const bool tracks = profile.definition.id == "d21-failed-subscribe-tracks-update-close";
            EXPECT_EQ(sends_subscribe_tracks(profile.definition), tracks);
            EXPECT_EQ(profile.definition.courtesy.publish,
                      tracks ? RawProbePublishResponse::Accept : RawProbePublishResponse::Ignore);
            EXPECT_EQ(profile.definition.courtesy.update, RawProbeUpdateResponse::Ignore);
        }
    }
}

TEST(Draft22FixtureProbes, Wire22OverlapProbesWithSubscribeTracksAcceptThePublish) {
    const ScopedWireDraft wire(22);
    const auto profiles = draft21_discovery_overlap_probes(std::chrono::milliseconds{1000}, kNamespace);
    std::size_t accepting = 0;
    for (const auto& profile : profiles) {
        SCOPED_TRACE(profile.definition.id);
        const bool tracks = sends_subscribe_tracks(profile.definition);
        accepting += tracks;
        EXPECT_EQ(profile.definition.courtesy.publish,
                  tracks ? RawProbePublishResponse::Accept : RawProbePublishResponse::Ignore);
        EXPECT_EQ(profile.definition.courtesy.update, RawProbeUpdateResponse::Ignore);
        // Only the courtesy differs from draft 21: the requests are the same bytes.
        auto pinned = kOverlapPins.at(profile.definition.id);
        if (tracks) pinned.replace(pinned.size() - 3, 1, "1");
        EXPECT_EQ(shape(profile.definition), pinned);
    }
    // d21-subscribe-tracks-overlap, d21-track-prefix-update-overlap, and both independent-spaces probes
    // (listed once per row they score).
    EXPECT_EQ(accepting, 6u);
}

TEST(Draft22FixtureProbes, Wire22SubscribeTracksCloseIsJudgedWithTheCourtesyAnswerRecorded) {
    const ScopedWireDraft wire(22);
    const auto profiles = draft21_response_probes();
    const auto* profile = find(profiles, "d21-failed-subscribe-tracks-update-close");
    ASSERT_NE(profile, nullptr);
    auto t = delivered(profile->definition, {{0, kRequestOk}});
    // The publisher's PUBLISH arrives on its own request stream and the runner answers it; neither is part
    // of the stimulus. The update was already delivered at that point.
    t.events.push_back(transport::StreamDataEvent{0, from_hex("1d0008000101" "6e01740100"), false});
    t.courtesy_writes.push_back({0, t.events.size(), RawProbeCourtesyKind::PublishOk});
    answer(t, from_hex("050003010000"), true);
    EXPECT_EQ(evaluate_draft21_response_probe(t, *profile), true);
}

}  // namespace
}  // namespace moq::interop::scenarios
