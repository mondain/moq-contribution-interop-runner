#include "moq/interop/scenarios/draft18_gap_a.h"
#include "support/scripted_publisher.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace moq::interop::scenarios {
namespace {
using namespace test;
namespace d18 = wire::draft18;

Bytes setup() { return encode_draft18(d18::SetupMessage{}); }
Bytes error(std::uint64_t code) {
    return encode_draft18(d18::RequestErrorMessage{code, 0, {}, std::nullopt});
}
d18::Parameter largest(std::uint64_t group, std::uint64_t object) {
    return {0x09, d18::Location{group, object}};
}
Bytes ok(d18::Parameters parameters = {}) {
    return encode_draft18(d18::RequestOkMessage{std::move(parameters), {}});
}
Bytes subscribe_ok(std::uint64_t alias = 4, d18::Parameters parameters = {}) {
    return encode_draft18(d18::SubscribeOkMessage{alias, std::move(parameters), {}});
}
Bytes fetch_ok(std::uint64_t group = 7, std::uint64_t object = 10) {
    return encode_draft18(d18::FetchOkMessage{0, {group, object}, {}, {}});
}
Bytes vi(std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("vi64");
    return {writer.bytes().begin(), writer.bytes().end()};
}
// FETCH data stream: type 5, request ID, flags 0x1c, Group, Object, priority.
Bytes fetch_data(std::uint64_t request_id, std::string_view payload, std::uint64_t group = 7,
                 std::uint64_t object = 9) {
    return concat({Bytes{std::byte{5}}, vi(request_id), vi(0x1c), vi(group), vi(object),
                   Bytes{std::byte{99}}, vi(payload.size()), bytes_of(payload)});
}
// Subgroup stream with an explicit Subgroup ID (mode 2); FIRST_OBJECT is 0x40.
Bytes subgroup(std::uint64_t alias, std::uint64_t group, std::uint64_t subgroup_id,
               std::vector<std::uint64_t> objects, bool first_object = true) {
    Bytes result = concat({vi(first_object ? 0x54 : 0x14), vi(alias), vi(group), vi(subgroup_id),
                           Bytes{std::byte{99}}});
    std::optional<std::uint64_t> previous;
    for (const auto id : objects) {
        const auto delta = previous ? id - *previous - 1 : id;
        const auto payload = bytes_of("p");
        result = concat({result, vi(delta), vi(payload.size()), payload});
        previous = id;
    }
    return result;
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

void answer(ScriptedPublisher& peer, std::uint64_t stream, const std::string& key, Bytes bytes,
            bool fin = false) {
    if (peer.sent(stream) && !peer.answered(key)) {
        peer.mark(key);
        peer.data(stream, std::move(bytes), fin);
    }
}
// Delivers Object 7/9 for the leading FETCH (request 1, local stream 1).
void published_object(ScriptedPublisher& peer, std::string_view payload = "A") {
    if (peer.sent(1) && !peer.answered("fetch")) {
        peer.mark("fetch");
        peer.data(1, fetch_ok(), true);
        peer.data(6, fetch_data(1, payload), true);
    }
}

TEST(Draft18GapAObject, FetchesObjectTwiceAndComparesPayloadBytes) {
    const auto probe = profile("D18-2-1-MUST-NOT-001");
    const auto reaction = [](std::string second) {
        return [second](ScriptedPublisher& peer) {
            published_object(peer);
            if (peer.sent(5) && !peer.answered("second")) {
                peer.mark("second");
                peer.data(5, fetch_ok(), true);
                peer.data(10, fetch_data(3, second), true);
            }
        };
    };
    ScriptedPublisher same(setup(), reaction("A"));
    EXPECT_EQ(score(probe, same), std::optional<bool>{true});
    ScriptedPublisher changed(setup(), reaction("B"));
    EXPECT_EQ(score(probe, changed), std::optional<bool>{false});
    // The second FETCH waits for the first complete Object.
    ScriptedPublisher no_data(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "fetch", fetch_ok(), true);
    });
    EXPECT_EQ(score(probe, no_data), std::nullopt);
    EXPECT_EQ(no_data.sent(5), nullptr);
    ScriptedPublisher no_second(setup(), [](ScriptedPublisher& peer) { published_object(peer); });
    EXPECT_EQ(score(probe, no_second), std::nullopt);
    ASSERT_NE(same.sent(5), nullptr);
    EXPECT_NE(same.sent(1)->bytes, same.sent(5)->bytes);  // distinct Request IDs
}

TEST(Draft18GapAObject, SubscribeOkIncludesLargestObjectOnceObjectsExist) {
    const auto probe = profile("D18-10-2-11-MUST-001");
    const auto reaction = [](Bytes reply) {
        return [reply](ScriptedPublisher& peer) {
            published_object(peer);
            answer(peer, 5, "reply", reply);
        };
    };
    ScriptedPublisher included(setup(), reaction(subscribe_ok(4, {largest(7, 9)})));
    EXPECT_EQ(score(probe, included), std::optional<bool>{true});
    ScriptedPublisher missing(setup(), reaction(subscribe_ok(4, {})));
    EXPECT_EQ(score(probe, missing), std::optional<bool>{false});
    ScriptedPublisher rejected(setup(), reaction(error(0x10)));
    EXPECT_EQ(score(probe, rejected), std::nullopt);
    // Nothing is subscribed before an Object has been observed.
    ScriptedPublisher unpublished(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "fetch", error(0x11), true);
    });
    EXPECT_EQ(score(probe, unpublished), std::nullopt);
    EXPECT_EQ(unpublished.sent(5), nullptr);
}

TEST(Draft18GapAObject, PublishIncludesLargestObjectForTheFixtureTrack) {
    const auto probe = profile("D18-10-2-11-MUST-002");
    const auto reaction = [](d18::Parameters parameters, std::string track) {
        return [parameters, track](ScriptedPublisher& peer) {
            published_object(peer);
            answer(peer, 5, "ok", ok());
            if (peer.answered("ok") && !peer.answered("publish")) {
                peer.mark("publish");
                peer.data(0, encode_draft18(d18::PublishMessage{2, d18::TrackNamespace{{bytes_of("n")}},
                    d18::TrackName{bytes_of(track)}, 9, parameters, {}}));
            }
        };
    };
    ScriptedPublisher included(setup(), reaction({largest(7, 9)}, "t"));
    EXPECT_EQ(score(probe, included), std::optional<bool>{true});
    ASSERT_NE(included.sent(5), nullptr);
    EXPECT_EQ(included.sent(5)->bytes, encode_draft18(d18::SubscribeTracksMessage{3,
        d18::TrackNamespace{{bytes_of("n")}}, {}}));
    ScriptedPublisher missing(setup(), reaction({}, "t"));
    EXPECT_EQ(score(probe, missing), std::optional<bool>{false});
    ScriptedPublisher other_track(setup(), reaction({}, "other"));
    EXPECT_EQ(score(probe, other_track), std::nullopt);
}

TEST(Draft18GapAObject, AcceptedSubscriptionUpdateIncludesLargestObject) {
    const auto probe = profile("D18-10-2-11-MUST-003");
    const auto reaction = [](Bytes update_reply) {
        return [update_reply](ScriptedPublisher& peer) {
            published_object(peer);
            answer(peer, 5, "subscribed", subscribe_ok());
            if (peer.answered("subscribed") && peer.sent(5) && peer.sent(5)->writes >= 2 &&
                !peer.answered("updated")) {
                peer.mark("updated");
                peer.data(5, update_reply);
            }
        };
    };
    ScriptedPublisher included(setup(), reaction(ok({largest(7, 9)})));
    EXPECT_EQ(score(probe, included), std::optional<bool>{true});
    // The update shares the SUBSCRIBE request stream (Section 10.9).
    EXPECT_EQ(included.sent(5)->bytes, concat({
        encode_draft18(d18::SubscribeMessage{3, d18::TrackNamespace{{bytes_of("n")}},
            d18::TrackName{bytes_of("t")}, {{0x10, d18::Uint8ParameterValue{0}}}}),
        encode_draft18(d18::RequestUpdateMessage{5, {{0x20, d18::Uint8ParameterValue{100}}}})}));
    ScriptedPublisher missing(setup(), reaction(ok()));
    EXPECT_EQ(score(probe, missing), std::optional<bool>{false});
    ScriptedPublisher refused(setup(), reaction(error(0x10)));
    EXPECT_EQ(score(probe, refused), std::nullopt);
}

TEST(Draft18GapAObject, AcceptedTrackStatusIncludesLargestObject) {
    const auto probe = profile("D18-10-2-11-MUST-004");
    const auto reaction = [](Bytes reply) {
        return [reply](ScriptedPublisher& peer) {
            published_object(peer);
            answer(peer, 5, "status", reply, true);
        };
    };
    ScriptedPublisher included(setup(), reaction(ok({largest(7, 9)})));
    EXPECT_EQ(score(probe, included), std::optional<bool>{true});
    ScriptedPublisher missing(setup(), reaction(ok()));
    EXPECT_EQ(score(probe, missing), std::optional<bool>{false});
    ScriptedPublisher refused(setup(), reaction(error(0x10)));
    EXPECT_EQ(score(probe, refused), std::nullopt);
}

TEST(Draft18GapAObject, NewSubgroupStreamsMustSetFirstObject) {
    const auto probe = profile("D18-2-2-MUST-001");
    const auto reaction = [](bool first_object, std::uint64_t group) {
        return [first_object, group](ScriptedPublisher& peer) {
            answer(peer, 1, "ok", subscribe_ok(4, {largest(5, 2)}));
            if (peer.answered("ok") && !peer.answered("data")) {
                peer.mark("data");
                peer.data(6, subgroup(4, group, 0, {0, 1}, first_object));
            }
        };
    };
    ScriptedPublisher marked(setup(), reaction(true, 6));
    EXPECT_EQ(score(probe, marked), std::optional<bool>{true});
    // Start of the stream is the next group: Largest Object 5 -> start {6, 0}.
    EXPECT_EQ(marked.sent(1)->bytes, encode_draft18(d18::SubscribeMessage{1,
        d18::TrackNamespace{{bytes_of("n")}}, d18::TrackName{bytes_of("t")},
        {{0x21, d18::SubscriptionFilter{d18::SubscriptionFilterType::NextGroupStart, std::nullopt,
                                         std::nullopt}}}}));
    ScriptedPublisher unmarked(setup(), reaction(false, 6));
    EXPECT_EQ(score(probe, unmarked), std::optional<bool>{false});
    // A stream from the already-started group may legitimately lack the bit.
    ScriptedPublisher old_group(setup(), reaction(false, 5));
    EXPECT_EQ(score(probe, old_group), std::nullopt);
    ScriptedPublisher other_alias(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "ok", subscribe_ok(4, {largest(5, 2)}));
        if (peer.answered("ok") && !peer.answered("data")) {
            peer.mark("data");
            peer.data(6, subgroup(8, 6, 0, {0}, false));
        }
    });
    EXPECT_EQ(score(probe, other_alias), std::nullopt);
}

TEST(Draft18GapAObject, SubgroupObjectsStayOnOneStreamUnlessResetOrReordered) {
    const auto probe = profile("D18-2-2-MUST-NOT-002");
    const auto reaction = [](std::function<void(ScriptedPublisher&)> deliver) {
        return [deliver](ScriptedPublisher& peer) {
            answer(peer, 1, "ok", subscribe_ok(4, {}));
            if (peer.answered("ok") && !peer.answered("data")) {
                peer.mark("data");
                deliver(peer);
            }
        };
    };
    // Group 3 is delivered on one stream per Subgroup, then Group 4 begins.
    ScriptedPublisher single(setup(), reaction([](ScriptedPublisher& peer) {
        peer.data(6, subgroup(4, 3, 0, {0, 1, 2}), true);
        peer.data(10, subgroup(4, 3, 1, {0, 1}), true);
        peer.data(14, subgroup(4, 4, 0, {0}), false);
    }));
    EXPECT_EQ(score(probe, single), std::optional<bool>{true});
    ScriptedPublisher split(setup(), reaction([](ScriptedPublisher& peer) {
        peer.data(6, subgroup(4, 3, 0, {0, 1}), true);
        peer.data(10, subgroup(4, 3, 0, {2, 3}, false), true);
        peer.data(14, subgroup(4, 4, 0, {0}), false);
    }));
    EXPECT_EQ(score(probe, split), std::optional<bool>{false});
    ScriptedPublisher after_reset(setup(), reaction([](ScriptedPublisher& peer) {
        peer.data(6, subgroup(4, 3, 0, {0, 1}), false);
        peer.peer_reset(6);
        peer.data(10, subgroup(4, 3, 0, {2, 3}, false), true);
        peer.data(14, subgroup(4, 4, 0, {0}), false);
    }));
    EXPECT_EQ(score(probe, after_reset), std::optional<bool>{true});
    ScriptedPublisher reordered(setup(), reaction([](ScriptedPublisher& peer) {
        peer.data(6, subgroup(4, 3, 0, {2, 3}), true);
        peer.data(10, subgroup(4, 3, 0, {0, 1}, false), true);
        peer.data(14, subgroup(4, 4, 0, {0}), false);
    }));
    EXPECT_EQ(score(probe, reordered), std::optional<bool>{true});
    // One Group alone cannot show that its Subgroups stayed on one stream.
    ScriptedPublisher one_group(setup(), reaction([](ScriptedPublisher& peer) {
        peer.data(6, subgroup(4, 3, 0, {0, 1}), true);
    }));
    EXPECT_EQ(score(probe, one_group), std::nullopt);
}

TEST(Draft18GapAObject, RejectedSubscribeCarriesNoObjects) {
    const auto probe = profile("D18-5-1-1-MUST-NOT-002");
    ScriptedPublisher clean(setup(), [](ScriptedPublisher& peer) { answer(peer, 1, "e", error(0x10), true); });
    EXPECT_EQ(score(probe, clean), std::optional<bool>{true});
    // The request names a track the publisher cannot have.
    const auto wire = clean.sent(1)->bytes;
    wire::Cursor cursor(wire);
    const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto& subscribe = std::get<d18::SubscribeMessage>(std::get<d18::Message>(decoded));
    EXPECT_NE(subscribe.track_name.bytes, bytes_of("t"));
    EXPECT_EQ(subscribe.track_namespace.fields, std::vector<Bytes>{bytes_of("n")});
    ScriptedPublisher datagram(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("e")) {
            peer.mark("e");
            peer.datagram(Bytes{std::byte{0}, std::byte{1}});
            peer.data(1, error(0x10), true);
        }
    });
    EXPECT_EQ(score(probe, datagram), std::optional<bool>{false});
    ScriptedPublisher stream(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("e")) {
            peer.mark("e");
            peer.data(6, subgroup(4, 3, 0, {0}));
            peer.data(1, error(0x10), true);
        }
    });
    EXPECT_EQ(score(probe, stream), std::optional<bool>{false});
    ScriptedPublisher accepted(setup(), [](ScriptedPublisher& peer) { answer(peer, 1, "e", subscribe_ok()); });
    EXPECT_EQ(score(probe, accepted), std::nullopt);
}

TEST(Draft18GapAObject, SubscriptionRangeCoversOnlyTheGroupAfterTheLargestObject) {
    const auto probe = profile("D18-5-1-2-MUST-NOT-001");
    const auto reaction = [](std::function<void(ScriptedPublisher&)> deliver) {
        return [deliver](ScriptedPublisher& peer) {
            answer(peer, 1, "status", ok({largest(5, 3)}), true);
            answer(peer, 5, "ok", subscribe_ok(4, {}));
            if (peer.answered("ok") && !peer.answered("data")) {
                peer.mark("data");
                deliver(peer);
            }
        };
    };
    ScriptedPublisher within(setup(), reaction([](ScriptedPublisher& peer) {
        peer.data(6, subgroup(4, 6, 0, {0, 1}));
    }));
    EXPECT_EQ(score(probe, within), std::optional<bool>{true});
    EXPECT_EQ(within.sent(5)->bytes, encode_draft18(d18::SubscribeMessage{3,
        d18::TrackNamespace{{bytes_of("n")}}, d18::TrackName{bytes_of("t")},
        {{0x21, d18::SubscriptionFilter{d18::SubscriptionFilterType::AbsoluteRange, d18::Location{6, 0},
                                         std::uint64_t{0}}}}}));
    ScriptedPublisher before(setup(), reaction([](ScriptedPublisher& peer) {
        peer.data(6, subgroup(4, 5, 0, {4}));
    }));
    EXPECT_EQ(score(probe, before), std::optional<bool>{false});
    ScriptedPublisher after(setup(), reaction([](ScriptedPublisher& peer) {
        peer.data(6, subgroup(4, 6, 0, {0}));
        peer.data(10, subgroup(4, 7, 0, {0}));
    }));
    EXPECT_EQ(score(probe, after), std::optional<bool>{false});
    ScriptedPublisher silent(setup(), reaction([](ScriptedPublisher&) {}));
    EXPECT_EQ(score(probe, silent), std::nullopt);
    // Without a Largest Object there is no range to compute.
    ScriptedPublisher empty_track(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "status", ok({}), true);
    });
    EXPECT_EQ(score(probe, empty_track), std::nullopt);
    EXPECT_EQ(empty_track.sent(5), nullptr);
}

TEST(Draft18GapAObject, JoiningFetchEndsAtTheSavedJoiningLocation) {
    const auto probe = profile("D18-5-1-MUST-003");
    const auto reaction = [](Bytes fetch_reply, bool advance) {
        return [fetch_reply, advance](ScriptedPublisher& peer) {
            answer(peer, 1, "subscribed", subscribe_ok(4, {}));
            if (peer.answered("subscribed") && peer.sent(1) && peer.sent(1)->writes >= 2 &&
                !peer.answered("updated")) {
                peer.mark("updated");
                peer.data(1, ok({largest(7, 3)}));
                if (advance) peer.data(6, subgroup(4, 7, 0, {4, 5}));
            }
            answer(peer, 5, "fetch", fetch_reply, true);
        };
    };
    ScriptedPublisher saved(setup(), reaction(fetch_ok(7, 4), true));
    EXPECT_EQ(score(probe, saved), std::optional<bool>{true});
    // The update shares the SUBSCRIBE stream; the joining FETCH is relative.
    EXPECT_EQ(saved.sent(1)->bytes, concat({
        encode_draft18(d18::SubscribeMessage{1, d18::TrackNamespace{{bytes_of("n")}},
            d18::TrackName{bytes_of("t")}, {{0x10, d18::Uint8ParameterValue{0}}}}),
        encode_draft18(d18::RequestUpdateMessage{3, {{0x10, d18::Uint8ParameterValue{1}}}})}));
    EXPECT_EQ(saved.sent(5)->bytes, encode_draft18(d18::FetchMessage{5,
        d18::RelativeJoiningFetch{1, 0}, {}}));
    ScriptedPublisher current_edge(setup(), reaction(fetch_ok(7, 6), true));
    EXPECT_EQ(score(probe, current_edge), std::optional<bool>{false});
    ScriptedPublisher refused(setup(), reaction(error(0x11), true));
    EXPECT_EQ(score(probe, refused), std::nullopt);
    // The FETCH waits until the track advanced beyond the Joining Location.
    ScriptedPublisher static_track(setup(), reaction(fetch_ok(7, 4), false));
    EXPECT_EQ(score(probe, static_track), std::nullopt);
    EXPECT_EQ(static_track.sent(5), nullptr);
}

}  // namespace
}  // namespace moq::interop::scenarios
