#include "moq/interop/scenarios/draft18_gap_a.h"
#include "support/scripted_publisher.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace moq::interop::scenarios {
namespace {
using namespace test;
namespace d18 = wire::draft18;

Bytes setup() { return encode_draft18(d18::SetupMessage{}); }
Bytes ok() { return encode_draft18(d18::RequestOkMessage{{}, {}}); }
Bytes error(std::uint64_t code) {
    return encode_draft18(d18::RequestErrorMessage{code, 0, {}, std::nullopt});
}
Bytes namespace_message(std::vector<Bytes> suffix) {
    return encode_draft18(d18::NamespaceMessage{d18::TrackNamespace{std::move(suffix)}});
}
Bytes publish(std::uint64_t request_id, std::vector<Bytes> fields, Bytes name, std::uint64_t alias = 9,
              d18::Parameters parameters = {}) {
    return encode_draft18(d18::PublishMessage{request_id, d18::TrackNamespace{std::move(fields)},
        d18::TrackName{std::move(name)}, alias, std::move(parameters), {}});
}
Bytes publish_namespace(std::uint64_t request_id, std::vector<Bytes> fields) {
    return encode_draft18(d18::PublishNamespaceMessage{request_id, d18::TrackNamespace{std::move(fields)}, {}});
}

Draft18GapAProbe profile(const std::string& requirement, std::vector<Bytes> ns = {bytes_of("n")}) {
    auto profiles = draft18_gap_a_probes(std::chrono::milliseconds(200), std::move(ns), bytes_of("t"));
    const auto found = std::find_if(profiles.begin(), profiles.end(),
        [&](const auto& candidate) { return candidate.requirement_id == requirement; });
    if (found == profiles.end()) throw std::runtime_error("missing profile " + requirement);
    return *found;
}

std::optional<bool> score(const Draft18GapAProbe& probe, ScriptedPublisher& publisher) {
    const auto transcript = run_probe(publisher, probe.definition);
    return evaluate_draft18_gap_a_probe(transcript, probe);
}

// Answers a stream once, when the probe has written to it.
void answer(ScriptedPublisher& peer, std::uint64_t stream, const std::string& key, Bytes bytes,
            bool fin = false) {
    if (peer.sent(stream) && !peer.answered(key)) {
        peer.mark(key);
        peer.data(stream, std::move(bytes), fin);
    }
}

TEST(Draft18GapADiscovery, NamespaceSubscriptionResponseMustPrecedeNamespaceMessages) {
    const auto probe = profile("D18-6-1-MUST-002");
    EXPECT_EQ(probe.definition.id, "subscribe-namespace-at-publisher-with-matching-namespace");
    ScriptedPublisher in_order(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "a", concat({ok(), namespace_message({})}));
    });
    EXPECT_EQ(score(probe, in_order), std::optional<bool>{true});
    ScriptedPublisher namespace_first(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "a", concat({namespace_message({}), ok()}));
    });
    EXPECT_EQ(score(probe, namespace_first), std::optional<bool>{false});
    ScriptedPublisher rejected(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "a", error(0x10), true);
    });
    EXPECT_EQ(score(probe, rejected), std::optional<bool>{true});
    ScriptedPublisher fragmented(setup(), [](ScriptedPublisher& peer) {
        const auto frame = ok();
        if (peer.sent(1) && !peer.answered("a")) {
            peer.mark("a");
            peer.data(1, Bytes(frame.begin(), frame.begin() + 2));
        } else if (peer.answered("a") && !peer.answered("b")) {
            peer.mark("b");
            peer.data(1, Bytes(frame.begin() + 2, frame.end()));
        }
    });
    EXPECT_EQ(score(probe, fragmented), std::optional<bool>{true});
    ScriptedPublisher silent(setup());
    EXPECT_EQ(score(probe, silent), std::nullopt);
    ScriptedPublisher closed_empty(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("a")) { peer.mark("a"); peer.fin(1); }
    });
    EXPECT_EQ(score(probe, closed_empty), std::nullopt);
}

TEST(Draft18GapADiscovery, ExactAndPrefixSubscriptionsEachReceiveTheirNamespace) {
    for (const auto& ns : std::vector<std::vector<Bytes>>{{bytes_of("n")}, {bytes_of("a"), bytes_of("b")}}) {
        const auto probe = profile("D18-6-2-MUST-001", ns);
        const std::vector<Bytes> suffix{ns.back()};
        ScriptedPublisher publisher(setup(), [&](ScriptedPublisher& peer) {
            answer(peer, 1, "exact", concat({ok(), namespace_message({})}));
            answer(peer, 5, "prefix", concat({ok(), namespace_message(suffix)}));
        });
        EXPECT_EQ(score(probe, publisher), std::optional<bool>{true});
        // The exact subscription is finished before the prefix one starts.
        ASSERT_NE(publisher.sent(1), nullptr);
        EXPECT_TRUE(publisher.sent(1)->fin);
        ASSERT_NE(publisher.sent(5), nullptr);
        EXPECT_EQ(publisher.sent(5)->bytes, encode_draft18(d18::SubscribeNamespaceMessage{3,
            d18::TrackNamespace{std::vector<Bytes>(ns.begin(), ns.end() - 1)}, {}}));
    }
}

TEST(Draft18GapADiscovery, MissingNamespaceMessageLeavesTheObligationUnscored) {
    const auto probe = profile("D18-6-2-MUST-001", {bytes_of("a"), bytes_of("b")});
    ScriptedPublisher no_exact_namespace(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "exact", ok());
    });
    EXPECT_EQ(score(probe, no_exact_namespace), std::nullopt);
    EXPECT_EQ(no_exact_namespace.sent(5), nullptr);
    ScriptedPublisher no_prefix_namespace(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "exact", concat({ok(), namespace_message({})}));
        answer(peer, 5, "prefix", ok());
    });
    EXPECT_EQ(score(probe, no_prefix_namespace), std::nullopt);
    ScriptedPublisher wrong_suffix(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "exact", concat({ok(), namespace_message({})}));
        answer(peer, 5, "prefix", concat({ok(), namespace_message({bytes_of("z")})}));
    });
    EXPECT_EQ(score(probe, wrong_suffix), std::nullopt);
    ScriptedPublisher overlap(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "exact", concat({ok(), namespace_message({})}));
        answer(peer, 5, "prefix", error(0x30), true);
    });
    EXPECT_EQ(score(probe, overlap), std::nullopt);
}

TEST(Draft18GapADiscovery, PublishedTrackNamespaceFieldsMustBeNonEmpty) {
    const auto probe = profile("D18-2-4-1-MUST-001");
    ScriptedPublisher good(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "ok", ok());
        if (peer.answered("ok") && !peer.answered("publish")) {
            peer.mark("publish");
            peer.data(0, publish(2, {bytes_of("n"), bytes_of("m")}, bytes_of("t")));
        }
    });
    EXPECT_EQ(score(probe, good), std::optional<bool>{true});
    ScriptedPublisher empty_tuple(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "ok", ok());
        if (peer.answered("ok") && !peer.answered("publish")) {
            peer.mark("publish");
            peer.data(0, publish(2, {}, bytes_of("t")));
        }
    });
    EXPECT_EQ(score(probe, empty_tuple), std::optional<bool>{true});
    // A zero-length field cannot be built with the encoder; craft the frame.
    ScriptedPublisher zero_field(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "ok", ok());
        if (peer.answered("ok") && !peer.answered("publish")) {
            peer.mark("publish");
            peer.data(0, Bytes{std::byte{0x1d}, std::byte{0}, std::byte{7}, std::byte{2}, std::byte{1},
                std::byte{0}, std::byte{1}, std::byte{'t'}, std::byte{9}, std::byte{0}, std::byte{0}});
        }
    });
    EXPECT_EQ(score(probe, zero_field), std::optional<bool>{false});
    ScriptedPublisher none(setup(), [](ScriptedPublisher& peer) { answer(peer, 1, "ok", ok()); });
    EXPECT_EQ(score(probe, none), std::nullopt);
}

TEST(Draft18GapADiscovery, PublishMustBeTheFirstMessageOnANewBidirectionalStream) {
    const auto probe = profile("D18-10-MUST-002");
    ScriptedPublisher first(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "ok", ok());
        if (peer.answered("ok") && !peer.answered("publish")) {
            peer.mark("publish");
            peer.data(0, publish(2, {bytes_of("n")}, bytes_of("t")));
        }
    });
    EXPECT_EQ(score(probe, first), std::optional<bool>{true});
    ScriptedPublisher on_response_stream(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "ok", concat({ok(), publish(2, {bytes_of("n")}, bytes_of("t"))}));
    });
    EXPECT_EQ(score(probe, on_response_stream), std::optional<bool>{false});
    ScriptedPublisher after_other(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "ok", ok());
        if (peer.answered("ok") && !peer.answered("publish")) {
            peer.mark("publish");
            peer.data(0, concat({publish_namespace(2, {bytes_of("n")}), publish(4, {bytes_of("n")}, bytes_of("t"))}));
        }
    });
    EXPECT_EQ(score(probe, after_other), std::optional<bool>{false});
    ScriptedPublisher on_control(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "ok", ok());
        if (peer.answered("ok") && !peer.answered("publish")) {
            peer.mark("publish");
            peer.data(2, publish(2, {bytes_of("n")}, bytes_of("t")));
        }
    });
    EXPECT_EQ(score(probe, on_control), std::optional<bool>{false});
    ScriptedPublisher none(setup(), [](ScriptedPublisher& peer) { answer(peer, 1, "ok", ok()); });
    EXPECT_EQ(score(probe, none), std::nullopt);
}

TEST(Draft18GapADiscovery, PublishNamespaceIsObservedFirstOnItsOwnRequestStream) {
    const auto placement = profile("D18-10-MUST-005");
    const auto routing = profile("D18-9-5-MUST-003");
    EXPECT_TRUE(placement.definition.writes.empty());
    EXPECT_TRUE(routing.definition.writes.empty());
    const auto announce = [](ScriptedPublisher& peer) {
        if (!peer.answered("a")) { peer.mark("a"); peer.data(0, publish_namespace(2, {bytes_of("n")})); }
    };
    ScriptedPublisher first(setup(), announce);
    EXPECT_EQ(score(placement, first), std::optional<bool>{true});
    ScriptedPublisher routed(setup(), announce);
    EXPECT_EQ(score(routing, routed), std::optional<bool>{true});
    ScriptedPublisher control(setup(), [](ScriptedPublisher& peer) {
        if (!peer.answered("a")) { peer.mark("a"); peer.data(2, publish_namespace(2, {bytes_of("n")})); }
    });
    EXPECT_EQ(score(placement, control), std::optional<bool>{false});
    ScriptedPublisher silent(setup());
    EXPECT_EQ(score(placement, silent), std::nullopt);
    EXPECT_EQ(score(routing, silent), std::nullopt);
    // A PUBLISH for one Track is not a namespace publication.
    ScriptedPublisher only_track(setup(), [](ScriptedPublisher& peer) {
        if (!peer.answered("a")) { peer.mark("a"); peer.data(0, publish(2, {bytes_of("n")}, bytes_of("t"))); }
    });
    EXPECT_EQ(score(routing, only_track), std::nullopt);
}

TEST(Draft18GapADiscovery, SubscribeCrossingAPendingPublishIsRejectedAsDuplicate) {
    const auto probe = profile("D18-5-1-MUST-005", {bytes_of("a"), bytes_of("b")});
    const auto reaction = [](Bytes reply) {
        return [reply](ScriptedPublisher& peer) {
            answer(peer, 1, "ok", ok());
            if (peer.answered("ok") && !peer.answered("publish")) {
                peer.mark("publish");
                peer.data(0, publish(2, {bytes_of("a"), bytes_of("b")}, bytes_of("t")));
            }
            answer(peer, 5, "reply", reply, true);
        };
    };
    ScriptedPublisher rejects(setup(), reaction(error(0x19)));
    EXPECT_EQ(score(probe, rejects), std::optional<bool>{true});
    ASSERT_NE(rejects.sent(5), nullptr);
    EXPECT_EQ(rejects.sent(5)->bytes, encode_draft18(d18::SubscribeMessage{3,
        d18::TrackNamespace{{bytes_of("a"), bytes_of("b")}}, d18::TrackName{bytes_of("t")}, {}}));
    // The pending PUBLISH is never answered with PUBLISH_OK.
    EXPECT_EQ(rejects.sent(0), nullptr);
    ScriptedPublisher accepts(setup(), reaction(encode_draft18(d18::SubscribeOkMessage{4, {}, {}})));
    EXPECT_EQ(score(probe, accepts), std::optional<bool>{false});
    ScriptedPublisher other_error(setup(), reaction(error(0x10)));
    EXPECT_EQ(score(probe, other_error), std::optional<bool>{false});
    ScriptedPublisher no_publish(setup(), [](ScriptedPublisher& peer) { answer(peer, 1, "ok", ok()); });
    EXPECT_EQ(score(probe, no_publish), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
