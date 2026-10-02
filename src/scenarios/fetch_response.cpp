#include "moq/interop/scenarios/fetch_response.h"
#include "moq/interop/wire/draft21/publish_done.h"
#include "moq/interop/wire/draft21/request_error.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"
#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>
namespace moq::interop::scenarios {
namespace {
namespace d21 = wire::draft21;
using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;
constexpr std::size_t kMaximumFrame = 65546;
constexpr std::size_t kMaximumEvents = 4096;
constexpr std::uint64_t kRequestId = 1;
Bytes bytes(std::initializer_list<unsigned> values) {
    Bytes result;
    for (const auto value : values)
        result.push_back(static_cast<std::byte>(value));
    return result;
}
struct Fixture {
    Namespace track_namespace;
    Bytes track_name;
};
bool valid_fixture(const Fixture& fixture) {
    if (fixture.track_namespace.size() > 32)
        return false;
    std::size_t total = fixture.track_name.size();
    if (total > 4096)
        return false;
    for (const auto& field : fixture.track_namespace) {
        if (field.empty() || field.size() > 4096 - total)
            return false;
        total += field.size();
    }
    if (fixture.track_namespace.empty())
        return true;
    return fixture.track_namespace.front() != bytes({'.'}) &&
           (!fixture.track_name.empty() ||
            fixture.track_namespace.front() != bytes({'.', 's', 'e', 's', 's', 'i', 'o', 'n'}));
}
Bytes encode_fetch(const Fixture& fixture) {
    if (!valid_fixture(fixture))
        throw std::invalid_argument("invalid FETCH track fixture");
    wire::ByteWriter output(kMaximumFrame);

    wire::ByteWriter body(65535);
    bool success = wire::write_vi64(kRequestId, body) &&
                   wire::write_vi64(fixture.track_namespace.size(), body);
    for (const auto& field : fixture.track_namespace)
        success = success && wire::write_length_prefixed_bytes(field, body);
    success = success && wire::write_length_prefixed_bytes(fixture.track_name, body);
    // Section9.20.10: explicit absolute start0/0, inclusive complete final
    // group. Three vi64 fields distinguish this from Next Object.
    wire::ByteWriter filter(27);
    success = success && wire::write_vi64(0, filter) && wire::write_vi64(0, filter) &&
              wire::write_vi64(std::numeric_limits<std::uint64_t>::max(), filter) &&
              wire::write_vi64(1, body) && wire::write_vi64(0x21, body) &&
              wire::write_length_prefixed_bytes(filter.bytes(), body) &&
              wire::write_vi64(0x16, output) &&
              output.append_byte(static_cast<std::byte>(body.size() >> 8u)) &&
              output.append_byte(static_cast<std::byte>(body.size() & 255u)) &&
              output.append_bytes(body.bytes());
    if (!success)
        throw std::invalid_argument("unencodable draft21 FETCH fixture");
    return {output.bytes().begin(), output.bytes().end()};
}
std::optional<Fixture> decode_fixture(std::span<const std::byte> input) {
    if (input.size() > kMaximumFrame)
        return std::nullopt;
    Fixture fixture;
    wire::Cursor cursor(input);

    const auto decoded = d21::decode_request_frame(cursor, true);
    const auto* frame = std::get_if<d21::RequestFrame>(&decoded);
    if (!frame || frame->type.type != 0x16 || cursor.remaining() != 0)
        return std::nullopt;
    wire::Cursor body(frame->body);
    const auto id = wire::read_vi64(body);
    const auto* request = std::get_if<std::uint64_t>(&id);
    const auto count = wire::read_vi64(body);
    const auto* fields = std::get_if<std::uint64_t>(&count);
    if (!request || *request != kRequestId || !fields || *fields > 32)
        return std::nullopt;
    std::size_t total = 0;
    for (std::uint64_t i = 0; i < *fields; ++i) {
        const auto field = wire::read_length_prefixed_bytes(body, 4096 - total);
        const auto* value = std::get_if<std::span<const std::byte>>(&field);
        if (!value || value->empty())
            return std::nullopt;
        total += value->size();
        fixture.track_namespace.emplace_back(value->begin(), value->end());
    }
    const auto name = wire::read_length_prefixed_bytes(body, 4096 - total);
    const auto* value = std::get_if<std::span<const std::byte>>(&name);
    if (!value)
        return std::nullopt;
    fixture.track_name.assign(value->begin(), value->end());
    if (!valid_fixture(fixture))
        return std::nullopt;
    // Namespace/name alone are configurable. Rebuilding rejects extra
    // parameters, altered range/ID, noncanonical frames, and trailing bytes.
    const auto expected = encode_fetch(fixture);
    if (input.size() != expected.size() ||
        !std::equal(input.begin(), input.end(), expected.begin()))
        return std::nullopt;
    return fixture;
}

enum class Branch { Accepted, Rejected };
struct Observation {
    std::size_t responses{0};
    std::optional<Branch> branch;
    bool fin{false};
    bool invalid{false};
};
bool terminal(const transport::TransportEvent& event) {
    return std::holds_alternative<transport::PeerCloseEvent>(event) ||
           std::holds_alternative<transport::LocalCloseEvent>(event) ||
           std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
           std::holds_alternative<transport::TransportErrorEvent>(event) ||
           std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}
Observation observe(const RawProbeTranscript& t) {
    Observation o;
    if (t.events.size() > kMaximumEvents || t.writes.size() != 1 || !t.writes.front().stream_id ||
        !t.delivery_event_count || *t.delivery_event_count > t.events.size()) {
        o.invalid = true;
        return o;
    }
    Bytes pending;
    const auto stream = *t.writes.front().stream_id;
    for (std::size_t i = 0; i < t.events.size(); ++i) {
        const auto& event = t.events[i];
        if (terminal(event)) {
            if (!o.fin)
                o.invalid = true;
            break;
        }
        if (const auto* reset = std::get_if<transport::PeerResetEvent>(&event);
            reset && reset->stream_id == stream) {
            o.invalid = true;
            return o;
        }
        const auto* data = std::get_if<transport::StreamDataEvent>(&event);
        if (!data || data->stream_id != stream)
            continue;
        if (i < *t.delivery_event_count || o.fin ||
            data->data.size() > 2 * kMaximumFrame - pending.size()) {
            o.invalid = true;
            return o;
        }
        pending.insert(pending.end(), data->data.begin(), data->data.end());
        wire::Cursor cursor(pending);
        while (cursor.remaining()) {
            const auto begin = cursor.offset();
            auto framed = d21::decode_request_frame(cursor, false);
            if (std::holds_alternative<wire::NeedMore>(framed))
                break;
            const auto* frame = std::get_if<d21::RequestFrame>(&framed);
            if (!frame) {
                o.invalid = true;
                return o;
            }
            // Reconstruct the frame span for the typed decoder, preserving its
            // parameter and property scope validation.
            const auto size = cursor.offset() - begin;
            wire::Cursor typed(std::span<const std::byte>(pending).subspan(begin, size));
            if (frame->type.type == 0x18) {
                if (!std::holds_alternative<d21::SuccessfulResponse>(
                        d21::decode_successful_response(typed, d21::ResponseContext::Fetch))) {
                    o.invalid = true;
                    return o;
                }
                o.branch = Branch::Accepted;
                ++o.responses;
            } else if (frame->type.type == 5) {
                const auto decoded = d21::decode_request_error(typed, true, false);
                const auto* error = std::get_if<d21::RequestErrorMessage>(&decoded);
                if (!error || !d21::valid_reason_phrase(error->reason)) {
                    o.invalid = true;
                    return o;
                }
                o.branch = Branch::Rejected;
                ++o.responses;
            }
            if (o.responses >= 2)
                return o;
        }
        pending.erase(pending.begin(),
                      pending.begin() + static_cast<std::ptrdiff_t>(cursor.offset()));
        if (data->fin) {
            o.fin = true;
            if (!pending.empty())
                o.invalid = true;
        }
    }
    return o;
}
} // namespace
std::vector<FetchResponseProbe> draft21_fetch_response_probes(std::chrono::milliseconds deadline,
                                                              Namespace track_namespace,
                                                              Bytes track_name) {
    if (deadline.count() <= 0)
        throw std::invalid_argument("invalid FETCH response deadline");
    auto fetch = encode_fetch({std::move(track_namespace), std::move(track_name)});
    std::vector<FetchResponseProbe> result;
    for (const auto* id : {"d21-fetch-accepted", "d21-fetch-rejected"}) {
        RawProbeDefinition definition{id,
                                      bytes({0xaf, 0, 0, 0}),
                                      {{RawProbeChannel::NewBidi, fetch, true}},
                                      true,
                                      [](auto input) {
                                          wire::Cursor c(input);
                                          return std::holds_alternative<d21::SetupMessage>(
                                              d21::decode_setup(c));
                                      },
                                      deadline,
                                      [](const auto& t) {
                                          const auto o = observe(t);
                                          return !o.invalid && (o.fin || o.responses >= 2);
                                      },
                                      {}};
        result.push_back(
            {"D21-3-2-1-MUST-052", "d21-one-fetch-ok-or-request-error", 21, std::move(definition)});
    }
    return result;
}
std::optional<bool> evaluate_fetch_response_probe(const RawProbeTranscript& t,
                                                  const FetchResponseProbe& p) {
    if (p.draft != 21 || p.definition.deadline.count() <= 0 || t.writes.size() != 1)
        return std::nullopt;
    const auto fixture = decode_fixture(t.writes.front().write.bytes);
    if (!fixture)
        return std::nullopt;
    const auto candidates = draft21_fetch_response_probes(
        p.definition.deadline, fixture->track_namespace, fixture->track_name);
    const auto expected = std::find_if(candidates.begin(), candidates.end(), [&](const auto& c) {
        return c.requirement_id == p.requirement_id && c.evaluator_id == p.evaluator_id &&
               c.definition.id == p.definition.id;
    });
    if (expected == candidates.end() || !raw_probe_stimulus_valid(t, expected->definition))
        return std::nullopt;
    const auto o = observe(t);
    if (o.invalid)
        return std::nullopt;
    if (o.responses >= 2)
        return false;
    if (!o.fin)
        return std::nullopt;
    if (o.responses == 0)
        return false;
    const auto branch =
        p.definition.id == "d21-fetch-accepted" ? Branch::Accepted : Branch::Rejected;
    if (o.branch != branch)
        return std::nullopt;
    return true;
}
} // namespace moq::interop::scenarios
