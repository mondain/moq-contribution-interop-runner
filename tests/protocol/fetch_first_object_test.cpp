#include "moq/interop/requirements/draft18_evaluators.h"
#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/scenarios/fetch_first_object.h"
#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> v) {
  Bytes r;
  for (auto x : v)
    r.push_back(static_cast<std::byte>(x));
  return r;
}
std::vector<FetchFirstObjectProbe> all() {
  auto r = draft18_fetch_first_object_probes();
  auto q = draft21_fetch_first_object_probes();
  r.insert(r.end(), q.begin(), q.end());
  return r;
}
RawProbeTranscript stimulus(const FetchFirstObjectProbe &p) {
  RawProbeTranscript t;
  t.scenario_id = p.definition.id;
  t.setup = {
      {RawProbeChannel::NewUni, b({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
  t.events = {transport::ConnectionEstablishedEvent{},
              transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false}};
  auto w = p.definition.writes.front();
  t.writes.push_back({w, 1, w.bytes.size(), true, 2});
  t.complete = t.stimulus_delivered = t.transport_established =
      t.peer_setup_received = true;
  t.delivery_event_count = 2;
  return t;
}
void ack(RawProbeTranscript &t, unsigned d, bool fin = true) {
  t.events.push_back(transport::StreamDataEvent{
      1, b({0x18, 0, 4, 0, 7, d == 18 ? 10u : 9u, 0}), fin});
}
Bytes object(unsigned) { return b({5, 1, 0x1c, 7, 9, 99, 1, 42}); }
TEST(FetchFirstObject,
     AllFourRowsRequireTypedNonzeroActualFirstObjectAndAckInEitherOrder) {
  auto ps = all();
  ASSERT_EQ(ps.size(), 4u);
  EXPECT_EQ(ps[0].requirement_id, "D18-11-4-4-1-MUST-001");
  EXPECT_EQ(ps[1].requirement_id, "D18-11-4-4-1-MUST-002");
  EXPECT_EQ(ps[2].requirement_id, "D21-11-4-1-1-MUST-551");
  EXPECT_EQ(ps[3].requirement_id, "D21-11-4-1-1-MUST-552");
  for (const auto &p : ps)
    for (bool ack_first : {false, true}) {
      auto t = stimulus(p);
      if (ack_first)
        ack(t, p.draft);
      for (auto byte : object(p.draft))
        t.events.push_back(transport::StreamDataEvent{10, Bytes{byte}, false});
      if (!ack_first)
        ack(t, p.draft);
      EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), true);
      t.complete = false;
      EXPECT_TRUE(p.definition.response_ready(t));
      EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), std::nullopt);
    }
}
TEST(FetchFirstObject, MissingFieldsFailOnlyOwnRowFromOrdinaryFlagPrefix) {
  for (const auto &p : all())
    for (unsigned flags : {0x10u, 0x14u, 0x18u}) {
      auto t = stimulus(p);
      ack(t, p.draft);
      t.events.push_back(
          transport::StreamDataEvent{6, b({5, 1, flags}), false});
      EXPECT_TRUE(p.definition.response_ready(t));
      const auto bit = p.field == FetchFirstObjectField::Group ? 8u : 4u;
      EXPECT_EQ(evaluate_fetch_first_object_probe(t, p),
                flags & bit ? std::optional<bool>{}
                            : std::optional<bool>{false});
    }
}
TEST(FetchFirstObject, NoProofFromRangeEmptyIncompleteWrongIdOrWrongPoint) {
  for (const auto &p : all())
    for (const auto &data :
         {b({5, 1}), b({5, 3, 0x1c, 7, 9, 99, 1, 42}),
          b({5, 1, 0x1c, 7, 9, 99, 1}), b({5, 1, 0x1c, 6, 9, 99, 1, 42}),
          b({5, 1, 0x1c, 7, 8, 99, 1, 42}),
          b({5, 1, 0x80, 0x8c, 7, 9, 0x1c, 7, 9, 99, 1, 42}),
          b({5, 1, 0x81, 0x0c, 7, 9}), b({5, 1, 0x82, 0x0c, 7, 9}),
          b({5, 1, 0x80, 0x80})}) {
      auto t = stimulus(p);
      ack(t, p.draft);
      t.events.push_back(transport::StreamDataEvent{6, data, true});
      EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), std::nullopt);
    }
}
TEST(FetchFirstObject, AssociationAndTerminalCutoffs) {
  for (const auto &p : all())
    for (unsigned mode = 0; mode < 8; ++mode) {
      auto t = stimulus(p);
      ack(t, p.draft);
      if (mode == 0)
        t.events.push_back(transport::PeerResetEvent{6, 1});
      if (mode == 1)
        t.events.push_back(transport::StreamDataEvent{6, {}, true});
      if (mode == 2)
        t.events.push_back(transport::PeerCloseEvent{
            transport::CloseErrorSpace::Application, 0, {}});
      if (mode == 3) {
        t.events.insert(t.events.begin() + 2,
                        transport::StreamDataEvent{6, b({5}), false});
        t.delivery_event_count = t.writes[0].delivery_event_count = 3;
      }
      t.events.push_back(transport::StreamDataEvent{mode == 4 ? 2u : 6u,
                                                    object(p.draft), true});
      if (mode == 5)
        t.events.push_back(
            transport::StreamDataEvent{10, object(p.draft), true});
      if (mode == 6) {
        t.events.push_back(transport::PeerCloseEvent{
            transport::CloseErrorSpace::Application, 0, {}});
        t.events.push_back(
            transport::StreamDataEvent{10, object(p.draft), true});
      }
      if (mode == 7)
        t.events.push_back(transport::PeerResetEvent{6, 1});
      EXPECT_EQ(evaluate_fetch_first_object_probe(t, p),
                mode >= 6 ? std::optional<bool>{true} : std::optional<bool>{});
    }
}
TEST(FetchFirstObject, AckMustBeTypedScopedCompleteAndAfterDelivery) {
  for (const auto &p : all())
    for (unsigned mode = 0; mode < 6; ++mode) {
      auto t = stimulus(p);
      t.events.push_back(transport::StreamDataEvent{6, object(p.draft), true});
      if (mode == 0)
        ack(t, p.draft);
      if (mode == 1)
        t.events.push_back(
            transport::StreamDataEvent{1, b({0x18, 0, 4, 0, 7, 9}), true});
      if (mode == 2)
        t.events.push_back(
            transport::StreamDataEvent{1, b({0x18, 0, 4, 2, 7, 9, 0}), true});
      if (mode == 3)
        t.events.push_back(
            transport::StreamDataEvent{5, b({0x18, 0, 4, 0, 7, 9, 0}), true});
      if (mode == 4) {
        t.events.insert(
            t.events.begin() + 2,
            transport::StreamDataEvent{
                1, b({0x18, 0, 4, 0, 7, p.draft == 18 ? 10u : 9u, 0}), true});
        t.delivery_event_count = t.writes[0].delivery_event_count = 3;
      }
      EXPECT_EQ(evaluate_fetch_first_object_probe(t, p),
                mode == 0 ? std::optional<bool>{true} : std::optional<bool>{});
    }
}
TEST(FetchFirstObject,
     FetchOkRejectsWrongEndPointAndParametersOutsideFetchScope) {
  for (const auto &p : all()) {
    const auto end = p.draft == 18 ? 10u : 9u;
    for (const auto &reply :
         {b({0x18, 0, 4, 0, 6, end, 0}), b({0x18, 0, 6, 0, 7, end, 1, 8, 0})}) {
      auto t = stimulus(p);
      t.events.push_back(transport::StreamDataEvent{1, reply, true});
      t.events.push_back(transport::StreamDataEvent{6, object(p.draft), true});
      EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), std::nullopt);
    }
  }
}
TEST(FetchFirstObject, DistinctCanonicalPointFixturesAndTrackConfiguration) {
  EXPECT_EQ(draft18_fetch_first_object_probes()[0].definition.writes[0].bytes,
            b({0x16, 0, 10, 1, 1, 0, 1, 'x', 7, 9, 7, 10, 0}));
  EXPECT_EQ(draft21_fetch_first_object_probes()[0].definition.writes[0].bytes,
            b({0x16, 0, 11, 1, 0, 1, 'x', 1, 0x21, 4, 7, 9, 0, 9}));
  for (unsigned d : {18u, 21u}) {
    auto ps = d == 18
                  ? draft18_fetch_first_object_probes(
                        std::chrono::milliseconds{10}, {b({'n'})}, b({'t'}))
                  : draft21_fetch_first_object_probes(
                        std::chrono::milliseconds{10}, {b({'n'})}, b({'t'}));
    for (const auto &p : ps) {
      auto t = stimulus(p);
      ack(t, d);
      t.events.push_back(transport::StreamDataEvent{10, object(d), true});
      EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), true);
    }
  }
  EXPECT_TRUE(fetch_first_object_fixture_valid({}, {}));
  EXPECT_FALSE(fetch_first_object_fixture_valid({b({'.'})}, b({'x'})));
  EXPECT_FALSE(fetch_first_object_fixture_valid(
      {b({'.', 's', 'e', 's', 's', 'i', 'o', 'n'})}, {}));
  EXPECT_FALSE(
      fetch_first_object_fixture_valid(std::vector<Bytes>(33, b({'n'})), {}));
  EXPECT_FALSE(fetch_first_object_fixture_valid({}, Bytes(4097)));
  EXPECT_THROW(draft18_fetch_first_object_probes(std::chrono::milliseconds{0}),
               std::invalid_argument);
  EXPECT_THROW(draft21_fetch_first_object_probes(std::chrono::milliseconds{10},
                                                 {b({'.'})}),
               std::invalid_argument);
}
TEST(FetchFirstObject, CanonicalStimulusAndDeliveryMetadataCannotBeClaimed) {
  for (const auto &p : all())
    for (unsigned mode = 0; mode < 13; ++mode) {
      auto t = stimulus(p);
      ack(t, p.draft);
      t.events.push_back(transport::StreamDataEvent{6, object(p.draft), true});
      if (mode == 0)
        t.writes[0].write.bytes[3] = std::byte{3};
      if (mode == 1)
        t.writes[0].write.bytes.back() = std::byte{1};
      if (mode == 2)
        t.writes[0].write.bytes[t.writes[0].write.bytes.size() - 2] =
            std::byte{6};
      if (mode == 3)
        t.writes[0].write.fin = false;
      if (mode == 4)
        t.writes[0].fin_accepted = false;
      if (mode == 5)
        t.writes[0].accepted--;
      if (mode == 6)
        t.writes[0].delivery_event_count = 3;
      if (mode == 7)
        t.scenario_id = "claimed-only";
      if (mode == 8)
        t.harness_failed = true;
      if (mode == 9)
        t.timed_out = true;
      if (mode == 10)
        t.events.resize(4097);
      if (mode == 11)
        t.events.push_back(transport::StreamDataEvent{10, Bytes(65547), false});
      if (mode == 12)
        for (unsigned i = 0; i < 65; ++i)
          t.events.push_back(transport::PeerResetEvent{14 + i * 4, 1});
      EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), std::nullopt);
    }
}
TEST(FetchFirstObject, AckFragmentationUnrelatedTrafficAndEarlyAckAreUnscored) {
  for (const auto &p : all()) {
    auto t = stimulus(p);
    t.events.push_back(transport::StreamDataEvent{6, object(p.draft), false});
    t.events.push_back(
        transport::StreamDataEvent{14, b({5, 3, 0x1c, 7, 9, 99, 1, 42}), true});
    const auto reply = b({0x18, 0, 4, 0, 7, p.draft == 18 ? 10u : 9u, 0});
    for (auto byte : reply)
      t.events.push_back(transport::StreamDataEvent{1, Bytes{byte}, false});
    EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), true);
    t = stimulus(p);
    t.events.push_back(transport::StreamDataEvent{1, reply, true});
    t.events.push_back(transport::PeerCloseEvent{
        transport::CloseErrorSpace::Application, 0, {}});
    t.events.push_back(transport::StreamDataEvent{6, object(p.draft), true});
    EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), std::nullopt);
    t = stimulus(p);
    t.events.push_back(transport::StreamDataEvent{6, b({5}), false});
    t.events.push_back(transport::PeerResetEvent{6, 1});
    ack(t, p.draft);
    t.events.push_back(
        transport::StreamDataEvent{6, b({1, 0x1c, 7, 9, 99, 1, 42}), true});
    EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), std::nullopt);
  }
}
TEST(FetchFirstObject, TypedFirstZeroPayloadIsCompleteWithoutInventedStatus) {
  for (const auto &p : all()) {
    auto t = stimulus(p);
    ack(t, p.draft);
    t.events.push_back(
        transport::StreamDataEvent{6, b({5, 1, 0x1c, 7, 9, 99, 0}), false});
    EXPECT_EQ(evaluate_fetch_first_object_probe(t, p),
              std::optional<bool>{true});
    t.events.push_back(transport::StreamDataEvent{6, b({0}), false});
    EXPECT_EQ(evaluate_fetch_first_object_probe(t, p), true);
  }
}
TEST(FetchFirstObjectCatalog,
     RealConfiguredContextsScoreBothRowsThroughDefaultDispatch) {
  using namespace requirements;
  const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
  struct Fixture {
    std::vector<Bytes> track_namespace;
    Bytes track_name;
  };
  for (const auto draft : {18u, 21u}) {
    const auto source = load_draft_source(
        draft, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source,
        root / "requirements" / ("draft" + std::to_string(draft) + ".json"));
    const auto bindings = draft == 18 ? draft18_executable_bindings()
                                      : draft21_executable_bindings();
    for (const auto &fixture : {Fixture{{b({'n'})}, b({'t'})},
                                Fixture{{b({'a'}), b({'b'})}, b({'v'})}}) {
      const auto profiles =
          draft == 18 ? draft18_fetch_first_object_probes(
                            std::chrono::milliseconds{1000},
                            fixture.track_namespace, fixture.track_name)
                      : draft21_fetch_first_object_probes(
                            std::chrono::milliseconds{1000},
                            fixture.track_namespace, fixture.track_name);
      ASSERT_EQ(profiles.size(), 2u);
      for (const auto &profile : profiles) {
        EXPECT_TRUE(std::ranges::any_of(bindings, [&](const auto &binding) {
          return binding.requirement_id == profile.requirement_id &&
                 binding.scenario_id == profile.definition.id &&
                 binding.evaluator_id == profile.evaluator_id;
        })) << profile.requirement_id;
      }
      const auto evaluate = [&](std::vector<RawProbeTranscript> transcripts) {
        if (draft == 21)
          return evaluate_draft21_raw_probes(catalog, transcripts);
        std::vector<ScenarioContext> contexts;
        for (auto &transcript : transcripts) {
          ScenarioContext context;
          context.scenario_id = transcript.scenario_id;
          context.complete = transcript.complete;
          context.stimulus_delivered = transcript.stimulus_delivered;
          context.raw_probe = std::move(transcript);
          contexts.push_back(std::move(context));
        }
        return evaluate_draft18(catalog, contexts);
      };
      const auto states = [&](const auto &outcomes) {
        std::vector<OutcomeState> result;
        for (const auto &profile : profiles) {
          const auto found =
              std::ranges::find_if(outcomes, [&](const auto &outcome) {
                return outcome.requirement_id == profile.requirement_id;
              });
          EXPECT_NE(found, outcomes.end()) << profile.requirement_id;
          result.push_back(found == outcomes.end() ? OutcomeState::NotRun
                                                   : found->state);
        }
        return result;
      };
      auto valid = stimulus(profiles.front());
      ack(valid, draft);
      valid.events.push_back(
          transport::StreamDataEvent{10, object(draft), true});
      const std::vector<OutcomeState> pass{OutcomeState::Pass,
                                           OutcomeState::Pass};
      const std::vector<OutcomeState> not_run{OutcomeState::NotRun,
                                              OutcomeState::NotRun};
      EXPECT_EQ(states(evaluate({valid})), pass);
      EXPECT_EQ(states(evaluate({})), not_run);
      EXPECT_EQ(states(evaluate({valid, valid})), not_run);
      for (const auto flags : {0x14u, 0x18u}) {
        auto missing = stimulus(profiles.front());
        ack(missing, draft);
        missing.events.push_back(
            transport::StreamDataEvent{10, b({5, 1, flags}), false});
        const std::vector<OutcomeState> expected =
            flags == 0x14u ? std::vector<OutcomeState>{OutcomeState::Fail,
                                                       OutcomeState::NotRun}
                           : std::vector<OutcomeState>{OutcomeState::NotRun,
                                                       OutcomeState::Fail};
        EXPECT_EQ(states(evaluate({missing})), expected);
      }
      auto missing_ack = stimulus(profiles.front());
      missing_ack.events.push_back(
          transport::StreamDataEvent{10, object(draft), true});
      EXPECT_EQ(states(evaluate({missing_ack})), not_run);
      missing_ack.events.push_back(transport::StreamDataEvent{
          1, b({0x18, 0, 4, 0, 7, draft == 18 ? 10u : 9u}), true});
      EXPECT_EQ(states(evaluate({missing_ack})), not_run);
      auto altered = valid;
      altered.writes.front().write.bytes[3] = std::byte{3};
      EXPECT_EQ(states(evaluate({altered})), not_run);
      altered = valid;
      altered.writes.front().write.bytes.back() = std::byte{1};
      EXPECT_EQ(states(evaluate({altered})), not_run);
    }
  }
}
} // namespace
} // namespace moq::interop::scenarios
