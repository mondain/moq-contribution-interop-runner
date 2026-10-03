#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/scenarios/draft18_close.h"
#include "moq/interop/app/scenario_registry.h"
#include <gtest/gtest.h>
#include <deque>
#include <map>

namespace moq::interop::scenarios {
namespace {
class ProbeTransport : public transport::SessionTransport {
public:
    transport::OpenResult open_bidi() override { return {transport::TransportStatus::Success, next_bidi += 4}; }
    transport::OpenResult open_uni() override { return {transport::TransportStatus::Success, next_uni += 4}; }
    transport::OperationResult write(transport::StreamId id, std::span<const std::byte> bytes, bool fin) override {
        if (block && id == 1) {
            if (output[id].empty() && !bytes.empty()) {
                output[id].push_back(bytes.front());
                return {transport::TransportStatus::Partial, 1, {}};
            }
            return {transport::TransportStatus::WouldBlock, 0, {}};
        }
        output[id].insert(output[id].end(), bytes.begin(), bytes.end());
        if (fin) fins.push_back(id);
        return {transport::TransportStatus::Success, bytes.size(), {}};
    }
    transport::OperationResult reset(transport::StreamId, std::uint64_t) override { return {}; }
    transport::OperationResult stop_sending(transport::StreamId id, std::uint64_t code) override {
        stop_calls.emplace_back(id,code);
        if (stop_results.empty()) return {transport::TransportStatus::Success,0,{}};
        const auto result = stop_results.front(); stop_results.pop_front(); return result;
    }
    transport::OperationResult send_datagram(std::span<const std::byte> bytes) override {
        if (datagram_status != transport::TransportStatus::Success)
            return {datagram_status,datagram_status == transport::TransportStatus::Partial ? 1u : 0u,{}};
        datagrams.emplace_back(bytes.begin(),bytes.end());
        return {transport::TransportStatus::Success,bytes.size(),{}};
    }
    transport::OperationResult close(std::uint64_t, std::span<const std::byte>) override { return {}; }
    std::vector<transport::TransportEvent> poll(std::size_t) override { auto result = std::move(events); events.clear(); return result; }
    std::uint64_t next_bidi = static_cast<std::uint64_t>(-3);
    std::uint64_t next_uni = static_cast<std::uint64_t>(-1);
    std::map<std::uint64_t, std::vector<std::byte>> output;
    std::vector<std::uint64_t> fins;
    std::vector<transport::TransportEvent> events;
    bool block = false;
    transport::TransportStatus datagram_status{transport::TransportStatus::Success};
    std::vector<std::vector<std::byte>> datagrams;
    std::vector<std::pair<transport::StreamId,std::uint64_t>> stop_calls;
    std::deque<transport::OperationResult> stop_results;
};
std::vector<std::byte> b(std::initializer_list<unsigned> bytes) { std::vector<std::byte> result; for(auto v:bytes) result.push_back(static_cast<std::byte>(v)); return result; }
RawProbeDefinition reactive_definition() {
    RawProbeDefinition definition{"peer-response", b({0xaf, 0, 0, 0}),
        {{RawProbeChannel::PeerBidi, b({5, 0, 3, 0x10, 0, 0}), false}}, true,
        [](auto input) { return input.size() == 4; }, std::chrono::milliseconds(10)};
    definition.peer_request_ready = [](auto input) {
        return std::vector<std::byte>(input.begin(), input.end()) == b({0x1d, 0, 1, 0});
    };
    return definition;
}
RawProbeDefinition staged_definition(bool peer_opened) {
    auto definition = reactive_definition();
    definition.id = peer_opened ? "staged-peer-response" : "staged-server-request";
    definition.writes.front().channel = peer_opened ? RawProbeChannel::PeerBidi : RawProbeChannel::NewBidi;
    definition.writes.front().bytes = peer_opened ? b({7,0,1,0}) : b({3,0,5,1,0,1,'x',0});
    if (!peer_opened) definition.peer_request_ready = {};
    RawProbeWrite next{definition.writes.front().channel,
        peer_opened ? b({7,0,3,0,0x22,3}) : b({2,0,2,3,0}),false};
    next.reuse_write_stream = 0;
    next.peer_response_ready = [peer_opened](auto input) {
        return std::vector<std::byte>(input.begin(),input.end()) ==
            (peer_opened ? b({2,0,2,2,0}) : b({4,0,2,0,0}));
    };
    definition.writes.push_back(std::move(next));
    return definition;
}
RawProbeTranscript staged_transcript(bool peer_opened) {
    const auto definition = staged_definition(peer_opened);
    RawProbeTranscript result;
    result.scenario_id = definition.id;
    result.setup = {{RawProbeChannel::NewUni,b({0xaf,0,0,0}),false},3,4,false};
    const auto stream = peer_opened ? 0u : 1u;
    result.transport_established = result.peer_setup_received = true;
    result.stimulus_delivered = result.complete = true;
    result.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
    if (peer_opened)
        result.events.push_back(transport::StreamDataEvent{0,b({0x1d,0,1,0}),false});
    const auto first_marker = result.events.size();
    result.events.push_back(transport::StreamDataEvent{stream,
        peer_opened ? b({2,0,2,2,0}) : b({4,0,2,0,0}),false});
    const auto second_marker = result.events.size();
    result.writes = {{definition.writes[0],stream,definition.writes[0].bytes.size(),false,first_marker},
                     {definition.writes[1],stream,definition.writes[1].bytes.size(),false,second_marker}};
    result.delivery_event_count = second_marker;
    result.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});
    return result;
}

bool independent_cross_stream_gate(const RawProbeGateInput& input) {
    if (input.prior_writes.size() != 1 || !input.prior_writes[0].stream_id ||
        input.prior_writes[0].accepted != 8 || !input.prior_writes[0].delivery_event_count ||
        *input.prior_writes[0].delivery_event_count > input.events.size()) return false;
    std::vector<std::byte> observed;
    for (std::size_t i=*input.prior_writes[0].delivery_event_count;i<input.events.size();++i) {
        if (const auto* data=std::get_if<transport::StreamDataEvent>(&input.events[i]);
            data && data->stream_id == 6) {
            if (data->fin || observed.size()+data->data.size()>2) return false;
            observed.insert(observed.end(),data->data.begin(),data->data.end());
        }
    }
    return observed == b({0x09,0x07});
}
RawProbeDefinition stop_definition(bool peer_opened=false) {
    RawProbeDefinition result{"stop-stage",b({0xaf,0,0,0}),
        {{peer_opened ? RawProbeChannel::PeerBidi : RawProbeChannel::NewBidi,b({0x11,0,5,1,0,0,0,0}),false}},
        true,[](auto input){return input.size()==4;},std::chrono::milliseconds(10)};
    RawProbeWrite stop{result.writes[0].channel,{},false,0,{}};
    stop.operation=RawProbeOperation::StopSending;
    stop.application_error=0x37;
    stop.evidence_ready=independent_cross_stream_gate;
    result.writes.push_back(std::move(stop));
    if (peer_opened) result.peer_request_ready=[](auto input){return input.size()==4;};
    return result;
}
RawProbeTranscript independent_stop_transcript() {
    const auto definition=stop_definition();
    RawProbeTranscript t;
    t.scenario_id="stop-stage";
    t.setup={{RawProbeChannel::NewUni,b({0xaf,0,0,0}),false},3,4,false};
    t.transport_established=t.peer_setup_received=t.stimulus_delivered=t.complete=true;
    t.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({0xaf,0,0,0}),false},
        transport::StreamDataEvent{6,b({0x09}),false},transport::StreamDataEvent{6,b({0x07}),false},
        transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    t.writes={{{RawProbeChannel::NewBidi,b({0x11,0,5,1,0,0,0,0}),false},1,8,false,2},
              {definition.writes[1],1,0,false,4,true}};
    t.delivery_event_count=4;
    return t;
}

TEST(RawProbeStop, ValidatesActualBidiReuseEmptyBytesNoOperationFinAndQuicCode) {
    ProbeTransport transport;
    for (unsigned mutation=0;mutation<10;++mutation) {
        auto definition=stop_definition();
        switch (mutation) {
            case 0: definition.writes[1].reuse_write_stream.reset();break;
            case 1: definition.writes[1].reuse_write_stream=1;break;
            case 2: definition.writes[1].channel=RawProbeChannel::Datagram;break;
            case 3: definition.writes[1].channel=RawProbeChannel::NewUni;break;
            case 4: definition.writes[1].channel=RawProbeChannel::Control;break;
            case 5: definition.writes[1].bytes=b({1});break;
            case 6: definition.writes[1].fin=true;break;
            case 7: definition.writes[0].channel=RawProbeChannel::NewUni;break;
            case 8: definition.writes[1].application_error=std::uint64_t{1}<<62;break;
            case 9: definition.writes[1].operation=static_cast<RawProbeOperation>(99);break;
        }
        EXPECT_THROW(RawProbeController(transport,definition),std::invalid_argument)<<mutation;
    }
    for (const bool peer_opened:{false,true}) {
        auto definition=stop_definition(peer_opened);
        definition.writes[1].application_error=(std::uint64_t{1}<<62)-1;
        EXPECT_NO_THROW(RawProbeController(transport,definition));
    }
}

TEST(RawProbeStop, AcceptedLocalFinAllowsStopOnStillOpenReceivingDirection) {
    auto definition=stop_definition();definition.writes[0].fin=true;
    ProbeTransport transport;
    ASSERT_NO_THROW(RawProbeController(transport,definition));
    RawProbeController controller(transport,definition);
    const auto now=RawProbeClock::time_point{};
    transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
    ASSERT_FALSE(controller.poll(now).stimulus_delivered);
    ASSERT_TRUE(controller.transcript().writes[0].fin_accepted);
    EXPECT_EQ(transport.fins,std::vector<transport::StreamId>{1});
    transport.events={transport::StreamDataEvent{6,b({0x09,0x07}),false}};
    ASSERT_TRUE(controller.poll(now).stimulus_delivered);
    ASSERT_EQ(transport.stop_calls.size(),1u);
    EXPECT_EQ(transport.stop_calls.front(),std::make_pair(transport::StreamId{1},std::uint64_t{0x37}));
    EXPECT_TRUE(controller.transcript().writes[1].operation_accepted);
    transport.events={transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    ASSERT_TRUE(controller.poll(now).complete);
    EXPECT_TRUE(raw_probe_stimulus_valid(controller.transcript(),definition));

    auto literal=independent_stop_transcript();literal.writes[0].write.fin=true;literal.writes[0].fin_accepted=true;
    EXPECT_TRUE(raw_probe_stimulus_valid(literal,definition));
    literal.writes[0].fin_accepted=false;
    EXPECT_FALSE(raw_probe_stimulus_valid(literal,definition));
    literal.writes[0].fin_accepted=true;
    literal.events.insert(literal.events.begin()+4,transport::StreamDataEvent{1,{},true});
    literal.writes[1].delivery_event_count=literal.delivery_event_count=5;
    EXPECT_FALSE(raw_probe_stimulus_valid(literal,definition));

    auto update=definition;update.writes[1].operation=RawProbeOperation::Write;
    update.writes[1].bytes=b({0x22});update.writes[1].application_error=0;
    EXPECT_THROW(RawProbeController(transport,update),std::invalid_argument);
}

TEST(RawProbeStop, ActualWouldBlockThenAcceptanceUsesExactStreamCodeAndFinalMarker) {
    for (const bool peer_opened:{false,true}) {
        ProbeTransport transport;
        const auto definition=stop_definition(peer_opened);
        RawProbeController controller(transport,definition);
        const auto now=RawProbeClock::time_point{};
        transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
        if(peer_opened)transport.events.push_back(transport::StreamDataEvent{0,b({0x1d,0,1,0}),false});
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        const auto stream=peer_opened?0u:1u;
        EXPECT_EQ(transport.output[stream],b({0x11,0,5,1,0,0,0,0}));
        EXPECT_TRUE(transport.stop_calls.empty());
        transport.events={transport::StreamDataEvent{10,b({0x09,0x07}),false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        transport.events={transport::StreamDataEvent{6,b({0x09}),false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        EXPECT_TRUE(transport.stop_calls.empty());
        transport.stop_results.push_back({transport::TransportStatus::WouldBlock,0,{}});
        transport.events={transport::StreamDataEvent{6,b({0x07}),false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        ASSERT_EQ(transport.stop_calls.size(),1u);
        EXPECT_EQ(transport.stop_calls[0],std::make_pair(transport::StreamId{stream},std::uint64_t{0x37}));
        EXPECT_FALSE(controller.transcript().writes[1].operation_accepted);
        EXPECT_FALSE(controller.transcript().writes[1].delivery_event_count);
        transport.events={transport::DatagramEvent{b({0x33})}};
        ASSERT_TRUE(controller.poll(now).stimulus_delivered);
        EXPECT_TRUE(controller.transcript().writes[1].operation_accepted);
        EXPECT_EQ(controller.transcript().writes[1].accepted,0u);
        EXPECT_FALSE(controller.transcript().writes[1].fin_accepted);
        EXPECT_EQ(controller.transcript().writes[1].delivery_event_count,controller.transcript().events.size());
        EXPECT_EQ(transport.next_bidi,peer_opened?static_cast<std::uint64_t>(-3):1u);
        EXPECT_EQ(transport.stop_calls.size(),2u);
        transport.events={transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
        ASSERT_TRUE(controller.poll(now).complete);
        EXPECT_EQ(evaluate_raw_probe_close(controller.transcript(),definition,3),true);
        EXPECT_EQ(transport.stop_calls.size(),2u);
    }
}

TEST(RawProbeStop, PartialInvalidOrNonzeroAcceptanceFailsHarness) {
    const transport::OperationResult invalid[]={{transport::TransportStatus::Success,1,{}},
        {transport::TransportStatus::WouldBlock,1,{}},{transport::TransportStatus::Partial,0,{}},
        {transport::TransportStatus::Partial,1,{}},{transport::TransportStatus::InvalidState,0,{}},
        {transport::TransportStatus::PeerStopped,0,{}},{transport::TransportStatus::PeerReset,0,{}}};
    for (const auto& result:invalid) {
        ProbeTransport transport;
        const auto definition=stop_definition();RawProbeController controller(transport,definition);
        transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
        controller.poll(RawProbeClock::time_point{});
        transport.stop_results.push_back(result);
        transport.events={transport::StreamDataEvent{6,b({0x09,0x07}),false}};
        const auto& observed=controller.poll(RawProbeClock::time_point{});
        EXPECT_TRUE(observed.harness_failed);
        EXPECT_FALSE(observed.stimulus_delivered);
        EXPECT_FALSE(observed.writes[1].operation_accepted);
    }
}

TEST(RawProbeStop, ProofRejectsFutureEvidenceWrongStreamAndTamperedOperation) {
    const auto definition=stop_definition();const auto good=independent_stop_transcript();
    EXPECT_TRUE(raw_probe_stimulus_valid(good,definition));
    for (unsigned mutation=0;mutation<12;++mutation) {
        auto changed=good;
        switch(mutation) {
            case 0: changed.writes[1].operation_accepted=false;break;
            case 1: changed.writes[1].write.operation=RawProbeOperation::Write;break;
            case 2: ++changed.writes[1].write.application_error;break;
            case 3: changed.writes[1].write.evidence_ready={};break;
            case 4: changed.writes[0].operation_accepted=true;break;
            case 5: changed.writes[1].accepted=1;break;
            case 6: changed.writes[1].stream_id=5;break;
            case 7: changed.writes[1].delivery_event_count.reset();break;
            case 8: changed.writes[1].delivery_event_count=changed.delivery_event_count=3;break;
            case 9: std::get<transport::StreamDataEvent>(changed.events[3]).stream_id=10;break;
            case 10: changed.writes[0].delivery_event_count=3;break;
            case 11: std::get<transport::StreamDataEvent>(changed.events[3]).data=b({0x08});break;
        }
        EXPECT_FALSE(raw_probe_stimulus_valid(changed,definition))<<mutation;
    }
}

TEST(RawProbeStop, QueuedCloseResetStopAndFinPreventStopOperation) {
    for (unsigned cancellation=0;cancellation<4;++cancellation) {
        ProbeTransport transport; const auto definition=stop_definition();RawProbeController controller(transport,definition);
        const auto now=RawProbeClock::time_point{};
        transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
        controller.poll(now);
        transport.events={transport::StreamDataEvent{6,b({0x09,0x07}),false}};
        switch(cancellation) {
            case 0: transport.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}});break;
            case 1: transport.events.push_back(transport::PeerResetEvent{1,7});break;
            case 2: transport.events.push_back(transport::PeerStopSendingEvent{1,7});break;
            case 3: transport.events.push_back(transport::StreamDataEvent{1,{},true});break;
        }
        const auto terminal = transport.events.back();
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        EXPECT_TRUE(transport.stop_calls.empty());
        auto forged=independent_stop_transcript();
        forged.events.insert(forged.events.begin()+4,terminal);
        forged.writes[1].delivery_event_count=forged.delivery_event_count=5;
        EXPECT_FALSE(raw_probe_stimulus_valid(forged,definition));
    }
}

TEST(RawProbeEvidenceGate, GatesOnlyAcceptedPriorWritesAndObservedEventPrefix) {
    auto definition=stop_definition();definition.writes[1].operation=RawProbeOperation::Write;
    definition.writes[1].application_error=0;definition.writes[1].bytes=b({0x22});
    definition.writes[1].reuse_write_stream.reset();
    std::size_t calls=0;
    definition.writes[1].evidence_ready=[&](const RawProbeGateInput& input) {
        ++calls;EXPECT_EQ(input.prior_writes.size(),1u);
        EXPECT_TRUE(input.prior_writes[0].delivery_event_count);
        return independent_cross_stream_gate(input);
    };
    ProbeTransport transport;transport.block=true;
    RawProbeController controller(transport,definition);const auto now=RawProbeClock::time_point{};
    transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({0xaf,0,0,0}),false},
        transport::StreamDataEvent{6,b({0x09,0x07}),false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);EXPECT_EQ(calls,0u);
    transport.block=false;EXPECT_FALSE(controller.poll(now).stimulus_delivered);EXPECT_GT(calls,0u);
    transport.events={transport::StreamDataEvent{6,b({0x09,0x07}),false}};
    ASSERT_TRUE(controller.poll(now).stimulus_delivered);
    EXPECT_EQ(transport.output[5],b({0x22}));
    transport.events={transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    ASSERT_TRUE(controller.poll(now).complete);
    EXPECT_TRUE(raw_probe_stimulus_valid(controller.transcript(),definition));
    auto forged=controller.transcript();forged.writes[1].delivery_event_count.reset();
    EXPECT_FALSE(raw_probe_stimulus_valid(forged,definition));
    forged=controller.transcript();forged.writes[1].delivery_event_count=forged.delivery_event_count=3;
    EXPECT_FALSE(raw_probe_stimulus_valid(forged,definition));
}

TEST(RawProbeEvidenceGate, BoundedCrossStreamInputCannotBeBypassedByCallback) {
    auto definition=stop_definition();definition.writes[1].evidence_ready=[](const auto&){return true;};
    ProbeTransport transport;RawProbeController controller(transport,definition);
    transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({0xaf,0,0,0}),false},
        transport::StreamDataEvent{6,std::vector<std::byte>(65547,std::byte{0}),false}};
    // Gate input beyond its bound truncates this context's evidence; it is not a harness fault.
    const auto& truncated=controller.poll(RawProbeClock::time_point{});
    EXPECT_TRUE(truncated.event_limit_reached);
    EXPECT_FALSE(truncated.harness_failed);
    EXPECT_FALSE(truncated.event_limit_reason.empty());
    EXPECT_TRUE(transport.stop_calls.empty());
    auto forged=independent_stop_transcript();
    std::get<transport::StreamDataEvent>(forged.events[2]).data.assign(65547,std::byte{0});
    EXPECT_FALSE(raw_probe_stimulus_valid(forged,definition));

    // The bound is cumulative across distinct streams and includes SETUP.
    for (bool exceeds : {false,true}) {
        ProbeTransport split;
        RawProbeController bounded(split,definition);
        split.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({0xaf,0,0,0}),false},
            transport::StreamDataEvent{6,std::vector<std::byte>(32771,std::byte{0}),false},
            transport::StreamDataEvent{10,std::vector<std::byte>(32771 + (exceeds ? 1u : 0u),std::byte{0}),false}};
        const auto& result=bounded.poll(RawProbeClock::time_point{});
        EXPECT_EQ(result.event_limit_reached,exceeds);
        EXPECT_FALSE(result.harness_failed);
        EXPECT_EQ(result.stimulus_delivered,!exceeds);
        EXPECT_EQ(split.stop_calls.size(),exceeds ? 0u : 1u);
        if (!exceeds) {
            split.events={transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
            bounded.poll(RawProbeClock::time_point{});
            EXPECT_TRUE(raw_probe_stimulus_valid(bounded.transcript(),definition));
        }
    }
}

TEST(RawProbeStaged, ControllerWithholdsReuseUntilActualFragmentedResponseAfterPriorAcceptance) {
    for (bool peer_opened : {false,true}) {
        SCOPED_TRACE(peer_opened);
        ProbeTransport transport;
        const auto definition = staged_definition(peer_opened);
        RawProbeController controller(transport,definition);
        const auto now = RawProbeClock::time_point{};
        const auto stream = peer_opened ? 0u : 1u;
        transport.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
        if (peer_opened)
            transport.events.push_back(transport::StreamDataEvent{0,b({0x1d,0,1,0}),false});
        const auto& first = controller.poll(now);
        EXPECT_FALSE(first.stimulus_delivered);
        EXPECT_EQ(transport.output[stream],definition.writes.front().bytes);
        ASSERT_TRUE(first.writes[0].delivery_event_count);
        EXPECT_FALSE(first.writes[1].delivery_event_count);
        const auto response = peer_opened ? b({2,0,2,2,0}) : b({4,0,2,0,0});
        transport.events = {transport::StreamDataEvent{stream+4,response,false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        transport.events = {transport::StreamDataEvent{stream,
            {response.begin(),response.begin()+3},false}};
        EXPECT_FALSE(controller.poll(now).stimulus_delivered);
        EXPECT_EQ(transport.output[stream],definition.writes.front().bytes);
        transport.events = {transport::StreamDataEvent{stream,
            {response.begin()+3,response.end()},false}};
        ASSERT_TRUE(controller.poll(now).stimulus_delivered);
        auto expected = definition.writes.front().bytes;
        expected.insert(expected.end(),definition.writes[1].bytes.begin(),definition.writes[1].bytes.end());
        EXPECT_EQ(transport.output[stream],expected);
        EXPECT_EQ(controller.transcript().writes[1].stream_id,stream);
        EXPECT_EQ(transport.next_bidi,peer_opened ? static_cast<std::uint64_t>(-3) : 1u);
        EXPECT_LT(*controller.transcript().writes[0].delivery_event_count,
                  *controller.transcript().writes[1].delivery_event_count);
        transport.events = {transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
        ASSERT_TRUE(controller.poll(now).complete);
        EXPECT_EQ(evaluate_raw_probe_close(controller.transcript(),definition,3),true);
    }
}

TEST(RawProbeStaged, ProofRejectsMissingEarlyWrongStreamPartialAndForgedAcceptanceEvidence) {
    for (bool peer_opened : {false,true}) {
        const auto definition = staged_definition(peer_opened);
        const auto valid = staged_transcript(peer_opened);
        EXPECT_EQ(evaluate_raw_probe_close(valid,definition,3),true);
        const auto response_index = *valid.writes[0].delivery_event_count;
        auto changed = valid;
        changed.writes[0].delivery_event_count.reset();
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        changed.writes[1].delivery_event_count.reset();
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        changed.writes[0].delivery_event_count = response_index+1;
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        changed.writes[1].delivery_event_count = response_index;
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        changed.writes[0].delivery_event_count = 1;
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        ++*changed.writes[1].delivery_event_count;
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        std::get<transport::StreamDataEvent>(changed.events[response_index]).stream_id += 4;
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        std::get<transport::StreamDataEvent>(changed.events[response_index]).data.pop_back();
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        changed.writes[1].stream_id = *changed.writes[0].stream_id+4;
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        changed.writes[1].write.reuse_write_stream.reset();
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
        changed = valid;
        --changed.writes[0].accepted;
        EXPECT_FALSE(evaluate_raw_probe_close(changed,definition,3).has_value());
    }
}

TEST(RawProbeStaged, ResetStopAndFinPreventContinuationAndCannotProveSuccess) {
    for (bool peer_opened : {false,true}) {
        for (unsigned kind : {0u,1u,2u}) {
            const auto definition = staged_definition(peer_opened);
            auto cancelled = staged_transcript(peer_opened);
            const auto response_index = *cancelled.writes[0].delivery_event_count;
            const auto stream = *cancelled.writes[0].stream_id;
            if (kind == 2)
                std::get<transport::StreamDataEvent>(cancelled.events[response_index]).fin = true;
            else {
                const transport::TransportEvent cancellation = kind == 0
                    ? transport::TransportEvent(transport::PeerResetEvent{stream,0})
                    : transport::TransportEvent(transport::PeerStopSendingEvent{stream,0});
                cancelled.events.insert(cancelled.events.begin()+response_index+1,cancellation);
                ++*cancelled.writes[1].delivery_event_count;
                ++*cancelled.delivery_event_count;
            }
            EXPECT_FALSE(evaluate_raw_probe_close(cancelled,definition,3).has_value());
            ProbeTransport transport;
            RawProbeController controller(transport,definition);
            transport.events = {cancelled.events.begin(),cancelled.events.begin()+response_index};
            const auto now = RawProbeClock::time_point{};
            EXPECT_FALSE(controller.poll(now).stimulus_delivered);
            transport.events = {cancelled.events.begin()+response_index,
                cancelled.events.begin()+*cancelled.delivery_event_count};
            EXPECT_FALSE(controller.poll(now).stimulus_delivered);
            EXPECT_EQ(transport.output[stream],definition.writes.front().bytes);
        }
    }
}
TEST(RawProbeStaged, ResponseBeforeFullPriorWriteAcceptanceCannotUnlockContinuation) {
    ProbeTransport transport;
    transport.block = true;
    const auto definition = staged_definition(false);
    RawProbeController controller(transport,definition);
    const auto now = RawProbeClock::time_point{};
    transport.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    EXPECT_FALSE(controller.transcript().writes[0].delivery_event_count);
    transport.events = {transport::StreamDataEvent{1,b({4,0,2,0,0}),false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    EXPECT_FALSE(controller.transcript().writes[0].delivery_event_count);
    transport.block = false;
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    ASSERT_EQ(controller.transcript().writes[0].delivery_event_count,3u);
    EXPECT_EQ(transport.output[1],definition.writes[0].bytes);
    transport.events = {transport::StreamDataEvent{1,b({4,0,2,0,0}),false}};
    ASSERT_TRUE(controller.poll(now).stimulus_delivered);
    transport.events = {transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    ASSERT_TRUE(controller.poll(now).complete);
    EXPECT_EQ(evaluate_raw_probe_close(controller.transcript(),definition,3),true);
}
TEST(RawProbeStaged, OversizedGateInputTruncatesEvidenceAndCannotProveSuccess) {
    const auto definition = staged_definition(false);
    auto forged = staged_transcript(false);
    std::get<transport::StreamDataEvent>(forged.events[2]).data.assign(65547,std::byte{0});
    EXPECT_FALSE(evaluate_raw_probe_close(forged,definition,3).has_value());
    ProbeTransport transport;
    RawProbeController controller(transport,definition);
    const auto now = RawProbeClock::time_point{};
    transport.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    transport.events = {forged.events[2]};
    const auto& truncated = controller.poll(now);
    EXPECT_TRUE(truncated.event_limit_reached);
    EXPECT_FALSE(truncated.harness_failed);
    EXPECT_EQ(transport.output[1],definition.writes[0].bytes);
}
TEST(RawProbeStaged, InvalidStreamReferencesAndReuseOfClosedOrStaleStateAreRejected) {
    auto invalid = staged_definition(false);
    invalid.writes[1].reuse_write_stream = 1;
    ProbeTransport transport;
    EXPECT_THROW(RawProbeController(transport,invalid),std::invalid_argument);
    invalid = staged_definition(false);
    invalid.writes[1].reuse_write_stream.reset();
    EXPECT_THROW(RawProbeController(transport,invalid),std::invalid_argument);
    invalid = staged_definition(false);
    invalid.writes[0].fin = true;
    EXPECT_THROW(RawProbeController(transport,invalid),std::invalid_argument);
    invalid = staged_definition(false);
    invalid.writes.push_back(invalid.writes[1]);
    EXPECT_THROW(RawProbeController(transport,invalid),std::invalid_argument);
    invalid.writes[2].reuse_write_stream = 1;
    EXPECT_NO_THROW(RawProbeController(transport,invalid));
}
TEST(RawProbe, RespondsOnlyToCompleteMatchingPeerOpenedRequest) {
    ProbeTransport transport;
    const auto definition = reactive_definition();
    RawProbeController controller(transport, definition);
    const auto now = RawProbeClock::time_point{};
    transport.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false},
        transport::StreamDataEvent{4, b({3, 0, 1, 0}), false},
        transport::StreamDataEvent{0, b({0x1d, 0, 1}), false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    EXPECT_TRUE(transport.output[0].empty());
    transport.events = {transport::StreamDataEvent{0, b({0}), false}};
    ASSERT_TRUE(controller.poll(now).stimulus_delivered);
    EXPECT_EQ(transport.output[0], definition.writes.front().bytes);
    EXPECT_EQ(transport.next_bidi, static_cast<std::uint64_t>(-3));
    transport.events = {transport::PeerCloseEvent{transport::CloseErrorSpace::Application, 3, {}}};
    ASSERT_TRUE(controller.poll(now).complete);
    const auto observed = controller.transcript();
    EXPECT_EQ(evaluate_raw_probe_close(observed, definition, 3), true);
    auto forged = observed;
    forged.writes.front().stream_id = 4;
    EXPECT_FALSE(evaluate_raw_probe_close(forged, definition, 3).has_value());
    forged = observed;
    std::get<transport::StreamDataEvent>(forged.events[3]).data = b({3, 0, 1});
    EXPECT_FALSE(evaluate_raw_probe_close(forged, definition, 3).has_value());
    forged = observed;
    forged.delivery_event_count = 4;
    EXPECT_FALSE(evaluate_raw_probe_close(forged, definition, 3).has_value());
    forged = observed;
    std::swap(forged.events[0], forged.events[1]);
    EXPECT_FALSE(evaluate_raw_probe_close(forged, definition, 3).has_value());
    forged = observed;
    std::swap(forged.events[0], forged.events[3]);
    EXPECT_FALSE(evaluate_raw_probe_close(forged, definition, 3).has_value());
    forged = observed;
    forged.events.insert(forged.events.begin() + 5, transport::PeerResetEvent{0, 0});
    forged.delivery_event_count = 6;
    EXPECT_FALSE(evaluate_raw_probe_close(forged, definition, 3).has_value());
    forged = observed;
    forged.events.insert(forged.events.end(), 4097, transport::PeerStopSendingEvent{8, 0});
    EXPECT_FALSE(evaluate_raw_probe_close(forged, definition, 3).has_value());
}
TEST(RawProbe, QueuedResetPreventsResponseAndForgedProof) {
    for (const bool stop : {false, true}) {
        ProbeTransport transport;
        const auto definition = reactive_definition();
        RawProbeController controller(transport, definition);
        transport.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false},
            transport::StreamDataEvent{0, b({0x1d, 0, 1, 0}), false}};
        if (stop) transport.events.push_back(transport::PeerStopSendingEvent{0, 0});
        else transport.events.push_back(transport::PeerResetEvent{0, 0});
        EXPECT_FALSE(controller.poll(RawProbeClock::time_point{}).stimulus_delivered);
        EXPECT_TRUE(transport.output[0].empty());
    }
}
TEST(RawProbe, SetupBeforeEstablishmentCannotUnlockPeerResponse) {
    ProbeTransport transport;
    RawProbeController controller(transport, reactive_definition());
    transport.events = {transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false},
        transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{0, b({0x1d, 0, 1, 0}), false}};
    const auto& result = controller.poll(RawProbeClock::time_point{});
    EXPECT_FALSE(result.stimulus_delivered);
    EXPECT_TRUE(result.harness_failed);
    EXPECT_TRUE(transport.output[0].empty());
}
TEST(RawProbe, PeerRequestBufferLimitTruncatesEvidenceWithoutHarnessFailure) {
    ProbeTransport transport;
    RawProbeController controller(transport, reactive_definition());
    transport.events = {transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2, b({0xaf, 0, 0, 0}), false},
        transport::StreamDataEvent{0, std::vector<std::byte>(65547), false}};
    const auto& result = controller.poll(RawProbeClock::time_point{});
    EXPECT_TRUE(result.event_limit_reached);
    EXPECT_FALSE(result.harness_failed);
    EXPECT_NE(result.event_limit_reason.find("unscored"), std::string::npos);
    EXPECT_TRUE(transport.output[0].empty());
}
TEST(RawProbe, WaitsForCompletePeerSetupAndRecordsAcceptedBytes) {
    ProbeTransport transport;
    RawProbeController controller(transport, draft18_close_probe("receive-request-id-wrong-peer-parity", std::chrono::milliseconds(10)));
    auto now = RawProbeClock::time_point{};
    transport.events = {transport::ConnectionEstablishedEvent{}, transport::StreamDataEvent{2,b({0xaf,0,0}),false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    EXPECT_TRUE(transport.output[1].empty());
    transport.events = {transport::StreamDataEvent{2,b({0}),false}};
    const auto& sent = controller.poll(now);
    ASSERT_TRUE(sent.stimulus_delivered);
    ASSERT_EQ(sent.writes.size(), 1u);
    EXPECT_EQ(transport.output[1], b({3,0,7,0,1,1,'n',1,'x',0}));
    EXPECT_EQ(sent.writes[0].accepted, 10u);
    transport.events = {transport::PeerCloseEvent{transport::CloseErrorSpace::Application,4,{}}};
    EXPECT_TRUE(controller.poll(now).complete);
    EXPECT_FALSE(controller.transcript().harness_failed);
}
TEST(RawProbe, DataStreamBeforeFragmentedSetupDoesNotPinSetupToFirstUni) {
    ProbeTransport transport;
    const auto definition=draft18_close_probe("receive-forward-outside-zero-one",std::chrono::milliseconds(10));
    RawProbeController controller(transport,definition);
    const auto now=RawProbeClock::time_point{};
    transport.events={transport::ConnectionEstablishedEvent{},
        transport::StreamDataEvent{2,b({5,0}),false},
        transport::StreamDataEvent{6,b({0xaf,0,0}),false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    transport.events={transport::StreamDataEvent{6,b({0}),false}};
    ASSERT_TRUE(controller.poll(now).stimulus_delivered);
    transport.events={transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    EXPECT_TRUE(controller.poll(now).complete);
    EXPECT_EQ(evaluate_raw_probe_close(controller.transcript(),definition,3),true);
}
TEST(RawProbe, PartialStimulusAndPeerCloseNeverClaimsDelivery) {
    ProbeTransport transport; transport.block = true;
    RawProbeController controller(transport, draft18_close_probe("receive-forward-outside-zero-one", std::chrono::milliseconds(10)));
    const auto now = RawProbeClock::time_point{};
    transport.events = {transport::ConnectionEstablishedEvent{}, transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
    EXPECT_FALSE(controller.poll(now).stimulus_delivered);
    transport.events = {transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    EXPECT_FALSE(controller.poll(now).complete);
    EXPECT_FALSE(controller.transcript().harness_failed);
}
TEST(RawProbe, AlreadyQueuedPeerClosePreventsAnyStimulusWrite) {
    for (bool after_setup : {false, true}) {
        ProbeTransport transport;
        const auto definition = draft18_close_probe(
            after_setup ? "receive-forward-outside-zero-one" : "receive-server-setup-with-path",
            std::chrono::milliseconds(10));
        RawProbeController controller(transport, definition);
        transport.events = {transport::ConnectionEstablishedEvent{}};
        if (after_setup)
            transport.events.push_back(transport::StreamDataEvent{2,b({0xaf,0,0,0}),false});
        transport.events.push_back(transport::PeerCloseEvent{transport::CloseErrorSpace::Application,8,{}});
        const auto& result = controller.poll(RawProbeClock::time_point{});
        EXPECT_FALSE(result.stimulus_delivered);
        EXPECT_FALSE(result.complete);
        EXPECT_TRUE(transport.output.empty());
    }
}
TEST(RawProbe, TimeoutAndEventLossDoNotCompleteProbe) {
    for (bool overflow : {false,true}) {
        ProbeTransport transport;
        RawProbeController controller(transport, draft18_close_probe("receive-forward-outside-zero-one", std::chrono::milliseconds(10)));
        const auto now = RawProbeClock::time_point{};
        transport.events = {transport::ConnectionEstablishedEvent{}, transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
        ASSERT_TRUE(controller.poll(now).stimulus_delivered);
        if (overflow) transport.events = {transport::EventQueueOverflowEvent{}};
        const auto& result = controller.poll(now + std::chrono::milliseconds(11));
        EXPECT_FALSE(result.complete);
        EXPECT_TRUE(result.harness_failed || result.timed_out);
    }
}
TEST(RawProbe, DatagramUsesAtomicTransportAcceptanceAndNegotiatedCapacity) {
    for (unsigned mode=0;mode<5;++mode) {
        ProbeTransport transport;
        if (mode==2) transport.datagram_status=transport::TransportStatus::Partial;
        if (mode==4) transport.datagram_status=transport::TransportStatus::InvalidState;
        RawProbeDefinition definition{"datagram-test",b({0xaf,0,0,0}),
            {{RawProbeChannel::Datagram,mode==3 ? b({0x22,0}) : b({0x22}),false}},true,
            [](std::span<const std::byte> input) { return input.size()==4; },std::chrono::milliseconds(10)};
        RawProbeController controller(transport,definition);
        transport.events={transport::ConnectionEstablishedEvent{{},{},{},mode==1 ? 0u : mode==3 ? 1u : 1200u},
            transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
        const auto& delivered=controller.poll(RawProbeClock::time_point{});
        if (mode==0) {
            ASSERT_EQ(transport.datagrams.size(),1u);
            EXPECT_EQ(transport.datagrams.front(),b({0x22}));
            EXPECT_TRUE(delivered.stimulus_delivered);
            ASSERT_EQ(delivered.writes.size(),1u);
            EXPECT_FALSE(delivered.writes.front().stream_id);
            transport.events={transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
            EXPECT_TRUE(controller.poll(RawProbeClock::time_point{}).complete);
            EXPECT_EQ(evaluate_raw_probe_close(controller.transcript(),definition,3),true);
            auto forged=controller.transcript();
            std::get<transport::ConnectionEstablishedEvent>(forged.events.front()).max_datagram_payload=0;
            EXPECT_FALSE(evaluate_raw_probe_close(forged,definition,3).has_value());
        } else {
            EXPECT_FALSE(delivered.stimulus_delivered);
            EXPECT_FALSE(evaluate_raw_probe_close(delivered,definition,3).has_value());
        }
    }
}
TEST(RawProbe, DuplicateHandshakeEvidenceCannotProveDatagramCapability) {
    ProbeTransport transport;
    RawProbeDefinition definition{"datagram-test",b({0xaf,0,0,0}),
        {{RawProbeChannel::Datagram,b({0x22}),false}},true,
        [](std::span<const std::byte> input) { return input.size()==4; },std::chrono::milliseconds(10)};
    RawProbeController controller(transport,definition);
    transport.events={transport::ConnectionEstablishedEvent{{},{},{},1200},
        transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
    ASSERT_TRUE(controller.poll(RawProbeClock::time_point{}).stimulus_delivered);
    transport.events={transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    ASSERT_TRUE(controller.poll(RawProbeClock::time_point{}).complete);
    auto forged=controller.transcript();
    forged.events.push_back(transport::ConnectionEstablishedEvent{{},{},{},1200});
    EXPECT_FALSE(evaluate_raw_probe_close(forged,definition,3).has_value());
}
TEST(Draft18CloseProfiles, ParameterTypeDeltaOverflowHasValidEarlierParameter) {
    const auto profiles=draft18_close_profiles();
    ASSERT_TRUE(std::any_of(profiles.begin(),profiles.end(),[](const auto& profile) {
        return profile.scenario_id=="receive-message-parameter-type-delta-overflow";
    }));
    const auto definition=draft18_close_probe("receive-message-parameter-type-delta-overflow",std::chrono::milliseconds(10));
    auto expected=b({3,0,18,1,1,1,'n',1,'x',2,2,0});
    expected.insert(expected.end(),9,std::byte{0xff});
    ASSERT_EQ(definition.writes.size(),1u);
    EXPECT_EQ(definition.writes.front().bytes,expected);
}
TEST(Draft18CloseProfiles, TokenCacheCloseProbesUseRegisteredGreaseTokensAndAdvertisedCapacity) {
    const std::array<std::string_view,2> ids{
        "register-same-peer-token-alias-twice-without-delete",
        "register-request-token-exceeding-advertised-cache-size"};
    for (const auto id : ids) {
        SCOPED_TRACE(std::string(id));
        const bool duplicate = id == ids.front();
        const auto profiles = draft18_close_profiles();
        const auto profile = std::find_if(profiles.begin(),profiles.end(),[&](const auto& item) {
            return item.scenario_id == id;
        });
        EXPECT_NE(profile,profiles.end());
        if (profile == profiles.end()) continue;
        EXPECT_EQ(profile->requirement_id,duplicate ? "D18-10-2-2-MUST-006" : "D18-10-2-2-MUST-011");
        EXPECT_EQ(profile->evaluator_id,duplicate ? "session-closed-duplicate-auth-token-alias" : "session-closed-auth-token-cache-overflow");
        EXPECT_EQ(profile->expected_close,duplicate ? 0x14u : 0x13u);
        const auto definition = draft18_close_probe(id,std::chrono::milliseconds(10));
        ASSERT_EQ(definition.writes.size(),duplicate ? 2u : 1u);
        EXPECT_EQ(definition.writes[0].bytes,b({3,0,14,1,1,1,'n',1,'x',1,3,5,1,7,0x80,0x9d,'x'}));
        if (duplicate)
            EXPECT_EQ(definition.writes[1].bytes,b({3,0,14,3,1,1,'n',1,'x',1,3,5,1,7,0x80,0x9d,'x'}));
        EXPECT_EQ(definition.peer_setup_ready(b({0xaf,0,0,0})),!duplicate);
        EXPECT_EQ(definition.peer_setup_ready(b({0xaf,0,0,2,4,0})),!duplicate);
        EXPECT_EQ(definition.peer_setup_ready(b({0xaf,0,0,2,4,16})),!duplicate);
        EXPECT_FALSE(definition.peer_setup_ready(b({0xaf,0,0,2,4,17})));
        EXPECT_FALSE(definition.peer_setup_ready(b({0xaf,0,0,2,4,33})));
        EXPECT_EQ(definition.peer_setup_ready(b({0xaf,0,0,2,4,34})),duplicate);
        EXPECT_EQ(definition.peer_setup_ready(b({0xaf,0,0,2,4,35})),duplicate);
        EXPECT_FALSE(definition.peer_setup_ready(b({0xaf,0,0,2,4})));
        for (const bool permitted : {false,true}) {
            ProbeTransport transport;
            RawProbeController controller(transport,definition);
            const auto setup = duplicate == permitted ? b({0xaf,0,0,2,4,34}) : b({0xaf,0,0,0});
            transport.events = {transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,setup,false}};
            EXPECT_EQ(controller.poll(RawProbeClock::time_point{}).stimulus_delivered,permitted);
            transport.events = {transport::PeerCloseEvent{transport::CloseErrorSpace::Application,
                duplicate ? 0x14u : 0x13u,{}}};
            const auto& observed = controller.poll(RawProbeClock::time_point{});
            if (permitted) EXPECT_EQ(evaluate_raw_probe_close(observed,definition,profile->expected_close),true);
            else EXPECT_FALSE(evaluate_raw_probe_close(observed,definition,profile->expected_close).has_value());
        }
    }
}
TEST(Draft18CloseProfiles, DiscoveryPrefixOverflowHasCompleteRequestBodies) {
    for (const auto& [scenario, type] : std::map<std::string, unsigned>{
             {"receive-subscribe-namespace-with-33-prefix-fields",0x50},
             {"receive-subscribe-tracks-with-33-prefix-fields",0x51}}) {
        SCOPED_TRACE(scenario);
        const auto profiles = draft18_close_profiles();
        const auto profile = std::find_if(profiles.begin(),profiles.end(),[&](const auto& item) {
            return item.scenario_id == scenario;
        });
        ASSERT_NE(profile,profiles.end());
        EXPECT_EQ(profile->expected_close,3u);
        auto expected = b({type,0,69,1,33});
        for (unsigned i=0;i<33;++i) {
            expected.push_back(std::byte{1});
            expected.push_back(std::byte{'n'});
        }
        expected.push_back(std::byte{0});
        const auto definition = draft18_close_probe(scenario,std::chrono::milliseconds(10));
        ASSERT_EQ(definition.writes.size(),1u);
        EXPECT_EQ(definition.writes.front().channel,RawProbeChannel::NewBidi);
        EXPECT_EQ(definition.writes.front().bytes,expected);
        ProbeTransport transport;
        RawProbeController controller(transport,definition);
        transport.events = {transport::ConnectionEstablishedEvent{},
            transport::StreamDataEvent{2,b({0xaf,0,0,0}),false}};
        ASSERT_TRUE(controller.poll(RawProbeClock::time_point{}).stimulus_delivered);
        transport.events = {transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
        ASSERT_TRUE(controller.poll(RawProbeClock::time_point{}).complete);
        EXPECT_EQ(evaluate_raw_probe_close(controller.transcript(),definition,3),true);
    }
}
TEST(Draft18CloseProfiles, AbsoluteRangeEndGroupOverflowsWithIndividuallyValidIntegers) {
    const auto profiles = draft18_close_profiles();
    ASSERT_TRUE(std::any_of(profiles.begin(),profiles.end(),[](const auto& profile) {
        return profile.requirement_id == "D18-5-1-2-MUST-001" &&
            profile.scenario_id == "absolute-range-end-group-overflow" && profile.expected_close == 3u;
    }));
    const auto definition = draft18_close_probe("absolute-range-end-group-overflow",std::chrono::milliseconds(10));
    ASSERT_EQ(definition.writes.size(),1u);
    EXPECT_EQ(definition.writes.front().channel,RawProbeChannel::NewBidi);
    EXPECT_EQ(definition.writes.front().bytes,
        b({3,0,21,1,1,1,'n',1,'x',1,0x21,12,4,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,0,1}));
}
TEST(Draft18CloseProfiles, DefinesApplicableSubgroupAndDatagramHeaderViolations) {
    const std::map<std::string,std::vector<std::byte>> expected{
        {"receive-subgroup-types-with-subgroup-id-mode-three",b({0x16})},
        {"receive-invalid-subgroup-header-type-0x80",b({0x80,0x80})},
        {"receive-unknown-datagram-type",b({0x40})},
        {"receive-invalid-object-datagram-type-0x10",b({0x10})},
        {"receive-datagram-types-0x22-0x23-0x26-0x27-0x2a-0x2b-0x2e-0x2f",b({0x22})}};
    for (const auto& [id,bytes] : expected) {
        SCOPED_TRACE(id);
        const auto profiles=draft18_close_profiles();
        ASSERT_TRUE(std::any_of(profiles.begin(),profiles.end(),[&](const auto& profile) { return profile.scenario_id==id; }));
        const auto definition=draft18_close_probe(id,std::chrono::milliseconds(10));
        ASSERT_FALSE(definition.writes.empty());
        EXPECT_EQ(definition.writes.front().bytes,bytes);
        EXPECT_EQ(definition.writes.front().channel,id.find("datagram")!=std::string::npos ? RawProbeChannel::Datagram : RawProbeChannel::NewUni);
        if (id.find("0x22-")!=std::string::npos) {
            ASSERT_EQ(definition.writes.size(),8u);
            const std::array<unsigned,8> types{0x22,0x23,0x26,0x27,0x2a,0x2b,0x2e,0x2f};
            for (std::size_t i=0;i<types.size();++i) EXPECT_EQ(definition.writes[i].bytes,b({types[i]}));
        }
    }
}
TEST(Draft18CloseProfiles, EveryCloseProfileIsRegisteredForHttpAndRunSelection) {
    for (const auto& profile : draft18_close_profiles())
        EXPECT_TRUE(app::executable_scenario(18,profile.scenario_id)) << profile.scenario_id;
}
TEST(Draft18CloseProfiles, BoundaryAndMultiStreamFixturesUseIndependentBytes) {
    auto check = [](std::string_view id, std::vector<std::byte> expected, bool setup = false) {
        SCOPED_TRACE(std::string(id));
        const auto definition = draft18_close_probe(id,std::chrono::milliseconds(10));
        EXPECT_EQ(setup ? definition.setup_bytes : definition.writes.at(0).bytes,expected);
    };
    check("receive-understood-key-value-invalid-serialization",b({0xaf,0,0,3,3,1,4}),true);
    check("receive-key-value-length-over-65535",b({0xaf,0,0,4,9,0xc1,0,0}),true);
    auto overflow = b({0xaf,0,0,12}); overflow.insert(overflow.end(),9,std::byte{0xff});
    auto tail=b({0,1,0}); overflow.insert(overflow.end(),tail.begin(),tail.end());
    check("receive-key-value-type-overflow",overflow,true);
    auto fields=b({3,0,71,1,33});
    for (unsigned i=0;i<33;++i) { fields.push_back(std::byte{1}); fields.push_back(std::byte{'n'}); }
    tail=b({1,'x',0}); fields.insert(fields.end(),tail.begin(),tail.end());
    check("receive-namespace-with-33-fields",fields);
    auto name_space=b({3,0x10,8,1,1,0x90,1}); name_space.insert(name_space.end(),4097,std::byte{'n'});
    name_space.insert(name_space.end(),tail.begin(),tail.end());
    check("receive-track-namespace-over-4096-bytes",name_space);
    auto name=b({3,0x10,7,1,1,1,'n',0x90,0}); name.insert(name.end(),4096,std::byte{'x'}); name.push_back(std::byte{0});
    check("receive-full-track-name-over-4096-bytes",name);
    auto uri=b({0x10,0x20,5,0xa0,1}); uri.insert(uri.end(),8193,std::byte{'x'}); uri.push_back(std::byte{0}); uri.push_back(std::byte{0});
    check("receive-goaway-uri-length-8193",uri);
    check("receive-two-goaways-on-control-stream",b({0x10,0,3,0,0,0,0x10,0,3,0,0,0}));
    check("receive-unknown-message-type",b({0x3f,0,0}));
    const auto duplicate=draft18_close_probe("receive-duplicate-request-id-across-request-streams",std::chrono::milliseconds(10));
    ASSERT_EQ(duplicate.writes.size(),2u);
    EXPECT_EQ(duplicate.writes[0].bytes,b({3,0,7,1,1,1,'n',1,'x',0}));
    EXPECT_EQ(duplicate.writes[1].bytes,duplicate.writes[0].bytes);
}
TEST(Draft18CloseProfiles, DuplicateRequestIdProbeNamesTheConfiguredTrack) {
    // The first SUBSCRIBE must be acceptable for the repeated Request ID to be the
    // only reason to close; an unknown track lets a publisher abort first.
    const auto probe = draft18_close_probe("receive-duplicate-request-id-across-request-streams",
        std::chrono::milliseconds(10),{b({'m','e','d','i','a'})},b({'v','i','d','e','_','1'}));
    ASSERT_EQ(probe.writes.size(),2u);
    EXPECT_EQ(probe.writes[0].bytes,b({3,0,16,1,1,5,'m','e','d','i','a',6,'v','i','d','e','_','1',0}));
    EXPECT_EQ(probe.writes[1].bytes,probe.writes[0].bytes);
    EXPECT_NE(probe.writes[0].channel,RawProbeChannel::Control);
    // Other scenarios keep their fixed bytes whatever track is supplied.
    const auto parity = draft18_close_probe("receive-request-id-wrong-peer-parity",
        std::chrono::milliseconds(10),{b({'m','e','d','i','a'})},b({'v','i','d','e','_','1'}));
    EXPECT_EQ(parity.writes.at(0).bytes,b({3,0,7,0,1,1,'n',1,'x',0}));
}
TEST(Draft18CloseProfiles, IndependentLiteralStimulusTranscripts) {
    const std::map<std::string, std::vector<std::byte>> fixtures{
        {"receive-unknown-unidirectional-stream-type",b({0})},
        {"receive-disallowed-initial-bidirectional-message",b({7,0,1,0})},
        {"receive-known-message-with-mismatched-payload-length",b({3,0,8,1,1,1,'n',1,'x',0,0})},
        {"receive-zero-length-namespace-field",b({3,0,6,1,1,0,1,'x',0})},
        {"receive-request-id-wrong-peer-parity",b({3,0,7,0,1,1,'n',1,'x',0})},
        {"receive-unnegotiated-unknown-message-parameter",b({3,0,9,1,1,1,'n',1,'x',1,0x3f,0})},
        {"receive-duplicate-nonrepeatable-message-parameter",b({3,0,11,1,1,1,'n',1,'x',2,0x10,1,0,1})},
        {"receive-message-parameter-on-disallowed-message-native-quic",b({3,0,9,1,1,1,'n',1,'x',1,8,0})},
        {"receive-undecodable-authorization-token-structure",b({3,0,9,1,1,1,'n',1,'x',1,3,0})},
        {"receive-group-order-zero-or-greater-than-two",b({3,0,9,1,1,1,'n',1,'x',1,0x22,0})},
        {"receive-forward-outside-zero-one",b({3,0,9,1,1,1,'n',1,'x',1,0x10,2})},
        {"receive-undefined-subscription-filter-type",b({3,0,10,1,1,1,'n',1,'x',1,0x21,1,0})},
        {"receive-fetch-with-unknown-type",b({0x16,0,3,1,0,0})},
        {"receive-control-goaway-with-wrong-receiver-request-id-parity",b({0x10,0,3,0,0,1})},
    };
    for (const auto& [scenario, bytes] : fixtures) {
        SCOPED_TRACE(scenario);
        const auto probe = draft18_close_probe(scenario, std::chrono::milliseconds(10));
        ASSERT_EQ(probe.writes.size(), 1u);
        EXPECT_EQ(probe.writes.front().bytes, bytes);
    }
    const auto authority = draft18_close_probe("receive-server-setup-with-authority", std::chrono::milliseconds(10));
    EXPECT_EQ(authority.setup_bytes,b({0xaf,0,0,3,5,1,'a'}));
    EXPECT_FALSE(authority.start_after_peer_setup);
    const auto path = draft18_close_probe("receive-server-setup-with-path", std::chrono::milliseconds(10));
    EXPECT_EQ(path.setup_bytes,b({0xaf,0,0,3,1,1,'/'}));
}
TEST(RawProbePrepared, WaitsWithoutOpeningAndFreezesActualSetupPrefix) {
    ProbeTransport transport;
    RawProbeDefinition definition{"prepared",b({1}),{{RawProbeChannel::NewBidi,{},true}},true,
        [](auto bytes){return bytes.size()==2;},std::chrono::milliseconds(10)};
    unsigned calls=0;
    definition.writes[0].prepare_bytes=[&](const RawProbeGateInput& input)->std::optional<std::vector<std::byte>> {
        ++calls;
        for (const auto& event:input.events)
            if (const auto* data=std::get_if<transport::StreamDataEvent>(&event);data && data->stream_id==6)
                return data->data;
        return std::nullopt;
    };
    RawProbeController controller(transport,definition);
    const auto now=RawProbeClock::now();
    transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({1}),false}};
    controller.poll(now);
    EXPECT_EQ(calls,0u);
    transport.events={transport::StreamDataEvent{2,b({2}),false}};
    controller.poll(now);
    EXPECT_EQ(calls,1u);
    EXPECT_EQ(transport.next_bidi,static_cast<std::uint64_t>(-3));
    EXPECT_EQ(transport.output.count(1),0u);
    transport.block=true;
    transport.events={transport::StreamDataEvent{6,b({7,8}),false}};
    controller.poll(now);
    EXPECT_EQ(calls,2u);
    EXPECT_EQ(controller.transcript().writes[0].prepared_event_count,4u);
    transport.events={transport::StreamDataEvent{6,b({9,10}),false}};
    transport.block=false;
    controller.poll(now);
    EXPECT_EQ(calls,2u);
    EXPECT_EQ(transport.output[1],b({7,8}));
    transport.events={transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    auto proof=controller.poll(now);
    ASSERT_TRUE(raw_probe_stimulus_valid(proof,definition));
    for (unsigned mutation=0;mutation<7;++mutation) {
        auto altered=proof;
        switch(mutation) {
            case 0: altered.writes[0].prepared_event_count.reset();break;
            case 1: altered.writes[0].prepared_event_count=2;break;
            case 2: altered.writes[0].prepared_event_count=6;break;
            case 3: altered.writes[0].write.bytes=b({9,10});break;
            case 4: altered.writes[0].write.prepare_bytes={};break;
            case 5: altered.setup.prepared_event_count=1;break;
            case 6: altered.events.push_back(transport::StreamDataEvent{6,b({3}),false});break;
        }
        EXPECT_FALSE(raw_probe_stimulus_valid(altered,definition))<<mutation;
    }
}
TEST(RawProbePrepared, RejectsInvalidDefinitionsAndEmptyOrOversizePayloads) {
    ProbeTransport transport;
    RawProbeDefinition definition{"prepared",b({1}),{{RawProbeChannel::NewBidi,{},false}},true,
        [](auto bytes){return bytes.size()==1;},std::chrono::milliseconds(10)};
    definition.writes[0].prepare_bytes=[](const auto&)->std::optional<std::vector<std::byte>> {return b({1});};
    auto invalid=definition;invalid.writes[0].bytes=b({2});
    EXPECT_THROW(RawProbeController(transport,invalid),std::invalid_argument);
    invalid=definition;invalid.writes[0].operation=RawProbeOperation::StopSending;
    EXPECT_THROW(RawProbeController(transport,invalid),std::invalid_argument);
    for (const auto size:{0u,65547u}) {
        definition.writes[0].prepare_bytes=[size](const auto&)->std::optional<std::vector<std::byte>> {return std::vector<std::byte>(size);};
        RawProbeController controller(transport,definition);
        transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({1}),false}};
        EXPECT_TRUE(controller.poll(RawProbeClock::now()).harness_failed);
        EXPECT_EQ(transport.output.count(1),0u);
    }
}
TEST(RawProbePrepared, BoundsEvidenceAndRejectsStaticPreparationMetadata) {
    ProbeTransport transport;
    RawProbeDefinition definition{"prepared",b({1}),{{RawProbeChannel::NewBidi,{},false}},true,
        [](auto bytes){return bytes.size()==1;},std::chrono::milliseconds(10)};
    unsigned calls=0;
    definition.writes[0].prepare_bytes=[&](const auto&)->std::optional<std::vector<std::byte>> {++calls;return b({1});};
    RawProbeController controller(transport,definition);
    transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({1}),false},
        transport::DatagramEvent{std::vector<std::byte>(65546)}};
    const auto& truncated=controller.poll(RawProbeClock::now());
    EXPECT_TRUE(truncated.event_limit_reached);
    EXPECT_FALSE(truncated.harness_failed);
    EXPECT_EQ(calls,0u);
    EXPECT_EQ(transport.output.count(1),0u);
    auto static_definition=staged_definition(false);
    auto static_proof=staged_transcript(false);
    ASSERT_TRUE(raw_probe_stimulus_valid(static_proof,static_definition));
    static_proof.writes[0].prepared_event_count=2;
    EXPECT_FALSE(raw_probe_stimulus_valid(static_proof,static_definition));
    static_proof=staged_transcript(false);
    static_proof.writes[0].write.prepare_bytes=definition.writes[0].prepare_bytes;
    EXPECT_FALSE(raw_probe_stimulus_valid(static_proof,static_definition));
}
TEST(RawProbePrepared, CannotPrepareBeforePriorWriteAcceptance) {
    ProbeTransport transport;
    auto definition=staged_definition(false);
    definition.writes[1].bytes.clear();
    definition.writes[1].prepare_bytes=[](const auto&)->std::optional<std::vector<std::byte>> {return b({8});};
    const auto now=RawProbeClock::now();
    RawProbeController controller(transport,definition);
    transport.events={transport::ConnectionEstablishedEvent{},transport::StreamDataEvent{2,b({1,2,3,4}),false}};
    controller.poll(now);
    transport.events={transport::StreamDataEvent{1,b({4,0,2,0,0}),false}};
    controller.poll(now);
    transport.events={transport::PeerCloseEvent{transport::CloseErrorSpace::Application,3,{}}};
    auto proof=controller.poll(now);
    ASSERT_TRUE(raw_probe_stimulus_valid(proof,definition));
    proof.writes[1].prepared_event_count=1;
    EXPECT_FALSE(raw_probe_stimulus_valid(proof,definition));
}
TEST(RawProbePrepared, RejectsInvalidGatePrefixBeforeInvokingPreparation) {
    ProbeTransport transport;
    auto definition=staged_definition(false);
    auto proof=staged_transcript(false);
    definition.writes[1].bytes.clear();
    unsigned calls=0;
    definition.writes[1].prepare_bytes=[&](const RawProbeGateInput& input)->std::optional<std::vector<std::byte>> {
        ++calls;
        if (input.events.size()<3) throw std::logic_error("missing prerequisite response");
        return b({2,0,2,3,0});
    };
    proof.setup.delivery_event_count=2;
    proof.writes[1].write.prepare_bytes=definition.writes[1].prepare_bytes;
    proof.writes[1].prepared_event_count=2;
    bool valid=true;
    EXPECT_NO_THROW(valid=raw_probe_stimulus_valid(proof,definition));
    EXPECT_FALSE(valid);
    EXPECT_EQ(calls,0u);
    proof.writes[1].prepared_event_count=3;
    ASSERT_TRUE(raw_probe_stimulus_valid(proof,definition));
    EXPECT_EQ(calls,1u);
    const auto original=proof;
    for (unsigned mutation=0;mutation<4;++mutation) {
        proof=original;
        switch(mutation) {
            case 0:proof.setup.delivery_event_count=0;break;
            case 1:proof.writes[1].stream_id.reset();break;
            case 2:proof.writes[1].write.channel=RawProbeChannel::Datagram;break;
            case 3:
                proof.events.insert(proof.events.begin(),transport::DatagramEvent{b({1})});
                proof.setup.delivery_event_count=1;
                proof.writes[0].delivery_event_count=3;
                proof.writes[1].delivery_event_count=4;
                proof.writes[1].prepared_event_count=4;
                proof.delivery_event_count=4;
                break;
        }
        bool result=true;
        EXPECT_NO_THROW(result=raw_probe_stimulus_valid(proof,definition));
        EXPECT_FALSE(result)<<mutation;
        EXPECT_EQ(calls,1u)<<mutation;
    }
}
} // namespace
} // namespace moq::interop::scenarios
