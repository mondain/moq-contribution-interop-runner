#include "moq/interop/scenarios/draft18_contribution.h"
#include "../support/contribution_wire.h"

#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using namespace test;
using std::chrono::milliseconds;

constexpr milliseconds kDeadline{60};

std::vector<Draft18ContributionProbe> probes() {
    return draft18_contribution_probes(kDeadline, {text("n")}, text("t"));
}

// Runs `peer` against the probe and evaluates the resulting transcript.
std::optional<bool> run(const Draft18ContributionProbe& p, const std::function<void(PeerView&)>& peer) {
    const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
        v.when("setup", true, [&] { v.data(2, setup_with({})); });
        peer(v);
    });
    return evaluate_draft18_contribution_probe(transcript, p);
}

template <class T>
const T* as(const std::optional<d18::Message>& message) {
    return message ? std::get_if<T>(&*message) : nullptr;
}

TEST(Draft18ContributionSubscribe, SubscribeNamesTheFixtureTrack) {
    const auto all = probes();
    const auto& p = probe(all, "accept-subscribe-for-known-publisher-track", "D18-10-7-MUST-001");
    EXPECT_EQ(p.evaluator_id, "successful-subscribe-responds-subscribe-ok");
    EXPECT_TRUE(p.requires_track);
    const auto message = decode_request(p.definition.writes.front().bytes);
    const auto* subscribe = as<d18::SubscribeMessage>(message);
    ASSERT_NE(subscribe, nullptr);
    EXPECT_EQ(subscribe->request_id, 1u);
    ASSERT_EQ(subscribe->track_namespace.fields.size(), 1u);
    EXPECT_EQ(subscribe->track_namespace.fields[0], text("n"));
    EXPECT_EQ(subscribe->track_name.bytes, text("t"));
}

// Section 10.7: a successful SUBSCRIBE is answered with SUBSCRIBE_OK.
TEST(Draft18ContributionSubscribe, SuccessfulSubscribeMustBeAnsweredWithSubscribeOk) {
    const auto all = probes();
    const auto& p = probe(all, "accept-subscribe-for-known-publisher-track", "D18-10-7-MUST-001");
    const auto reply = [](Bytes bytes) { return [bytes](PeerView& v) {
        v.when("reply", v.sent(1), [&] { v.data(1, bytes); }); }; };
    EXPECT_EQ(run(p, reply(subscribe_ok())), std::optional<bool>{true});
    // A refusal does not show how success is answered.
    EXPECT_EQ(run(p, reply(error(0x10))), std::nullopt);
    // Any other first response to a SUBSCRIBE is wrong.
    EXPECT_EQ(run(p, reply(ok())), std::optional<bool>{false});
    EXPECT_EQ(run(p, [](PeerView&) {}), std::nullopt);
}

// Section 10.7: forwarded Objects follow a successful Forward State 1 subscription.
TEST(Draft18ContributionSubscribe, ObjectOnTheAssignedAliasProvesDelivery) {
    const auto all = probes();
    const auto& p = probe(all, "accept-forward-one-subscribe-then-publish-matching-object", "D18-10-7-MUST-002");
    EXPECT_EQ(p.evaluator_id, "matching-new-object-delivered-on-established-subscription");
    const auto message = decode_request(p.definition.writes.front().bytes);
    const auto* subscribe = as<d18::SubscribeMessage>(message);
    ASSERT_NE(subscribe, nullptr);
    ASSERT_EQ(subscribe->parameters.size(), 1u);
    EXPECT_EQ(subscribe->parameters[0].type, 0x10u);  // FORWARD
    EXPECT_EQ(std::get<d18::Uint8ParameterValue>(subscribe->parameters[0].value).value, 1u);
    const auto delivered = [&](std::uint64_t alias, bool datagram) {
        return run(p, [&](PeerView& v) {
            v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5)); });
            v.when("object", v.sent(1) && v.step > 3, [&] {
                if (datagram) {
                    auto bytes = vi(0x00);
                    bytes = concat(bytes, vi(alias));
                    bytes = concat(bytes, vi(2));   // group
                    bytes = concat(bytes, vi(0));   // object
                    bytes.push_back(std::byte{128});
                    bytes = concat(bytes, text("hi"));
                    v.push(transport::DatagramEvent{bytes});
                } else {
                    v.data(6, concat(subgroup_header(alias, 2), subgroup_object(0, text("hi"))));
                }
            });
        });
    };
    EXPECT_EQ(delivered(5, false), std::optional<bool>{true});
    EXPECT_EQ(delivered(5, true), std::optional<bool>{true});
    // An Object for another alias is not this subscription's Object.
    EXPECT_EQ(delivered(9, false), std::nullopt);
    EXPECT_EQ(delivered(9, true), std::nullopt);
}

// Section 10.9: exactly one REQUEST_OK or REQUEST_ERROR answers a REQUEST_UPDATE.
TEST(Draft18ContributionSubscribe, UpdateGetsExactlyOneResponse) {
    const auto all = probes();
    const auto& p = probe(all, "receive-one-request-update-on-established-publisher-request", "D18-10-9-MUST-001");
    EXPECT_EQ(p.evaluator_id, "exactly-one-request-ok-or-request-error");
    ASSERT_EQ(p.definition.writes.size(), 2u);
    EXPECT_EQ(p.definition.writes[1].reuse_write_stream, 0u);
    ASSERT_TRUE(p.definition.writes[1].peer_response_ready);
    const auto update = decode_request(p.definition.writes[1].bytes);
    ASSERT_NE(as<d18::RequestUpdateMessage>(update), nullptr);
    EXPECT_EQ(as<d18::RequestUpdateMessage>(update)->request_id, 3u);
    const auto answer = [&](std::vector<Bytes> replies) {
        return run(p, [&](PeerView& v) {
            v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok()); });
            v.when("replies", v.sent(1) && v.transport.output[1].size() > 8, [&] {
                for (const auto& reply : replies) v.data(1, reply);
            });
        });
    };
    EXPECT_EQ(answer({ok()}), std::optional<bool>{true});
    EXPECT_EQ(answer({error(0x3)}), std::optional<bool>{true});
    EXPECT_EQ(answer({ok(), ok()}), std::optional<bool>{false});
    EXPECT_EQ(answer({ok(), error(0x3)}), std::optional<bool>{false});
    EXPECT_EQ(answer({}), std::nullopt);
}

// Section 10.9.1: a REQUEST_OK is sent for each successful update.
TEST(Draft18ContributionSubscribe, EachCoalescedUpdateGetsItsOwnOk) {
    const auto all = probes();
    const auto& p = probe(all, "receive-multiple-successful-request-updates-on-one-stream", "D18-10-9-1-MUST-005");
    EXPECT_EQ(p.evaluator_id, "one-request-ok-per-successful-coalesced-update");
    ASSERT_EQ(p.definition.writes.size(), 4u);
    std::set<std::uint64_t> ids;
    for (std::size_t i = 1; i < 4; ++i) {
        EXPECT_EQ(p.definition.writes[i].reuse_write_stream, 0u);
        EXPECT_EQ(p.definition.writes[i].peer_response_ready ? 1 : 0, i == 1 ? 1 : 0);
        const auto message = decode_request(p.definition.writes[i].bytes);
        ASSERT_NE(as<d18::RequestUpdateMessage>(message), nullptr);
        ids.insert(as<d18::RequestUpdateMessage>(message)->request_id);
    }
    EXPECT_EQ(ids, (std::set<std::uint64_t>{3, 5, 7}));
    const auto answer = [&](std::vector<Bytes> replies) {
        return run(p, [&](PeerView& v) {
            v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok()); });
            v.when("replies", v.sent(1) && v.transport.output[1].size() > 20, [&] {
                for (const auto& reply : replies) v.data(1, reply);
            });
        });
    };
    EXPECT_EQ(answer({ok(), ok(), ok()}), std::optional<bool>{true});
    // One reply for three successful updates breaks the rule once the window passes.
    EXPECT_EQ(answer({ok()}), std::optional<bool>{false});
    EXPECT_EQ(answer({ok(), ok()}), std::optional<bool>{false});
    // A single REQUEST_ERROR for a failed batch is allowed and says nothing.
    EXPECT_EQ(answer({error(0x3)}), std::nullopt);
    EXPECT_EQ(answer({}), std::nullopt);
}

// Section 10.11: Stream Count is 0 when no data stream was opened.
TEST(Draft18ContributionSubscribe, PublishDoneReportsZeroStreamsWhenNoneWereOpened) {
    const auto all = probes();
    const auto& p = probe(all, "publisher-ends-subscription-with-no-data-streams", "D18-10-11-MUST-001");
    EXPECT_EQ(p.evaluator_id, "publish-done-stream-count-zero");
    const auto message = decode_request(p.definition.writes.front().bytes);
    const auto* subscribe = as<d18::SubscribeMessage>(message);
    ASSERT_NE(subscribe, nullptr);
    EXPECT_EQ(std::get<d18::Uint8ParameterValue>(subscribe->parameters.at(0).value).value, 0u);
    const auto done = [&](std::uint64_t count, bool with_stream) {
        return run(p, [&](PeerView& v) {
            v.when("ok", v.sent(1), [&] {
                v.data(1, concat(subscribe_ok(),
                                 encode(d18::PublishDoneMessage{0x3, count, d18::ReasonPhrase{}})));
                if (with_stream) v.data(6, subgroup_header(5, 2));
            });
        });
    };
    EXPECT_EQ(done(0, false), std::optional<bool>{true});
    EXPECT_EQ(done(1, false), std::optional<bool>{false});
    EXPECT_EQ(done((1ull << 62) - 1, false), std::optional<bool>{false});
    // A data stream means Forward State 0 was not honoured; the count is then unknowable.
    EXPECT_EQ(done(0, true), std::nullopt);
    EXPECT_EQ(run(p, [](PeerView& v) { v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok()); }); }),
              std::nullopt);
}

struct FetchCase { const char* scenario; const char* requirement; };

// Sections 10.12.2 and 10.12.3: INVALID_RANGE answers an unservable fetch.
TEST(Draft18ContributionSubscribe, JoiningFetchOfForwardZeroSubscriptionIsInvalidRange) {
    const auto all = probes();
    const auto& p = probe(all, "receive-joining-fetch-for-forward-zero-subscription", "D18-10-12-2-MUST-001");
    EXPECT_EQ(p.evaluator_id, "request-error-invalid-range");
    ASSERT_EQ(p.definition.writes.size(), 2u);
    const auto subscribe = decode_request(p.definition.writes[0].bytes);
    EXPECT_EQ(std::get<d18::Uint8ParameterValue>(as<d18::SubscribeMessage>(subscribe)->parameters.at(0).value).value, 0u);
    const auto joining = decode_request(p.definition.writes[1].bytes);
    const auto* fetch = as<d18::FetchMessage>(joining);
    ASSERT_NE(fetch, nullptr);
    EXPECT_EQ(fetch->request_id, 3u);
    const auto* relative = std::get_if<d18::RelativeJoiningFetch>(&fetch->fetch);
    ASSERT_NE(relative, nullptr);
    EXPECT_EQ(relative->joining_request_id, 1u);
    const auto answer = [&](std::optional<Bytes> second, bool establish = true) {
        return run(p, [&](PeerView& v) {
            if (establish) v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok()); });
            v.when("fetch-sent", v.sent(5), [&] { EXPECT_TRUE(establish); });
            if (second) v.when("reply", v.sent(5), [&] { v.data(5, *second); });
        });
    };
    EXPECT_EQ(answer(error(0x11)), std::optional<bool>{true});
    EXPECT_EQ(answer(error(0x10)), std::optional<bool>{false});
    EXPECT_EQ(answer(encode(d18::FetchOkMessage{0, {2, 3}, {}, {}})), std::optional<bool>{false});
    EXPECT_EQ(answer(error(0x9)), std::nullopt);
    EXPECT_EQ(answer(std::nullopt), std::nullopt);
    // The joining request must not be sent before the subscription is established.
    EXPECT_EQ(answer(error(0x11), false), std::nullopt);
}

TEST(Draft18ContributionSubscribe, EmptyTrackRowsRequireSubscribeOkWithoutLargestObject) {
    const auto all = probes();
    struct Row { const char* scenario; const char* requirement; bool joining; };
    for (const auto& row : {Row{"receive-joining-fetch-for-track-with-no-published-objects", "D18-10-12-2-MUST-003", true},
                            Row{"receive-standalone-fetch-for-track-with-no-published-objects", "D18-10-12-3-MUST-004", false}}) {
        SCOPED_TRACE(row.requirement);
        const auto& p = probe(all, row.scenario, row.requirement);
        EXPECT_EQ(p.evaluator_id, "request-error-invalid-range");
        const auto fetch = decode_request(p.definition.writes[1].bytes);
        const auto* message = as<d18::FetchMessage>(fetch);
        ASSERT_NE(message, nullptr);
        EXPECT_EQ(std::holds_alternative<d18::RelativeJoiningFetch>(message->fetch), row.joining);
        EXPECT_EQ(std::holds_alternative<d18::StandaloneFetch>(message->fetch), !row.joining);
        const auto answer = [&](d18::Parameters parameters, Bytes second) {
            return run(p, [&](PeerView& v) {
                v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5, parameters)); });
                v.when("reply", v.sent(5), [&] { v.data(5, second); });
            });
        };
        EXPECT_EQ(answer({}, error(0x11)), std::optional<bool>{true});
        EXPECT_EQ(answer({}, error(0x10)), std::optional<bool>{false});
        EXPECT_EQ(answer({}, encode(d18::FetchOkMessage{0, {0, 1}, {}, {}})), std::optional<bool>{false});
        EXPECT_EQ(answer({}, error(0x2)), std::nullopt);
        // Published Objects mean the track is not empty: the precondition fails.
        EXPECT_EQ(answer({largest(3, 4)}, error(0x11)), std::nullopt);
        EXPECT_EQ(answer({largest(3, 4)}, encode(d18::FetchOkMessage{0, {3, 5}, {}, {}})), std::nullopt);
    }
}

TEST(Draft18ContributionSubscribe, FetchStartingBeyondLargestObjectIsInvalidRange) {
    const auto all = probes();
    const auto& p = probe(all, "receive-fetch-start-beyond-largest-published-object", "D18-10-12-3-MUST-005");
    EXPECT_EQ(p.evaluator_id, "request-error-invalid-range");
    ASSERT_EQ(p.definition.writes.size(), 2u);
    ASSERT_TRUE(p.definition.writes[1].prepare_bytes);
    std::optional<d18::Location> start;
    const auto answer = [&](d18::Parameters parameters, Bytes second) {
        const auto transcript = drive_probe(p.definition, [&](PeerView& v) {
            v.when("setup", true, [&] { v.data(2, setup_with({})); });
            v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5, parameters)); });
            v.when("reply", v.sent(5), [&] {
                const auto message = decode_request(v.transport.output[5]);
                const auto* fetch = as<d18::FetchMessage>(message);
                ASSERT_NE(fetch, nullptr);
                start = std::get<d18::StandaloneFetch>(fetch->fetch).start;
                v.data(5, second);
            });
        });
        return evaluate_draft18_contribution_probe(transcript, p);
    };
    EXPECT_EQ(answer({largest(7, 9)}, error(0x11)), std::optional<bool>{true});
    ASSERT_TRUE(start);
    EXPECT_GT(start->group, 7u + 1000);
    EXPECT_EQ(answer({largest(7, 9)}, encode(d18::FetchOkMessage{0, {8, 1}, {}, {}})), std::optional<bool>{false});
    EXPECT_EQ(answer({largest(7, 9)}, error(0x10)), std::optional<bool>{false});
    EXPECT_EQ(answer({largest(7, 9)}, error(0x2)), std::nullopt);
    // Without a Largest Object nothing can be requested beyond it: no stimulus.
    EXPECT_EQ(answer({}, error(0x11)), std::nullopt);
}

// Sections 5.1 and 10.12.2: the update is processed before the Joining Fetch
// and the fetch ends at the Joining Location from REQUEST_UPDATE_OK.
TEST(Draft18ContributionSubscribe, JoiningFetchUsesTheUpdatedForwardStateAndJoiningLocation) {
    const auto all = probes();
    const auto& p = probe(all, "receive-forward-state-update-then-joining-fetch", "D18-10-12-2-MUST-002");
    EXPECT_EQ(p.evaluator_id, "joining-fetch-uses-updated-forward-state-and-joining-location");
    ASSERT_EQ(p.definition.writes.size(), 3u);
    const auto update = decode_request(p.definition.writes[1].bytes);
    ASSERT_NE(as<d18::RequestUpdateMessage>(update), nullptr);
    EXPECT_EQ(std::get<d18::Uint8ParameterValue>(as<d18::RequestUpdateMessage>(update)->parameters.at(0).value).value, 1u);
    const auto answer = [&](Bytes update_reply, Bytes fetch_reply) {
        return run(p, [&](PeerView& v) {
            v.when("ok", v.sent(1), [&] { v.data(1, subscribe_ok(5, {largest(1, 1)})); });
            v.when("update", v.sent(1) && v.sent(5) && v.transport.output[1].size() > 8, [&] {
                v.data(5, fetch_reply);
                v.data(1, update_reply);
            });
        });
    };
    const auto fetch_ok = [](std::uint64_t group, std::uint64_t object) {
        return encode(d18::FetchOkMessage{0, {group, object}, {}, {}});
    };
    // Joining Location {4, 2} comes from the update, so the fetch ends at {4, 3}.
    EXPECT_EQ(answer(ok({largest(4, 2)}), fetch_ok(4, 3)), std::optional<bool>{true});
    EXPECT_EQ(answer(ok({largest(4, 2)}), fetch_ok(1, 2)), std::optional<bool>{false});
    EXPECT_EQ(answer(ok({largest(4, 2)}), error(0x11)), std::optional<bool>{false});
    // Without LARGEST_OBJECT no Object was published, so INVALID_RANGE is right.
    EXPECT_EQ(answer(ok(), error(0x11)), std::optional<bool>{true});
    EXPECT_EQ(answer(ok(), fetch_ok(0, 1)), std::optional<bool>{false});
    EXPECT_EQ(answer(error(0x3), fetch_ok(4, 3)), std::nullopt);
    EXPECT_EQ(answer(ok({largest(4, 2)}), error(0x2)), std::nullopt);
}

// Section 10.6.1: a redirect for SUBSCRIBE_NAMESPACE leaves Track Name empty.
TEST(Draft18ContributionSubscribe, NamespaceRedirectLeavesTrackNameEmpty) {
    const auto all = probes();
    const auto& p = probe(all, "publisher-redirects-subscribe-namespace", "D18-10-6-1-MUST-002");
    EXPECT_EQ(p.evaluator_id, "namespace-redirect-track-name-empty");
    const auto message = decode_request(p.definition.writes.front().bytes);
    const auto* request = as<d18::SubscribeNamespaceMessage>(message);
    ASSERT_NE(request, nullptr);
    ASSERT_EQ(request->track_namespace_prefix.fields.size(), 1u);
    EXPECT_EQ(request->track_namespace_prefix.fields[0], text("n"));
    const auto redirect = [&](std::vector<std::byte> track_name) {
        return run(p, [&](PeerView& v) {
            v.when("reply", v.sent(1), [&] {
                v.data(1, encode(d18::RequestErrorMessage{0x34, 0, {}, d18::Redirect{
                    text("moqt://other"), d18::TrackNamespace{{text("n")}}, d18::TrackName{track_name}}}));
            });
        });
    };
    EXPECT_EQ(redirect({}), std::optional<bool>{true});
    EXPECT_EQ(redirect(text("t")), std::optional<bool>{false});
    // No redirect, no rule to check.
    EXPECT_EQ(run(p, [](PeerView& v) { v.when("reply", v.sent(1), [&] { v.data(1, error(0x3)); }); }),
              std::nullopt);
}

// Section 10.18: NAMESPACE_DONE only follows its NAMESPACE.
TEST(Draft18ContributionSubscribe, NamespaceDoneFollowsItsNamespace) {
    const auto all = probes();
    const auto& p = probe(all, "publish-and-withdraw-namespace-during-discovery", "D18-10-18-MUST-NOT-001");
    EXPECT_EQ(p.evaluator_id, "namespace-precedes-corresponding-namespace-done");
    const auto suffix = d18::TrackNamespace{{text("s")}};
    const auto announce = encode(d18::NamespaceMessage{suffix});
    const auto withdraw = encode(d18::NamespaceDoneMessage{suffix});
    const auto other = encode(d18::NamespaceDoneMessage{d18::TrackNamespace{{text("q")}}});
    const auto stream = [&](std::vector<Bytes> messages) {
        return run(p, [&](PeerView& v) {
            v.when("reply", v.sent(1), [&] {
                Bytes data = ok();
                for (const auto& message : messages) data = concat(data, message);
                v.data(1, data);
            });
        });
    };
    EXPECT_EQ(stream({announce, withdraw}), std::optional<bool>{true});
    EXPECT_EQ(stream({withdraw}), std::optional<bool>{false});
    EXPECT_EQ(stream({announce, other}), std::optional<bool>{false});
    EXPECT_EQ(stream({announce, withdraw, withdraw}), std::optional<bool>{false});
    EXPECT_EQ(stream({announce}), std::nullopt);
}

// Section 14: an unknown Auth Token Type alone does not end the session.
TEST(Draft18ContributionSubscribe, UnknownAuthTokenTypeDoesNotCloseTheSession) {
    const auto all = probes();
    const auto& p = probe(all, "subscribe-unknown-auth-token-type", "D18-14-MUST-007");
    EXPECT_EQ(p.evaluator_id, "unknown-auth-token-type-does-not-close-session");
    const auto message = decode_request(p.definition.writes.front().bytes);
    const auto* subscribe = as<d18::SubscribeMessage>(message);
    ASSERT_NE(subscribe, nullptr);
    ASSERT_EQ(subscribe->parameters.size(), 1u);
    const auto& token = std::get<d18::Token>(subscribe->parameters[0].value);
    EXPECT_EQ(token.alias_type, d18::TokenAliasType::UseValue);
    EXPECT_EQ(token.token_type, 0x9du);
    const auto reply = [&](Bytes bytes) { return run(p, [&](PeerView& v) {
        v.when("reply", v.sent(1), [&] { v.data(1, bytes); }); }); };
    EXPECT_EQ(reply(subscribe_ok()), std::optional<bool>{true});
    // A request-level rejection is permitted.
    EXPECT_EQ(reply(error(0x4)), std::optional<bool>{true});
    EXPECT_EQ(run(p, [](PeerView& v) {
        v.when("close", v.sent(1), [&] {
            v.push(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
        });
    }), std::optional<bool>{false});
    EXPECT_EQ(run(p, [](PeerView&) {}), std::nullopt);
}

}  // namespace
}  // namespace moq::interop::scenarios
