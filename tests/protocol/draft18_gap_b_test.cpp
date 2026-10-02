#include "moq/interop/scenarios/draft18_gap_a.h"
#include "support/scripted_publisher.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace moq::interop::scenarios {
namespace {
using namespace test;
namespace d18 = wire::draft18;

Bytes setup() { return encode_draft18(d18::SetupMessage{}); }
Bytes ok(d18::Parameters parameters = {}) {
    return encode_draft18(d18::RequestOkMessage{std::move(parameters), {}});
}
Bytes error(std::uint64_t code) {
    return encode_draft18(d18::RequestErrorMessage{code, 0, {}, std::nullopt});
}
Bytes track_status(std::uint64_t request_id) {
    return encode_draft18(d18::TrackStatusMessage{request_id, d18::TrackNamespace{{bytes_of("n")}},
        d18::TrackName{bytes_of("t")}, {}});
}
Bytes publish_namespace(std::uint64_t request_id) {
    return encode_draft18(d18::PublishNamespaceMessage{request_id, d18::TrackNamespace{{bytes_of("n")}}, {}});
}
Bytes publish(std::uint64_t request_id, d18::Parameters parameters = {}) {
    return encode_draft18(d18::PublishMessage{request_id, d18::TrackNamespace{{bytes_of("n")}},
        d18::TrackName{bytes_of("t")}, 9, std::move(parameters), {}});
}
Bytes vi(std::uint64_t value) {
    wire::ByteWriter writer(9);
    if (!wire::write_vi64(value, writer)) throw std::logic_error("vi64");
    return {writer.bytes().begin(), writer.bytes().end()};
}

Draft18GapAProbe profile(const std::string& requirement) {
    auto profiles = draft18_gap_a_probes(std::chrono::milliseconds(200), {bytes_of("n")}, bytes_of("t"));
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
void once(ScriptedPublisher& peer, const std::string& key, const std::function<void()>& action) {
    if (!peer.answered(key)) { peer.mark(key); action(); }
}

// Section 10 (Table 5): TRACK_STATUS is a "First" message.
TEST(Draft18GapB, TrackStatusMustOpenItsOwnBidirectionalStream) {
    const auto probe = profile("D18-10-MUST-004");
    EXPECT_EQ(probe.definition.id, "publisher-queries-track-status-before-resuming-publication");
    EXPECT_EQ(probe.evaluator_id, "request-is-first-message-on-new-bidirectional-stream");
    EXPECT_TRUE(probe.definition.writes.empty());
    ScriptedPublisher first(setup(), [](ScriptedPublisher& peer) {
        once(peer, "q", [&] { peer.data(0, track_status(2)); });
    });
    EXPECT_EQ(score(probe, first), std::optional<bool>{true});
    ScriptedPublisher after_other(setup(), [](ScriptedPublisher& peer) {
        once(peer, "q", [&] { peer.data(0, concat({publish_namespace(2), track_status(4)})); });
    });
    EXPECT_EQ(score(probe, after_other), std::optional<bool>{false});
    ScriptedPublisher on_control(setup(), [](ScriptedPublisher& peer) {
        once(peer, "q", [&] { peer.data(2, track_status(2)); });
    });
    EXPECT_EQ(score(probe, on_control), std::optional<bool>{false});
    ScriptedPublisher other_requests_only(setup(), [](ScriptedPublisher& peer) {
        once(peer, "q", [&] { peer.data(0, publish_namespace(2)); });
    });
    EXPECT_EQ(score(probe, other_requests_only), std::nullopt);
    ScriptedPublisher silent(setup());
    EXPECT_EQ(score(probe, silent), std::nullopt);
}

// Section 10.2: parameters must be defined or negotiated; the probe's SETUP
// negotiates none, so frame 0x0C (not in Table 13) is a violation.
TEST(Draft18GapB, SentMessageParametersMustBeDefined) {
    const auto probe = profile("D18-10-2-MUST-003");
    EXPECT_EQ(probe.definition.id, "publish-with-and-without-parameter-extension-negotiation");
    EXPECT_EQ(probe.evaluator_id, "sent-message-parameters-defined-or-negotiated");
    const auto reaction = [](Bytes subscribe_reply, Bytes status_reply, std::optional<Bytes> announced) {
        return [subscribe_reply, status_reply, announced](ScriptedPublisher& peer) {
            answer(peer, 1, "subscribe", subscribe_reply);
            answer(peer, 5, "status", status_reply, true);
            if (announced && peer.answered("status")) once(peer, "publish", [&] { peer.data(0, *announced); });
        };
    };
    const Bytes subscribe_ok_plain = encode_draft18(d18::SubscribeOkMessage{4, {}, {}});
    const d18::Parameters largest{{0x09, d18::Location{7, 9}}};
    ScriptedPublisher defined(setup(), reaction(encode_draft18(d18::SubscribeOkMessage{4, largest, {}}),
                                                ok(), std::nullopt));
    EXPECT_EQ(score(probe, defined), std::optional<bool>{true});
    ScriptedPublisher in_status(setup(), reaction(subscribe_ok_plain, ok(largest), std::nullopt));
    EXPECT_EQ(score(probe, in_status), std::optional<bool>{true});
    // Nothing carries a parameter: no obligation was exercised.
    ScriptedPublisher none(setup(), reaction(subscribe_ok_plain, ok(), std::nullopt));
    EXPECT_EQ(score(probe, none), std::nullopt);
    const Bytes undefined_ok{std::byte{7}, std::byte{0}, std::byte{3}, std::byte{1}, std::byte{0x0c},
                             std::byte{5}};
    ScriptedPublisher unknown(setup(), reaction(subscribe_ok_plain, undefined_ok, std::nullopt));
    EXPECT_EQ(score(probe, unknown), std::optional<bool>{false});
    ScriptedPublisher in_publish(setup(), reaction(subscribe_ok_plain, ok(), publish(2, largest)));
    EXPECT_EQ(score(probe, in_publish), std::optional<bool>{true});
    ScriptedPublisher silent(setup());
    EXPECT_EQ(score(probe, silent), std::nullopt);
}

// Section 10.2: SUBSCRIBE, PUBLISH_OK and FETCH parameters do not alter payloads.
Bytes fetch_ok() { return encode_draft18(d18::FetchOkMessage{0, {7, 10}, {}, {}}); }
Bytes fetch_data(std::uint64_t request_id, std::string_view payload) {
    return concat({Bytes{std::byte{5}}, vi(request_id), vi(0x1c), vi(7), vi(9), Bytes{std::byte{99}},
                   vi(payload.size()), bytes_of(payload)});
}
Bytes subgroup_with(std::uint64_t alias, std::string_view payload) {
    return concat({vi(0x54), vi(alias), vi(7), vi(0), Bytes{std::byte{99}}, vi(9), vi(payload.size()),
                   bytes_of(payload)});
}

TEST(Draft18GapB, SameObjectPayloadMustNotDependOnRequestParameters) {
    const auto probe = profile("D18-10-2-MUST-NOT-002");
    EXPECT_EQ(probe.evaluator_id, "same-object-payload-independent-of-message-parameters");
    ASSERT_EQ(probe.definition.writes.size(), 3u);
    // The three retrievals differ only in delivery parameters.
    const auto decode = [](const Bytes& bytes) {
        wire::Cursor cursor(bytes);
        const auto decoded = d18::decode_message(d18::StreamRole::Request, cursor, {});
        return std::get<d18::Message>(decoded);
    };
    const auto first = std::get<d18::FetchMessage>(decode(probe.definition.writes[0].bytes));
    const auto second = std::get<d18::FetchMessage>(decode(probe.definition.writes[1].bytes));
    EXPECT_NE(first.parameters.size(), 0u);
    EXPECT_EQ(std::get<d18::StandaloneFetch>(first.fetch).start, std::get<d18::StandaloneFetch>(second.fetch).start);
    EXPECT_NE(std::get<d18::Uint8ParameterValue>(first.parameters[0].value).value,
              std::get<d18::Uint8ParameterValue>(second.parameters[0].value).value);
    const auto third = std::get<d18::SubscribeMessage>(decode(probe.definition.writes[2].bytes));
    EXPECT_EQ(third.request_id, 5u);

    const auto reaction = [](std::string a, std::string b, std::optional<std::string> subscribed) {
        return [a, b, subscribed](ScriptedPublisher& peer) {
            if (peer.sent(1) && !peer.answered("a")) {
                peer.mark("a"); peer.data(1, fetch_ok(), true); peer.data(6, fetch_data(1, a), true);
            }
            if (peer.sent(5) && !peer.answered("b")) {
                peer.mark("b"); peer.data(5, fetch_ok(), true); peer.data(10, fetch_data(3, b), true);
            }
            if (peer.sent(9) && !peer.answered("s")) {
                peer.mark("s");
                peer.data(9, encode_draft18(d18::SubscribeOkMessage{4, {}, {}}));
                if (subscribed) peer.data(14, subgroup_with(4, *subscribed));
            }
        };
    };
    ScriptedPublisher same(setup(), reaction("A", "A", "A"));
    EXPECT_EQ(score(probe, same), std::optional<bool>{true});
    ScriptedPublisher fetch_differs(setup(), reaction("A", "B", "A"));
    EXPECT_EQ(score(probe, fetch_differs), std::optional<bool>{false});
    ScriptedPublisher subscribe_differs(setup(), reaction("A", "A", "B"));
    EXPECT_EQ(score(probe, subscribe_differs), std::optional<bool>{false});
    ScriptedPublisher fetches_only(setup(), reaction("A", "A", std::nullopt));
    EXPECT_EQ(score(probe, fetches_only), std::optional<bool>{true});
    // A single retrieval has nothing to compare against.
    ScriptedPublisher one_fetch(setup(), [](ScriptedPublisher& peer) {
        if (peer.sent(1) && !peer.answered("a")) {
            peer.mark("a"); peer.data(1, fetch_ok(), true); peer.data(6, fetch_data(1, "A"), true);
        }
        answer(peer, 5, "b", error(0x10), true);
        answer(peer, 9, "s", error(0x10), true);
    });
    EXPECT_EQ(score(probe, one_fetch), std::nullopt);
    ScriptedPublisher silent(setup());
    EXPECT_EQ(score(probe, silent), std::nullopt);
}

// Section 10.2.2: no DELETE while a USE_ALIAS message awaits its response.
d18::Parameters token(d18::TokenAliasType type, std::uint64_t alias) {
    d18::Token value{type, alias, std::nullopt, {}};
    if (type == d18::TokenAliasType::Register) { value.token_type = 1; value.token_value = bytes_of("tok"); }
    return {{0x03, value}};
}
Bytes announce(std::uint64_t request_id, d18::Parameters parameters) {
    return encode_draft18(d18::PublishNamespaceMessage{request_id, d18::TrackNamespace{{bytes_of("n")}},
        std::move(parameters)});
}
Bytes update(std::uint64_t request_id, d18::Parameters parameters) {
    return encode_draft18(d18::RequestUpdateMessage{request_id, std::move(parameters)});
}

TEST(Draft18GapB, DeleteMustWaitForEveryUseAliasResponse) {
    const auto probe = profile("D18-10-2-2-MUST-NOT-002");
    EXPECT_EQ(probe.definition.id, "withhold-use-alias-response-while-publisher-retires-token");
    EXPECT_EQ(probe.evaluator_id, "token-delete-not-sent-before-all-use-alias-responses");
    const auto reaction = [](std::function<void(ScriptedPublisher&)> later) {
        return [later](ScriptedPublisher& peer) {
            once(peer, "register", [&] { peer.data(0, announce(2, token(d18::TokenAliasType::Register, 1))); });
            // The runner's REQUEST_OK reaches stream 0 once the registration was seen.
            if (peer.sent(0)) later(peer);
        };
    };
    // DELETE while a USE_ALIAS on another stream is unanswered: violation.
    ScriptedPublisher early(setup(), reaction([](ScriptedPublisher& peer) {
        once(peer, "use", [&] { peer.data(4, announce(4, token(d18::TokenAliasType::UseAlias, 1))); });
        if (peer.answered("use"))
            once(peer, "delete", [&] { peer.data(8, announce(6, token(d18::TokenAliasType::Delete, 1))); });
    }));
    EXPECT_EQ(score(probe, early), std::optional<bool>{false});
    ASSERT_NE(early.sent(0), nullptr);
    EXPECT_EQ(early.sent(0)->bytes, ok());
    EXPECT_EQ(early.sent(4), nullptr);
    // A DELETE with no USE_ALIAS outstanding is allowed.
    ScriptedPublisher clean(setup(), reaction([](ScriptedPublisher& peer) {
        once(peer, "delete", [&] { peer.data(0, update(4, token(d18::TokenAliasType::Delete, 1))); });
    }));
    EXPECT_EQ(score(probe, clean), std::optional<bool>{true});
    // USE_ALIAS on the acknowledged stream itself after the response is answered.
    ScriptedPublisher answered_use(setup(), reaction([](ScriptedPublisher& peer) {
        once(peer, "use", [&] { peer.data(0, update(4, token(d18::TokenAliasType::UseAlias, 1))); });
        if (peer.answered("use"))
            once(peer, "delete", [&] { peer.data(0, update(6, token(d18::TokenAliasType::Delete, 1))); });
    }));
    EXPECT_EQ(score(probe, answered_use), std::optional<bool>{true});
    // No retirement observed: nothing established.
    ScriptedPublisher kept(setup(), reaction([](ScriptedPublisher& peer) {
        once(peer, "use", [&] { peer.data(4, announce(4, token(d18::TokenAliasType::UseAlias, 1))); });
    }));
    EXPECT_EQ(score(probe, kept), std::nullopt);
    // DELETE of an alias the publisher never registered is outside this rule.
    ScriptedPublisher unregistered(setup(), [](ScriptedPublisher& peer) {
        once(peer, "register", [&] { peer.data(0, announce(2, token(d18::TokenAliasType::Register, 1))); });
        if (peer.sent(0)) once(peer, "delete", [&] { peer.data(4, announce(4, token(d18::TokenAliasType::Delete, 7))); });
    });
    EXPECT_EQ(score(probe, unregistered), std::nullopt);
    // Without any token-bearing request the runner never sends the response.
    ScriptedPublisher plain(setup(), [](ScriptedPublisher& peer) {
        once(peer, "announce", [&] { peer.data(0, announce(2, {})); });
    });
    EXPECT_EQ(score(probe, plain), std::nullopt);
    EXPECT_EQ(plain.sent(0), nullptr);
}

// Section 6.1: SUBSCRIBE_TRACKS response and PUBLISH_BLOCKED ordering.
Bytes blocked(std::vector<Bytes> suffix, const std::string& name) {
    return encode_draft18(d18::PublishBlockedMessage{d18::TrackNamespace{std::move(suffix)},
        d18::TrackName{bytes_of(name)}});
}

TEST(Draft18GapB, SubscribeTracksResponseMustPrecedePublishBlocked) {
    const auto probe = profile("D18-6-1-MUST-004");
    EXPECT_EQ(probe.definition.id, "subscribe-tracks-with-no-bidirectional-stream-credit");
    EXPECT_EQ(probe.evaluator_id, "track-subscription-response-precedes-publish-blocked");
    // The peer's only bidirectional stream is spent on its own announcement.
    ASSERT_TRUE(probe.definition.initial_peer_bidi_streams.has_value());
    EXPECT_EQ(*probe.definition.initial_peer_bidi_streams, 1u);
    EXPECT_TRUE(probe.definition.acknowledge_publisher_namespace);
    ASSERT_EQ(probe.definition.writes.size(), 1u);
    ScriptedPublisher in_order(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "a", concat({ok(), blocked({}, "t")}));
    });
    EXPECT_EQ(score(probe, in_order), std::optional<bool>{true});
    ScriptedPublisher rejected(setup(), [](ScriptedPublisher& peer) { answer(peer, 1, "a", error(0x10), true); });
    EXPECT_EQ(score(probe, rejected), std::optional<bool>{true});
    ScriptedPublisher blocked_first(setup(), [](ScriptedPublisher& peer) {
        answer(peer, 1, "a", concat({blocked({}, "t"), ok()}));
    });
    EXPECT_EQ(score(probe, blocked_first), std::optional<bool>{false});
    ScriptedPublisher fragmented(setup(), [](ScriptedPublisher& peer) {
        const auto frame = ok();
        if (peer.sent(1) && !peer.answered("a")) { peer.mark("a"); peer.data(1, Bytes(frame.begin(), frame.begin() + 2)); }
        else if (peer.answered("a") && !peer.answered("b")) { peer.mark("b"); peer.data(1, Bytes(frame.begin() + 2, frame.end())); }
    });
    EXPECT_EQ(score(probe, fragmented), std::optional<bool>{true});
    ScriptedPublisher silent(setup());
    EXPECT_EQ(score(probe, silent), std::nullopt);
}

// Section 6.1: no PUBLISH for a Track after PUBLISH_BLOCKED, even once the
// runner restores bidirectional credit.
TEST(Draft18GapB, PublishMustNotFollowPublishBlockedForTheSameTrack) {
    const auto probe = profile("D18-6-1-MUST-NOT-001");
    EXPECT_EQ(probe.definition.id, "restore-bidi-stream-credit-after-publish-blocked");
    EXPECT_EQ(probe.evaluator_id, "no-publish-for-blocked-track-after-credit-restored");
    ASSERT_EQ(probe.definition.writes.size(), 2u);
    EXPECT_EQ(probe.definition.writes[1].channel, RawProbeChannel::Credit);
    EXPECT_EQ(probe.definition.initial_peer_bidi_streams, std::optional<std::uint64_t>{1});
    const auto run = [&](std::function<void(ScriptedPublisher&)> after_credit, bool send_blocked = true) {
        ScriptedPublisher peer(setup(), [=](ScriptedPublisher& p) {
            if (p.sent(1) && !p.answered("tracks")) {
                p.mark("tracks");
                p.data(1, send_blocked ? concat({ok(), blocked({bytes_of("sub")}, "t")}) : ok());
            }
            if (p.granted_bidi() > 0) after_credit(p);
        });
        const auto result = score(probe, peer);
        EXPECT_EQ(peer.granted_bidi() > 0, send_blocked);
        return result;
    };
    // The credit is only restored once PUBLISH_BLOCKED has arrived.
    EXPECT_EQ(run([](ScriptedPublisher&) {}, false), std::nullopt);
    // A different Track may be published; the blocked one is not.
    EXPECT_EQ(run([](ScriptedPublisher& p) {
        once(p, "p", [&] { p.data(0, publish(2)); });  // namespace {n}, track "t": not the blocked one
    }), std::optional<bool>{true});
    EXPECT_EQ(run([](ScriptedPublisher& p) {
        once(p, "p", [&] {
            p.data(0, encode_draft18(d18::PublishMessage{2, d18::TrackNamespace{{bytes_of("n"), bytes_of("sub")}},
                d18::TrackName{bytes_of("t")}, 9, {}, {}}));
        });
    }), std::optional<bool>{false});
    EXPECT_EQ(run([](ScriptedPublisher&) {}), std::optional<bool>{true});
}

// The controller answers the publisher's PUBLISH_NAMESPACE so that it proceeds.
TEST(Draft18GapB, PublisherNamespaceIsAcknowledgedWithoutBecomingStimulus) {
    const auto probe = profile("D18-10-MUST-004");
    EXPECT_TRUE(probe.definition.acknowledge_publisher_namespace);
    ScriptedPublisher peer(setup(), [](ScriptedPublisher& p) {
        once(p, "announce", [&] { p.data(0, publish_namespace(2)); });
        once(p, "with-params", [&] {
            p.data(4, encode_draft18(d18::PublishNamespaceMessage{4, d18::TrackNamespace{{bytes_of("n")}},
                {{0x03, d18::Token{d18::TokenAliasType::UseValue, std::nullopt, 1, bytes_of("x")}}}}));
        });
        once(p, "track-status", [&] { p.data(8, track_status(6)); });
    });
    const auto transcript = run_probe(peer, probe.definition);
    ASSERT_NE(peer.sent(0), nullptr);
    EXPECT_EQ(peer.sent(0)->bytes, ok());
    EXPECT_FALSE(peer.sent(0)->fin);
    // A token-bearing announcement is never silently authorized.
    EXPECT_EQ(peer.sent(4), nullptr);
    ASSERT_EQ(transcript.acknowledgements.size(), 1u);
    EXPECT_EQ(transcript.acknowledgements.front().stream_id, 0u);
    EXPECT_EQ(evaluate_draft18_gap_a_probe(transcript, probe), std::optional<bool>{true});
}

}  // namespace
}  // namespace moq::interop::scenarios
