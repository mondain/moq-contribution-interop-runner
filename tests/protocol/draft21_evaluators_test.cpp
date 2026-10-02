#include "moq/interop/requirements/draft21_evaluators.h"
#include "moq/interop/scenarios/draft21_close.h"
#include "moq/interop/scenarios/draft21_peer_close.h"
#include "moq/interop/scenarios/draft21_request.h"
#include "moq/interop/scenarios/draft21_response.h"
#include "../support/raw_probe_transcript.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <stdexcept>
#include <tuple>

namespace moq::interop::requirements {
namespace {

const Outcome& outcome_for(const std::vector<Outcome>& outcomes,
                           const std::string& id) {
    const auto found = std::find_if(outcomes.begin(), outcomes.end(),
                                    [&](const Outcome& outcome) {
                                        return outcome.requirement_id == id;
                                    });
    if (found == outcomes.end()) throw std::logic_error("missing outcome");
    return *found;
}

scenarios::RawProbeTranscript close_probe_context(const scenarios::Draft21CloseProbe& probe,
    std::span<const std::byte> peer_request = {}) {
    const auto& id = probe.definition.id;
    if (id == "d21-publish-established-subscriber-sends-publish-state-notify") {
        const auto default_publish = test::probe_bytes({0x1d,0,8,0,0,1,'x',0,0,4,1});
        auto result = test::raw_probe_transcript(probe.definition,
            peer_request.empty() ? std::span<const std::byte>(default_publish) : peer_request);
        result.setup.delivery_event_count = 1;
        for (auto& write : result.writes) {
            write.stream_id = 0;
            write.delivery_event_count = result.events.size();
        }
        return result;
    }
    auto result = test::raw_probe_transcript(probe.definition);
    const bool discovery = id == "d21-discovery-update-invalid-forward";
    const bool duplicate = id == "d21-duplicate-request-update-id";
    const bool namespace_notify = id == "d21-publish-state-notify-on-namespace-request";
    const bool fetch_notify = id == "d21-publish-state-notify-on-fetch";
    if (discovery || duplicate || namespace_notify || fetch_notify ||
        id == "d21-subscriber-sends-publish-state-notify" ||
        id == "d21-group-order-in-subscription-update") {
        result.writes.front().delivery_event_count = result.events.size();
        // Section9.7: alias0, no parameters, MAX_CACHE_DURATION1. Section9.3:
        // the discovery and update responses have no parameters/properties.
        result.events.push_back(transport::StreamDataEvent{1, fetch_notify
            ? test::probe_bytes({0x18, 0, 4, 0, 0, 0, 0})
            : discovery || namespace_notify ? test::probe_bytes({7, 0, 1, 0})
                                            : test::probe_bytes({4, 0, 4, 0, 0, 4, 1}), false});
        result.writes.at(1).stream_id = 1;
        result.writes.at(1).delivery_event_count = result.events.size();
        if (duplicate) {
            result.events.push_back(transport::StreamDataEvent{1, test::probe_bytes({7, 0, 1, 0}), false});
            result.writes.at(2).stream_id = 1;
            result.writes.at(2).delivery_event_count = result.events.size();
        }
        result.delivery_event_count = result.events.size();
    }
    return result;
}

RequirementCatalog small_catalog() {
    return {21, "fixture-digest", true,
            {{"D21-6-3-MUST-NOT-141", Strength::MustNot,
              {"6.3", 2111, 2114, 1, 1}, "publisher",
              "Only permitted messages start a request stream.",
              Applicability::Applicable, Testability::Testable,
              {"d21-publisher-request-stream-placement"},
              {"d21-request-stream-first-message-allowed"}, ""},
             {"D21-9-MUST-282", Strength::Must,
              {"9", 3368, 3369, 1, 1}, "publisher",
              "PUBLISH is first on its request stream.",
              Applicability::Applicable, Testability::Testable,
              {"d21-publisher-request-stream-placement"},
              {"d21-publisher-first-message-placement"}, ""},
             {"D21-OTHER-SHOULD-001", Strength::Should,
              {"9", 3370, 3371, 1, 1}, "publisher", "Other obligation",
              Applicability::Applicable, Testability::Testable,
              {"another-scenario"}, {"another-evaluator"}, ""}}};
}

scenarios::Draft21AnnouncementContext passed_context() {
    return {true, true, true,
            {{scenarios::Draft21AnnouncementEventKind::TransportEstablished,
              std::nullopt, std::nullopt},
             {scenarios::Draft21AnnouncementEventKind::PublishObserved, 0, 0},
             {scenarios::Draft21AnnouncementEventKind::ResponseDelivered, 0, 0}}};
}

TEST(Draft21Evaluators, PassesOnlyObservedPublisherOpening) {
    const auto outcomes = evaluate_draft21_announcement(
        small_catalog(), passed_context());
    EXPECT_EQ(outcome_for(outcomes, "D21-9-MUST-282").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(outcomes, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(outcomes, "D21-OTHER-SHOULD-001").state,
              OutcomeState::NotRun);
}

TEST(Draft21Evaluators, IncompleteExchangeStaysNotRun) {
    auto context = passed_context();
    context.complete = false;
    context.response_delivered = false;
    const auto outcomes = evaluate_draft21_announcement(
        small_catalog(), context);
    EXPECT_EQ(outcome_for(outcomes, "D21-9-MUST-282").state,
              OutcomeState::NotRun);
    EXPECT_EQ(outcome_for(outcomes, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::NotRun);
}

TEST(Draft21Evaluators, InvalidFirstMessageFailsMustNotWithoutCompletedPublish) {
    auto context = passed_context();
    context.complete = false;
    context.target_publish_seen = false;
    context.response_delivered = false;
    context.evidence = {
        {scenarios::Draft21AnnouncementEventKind::InvalidRequestOpener,
         0, std::nullopt}};
    const auto outcomes = evaluate_draft21_announcement(
        small_catalog(), context);
    EXPECT_EQ(outcome_for(outcomes, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::Fail);
    EXPECT_EQ(outcome_for(outcomes, "D21-9-MUST-282").state,
              OutcomeState::NotRun);
    EXPECT_EQ(score(small_catalog(), outcomes).verdict, RunVerdict::Fail);
}

TEST(Draft21Evaluators, CheckedInCatalogRetainsAllOtherRows) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    const auto outcomes = evaluate_draft21_announcement(
        catalog, passed_context());
    ASSERT_EQ(outcomes.size(), catalog.requirements.size());
    EXPECT_EQ(outcome_for(outcomes, "D21-9-MUST-282").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(outcomes, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::Pass);
    EXPECT_EQ(score(catalog, outcomes).verdict, RunVerdict::Incomplete);

    auto violation = passed_context();
    violation.complete = false;
    violation.target_publish_seen = false;
    violation.response_delivered = false;
    violation.evidence = {
        {scenarios::Draft21AnnouncementEventKind::InvalidRequestOpener,
         0, std::nullopt}};
    const auto failed = evaluate_draft21_announcement(catalog, violation);
    EXPECT_EQ(outcome_for(failed, "D21-6-3-MUST-NOT-141").state,
              OutcomeState::Fail);
    EXPECT_EQ(score(catalog, failed).verdict, RunVerdict::Fail);
}

TEST(Draft21Evaluators, SetupProbeNeedsSetupBeforePublisherAction) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    auto context = passed_context();
    context.setup_probe = scenarios::Draft21SetupProbe::UnknownOption;
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-MUST-287").state,
              OutcomeState::NotRun);

    context.evidence.insert(
        context.evidence.begin() + 1,
        {scenarios::Draft21AnnouncementEventKind::LocalSetupSent, 3,
         std::nullopt});
    const auto one = evaluate_draft21_announcement(catalog, context);
    EXPECT_EQ(outcome_for(one, "D21-9-1-MUST-287").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(one, "D21-9-1-MUST-288").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(one, "D21-9-1-MUST-290").state,
              OutcomeState::NotRun);
    EXPECT_EQ(outcome_for(one, "D21-9-MUST-282").state,
              OutcomeState::NotRun);

    context.setup_probe = scenarios::Draft21SetupProbe::DuplicateUnknownOption;
    const auto duplicate = evaluate_draft21_announcement(catalog, context);
    EXPECT_EQ(outcome_for(duplicate, "D21-9-1-MUST-287").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(duplicate, "D21-9-1-MUST-288").state,
              OutcomeState::Pass);
    EXPECT_EQ(outcome_for(duplicate, "D21-9-1-MUST-290").state,
              OutcomeState::Pass);

    std::swap(context.evidence[1], context.evidence[2]);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-MUST-290").state,
              OutcomeState::NotRun);
}

TEST(Draft21Evaluators, ForbiddenServerUriOptionsRequireExactPeerClose) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    for (const auto& probe : {
             std::pair{scenarios::Draft21SetupProbe::ServerAuthority,
                       "D21-9-1-1-MUST-293"},
             std::pair{scenarios::Draft21SetupProbe::ServerPath,
                       "D21-9-1-2-MUST-300"}}) {
        auto context = passed_context();
        context.complete = false;
        context.target_publish_seen = false;
        context.response_delivered = false;
        context.setup_probe = probe.first;
        context.evidence = {
            {scenarios::Draft21AnnouncementEventKind::LocalSetupSent, 3,
             std::nullopt},
            {scenarios::Draft21AnnouncementEventKind::PeerClosed,
             std::nullopt, std::nullopt}};
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::NotRun);
        const auto expected = probe.first ==
                                      scenarios::Draft21SetupProbe::ServerAuthority
                                  ? 0x19u : 0x8u;
        context.evidence[1].application_close_code = expected;
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::Pass);
        context.evidence[1].application_close_code = 3;
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::Fail);
        context.evidence.clear();
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::NotRun);

        context = passed_context();
        context.setup_probe = probe.first;
        context.evidence.insert(
            context.evidence.begin() + 1,
            {scenarios::Draft21AnnouncementEventKind::LocalSetupSent, 3,
             std::nullopt});
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              probe.second).state, OutcomeState::Fail);
    }
}

TEST(Draft21Evaluators, WebTransportServerUriOptionRowsRequireExactClose) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    for (const auto& [probe, webtransport_row, native_row, expected] : {
             std::tuple{scenarios::Draft21SetupProbe::ServerAuthority,
                        "D21-9-1-1-MUST-294", "D21-9-1-1-MUST-293", 0x19u},
             std::tuple{scenarios::Draft21SetupProbe::ServerPath,
                        "D21-9-1-2-MUST-301", "D21-9-1-2-MUST-300", 0x8u}}) {
        auto context = passed_context();
        context.complete = false;
        context.target_publish_seen = false;
        context.response_delivered = false;
        context.setup_probe = probe;
        context.evidence = {
            {scenarios::Draft21AnnouncementEventKind::LocalSetupSent, 3,
             std::nullopt},
            {scenarios::Draft21AnnouncementEventKind::PeerClosed,
             std::nullopt, std::nullopt, expected}};
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              webtransport_row).state, OutcomeState::NotRun);
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              native_row).state, OutcomeState::Pass);

        context.webtransport = true;
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              webtransport_row).state, OutcomeState::Pass);
        context.evidence[1].application_close_code = 3;
        EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                              webtransport_row).state, OutcomeState::Fail);
    }
}

TEST(Draft21Evaluators, PublisherWebTransportSetupOmitsAuthorityAndPath) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    auto context = passed_context();
    context.webtransport = true;
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-1-MUST-NOT-292").state, OutcomeState::NotRun);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::NotRun);

    context.evidence.insert(context.evidence.begin() + 1,
        {scenarios::Draft21AnnouncementEventKind::PeerSetupReceived, 2,
         std::nullopt});
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-1-MUST-NOT-292").state, OutcomeState::Pass);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::Pass);

    context.complete = false;
    context.peer_setup_option_types = {5};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-1-MUST-NOT-292").state, OutcomeState::Fail);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::NotRun);
    context.peer_setup_option_types = {1};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-1-MUST-NOT-292").state, OutcomeState::NotRun);
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::Fail);
    context.webtransport = false;
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          "D21-9-1-2-MUST-NOT-299").state, OutcomeState::NotRun);
}

TEST(Draft21Evaluators, PublisherSetupRepeatsOnlyPermittedOptionTypes) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    constexpr const char* row = "D21-9-1-MUST-NOT-289";
    auto context = passed_context();
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          row).state, OutcomeState::NotRun);
    context.evidence.insert(context.evidence.begin() + 1,
        {scenarios::Draft21AnnouncementEventKind::PeerSetupReceived, 2,
         std::nullopt});
    context.peer_setup_option_types = {3, 3};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          row).state, OutcomeState::Pass);
    context.peer_setup_option_types = {3, 3, 0x9d, 0x9d};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          row).state, OutcomeState::NotRun);
    context.complete = false;
    context.peer_setup_option_types = {4, 4};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          row).state, OutcomeState::Fail);
    context.peer_setup_option_types = {4, 4, 0x9d, 0x9d};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          row).state, OutcomeState::Fail);
    context.peer_setup_option_types = {1, 1};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          row).state, OutcomeState::Fail);
    context.peer_setup_option_types = {3, 3};
    EXPECT_EQ(outcome_for(evaluate_draft21_announcement(catalog, context),
                          row).state, OutcomeState::NotRun);
}

TEST(Draft21SubscriberNotifyDirection, ActualCatalogNeedsBothOriginsAndConfiguredTargets) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(21,root / "docs",root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source,root / "requirements/draft21.json");
    for (const bool configured : {false,true}) {
        std::vector<scenarios::RawProbeTranscript> contexts;
        const auto profiles = scenarios::draft21_close_probes(std::chrono::milliseconds(1000),
            configured ? std::vector<std::vector<std::byte>>{test::probe_bytes({'n'})}
                       : std::vector<std::vector<std::byte>>{},
            test::probe_bytes({configured ? static_cast<unsigned>('t') : static_cast<unsigned>('x')}));
        for (const auto& profile : profiles) {
            if (profile.requirement_id != "D21-9-10-MUST-370") continue;
            const auto publish = configured ? test::probe_bytes({0x1d,0,10,0,1,1,'n',1,'t',0,0,4,1})
                                            : test::probe_bytes({0x1d,0,8,0,0,1,'x',0,0,4,1});
            auto transcript = close_probe_context(profile,publish);
            transcript.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
            contexts.push_back(std::move(transcript));
        }
        ASSERT_EQ(contexts.size(),2u);
        const auto state = [&](const auto& transcripts) {
            return outcome_for(evaluate_draft21_raw_probes(catalog,transcripts),"D21-9-10-MUST-370").state;
        };
        EXPECT_EQ(state(contexts),OutcomeState::Pass);
        for (std::size_t index=0;index<contexts.size();++index) {
            auto missing=contexts; missing.erase(missing.begin()+static_cast<std::ptrdiff_t>(index));
            EXPECT_EQ(state(missing),OutcomeState::NotRun);
            auto duplicate=contexts; duplicate.push_back(contexts[index]);
            EXPECT_EQ(state(duplicate),OutcomeState::NotRun);
            auto failed=contexts;
            std::get<transport::PeerCloseEvent>(failed[index].events.back()).error_code=9;
            EXPECT_EQ(state(failed),OutcomeState::Fail);
            EXPECT_EQ(state(std::vector{failed[index]}),OutcomeState::Fail);
            auto partial=contexts; --partial[index].writes.back().accepted;
            EXPECT_EQ(state(partial),OutcomeState::NotRun);
            auto absent_marker=contexts; absent_marker[index].writes.back().delivery_event_count.reset();
            EXPECT_EQ(state(absent_marker),OutcomeState::NotRun);
        }
    }
}

TEST(Draft21CloseEvaluators, EveryProbeRequiresAcceptedStimulusAndExactPeerApplicationClose) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(
        21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(
        source, root / "requirements/draft21.json");
    for (const auto& probe : scenarios::draft21_close_probes()) {
        SCOPED_TRACE(probe.definition.id);
        auto transcript = close_probe_context(probe);
        const auto actual = probe.expected_close.value_or(3);
        transcript.events.push_back(transport::PeerCloseEvent{
            transport::CloseErrorSpace::Application, actual, {}});
        const auto row = std::find_if(catalog.requirements.begin(), catalog.requirements.end(),
            [&](const auto& candidate) { return candidate.id == probe.requirement_id; });
        ASSERT_NE(row, catalog.requirements.end());
        EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, transcript),
                              probe.requirement_id).state,
                  row->scenarios.size() == 1 ? OutcomeState::Pass : OutcomeState::NotRun);
        auto wrong_direction = transcript;
        wrong_direction.setup.stream_id = 2;
        EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, wrong_direction),
                              probe.requirement_id).state, OutcomeState::NotRun);
        if (!transcript.writes.empty() &&
            transcript.writes.front().write.channel != scenarios::RawProbeChannel::Datagram) {
            wrong_direction = transcript;
            wrong_direction.writes.front().stream_id =
                transcript.writes.front().write.channel == scenarios::RawProbeChannel::PeerBidi ? 1 : 0;
            EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, wrong_direction),
                                  probe.requirement_id).state, OutcomeState::NotRun);
        }
        if (!transcript.writes.empty() &&
            transcript.writes.front().write.channel == scenarios::RawProbeChannel::Datagram) {
            auto incomplete = transcript;
            --incomplete.writes.front().accepted;
            EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, incomplete),
                                  probe.requirement_id).state, OutcomeState::NotRun);
            incomplete = transcript;
            incomplete.writes.front().stream_id = 1;
            EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, incomplete),
                                  probe.requirement_id).state, OutcomeState::NotRun);
            for (const auto capacity : {std::size_t{0},
                                       transcript.writes.front().write.bytes.size() - 1}) {
                incomplete = transcript;
                incomplete.max_datagram_payload = capacity;
                std::get<transport::ConnectionEstablishedEvent>(incomplete.events.front())
                    .max_datagram_payload = capacity;
                EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, incomplete),
                                      probe.requirement_id).state, OutcomeState::NotRun);
            }
            incomplete = transcript;
            incomplete.max_datagram_payload = 0;
            EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, incomplete),
                                  probe.requirement_id).state, OutcomeState::NotRun);
        }
        if (probe.expected_close) {
            transcript.events.back() = transport::PeerCloseEvent{
                transport::CloseErrorSpace::Application, 99, {}};
            EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, transcript),
                                  probe.requirement_id).state, OutcomeState::Fail);
        }
        for (const auto& terminal : std::vector<transport::TransportEvent>{
                 transport::PeerCloseEvent{transport::CloseErrorSpace::Transport,
                                            actual, {}},
                 transport::LocalCloseEvent{transport::CloseErrorSpace::Application,
                                             actual, {}},
                 transport::IdleTimeoutEvent{}}) {
            transcript.events.back() = terminal;
            EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, transcript),
                                  probe.requirement_id).state, OutcomeState::NotRun);
        }
        transcript.events.back() = transport::PeerCloseEvent{
            transport::CloseErrorSpace::Application, actual, {}};
        --transcript.setup.accepted;
        EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, transcript),
                              probe.requirement_id).state, OutcomeState::NotRun);
        ++transcript.setup.accepted;
        transcript.harness_failed = true;
        EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, transcript),
                              probe.requirement_id).state, OutcomeState::NotRun);
        transcript.harness_failed = false;
        transcript.stimulus_delivered = false;
        EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, transcript),
                              probe.requirement_id).state, OutcomeState::NotRun);
    }
}

}  // namespace
TEST(Draft21RequestEvaluators, CatalogRowsRequireMatchingResponseAfterDelivery) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    const auto checked = RequirementCatalog::load(source, root / "requirements/draft21.json");
    for (const auto& profile : scenarios::draft21_request_profiles()) {
        SCOPED_TRACE(profile.definition.id);
        auto transcript = test::raw_probe_transcript(profile.definition);
        if (profile.compatibility_error)
            transcript.unknown_auth_token_alias_compatibility_code = profile.expected_error;
        transcript.events.push_back(transport::StreamDataEvent{
            1, test::probe_bytes({5, 0, 3, static_cast<unsigned>(profile.expected_error), 0, 0}), true});
        const auto evaluate = [&] {
            return outcome_for(evaluate_draft21_request_probe(checked, transcript), profile.requirement_id).state;
        };
        const auto row = std::find_if(checked.requirements.begin(), checked.requirements.end(),
            [&](const auto& candidate) { return candidate.id == profile.requirement_id; });
        ASSERT_NE(row, checked.requirements.end());
        EXPECT_EQ(evaluate(), row->scenarios.size() == 1 ? OutcomeState::Pass : OutcomeState::NotRun);
        const auto original = transcript;
        std::get<transport::StreamDataEvent>(transcript.events.back()).data[3] =
            static_cast<std::byte>(profile.expected_error + 1);
        EXPECT_EQ(evaluate(), OutcomeState::Fail);
        transcript = original;
        --transcript.writes.front().accepted;
        EXPECT_EQ(evaluate(), OutcomeState::NotRun);
        transcript = original;
        std::get<transport::StreamDataEvent>(transcript.events.back()).stream_id = 5;
        EXPECT_EQ(evaluate(), OutcomeState::NotRun);
        transcript = original;
        transcript.delivery_event_count = transcript.events.size();
        EXPECT_EQ(evaluate(), OutcomeState::NotRun);
    }
}

TEST(Draft21RawFamilies, SingleSuccessfulContextCannotPassAMultipleScenarioRow) {
    // Draft 21 sections 9.20.16 and 8.6 have independent nested fill and
    // range-overflow contexts in the checked-in catalog.
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    const auto catalog = RequirementCatalog::load(source, root / "requirements/draft21.json");
    const auto probes = scenarios::draft21_close_probes();
    const auto close = std::find_if(probes.begin(), probes.end(), [](const auto& probe) {
        return probe.definition.id == "d21-fill-forbidden-nested-authorization";
    });
    ASSERT_NE(close, probes.end());
    auto closed = test::raw_probe_transcript(close->definition);
    closed.events.push_back(transport::PeerCloseEvent{
        transport::CloseErrorSpace::Application, 3, {}});
    EXPECT_EQ(outcome_for(evaluate_draft21_close_probe(catalog, closed),
                          "D21-9-20-16-MUST-447").state, OutcomeState::NotRun);
    const auto profiles = scenarios::draft21_request_profiles();
    const auto request = std::find_if(profiles.begin(), profiles.end(), [](const auto& profile) {
        return profile.definition.id == "d21-range-filter-start-delta-overflow";
    });
    ASSERT_NE(request, profiles.end());
    auto rejected = test::raw_probe_transcript(request->definition);
    rejected.events.push_back(transport::StreamDataEvent{
        1, test::probe_bytes({5, 0, 3, 0x36, 0, 0}), true});
    EXPECT_EQ(outcome_for(evaluate_draft21_request_probe(catalog, rejected),
                          "D21-8-6-MUST-249").state, OutcomeState::NotRun);
}

namespace {
RequirementCatalog raw_catalog() {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    return RequirementCatalog::load(source, root / "requirements/draft21.json");
}

std::vector<scenarios::RawProbeTranscript> successful_fill_family() {
    std::vector<scenarios::RawProbeTranscript> transcripts;
    for (const auto& probe : scenarios::draft21_close_probes()) {
        if (probe.requirement_id != "D21-9-20-16-MUST-447") continue;
        auto transcript = test::raw_probe_transcript(probe.definition);
        transcript.events.push_back(transport::PeerCloseEvent{
            transport::CloseErrorSpace::Application, 3, {}});
        transcripts.push_back(std::move(transcript));
    }
    return transcripts;
}

std::vector<scenarios::RawProbeTranscript> successful_filter_family(const char* requirement) {
    std::vector<scenarios::RawProbeTranscript> transcripts;
    for (const auto& profile : scenarios::draft21_request_profiles()) {
        if (profile.requirement_id != requirement) continue;
        auto transcript = test::raw_probe_transcript(profile.definition);
        transcript.events.push_back(transport::StreamDataEvent{
            1, test::probe_bytes({5, 0, 3, 0x36, 0, 0}), true});
        transcripts.push_back(std::move(transcript));
    }
    return transcripts;
}
TEST(Draft21PeerCloseEvaluators, CompletePeerRequestAndResponseAcceptanceGateRealCatalogRows) {
    const auto root = std::filesystem::path(MOQ_INTEROP_PROJECT_SOURCE_DIR);
    const auto source = load_draft_source(21, root / "docs", root / "requirements/draft-digests.json");
    const auto checked = RequirementCatalog::load(source, root / "requirements/draft21.json");
    std::vector<scenarios::RawProbeTranscript> ok_contexts;
    for (const auto& profile : scenarios::draft21_peer_close_probes()) {
        SCOPED_TRACE(profile.definition.id);
        const bool publish = profile.definition.id == "d21-publish-request-error-oversized-reason" ||
                             profile.definition.id == "d21-publish-ok-with-track-properties" ||
                             profile.definition.id == "d21-publish-update-ok-with-track-properties";
        const auto request = publish ? test::probe_bytes({0x1d, 0, 10, 0, 1, 1, 'n', 1, 'x', 0, 0, 4, 1}) :
                                      test::probe_bytes({6, 0, 5, 0, 1, 1, 'n', 0});
        auto transcript = test::raw_probe_transcript(profile.definition, request);
        if (profile.definition.id == "d21-publish-update-ok-with-track-properties") {
            transcript.writes[0].delivery_event_count = 3;
            transcript.events.push_back(transport::StreamDataEvent{0, test::probe_bytes({2, 0, 2, 2, 0}), false});
            transcript.writes[1].delivery_event_count = 4;
            transcript.delivery_event_count = 4;
        } else if (profile.definition.writes.size() == 2) {
            for (auto& write : transcript.writes) write.delivery_event_count = 3;
        }
        transcript.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
        ok_contexts.push_back(transcript);
        const auto evaluate = [&] {
            return outcome_for(evaluate_draft21_close_probe(checked, transcript), profile.requirement_id).state;
        };
        EXPECT_EQ(evaluate(), profile.requirement_id == "D21-9-3-MUST-337" ||
                              profile.requirement_id == "D21-9-5-MUST-344"
                              ? OutcomeState::NotRun : OutcomeState::Pass);
        transcript.writes.front().accepted--;
        EXPECT_EQ(evaluate(), OutcomeState::NotRun);
        transcript = ok_contexts.back();
        std::get<transport::PeerCloseEvent>(transcript.events.back()).error_code = 4;
        EXPECT_EQ(evaluate(), OutcomeState::Fail);
        transcript = ok_contexts.back();
        transcript.delivery_event_count = 2;
        EXPECT_EQ(evaluate(), OutcomeState::NotRun);
    }
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(checked, ok_contexts), "D21-9-3-MUST-337").state,
              OutcomeState::Pass);
}

}  // namespace

TEST(Draft21RawFamilies, EveryCloseContextMustHaveOneCompleteSuccess) {
    // Draft 21 section 9.20.16 Table 6: AUTHORIZATION_TOKEN,
    // TRACK_PROPERTY_FILTER and recursive FILL_PARAMETERS are separate stimuli.
    const auto catalog = raw_catalog();
    const auto complete = successful_fill_family();
    ASSERT_EQ(complete.size(), 3u);
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, complete),
                          "D21-9-20-16-MUST-447").state, OutcomeState::Pass);
    auto incomplete = complete;
    incomplete.pop_back();
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, incomplete),
                          "D21-9-20-16-MUST-447").state, OutcomeState::NotRun);
    incomplete = complete;
    incomplete.back().complete = false;
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, incomplete),
                          "D21-9-20-16-MUST-447").state, OutcomeState::NotRun);
    auto duplicate = complete;
    duplicate.push_back(complete.front());
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, duplicate),
                          "D21-9-20-16-MUST-447").state, OutcomeState::NotRun);
    duplicate.back().complete = false;
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, duplicate),
                          "D21-9-20-16-MUST-447").state, OutcomeState::NotRun);
}

TEST(Draft21RawFamilies, CloseFailureDominatesMissingDuplicateOrSuccessfulContexts) {
    // Draft 21 section 9.20.16 mandates PROTOCOL_VIOLATION, not code 99.
    const auto catalog = raw_catalog();
    const auto complete = successful_fill_family();
    ASSERT_EQ(complete.size(), 3u);
    auto failed = complete.front();
    failed.events.back() = transport::PeerCloseEvent{
        transport::CloseErrorSpace::Application, 99, {}};
    std::vector<scenarios::RawProbeTranscript> contexts{failed};
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts),
                          "D21-9-20-16-MUST-447").state, OutcomeState::Fail);
    contexts.insert(contexts.begin(), complete.begin(), complete.end());
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts),
                          "D21-9-20-16-MUST-447").state, OutcomeState::Fail);
    std::reverse(contexts.begin(), contexts.end());
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts),
                          "D21-9-20-16-MUST-447").state, OutcomeState::Fail);
}

TEST(Draft21RawFamilies, RequestFamilyNeedsBothIndependentRangeOverflowContexts) {
    // Draft 21 section 8.6 separately overflows Start and End deltas.
    const auto catalog = raw_catalog();
    const auto complete = successful_filter_family("D21-8-6-MUST-249");
    ASSERT_EQ(complete.size(), 2u);
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, complete),
                          "D21-8-6-MUST-249").state, OutcomeState::Pass);
    auto missing = complete;
    missing.pop_back();
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, missing),
                          "D21-8-6-MUST-249").state, OutcomeState::NotRun);
    missing = complete;
    --missing.back().writes.front().accepted;
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, missing),
                          "D21-8-6-MUST-249").state, OutcomeState::NotRun);
    auto duplicate = complete;
    duplicate.push_back(complete.front());
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, duplicate),
                          "D21-8-6-MUST-249").state, OutcomeState::NotRun);
    duplicate.back().complete = false;
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, duplicate),
                          "D21-8-6-MUST-249").state, OutcomeState::NotRun);
    auto failed = complete.front();
    std::get<transport::StreamDataEvent>(failed.events.back()).data[3] = std::byte{0x10};
    duplicate.push_back(std::move(failed));
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, duplicate),
                          "D21-8-6-MUST-249").state, OutcomeState::Fail);
}

TEST(Draft21RawFamilies, UnimplementedCatalogContextsCannotBecomePassAliases) {
    // Draft 21 section 3.3.2 requires the duplicate-key check on requests
    // and updates. The implemented request context alone cannot pass that row.
    const auto catalog = raw_catalog();
    auto contexts = successful_filter_family("D21-3-3-2-MUST-064");
    ASSERT_EQ(contexts.size(), 1u);
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts),
                          "D21-3-3-2-MUST-064").state, OutcomeState::NotRun);
    auto fabricated = contexts.front();
    fabricated.scenario_id = "d21-duplicate-range-filter-key-in-update";
    contexts.push_back(std::move(fabricated));
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts),
                          "D21-3-3-2-MUST-064").state, OutcomeState::NotRun);
    std::get<transport::StreamDataEvent>(contexts.front().events.back()).data[3] = std::byte{0x10};
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts),
                          "D21-3-3-2-MUST-064").state, OutcomeState::Fail);
}

TEST(Draft21RawFamilies, MixedFamiliesPreserveClassificationAndBindingChecks) {
    // Draft 21 sections 8.6 and 9.20.16 are independently classified rows.
    auto catalog = raw_catalog();
    auto contexts = successful_fill_family();
    const auto requests = successful_filter_family("D21-8-6-MUST-249");
    contexts.insert(contexts.end(), requests.begin(), requests.end());
    const auto outcomes = evaluate_draft21_raw_probes(catalog, contexts);
    EXPECT_EQ(outcomes.size(), catalog.requirements.size());
    EXPECT_EQ(outcome_for(outcomes, "D21-9-20-16-MUST-447").state, OutcomeState::Pass);
    EXPECT_EQ(outcome_for(outcomes, "D21-8-6-MUST-249").state, OutcomeState::Pass);
    const auto row = std::find_if(catalog.requirements.begin(), catalog.requirements.end(),
        [](const auto& candidate) { return candidate.id == "D21-9-20-16-MUST-447"; });
    ASSERT_NE(row, catalog.requirements.end());
    row->evaluators.push_back("unimplemented-evaluator");
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts), row->id).state,
              OutcomeState::NotRun);
    row->evaluators = {"unrelated-evaluator"};
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts), row->id).state,
              OutcomeState::NotRun);
    row->testability = Testability::NotTestable;
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts), row->id).state,
              OutcomeState::NotTestable);
    row->applicability = Applicability::NotApplicable;
    EXPECT_EQ(outcome_for(evaluate_draft21_raw_probes(catalog, contexts), row->id).state,
              OutcomeState::NotApplicable);
}

TEST(Draft21EstablishedFamilies, EveryRequiredContextMustSucceedAndActualFailureDominates) {
    const auto catalog = raw_catalog();
    for (const auto* requirement : {"D21-9-20-1-MUST-404", "D21-9-20-19-MUST-460", "D21-6-4-2-1-MUST-155"}) {
        SCOPED_TRACE(requirement);
        std::vector<scenarios::RawProbeTranscript> contexts;
        for (const auto& probe : scenarios::draft21_close_probes()) {
            if (probe.requirement_id != requirement) continue;
            auto observed = close_probe_context(probe);
            observed.events.push_back(transport::PeerCloseEvent{
                transport::CloseErrorSpace::Application, probe.expected_close.value(), {}});
            contexts.push_back(std::move(observed));
        }
        const auto row = std::find_if(catalog.requirements.begin(), catalog.requirements.end(),
            [&](const auto& item) { return item.id == requirement; });
        ASSERT_NE(row, catalog.requirements.end());
        ASSERT_EQ(contexts.size(), row->scenarios.size());
        const auto evaluate = [&](const auto& values) {
            return outcome_for(evaluate_draft21_raw_probes(catalog, values), requirement).state;
        };
        EXPECT_EQ(evaluate(contexts), OutcomeState::Pass);
        auto missing = contexts;
        missing.pop_back();
        EXPECT_EQ(evaluate(missing), OutcomeState::NotRun);
        auto duplicate = contexts;
        duplicate.push_back(contexts.back());
        EXPECT_EQ(evaluate(duplicate), OutcomeState::NotRun);
        auto broken = contexts;
        const auto staged = std::find_if(broken.begin(), broken.end(), [](const auto& item) {
            return std::any_of(item.writes.begin(), item.writes.end(), [](const auto& write) {
                return static_cast<bool>(write.write.peer_response_ready);
            });
        });
        ASSERT_NE(staged, broken.end());
        // The fixture requires an actual response after initial request acceptance.
        staged->writes.front().delivery_event_count = staged->events.size() - 1;
        EXPECT_EQ(evaluate(broken), OutcomeState::NotRun);
        std::get<transport::PeerCloseEvent>(contexts.back().events.back()).error_code = 99;
        EXPECT_EQ(evaluate(contexts), OutcomeState::Fail);
        missing.back() = contexts.back();
        EXPECT_EQ(evaluate(missing), OutcomeState::Fail);
        std::reverse(contexts.begin(), contexts.end());
        EXPECT_EQ(evaluate(contexts), OutcomeState::Fail);
    }
}

TEST(Draft21EstablishedFamilies, BindingsRequirePeerResponseTransportEvidence) {
    const auto bindings = draft21_executable_bindings();
    for (const auto* scenario : {"d21-group-order-in-subscription-update",
                                 "d21-discovery-update-invalid-forward",
                                 "d21-duplicate-request-update-id",
                                 "d21-publish-state-notify-on-namespace-request",
                                 "d21-publish-state-notify-on-fetch"}) {
        const auto found = std::find_if(bindings.begin(), bindings.end(), [&](const auto& binding) {
            return binding.scenario_id == scenario;
        });
        ASSERT_NE(found, bindings.end());
        EXPECT_NE(std::find(found->evidence_kinds.begin(), found->evidence_kinds.end(),
                            "raw_probe_transport_event"), found->evidence_kinds.end());
    }
}

TEST(Draft21NotifyRequestTypeFamilies, ActualCatalogRequiresBothEstablishedContexts) {
    const auto catalog = raw_catalog();
    const std::string requirement = "D21-9-10-MUST-369";
    const auto row = std::find_if(catalog.requirements.begin(), catalog.requirements.end(),
        [&](const auto& item) { return item.id == requirement; });
    ASSERT_NE(row, catalog.requirements.end());
    EXPECT_EQ(row->scenarios, (std::vector<std::string>{
        "d21-publish-state-notify-on-namespace-request", "d21-publish-state-notify-on-fetch"}));
    std::vector<scenarios::RawProbeTranscript> contexts;
    // The evaluator recovers this configured FETCH target from actual bytes.
    for (const auto& probe : scenarios::draft21_close_probes(
             std::chrono::milliseconds{1000}, {test::probe_bytes({'n'})}, test::probe_bytes({'t'}))) {
        if (probe.requirement_id != requirement) continue;
        auto observed = close_probe_context(probe);
        observed.events.push_back(transport::PeerCloseEvent{
            transport::CloseErrorSpace::Application, 3, {}});
        contexts.push_back(std::move(observed));
    }
    ASSERT_EQ(contexts.size(), 2u);
    const auto evaluate = [&](const auto& values) {
        return outcome_for(evaluate_draft21_raw_probes(catalog, values), requirement).state;
    };
    EXPECT_EQ(evaluate(contexts), OutcomeState::Pass);
    for (std::size_t index = 0; index < contexts.size(); ++index) {
        SCOPED_TRACE(contexts[index].scenario_id);
        EXPECT_EQ(evaluate(std::vector{contexts[index]}), OutcomeState::NotRun);
        auto changed = contexts;
        changed.push_back(contexts[index]);
        EXPECT_EQ(evaluate(changed), OutcomeState::NotRun);
        changed = contexts;
        --changed[index].writes.front().accepted;
        EXPECT_EQ(evaluate(changed), OutcomeState::NotRun);
        changed = contexts;
        --changed[index].writes.back().accepted;
        EXPECT_EQ(evaluate(changed), OutcomeState::NotRun);
        changed = contexts;
        changed[index].writes.front().delivery_event_count = 3;
        EXPECT_EQ(evaluate(changed), OutcomeState::NotRun);
        changed = contexts;
        changed[index].writes.back().stream_id = 5;
        EXPECT_EQ(evaluate(changed), OutcomeState::NotRun);
        changed = contexts;
        auto& response = std::get<transport::StreamDataEvent>(changed[index].events[2]);
        response.stream_id = 5;
        EXPECT_EQ(evaluate(changed), OutcomeState::NotRun);
        changed = contexts;
        std::get<transport::StreamDataEvent>(changed[index].events[2]).data = index == 0
            ? test::probe_bytes({7, 0, 3, 0, 4, 1})
            : test::probe_bytes({0x18, 0, 4, 2, 0, 0, 0});
        EXPECT_EQ(evaluate(changed), OutcomeState::NotRun);
        changed = contexts;
        changed[index].events.pop_back();
        EXPECT_EQ(evaluate(changed), OutcomeState::NotRun);
        changed = contexts;
        std::get<transport::PeerCloseEvent>(changed[index].events.back()).error_code = 9;
        EXPECT_EQ(evaluate(changed), OutcomeState::Fail);
        EXPECT_EQ(evaluate(std::vector{changed[index]}), OutcomeState::Fail);
    }
}

namespace {
scenarios::RawProbeTranscript response_context(const scenarios::Draft21ResponseProbe& profile) {
    const bool publish = profile.expectation == scenarios::Draft21ResponseExpectation::PermittedPublishUpdate;
    const auto opening = publish ? test::probe_bytes({0x1d, 0, 10, 0, 1, 1, 'n', 1, 'x', 0, 0, 4, 1})
                                 : std::vector<std::byte>{};
    auto result = test::raw_probe_transcript(profile.definition, opening);
    result.writes.front().delivery_event_count = result.events.size();
    const auto stream = result.writes.front().stream_id.value();
    if (!publish) {
        result.events.push_back(transport::StreamDataEvent{stream,
            profile.expectation == scenarios::Draft21ResponseExpectation::FailedSubscriptionCleanup
                ? test::probe_bytes({4, 0, 4, 0, 0, 4, 1}) : test::probe_bytes({7, 0, 1, 0}), false});
    }
    result.writes.back().stream_id = stream;
    result.writes.back().delivery_event_count = result.events.size();
    result.delivery_event_count = result.events.size();
    auto reply = publish ? test::probe_bytes({7, 0, 1, 0}) : test::probe_bytes({5, 0, 3, 1, 0, 0});
    if (profile.expectation == scenarios::Draft21ResponseExpectation::FailedSubscriptionCleanup) {
        const auto done = test::probe_bytes({0x0b, 0, 3, 8, 0, 0});
        reply.insert(reply.end(), done.begin(), done.end());
    }
    result.events.push_back(transport::StreamDataEvent{stream, reply, !publish});
    return result;
}
}

TEST(Draft21ResponseFamilies, ActualCatalogNeedsEveryNamedContextAndFailureDominates) {
    const auto catalog = raw_catalog();
    for (const auto* requirement : {"D21-9-5-MUST-344", "D21-9-5-1-MUST-346",
                                   "D21-9-5-1-MUST-348", "D21-9-5-1-MUST-349"}) {
        SCOPED_TRACE(requirement);
        std::vector<scenarios::RawProbeTranscript> contexts;
        for (const auto& profile : scenarios::draft21_response_probes()) {
            if (profile.requirement_id == requirement) contexts.push_back(response_context(profile));
        }
        for (const auto& probe : scenarios::draft21_close_probes()) {
            if (probe.requirement_id != requirement) continue;
            auto observed = close_probe_context(probe);
            observed.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
            contexts.push_back(std::move(observed));
        }
        for (const auto& probe : scenarios::draft21_peer_close_probes()) {
            if (probe.requirement_id != requirement) continue;
            auto observed = test::raw_probe_transcript(probe.definition,
                test::probe_bytes({6, 0, 5, 0, 1, 1, 'n', 0}));
            for (auto& write : observed.writes) write.delivery_event_count = observed.events.size();
            observed.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
            contexts.push_back(std::move(observed));
        }
        const auto row = std::find_if(catalog.requirements.begin(), catalog.requirements.end(),
            [&](const auto& item) { return item.id == requirement; });
        ASSERT_NE(row, catalog.requirements.end());
        ASSERT_EQ(contexts.size(), row->scenarios.size());
        const auto evaluate = [&](const auto& values) {
            return outcome_for(evaluate_draft21_raw_probes(catalog, values), requirement).state;
        };
        EXPECT_EQ(evaluate(contexts), OutcomeState::Pass);
        auto missing = contexts;
        missing.pop_back();
        EXPECT_EQ(evaluate(missing), OutcomeState::NotRun);
        auto duplicate = contexts;
        duplicate.push_back(contexts.front());
        EXPECT_EQ(evaluate(duplicate), OutcomeState::NotRun);
        auto unaccepted = contexts;
        --unaccepted.front().writes.back().accepted;
        EXPECT_EQ(evaluate(unaccepted), OutcomeState::NotRun);
        auto failed = contexts;
        if (std::string(requirement) == "D21-9-5-MUST-344") {
            failed.front().events.back() = transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}};
        } else {
            // A valid error with no sending-direction closure cannot establish cleanup.
            std::get<transport::StreamDataEvent>(failed.front().events.back()).fin = false;
            EXPECT_EQ(evaluate(failed), OutcomeState::NotRun);
            if (std::string(requirement) == "D21-9-5-1-MUST-346") {
                auto& reply = std::get<transport::StreamDataEvent>(failed.front().events.back());
                reply.data = test::probe_bytes({5, 0, 3, 1, 0, 0});
                reply.fin = true;
            } else continue;
        }
        EXPECT_EQ(evaluate(failed), OutcomeState::Fail);
        failed.push_back(failed.front());
        EXPECT_EQ(evaluate(failed), OutcomeState::Fail);
        std::reverse(failed.begin(), failed.end());
        EXPECT_EQ(evaluate(failed), OutcomeState::Fail);
    }
}

}  // namespace moq::interop::requirements
