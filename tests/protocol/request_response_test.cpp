#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/scenarios/request_response.h"
#include <filesystem>
#include <gtest/gtest.h>
namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes bytes(std::initializer_list<unsigned> v) {
  Bytes r;
  for (auto x : v)
    r.push_back(static_cast<std::byte>(x));
  return r;
}
RawProbeTranscript stimulus(const RequestResponseProbe &p) {
  RawProbeTranscript t;
  t.scenario_id = p.definition.id;
  t.setup = {
      {RawProbeChannel::NewUni, bytes({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
  t.events = {transport::ConnectionEstablishedEvent{},
              transport::StreamDataEvent{2, bytes({0xaf, 0, 0, 0}), false}};
  auto w = p.definition.writes.front();
  t.writes.push_back({w, 1, w.bytes.size(), true, 2});
  t.complete = t.stimulus_delivered = t.transport_established =
      t.peer_setup_received = true;
  t.delivery_event_count = 2;
  return t;
}
Bytes reply(std::size_t i) {
  return i % 2 ? bytes({5, 0, 3, 1, 0, 0})
               : (i < 2 ? bytes({4, 0, 2, 0, 0}) : bytes({7, 0, 1, 0}));
}
TEST(RequestResponse, SixCanonicalProfiles) {
  const auto ps = draft21_request_response_probes();
  ASSERT_EQ(ps.size(), 6u);
  for (std::size_t i = 0; i < ps.size(); ++i) {
    auto t = stimulus(ps[i]);
    EXPECT_EQ(t.writes.front().write.bytes,
              i < 2   ? bytes({3, 0, 5, 1, 0, 1, 'x', 0})
              : i < 4 ? bytes({0x50, 0, 3, 1, 0, 0})
                      : bytes({0x51, 0, 3, 1, 0, 0}));
    t.events.push_back(transport::StreamDataEvent{1, reply(i), false});
    EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), std::nullopt);
    EXPECT_FALSE(ps[i].definition.response_ready(t));
    t.events.push_back(transport::StreamDataEvent{1, {}, true});
    EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), true);
    EXPECT_TRUE(ps[i].definition.response_ready(t));
    auto other = stimulus(ps[i ^ 1]);
    other.events.insert(other.events.end(), t.events.begin() + 2,
                        t.events.end());
    EXPECT_EQ(evaluate_request_response_probe(other, ps[i ^ 1]), std::nullopt);
  }
}
TEST(RequestResponse, DuplicatesFailWithoutFinAndEmptyFinFails) {
  const auto ps = draft21_request_response_probes();
  for (std::size_t i = 0; i < ps.size(); ++i) {
    for (const auto &first : {reply(i & ~1u), reply(i | 1u)}) {
      auto t = stimulus(ps[i]);
      t.events.push_back(transport::StreamDataEvent{1, first, false});
      t.events.push_back(transport::StreamDataEvent{1, reply(i), false});
      EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), false);
      EXPECT_TRUE(ps[i].definition.response_ready(t));
    }
    auto t = stimulus(ps[i]);
    t.events.push_back(transport::StreamDataEvent{1, {}, true});
    EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), false);
  }
}
TEST(RequestResponse, DiscoveryRequiresFirstResponseButSubscribeRowOnlyCounts) {
  const auto ps = draft21_request_response_probes();
  for (std::size_t i = 0; i < ps.size(); ++i) {
    auto t = stimulus(ps[i]);
    // A complete, framed NAMESPACE message is sufficient to establish
    // that the first discovery frame is neither OK nor ERROR.
    t.events.push_back(
        transport::StreamDataEvent{1, bytes({8, 0, 1, 0}), false});
    if (i >= 2) {
      EXPECT_TRUE(ps[i].definition.response_ready(t));
      EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), false);
    }
    t.events.push_back(transport::StreamDataEvent{1, reply(i), true});
    EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), i < 2);
  }
}
TEST(RequestResponse, FragmentedResponseAndUnrelatedStreamAreHandled) {
  const auto ps = draft21_request_response_probes();
  for (std::size_t i = 0; i < ps.size(); ++i) {
    auto t = stimulus(ps[i]);
    t.events.push_back(transport::StreamDataEvent{5, reply(i), true});
    auto r = reply(i);
    t.events.push_back(
        transport::StreamDataEvent{1, Bytes(r.begin(), r.begin() + 2), false});
    t.events.push_back(
        transport::StreamDataEvent{1, Bytes(r.begin() + 2, r.end()), true});
    EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), true);
  }
}
TEST(RequestResponse, MalformedTypedReplyOrIncompleteFrameIsUnscored) {
  const auto ps = draft21_request_response_probes();
  const std::vector<Bytes> malformed = {
      bytes({5, 0, 4, 1, 0, 1, 0x80}),
      bytes({5, 0, 3, 1}),
  };
  for (const auto &p : ps) {
    for (const auto &r : malformed) {
      auto t = stimulus(p);
      t.events.push_back(transport::StreamDataEvent{1, r, true});
      EXPECT_EQ(evaluate_request_response_probe(t, p), std::nullopt);
    }
  }
  for (std::size_t i = 0; i < ps.size(); i += 2) {
    auto t = stimulus(ps[i]);
    // Parameter 9 is only valid for SUBSCRIBE; namespace/tracks forbid it.
    const auto bad =
        i == 0 ? bytes({4, 0, 4, 0, 1, 8, 0}) : bytes({7, 0, 4, 1, 9, 0, 0});
    t.events.push_back(transport::StreamDataEvent{1, bad, true});
    if (i == 0)
      EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), true);
    else
      EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), std::nullopt);
  }
  auto t = stimulus(ps[2]);
  t.events.push_back(
      transport::StreamDataEvent{1, bytes({7, 0, 3, 0, 8, 0}), true});
  EXPECT_EQ(evaluate_request_response_probe(t, ps[2]), std::nullopt);
  t = stimulus(ps[4]);
  t.events.push_back(
      transport::StreamDataEvent{1, bytes({7, 0, 3, 0, 0x30, 2}), true});
  EXPECT_EQ(evaluate_request_response_probe(t, ps[4]), std::nullopt);
}
TEST(RequestResponse, ResetCloseMissingFinAndInvalidHarnessAreUnscored) {
  const auto ps = draft21_request_response_probes();
  for (std::size_t i = 0; i < ps.size(); ++i) {
    for (unsigned v = 0; v < 12; ++v) {
      auto t = stimulus(ps[i]);
      t.events.push_back(transport::StreamDataEvent{1, reply(i), v >= 3});
      if (v == 0)
        t.events.push_back(transport::PeerResetEvent{1, 1});
      if (v == 1)
        t.events.push_back(transport::PeerCloseEvent{
            transport::CloseErrorSpace::Application, 0, {}});
      if (v == 3)
        t.harness_failed = true;
      if (v == 4)
        t.timed_out = true;
      if (v == 5)
        t.complete = false;
      if (v == 6)
        t.stimulus_delivered = false;
      if (v == 7)
        t.writes.front().fin_accepted = false;
      if (v == 8) {
        t.writes.front().stream_id = 5;
        std::get<transport::StreamDataEvent>(t.events.back()).stream_id = 5;
      }
      if (v == 9)
        t.events.resize(4097);
      if (v == 10)
        t.events.push_back(transport::StreamDataEvent{1, reply(i), false});
      if (v == 11) {
        t.events.insert(t.events.begin() + 1,
                        transport::StreamDataEvent{1, reply(i), false});
        t.writes.front().delivery_event_count = t.delivery_event_count = 3;
      }
      EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), std::nullopt) << v;
    }
  }
}
TEST(RequestResponse, StrictConfiguredFixtureAndBounds) {
  const auto ps = draft21_request_response_probes(
      std::chrono::milliseconds{1000}, {bytes({'n'})}, bytes({'t'}));
  EXPECT_THROW(draft21_request_response_probes(std::chrono::milliseconds{0}),
               std::invalid_argument);
  EXPECT_THROW(draft21_request_response_probes(std::chrono::milliseconds{1},
                                               {bytes({'.'})}),
               std::invalid_argument);
  EXPECT_THROW(
      draft21_request_response_probes(std::chrono::milliseconds{1}, {Bytes{}}),
      std::invalid_argument);
  EXPECT_THROW(
      draft21_request_response_probes(std::chrono::milliseconds{1},
                                      std::vector<Bytes>(33, bytes({'n'}))),
      std::invalid_argument);
  EXPECT_THROW(draft21_request_response_probes(std::chrono::milliseconds{1},
                                               {Bytes(4097, std::byte{'n'})}),
               std::invalid_argument);
  EXPECT_EQ(draft21_request_response_probes(
                std::chrono::milliseconds{1},
                {bytes({'.', 's', 'e', 's', 's', 'i', 'o', 'n'})}, {})
                .size(),
            4u);
  for (std::size_t i = 0; i < ps.size(); ++i) {
    auto t = stimulus(ps[i]);
    t.events.push_back(transport::StreamDataEvent{1, reply(i), true});
    EXPECT_EQ(evaluate_request_response_probe(t, ps[i]), true);
    for (unsigned v = 0; v < 4; ++v) {
      auto bad = t;
      auto &b = bad.writes.front().write.bytes;
      if (v == 0)
        b[i < 2 ? 3 : 4] = std::byte{3};
      if (v == 1)
        b.back() = std::byte{1};
      if (v == 2)
        b.push_back(std::byte{0});
      if (v == 3)
        bad.setup.write.bytes = bytes({0xaf, 0, 0, 1});
      EXPECT_EQ(evaluate_request_response_probe(bad, ps[i]), std::nullopt);
    }
  }
}
TEST(RequestResponse, DiscoveryTrackPropertiesAndBoundedPendingBytes) {
  const auto ps = draft21_request_response_probes();
  auto t = stimulus(ps[4]);
  t.events.push_back(
      transport::StreamDataEvent{1, bytes({7, 0, 3, 0, 0x30, 1}), true});
  EXPECT_EQ(evaluate_request_response_probe(t, ps[4]), true);
  t = stimulus(ps[0]);
  t.events.push_back(
      transport::StreamDataEvent{1, bytes({4, 0, 3, 0, 0, 0x30}), true});
  EXPECT_EQ(evaluate_request_response_probe(t, ps[0]), std::nullopt);
  t = stimulus(ps[0]);
  t.events.push_back(
      transport::StreamDataEvent{1, Bytes(2 * 65546 + 1, std::byte{0}), false});
  EXPECT_EQ(evaluate_request_response_probe(t, ps[0]), std::nullopt);
}
TEST(RequestResponse, RedirectErrorUsesRequestNamespaceScope) {
  const auto ps = draft21_request_response_probes();
  // Client Redirect URI is empty. Namespace contains one field 'n'.
  const auto track_redirect = bytes({5, 0, 9, 0x34, 0, 0, 0, 1, 1, 'n', 1, 't'});
  const auto namespace_redirect = bytes({5, 0, 8, 0x34, 0, 0, 0, 1, 1, 'n', 0});
  auto t = stimulus(ps[1]);
  t.events.push_back(transport::StreamDataEvent{1, track_redirect, true});
  EXPECT_EQ(evaluate_request_response_probe(t, ps[1]), true);
  for (const auto index : {3u, 5u}) {
    t = stimulus(ps[index]);
    t.events.push_back(transport::StreamDataEvent{1, track_redirect, true});
    EXPECT_EQ(evaluate_request_response_probe(t, ps[index]), std::nullopt);
    t = stimulus(ps[index]);
    t.events.push_back(transport::StreamDataEvent{1, namespace_redirect, true});
    EXPECT_EQ(evaluate_request_response_probe(t, ps[index]), true);
  }
}
TEST(RequestResponse, CatalogRequiresEveryUniqueContextAndFailureDominates) {
  using namespace requirements;
  const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
  const auto source = load_draft_source(
      21, root / "docs", root / "requirements/draft-digests.json");
  const auto catalog =
      RequirementCatalog::load(source, root / "requirements/draft21.json");
  const auto ps = draft21_request_response_probes();
  std::vector<RawProbeTranscript> ts;
  for (std::size_t i = 0; i < ps.size(); ++i) {
    auto t = stimulus(ps[i]);
    t.events.push_back(transport::StreamDataEvent{1, reply(i), true});
    ts.push_back(t);
  }
  const auto state = [&](const auto &input, const std::string &id) {
    for (const auto &o : evaluate_draft21_raw_probes(catalog, input))
      if (o.requirement_id == id)
        return o.state;
    return OutcomeState::NotRun;
  };
  const std::vector<std::string> ids{"D21-3-1-MUST-033", "D21-4-1-MUST-082", "D21-4-1-MUST-083"};
  for (std::size_t i = 0; i < 6; i += 2) {
    EXPECT_EQ(state(ts, ids[i / 2]), OutcomeState::Pass);
    auto partial = ts;
    partial.erase(partial.begin() + static_cast<std::ptrdiff_t>(i + 1));
    EXPECT_EQ(state(partial, ids[i / 2]), OutcomeState::NotRun);
    auto duplicate = ts;
    duplicate.push_back(ts[i]);
    EXPECT_EQ(state(duplicate, ids[i / 2]), OutcomeState::NotRun);
    auto failed = partial;
    auto &r =
        std::get<transport::StreamDataEvent>(failed[i].events.back()).data;
    const auto second = r;
    r.insert(r.end(), second.begin(), second.end());
    EXPECT_EQ(state(failed, ids[i / 2]), OutcomeState::Fail);
  }
}
} // namespace
} // namespace moq::interop::scenarios
