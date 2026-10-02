#include "moq/interop/scenarios/object_repeat.h"
#include "moq/interop/wire/cursor.h"
#include <algorithm>
#include <stdexcept>
#include <gtest/gtest.h>

namespace moq::interop::scenarios {
namespace {
using Bytes = std::vector<std::byte>;
Bytes b(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values) result.push_back(static_cast<std::byte>(value));
    return result;
}
RawProbeTranscript initial(const ObjectRepeatProbe& p) {
    RawProbeTranscript t;
    t.scenario_id = p.definition.id;
    t.setup = {{RawProbeChannel::NewUni, b({0xaf, 0, 0, 0}), false}, 3, 4, false, 1};
    t.events = {transport::ConnectionEstablishedEvent{}, transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false}};
    const auto& write = p.definition.writes.front();
    t.writes.push_back({write, 1, write.bytes.size(), write.fin, 2});
    t.complete = t.stimulus_delivered = t.transport_established = t.peer_setup_received = true;
    t.delivery_event_count = 2;
    return t;
}
void fetch_ack(RawProbeTranscript& t, unsigned draft, std::uint64_t stream = 1) {
    t.events.push_back(transport::StreamDataEvent{stream, b({0x18, 0, 4, 0, 7, draft == 18 ? 10u : 9u, 0}), true});
}
TEST(ObjectRepeat, LegalImmutableObjectRequiresActualTypedAckCompleteObjectAndFin) {
    for (const unsigned draft : {18u, 21u}) {
        const auto profiles = draft == 18 ? draft18_object_repeat_probes() : draft21_object_repeat_probes();
        const auto profile = std::find_if(profiles.begin(), profiles.end(), [](const auto& p) {
            return p.aspect == ObjectRepeatAspect::ImmutableCount;
        });
        ASSERT_NE(profile, profiles.end());
        auto t = initial(*profile);
        fetch_ack(t, draft);
        // FETCH type5, req1, flags include absolute Group/Object and priority,
        // one empty Immutable Properties container, complete payload and FIN.
        t.events.push_back(transport::StreamDataEvent{6, b({5, 1, 0x3c, 7, 9, 99, 2, 0xb, 0, 1, 42}), true});
        EXPECT_EQ(evaluate_object_repeat_probe(t, *profile), std::optional<bool>{true});
    }
}
}  // namespace
}  // namespace moq::interop::scenarios

namespace moq::interop::scenarios {
namespace {
std::vector<ObjectRepeatProbe> all_profiles() {
    auto result = draft18_object_repeat_probes();
    auto d21 = draft21_object_repeat_probes();
    result.insert(result.end(), d21.begin(), d21.end());
    return result;
}
ObjectRepeatProbe payload_profile(std::vector<Bytes> ns = {b({'n'})}, Bytes name = b({'t'})) {
    const auto profiles = draft21_object_repeat_probes(std::chrono::milliseconds(1000), std::move(ns), std::move(name));
    return *std::find_if(profiles.begin(), profiles.end(), [](const auto& profile) {
        return profile.aspect == ObjectRepeatAspect::Payload;
    });
}
void vi(Bytes& result, std::uint64_t value) {
    wire::ByteWriter writer(9);
    ASSERT_TRUE(wire::write_vi64(value, writer));
    result.insert(result.end(), writer.bytes().begin(), writer.bytes().end());
}
Bytes fetch_object(unsigned request_id = 1, Bytes properties = {}, Bytes payload = b({42}),
                   unsigned flags = 0x1c, unsigned group = 7, unsigned object = 9) {
    Bytes result = b({5, request_id});
    vi(result, flags);
    vi(result, group);
    if ((flags & 0x40) == 0 && (flags & 3) == 3) vi(result, 0);
    vi(result, object);
    result.push_back(std::byte{99});
    if (flags & 0x20) {
        vi(result, properties.size());
        result.insert(result.end(), properties.begin(), properties.end());
    }
    vi(result, payload.size());
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
Bytes subgroup(Bytes payload = b({42}), unsigned alias = 4, unsigned group = 7, unsigned object = 9) {
    auto result = b({0x10, alias, group, 99, object});
    vi(result, payload.size());
    if (payload.empty()) result.push_back(std::byte{0});
    result.insert(result.end(), payload.begin(), payload.end());
    return result;
}
void subscribe_ack(RawProbeTranscript& t, unsigned alias = 4) {
    t.events.push_back(transport::StreamDataEvent{1, b({4, 0, 2, alias, 0}), false});
}
void second_write(RawProbeTranscript& t, const ObjectRepeatProbe& p) {
    const auto& write = p.definition.writes[1];
    t.writes.push_back({write, 5, write.bytes.size(), write.fin, t.events.size()});
    t.delivery_event_count = t.events.size();
}
RawProbeTranscript payload_transcript(const ObjectRepeatProbe& p, Bytes first = b({42}), Bytes second = b({42}),
                                      bool ack_first = true) {
    auto t = initial(p);
    if (ack_first) subscribe_ack(t);
    t.events.push_back(transport::StreamDataEvent{6, subgroup(std::move(first)), true});
    if (!ack_first) subscribe_ack(t);
    second_write(t, p);
    if (ack_first) fetch_ack(t, 21, 5);
    t.events.push_back(transport::StreamDataEvent{10, fetch_object(3, {}, std::move(second)), true});
    if (!ack_first) fetch_ack(t, 21, 5);
    return t;
}
TEST(ObjectRepeat, ExactlyThreeCatalogProfilesAndCanonicalActualRequests) {
    const auto profiles = all_profiles();
    ASSERT_EQ(profiles.size(), 3u);
    EXPECT_EQ(profiles[0].requirement_id, "D18-12-7-MUST-NOT-005");
    EXPECT_EQ(profiles[1].requirement_id, "D21-2-1-MUST-NOT-016");
    EXPECT_EQ(profiles[2].requirement_id, "D21-10-7-MUST-NOT-492");
    const auto p = payload_profile();
    ASSERT_EQ(p.definition.writes.size(), 2u);
    EXPECT_EQ(p.definition.writes[0].bytes, b({3, 0, 13, 1, 1, 1, 'n', 1, 't', 2, 0x10, 1, 0x11, 2, 7, 9}));
    EXPECT_EQ(p.definition.writes[1].bytes, b({0x16, 0, 13, 3, 1, 1, 'n', 1, 't', 1, 0x21, 4, 7, 9, 0, 9}));
    EXPECT_FALSE(p.definition.writes[0].fin);
    EXPECT_TRUE(p.definition.writes[1].fin);
    EXPECT_TRUE(static_cast<bool>(p.definition.writes[1].evidence_ready));
}
TEST(ObjectRepeat, PayloadComparesActualSubscriptionAndFetchForConfiguredFullIdentity) {
    for (const bool ack_first : {false, true}) {
        for (const auto& ns : std::vector<std::vector<Bytes>>{{b({'n'})}, {b({'a'}), b({'b'})}}) {
            const auto p = payload_profile(ns, b({'t'}));
            auto t = payload_transcript(p, b({42, 43}), b({42, 43}), ack_first);
            EXPECT_EQ(evaluate_object_repeat_probe(t, p), true);
            EXPECT_TRUE(p.definition.response_ready(t));
            t = payload_transcript(p, b({42, 43}), b({42, 44}), ack_first);
            EXPECT_EQ(evaluate_object_repeat_probe(t, p), false);
            EXPECT_TRUE(p.definition.response_ready(t));
            t.complete = false;
            EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
        }
    }
}
TEST(ObjectRepeat, FetchGateRequiresCompleteTypedAliasAssociatedOrdinaryObjectAndFin) {
    const auto p = payload_profile();
    for (unsigned mode = 0; mode < 10; ++mode) {
        SCOPED_TRACE(mode);
        auto t = initial(p);
        if (mode != 0) subscribe_ack(t);
        if (mode == 1) t.events.back() = transport::StreamDataEvent{1, b({4, 0}), false};
        const auto object = mode == 2 ? subgroup(b({42}), 5)
                           : mode == 3 ? subgroup(b({42}), 4, 8)
                           : mode == 4 ? subgroup(b({42}), 4, 7, 10)
                           : mode == 5 ? b({0x10, 4, 7, 99, 9, 2, 42})
                           : mode == 6 ? b({0x10, 4, 7, 99, 9, 0, 3})
                                       : subgroup();
        if (mode == 7) t.events.push_back(transport::PeerResetEvent{6, 1});
        if (mode == 8) t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
        t.events.push_back(transport::StreamDataEvent{6, object, mode != 9});
        EXPECT_FALSE(p.definition.writes[1].evidence_ready({t.writes, t.events}));
    }
    auto t = initial(p);
    t.events.push_back(transport::StreamDataEvent{6, subgroup(), true});
    EXPECT_FALSE(p.definition.writes[1].evidence_ready({t.writes, t.events}));
    subscribe_ack(t);
    EXPECT_TRUE(p.definition.writes[1].evidence_ready({t.writes, t.events}));
}
TEST(ObjectRepeat, FullPayloadRetentionDetectsDifferenceAfterDefaultDecoderLimit) {
    const auto p = payload_profile();
    Bytes first(5000, std::byte{42}), second = first;
    auto t = payload_transcript(p, first, second);
    EXPECT_EQ(evaluate_object_repeat_probe(t, p), true);
    second.back() = std::byte{43};
    t = payload_transcript(p, first, second);
    EXPECT_EQ(evaluate_object_repeat_probe(t, p), false);
    t = payload_transcript(p, Bytes(40000, std::byte{42}), Bytes(40000, std::byte{42}));
    EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
}
TEST(ObjectRepeat, ZeroPayloadOrdinaryStatusIsComparedButUnavailableIsNotChangedContents) {
    const auto p = payload_profile();
    auto t = payload_transcript(p, {}, {});
    EXPECT_EQ(evaluate_object_repeat_probe(t, p), true);
    t = payload_transcript(p, {}, b({42}));
    EXPECT_EQ(evaluate_object_repeat_probe(t, p), false);
    t.events[3] = transport::StreamDataEvent{6, b({0x10, 4, 7, 99, 9, 0, 3}), true};
    EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
}
TEST(ObjectRepeat, ImmutableOuterCountFailsFromActualCompletePropertiesHeader) {
    for (const auto& p : all_profiles()) {
        if (p.aspect != ObjectRepeatAspect::ImmutableCount) continue;
        for (const bool payload_finished : {false, true}) {
            for (const bool ack_first : {false, true}) {
                auto t = initial(p);
                if (ack_first) fetch_ack(t, p.draft);
                auto object = fetch_object(1, b({0xb, 0, 0, 0}), b({42}), 0x3c);
                if (!payload_finished) object.resize(11); // through actual complete properties; no payload
                t.events.push_back(transport::StreamDataEvent{6, object, payload_finished});
                if (!ack_first) fetch_ack(t, p.draft);
                EXPECT_EQ(evaluate_object_repeat_probe(t, p), false);
                EXPECT_TRUE(p.definition.response_ready(t));
            }
        }
    }
}
TEST(ObjectRepeat, ImmutableZeroAndOneRequireCompleteTypedObjectFinAndNoNestedMalformedList) {
    for (const auto& p : all_profiles()) {
        if (p.aspect != ObjectRepeatAspect::ImmutableCount) continue;
        for (const auto& properties : std::vector<Bytes>{{}, b({0xb, 0}), b({0xb, 2, 2, 1})}) {
            auto t = initial(p);
            fetch_ack(t, p.draft);
            const auto object = fetch_object(1, properties, b({42}), properties.empty() ? 0x1c : 0x3c);
            t.events.push_back(transport::StreamDataEvent{6, object, true});
            EXPECT_EQ(evaluate_object_repeat_probe(t, p), true);
            t.events.back() = transport::StreamDataEvent{6, object, false};
            EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
        }
        for (const auto& malformed : {b({0xb, 2, 0xb, 0}), b({0xb, 1, 2}), b({4, 1}), b({0xb, 2, 4, 1}), b({0xb, 0, 0, 1})}) {
            auto t = initial(p);
            fetch_ack(t, p.draft);
            t.events.push_back(transport::StreamDataEvent{6, fetch_object(1, malformed, b({42}), 0x3c), true});
            EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
        }
    }
}
TEST(ObjectRepeat, AllLegalFirstObjectFlagsAreAcceptedIncludingDatagramIgnoredLowBits) {
    for (const auto& p : all_profiles()) {
        if (p.aspect != ObjectRepeatAspect::ImmutableCount) continue;
        for (const unsigned flags : {0x1cu, 0x1fu, 0x3cu, 0x3fu, 0x5cu, 0x5du, 0x5eu, 0x5fu, 0x7cu, 0x7fu}) {
            SCOPED_TRACE(flags);
            auto t = initial(p);
            fetch_ack(t, p.draft);
            t.events.push_back(transport::StreamDataEvent{6, fetch_object(1, {}, b({42}), flags), true});
            EXPECT_EQ(evaluate_object_repeat_probe(t, p), true);
        }
    }
}
TEST(ObjectRepeat, MissingAckWrongRequestRangeLocationOrStreamAssociationCannotProveCount) {
    for (const auto& p : all_profiles()) {
        if (p.aspect != ObjectRepeatAspect::ImmutableCount) continue;
        for (unsigned mode = 0; mode < 9; ++mode) {
            auto t = initial(p);
            if (mode != 0) fetch_ack(t, p.draft);
            if (mode == 1) t.events.back() = transport::StreamDataEvent{1, b({0x18, 0, 4, 0, 7, 8, 0}), true};
            auto object = mode == 2 ? fetch_object(3)
                        : mode == 3 ? fetch_object(1, {}, b({42}), 0x1c, 8)
                        : mode == 4 ? fetch_object(1, {}, b({42}), 0x1c, 7, 10)
                        : mode == 5 ? b({5, 1, 0x80, 0x8c, 7, 9})
                        : mode == 6 ? b({5, 1, 0x0c, 7, 9, 1, 42})
                                    : fetch_object();
            t.events.push_back(transport::StreamDataEvent{6, object, true});
            if (mode == 7) t.events.push_back(transport::StreamDataEvent{10, object, true});
            if (mode == 8) t.events.push_back(transport::PeerResetEvent{6, 1});
            EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
        }
    }
}
TEST(ObjectRepeat, ActualMarkersRegenerateBothRequestsAndCannotUseFutureGateEvidence) {
    const auto p = payload_profile();
    for (unsigned mode = 0; mode < 10; ++mode) {
        auto t = payload_transcript(p);
        if (mode == 0) t.writes[0].write.bytes[3] = std::byte{3};
        if (mode == 1) t.writes[1].write.bytes[3] = std::byte{1};
        if (mode == 2) t.writes[1].write.bytes.back() = std::byte{10};
        if (mode == 3) t.writes[1].delivery_event_count = 3;
        if (mode == 4) t.writes[1].accepted--;
        if (mode == 5) t.writes[1].fin_accepted = false;
        if (mode == 6) t.writes[1].stream_id = 1;
        if (mode == 7) t.writes[0].delivery_event_count.reset();
        if (mode == 8) t.writes[1].prepared_event_count = 4;
        if (mode == 9) t.scenario_id = "another-profile";
        EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
    }
}
TEST(ObjectRepeat, ResetTerminalAndEvidenceBoundsNeverReviveMissingProof) {
    const auto p = payload_profile();
    for (unsigned mode = 0; mode < 9; ++mode) {
        auto t = payload_transcript(p);
        if (mode == 0) t.events.insert(t.events.begin() + 2, transport::PeerResetEvent{1, 1});
        if (mode == 1) t.events.insert(t.events.begin() + 4, transport::PeerResetEvent{10, 1});
        if (mode == 2) t.events.insert(t.events.begin() + 4, transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
        if (mode == 3) t.events.insert(t.events.begin() + 4, transport::TransportErrorEvent{});
        if (mode == 4) t.harness_failed = true;
        if (mode == 5) t.timed_out = true;
        if (mode == 6) t.events.resize(4097, transport::StreamDataEvent{14, {}, false});
        if (mode == 7) t.events.push_back(transport::StreamDataEvent{14, Bytes(65546, std::byte{0}), false});
        if (mode == 8) t.events.push_back(transport::StreamDataEvent{10, fetch_object(3), true});
        EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
    }
    auto t = payload_transcript(p);
    t.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}});
    EXPECT_EQ(evaluate_object_repeat_probe(t, p), true);
}
TEST(ObjectRepeat, FragmentedAckHeaderPropertiesPayloadAndFinPreserveProof) {
    for (const auto& p : all_profiles()) {
        if (p.aspect != ObjectRepeatAspect::ImmutableCount) continue;
        auto t = initial(p);
        const auto ack = b({0x18, 0, 4, 0, 7, p.draft == 18 ? 10u : 9u, 0});
        const auto object = fetch_object(1, b({0xb, 2, 2, 1}), b({42, 43}), 0x3c);
        for (const auto value : object) t.events.push_back(transport::StreamDataEvent{6, Bytes{value}, false});
        for (const auto value : ack) t.events.push_back(transport::StreamDataEvent{1, Bytes{value}, false});
        EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
        t.events.push_back(transport::StreamDataEvent{6, {}, true});
        EXPECT_EQ(evaluate_object_repeat_probe(t, p), true);
    }
}
TEST(ObjectRepeat, InvalidTrackFixtureAndDeadlineRejectedBeforeDefinitions) {
    for (const unsigned draft : {18u, 21u}) {
        const auto factory = draft == 18 ? draft18_object_repeat_probes : draft21_object_repeat_probes;
        EXPECT_THROW(factory(std::chrono::milliseconds(0), {}, b({'x'})), std::invalid_argument);
        EXPECT_THROW(factory(std::chrono::milliseconds(1), {b({'.'})}, b({'x'})), std::invalid_argument);
        EXPECT_THROW(factory(std::chrono::milliseconds(1), std::vector<Bytes>(33, b({'n'})), b({'x'})), std::invalid_argument);
        EXPECT_THROW(factory(std::chrono::milliseconds(1), {}, Bytes(4097, std::byte{'x'})), std::invalid_argument);
    }
}
}  // namespace
}  // namespace moq::interop::scenarios

namespace moq::interop::scenarios {
namespace {
TEST(ObjectRepeat, OversizedDatagramConnectionMetadataAndStimulusCannotEvadeAggregateBound) {
    const auto p = payload_profile();
    for (unsigned mode = 0; mode < 6; ++mode) {
        SCOPED_TRACE(mode);
        auto t = payload_transcript(p);
        const Bytes excessive(65546, std::byte{0});
        if (mode == 0) t.events.push_back(transport::DatagramEvent{excessive});
        if (mode == 1) std::get<transport::ConnectionEstablishedEvent>(t.events[0]).alpn = excessive;
        if (mode == 2) std::get<transport::ConnectionEstablishedEvent>(t.events[0]).local_connection_id = excessive;
        if (mode == 3) std::get<transport::ConnectionEstablishedEvent>(t.events[0]).peer_connection_id = excessive;
        if (mode == 4) t.setup.write.bytes = excessive;
        if (mode == 5) t.writes[0].write.bytes = excessive;
        EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
    }
    auto t = payload_transcript(p);
    // Each irrelevant event alone fits; their actual aggregate does not.
    t.events.push_back(transport::DatagramEvent{Bytes(33000, std::byte{0})});
    t.events.push_back(transport::DatagramEvent{Bytes(33000, std::byte{0})});
    EXPECT_EQ(evaluate_object_repeat_probe(t, p), std::nullopt);
}
}  // namespace
}  // namespace moq::interop::scenarios
