#include "moq/interop/scenarios/fetch_first_object.h"
#include "moq/interop/scenarios/draft18_response.h"
#include "moq/interop/wire/draft18/objects.h"
#include "moq/interop/wire/draft21/objects.h"
#include "moq/interop/wire/draft21/request_frame.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/successful_response.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>
namespace moq::interop::scenarios {
namespace {
namespace d18 = wire::draft18;
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
bool valid_fixture_fields(const Namespace &track_namespace,
                          const Bytes &track_name) {
  if (track_namespace.size() > 32)
    return false;
  std::size_t total = track_name.size();
  if (total > 4096)
    return false;
  for (const auto &field : track_namespace) {
    if (field.empty() || field.size() > 4096 - total)
      return false;
    total += field.size();
  }
  if (track_namespace.empty())
    return true;
  return track_namespace.front() != bytes({'.'}) &&
         (!track_name.empty() ||
          track_namespace.front() !=
              bytes({'.', 's', 'e', 's', 's', 'i', 'o', 'n'}));
}
bool valid_fixture(const Fixture &fixture) {
  return valid_fixture_fields(fixture.track_namespace, fixture.track_name);
}
Bytes encode_fetch(unsigned draft, const Fixture &fixture) {
  if (!valid_fixture(fixture))
    throw std::invalid_argument("invalid FETCH track fixture");
  wire::ByteWriter output(kMaximumFrame);
  if (draft == 18) {
    const d18::FetchMessage message{
        kRequestId,
        d18::StandaloneFetch{
            {fixture.track_namespace}, {fixture.track_name}, {7, 9}, {7, 10}},
        {}};
    if (!d18::encode_message(d18::Message{message}, output).has_value())
      throw std::invalid_argument("unencodable draft18 FETCH fixture");
  } else {
    wire::ByteWriter body(65535);
    bool success = wire::write_vi64(kRequestId, body) &&
                   wire::write_vi64(fixture.track_namespace.size(), body);
    for (const auto &field : fixture.track_namespace)
      success = success && wire::write_length_prefixed_bytes(field, body);
    success =
        success && wire::write_length_prefixed_bytes(fixture.track_name, body);
    // Draft21 uses an inclusive end and an End Group delta of zero.
    wire::ByteWriter filter(27);
    success = success && wire::write_vi64(7, filter) &&
              wire::write_vi64(9, filter) && wire::write_vi64(0, filter) &&
              wire::write_vi64(9, filter) && wire::write_vi64(1, body) &&
              wire::write_vi64(0x21, body) &&
              wire::write_length_prefixed_bytes(filter.bytes(), body) &&
              wire::write_vi64(0x16, output) &&
              output.append_byte(static_cast<std::byte>(body.size() >> 8u)) &&
              output.append_byte(static_cast<std::byte>(body.size() & 255u)) &&
              output.append_bytes(body.bytes());
    if (!success)
      throw std::invalid_argument("unencodable draft21 FETCH fixture");
  }
  return {output.bytes().begin(), output.bytes().end()};
}
std::optional<Fixture> decode_fixture(unsigned draft,
                                      std::span<const std::byte> input) {
  if (input.size() > kMaximumFrame)
    return std::nullopt;
  Fixture fixture;
  wire::Cursor cursor(input);
  if (draft == 18) {
    const auto decoded =
        d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto *message = std::get_if<d18::Message>(&decoded);
    const auto *fetch =
        message ? std::get_if<d18::FetchMessage>(message) : nullptr;
    const auto *standalone =
        fetch ? std::get_if<d18::StandaloneFetch>(&fetch->fetch) : nullptr;
    if (!standalone || cursor.remaining() != 0 ||
        fetch->request_id != kRequestId)
      return std::nullopt;
    fixture = {standalone->track_namespace.fields,
               standalone->track_name.bytes};
  } else {
    const auto decoded = d21::decode_request_frame(cursor, true);
    const auto *frame = std::get_if<d21::RequestFrame>(&decoded);
    if (!frame || frame->type.type != 0x16 || cursor.remaining() != 0)
      return std::nullopt;
    wire::Cursor body(frame->body);
    const auto id = wire::read_vi64(body);
    const auto *request = std::get_if<std::uint64_t>(&id);
    const auto count = wire::read_vi64(body);
    const auto *fields = std::get_if<std::uint64_t>(&count);
    if (!request || *request != kRequestId || !fields || *fields > 32)
      return std::nullopt;
    std::size_t total = 0;
    for (std::uint64_t i = 0; i < *fields; ++i) {
      const auto field = wire::read_length_prefixed_bytes(body, 4096 - total);
      const auto *value = std::get_if<std::span<const std::byte>>(&field);
      if (!value || value->empty())
        return std::nullopt;
      total += value->size();
      fixture.track_namespace.emplace_back(value->begin(), value->end());
    }
    const auto name = wire::read_length_prefixed_bytes(body, 4096 - total);
    const auto *value = std::get_if<std::span<const std::byte>>(&name);
    if (!value)
      return std::nullopt;
    fixture.track_name.assign(value->begin(), value->end());
  }
  if (!valid_fixture(fixture))
    return std::nullopt;
  // Namespace/name alone are configurable. Rebuilding rejects extra
  // parameters, altered range/ID, noncanonical frames, and trailing bytes.
  const auto expected = encode_fetch(draft, fixture);
  if (input.size() != expected.size() ||
      !std::equal(input.begin(), input.end(), expected.begin()))
    return std::nullopt;
  return fixture;
}
bool setup_ready(unsigned draft, std::span<const std::byte> input) {
  wire::Cursor cursor(input);
  if (draft == 18) {
    const auto decoded =
        d18::decode_message(d18::StreamRole::Control, cursor, {});
    const auto *message = std::get_if<d18::Message>(&decoded);
    return message && std::holds_alternative<d18::SetupMessage>(*message);
  }
  return std::holds_alternative<d21::SetupMessage>(d21::decode_setup(cursor));
}
bool valid_ok(unsigned draft, std::span<const std::byte> input) {
  wire::Cursor cursor(input);
  if (draft == 18) {
    const auto decoded =
        d18::decode_message(d18::StreamRole::Request, cursor, {});
    const auto *message = std::get_if<d18::Message>(&decoded);
    const auto *ok =
        message ? std::get_if<d18::FetchOkMessage>(message) : nullptr;
    return ok && ok->end_of_track <= 1 && ok->end_location.group == 7 &&
           ok->end_location.object == 10 && cursor.remaining() == 0 &&
           draft18_track_properties_valid(ok->track_properties);
  }
  const auto decoded =
      d21::decode_successful_response(cursor, d21::ResponseContext::Fetch);
  const auto *ok = std::get_if<d21::SuccessfulResponse>(&decoded);
  return ok && ok->end_location && ok->end_location->group == 7 &&
         ok->end_location->object == 9 && cursor.remaining() == 0;
}
bool session_terminal(const transport::TransportEvent &event) {
  return std::holds_alternative<transport::PeerCloseEvent>(event) ||
         std::holds_alternative<transport::LocalCloseEvent>(event) ||
         std::holds_alternative<transport::IdleTimeoutEvent>(event) ||
         std::holds_alternative<transport::TransportErrorEvent>(event) ||
         std::holds_alternative<transport::EventQueueOverflowEvent>(event);
}
bool bounded_events(const RawProbeTranscript &t) {
  if (t.events.size() > kMaximumEvents)
    return false;
  std::size_t total = 0;
  for (const auto &event : t.events) {
    if (session_terminal(event))
      break;
    if (const auto *data = std::get_if<transport::StreamDataEvent>(&event)) {
      if (data->data.size() > kMaximumFrame - total)
        return false;
      total += data->data.size();
    }
  }
  return true;
}
struct Candidate {
  Bytes bytes;
  std::size_t first;
  bool closed{false};
};
template <class Decoder, class Object>
bool complete_first_object(const Bytes &input) {
  Decoder decoder(
      [](std::uint64_t id)
          -> std::optional<
              std::conditional_t<std::is_same_v<Decoder, d18::FetchDecoder>,
                                 d18::FetchGroupOrder, d21::FetchGroupOrder>> {
        using Order =
            std::conditional_t<std::is_same_v<Decoder, d18::FetchDecoder>,
                               d18::FetchGroupOrder, d21::FetchGroupOrder>;
        return id == kRequestId ? std::optional{Order::Ascending}
                                : std::nullopt;
      });
  const auto result = decoder.push(input, false);
  if (!result.header || result.header->request_id != kRequestId ||
      result.events.empty())
    return false;
  const auto *first = std::get_if<Object>(&result.events.front());
  return first && first->request_id == kRequestId &&
         first->serialization_flags &&
         (*first->serialization_flags & 12u) == 12u && first->group_id == 7 &&
         first->object_id == 9;
}
std::optional<bool> observe(const RawProbeTranscript &t, unsigned draft,
                            FetchFirstObjectField field) {
  if (!bounded_events(t) || t.writes.size() != 1 ||
      !t.writes.front().stream_id || !t.writes.front().delivery_event_count ||
      !t.delivery_event_count ||
      *t.delivery_event_count != *t.writes.front().delivery_event_count ||
      *t.delivery_event_count > t.events.size())
    return {};
  const auto request_stream = *t.writes.front().stream_id;
  const auto marker = *t.delivery_event_count;
  Bytes response;
  bool response_closed = false;
  bool accepted = false;
  std::size_t cumulative = 0;
  std::map<transport::StreamId, Candidate> streams;
  for (std::size_t i = 0; i < t.events.size(); ++i) {
    const auto &event = t.events[i];
    if (session_terminal(event))
      break;
    if (const auto *data = std::get_if<transport::StreamDataEvent>(&event)) {
      if (data->stream_id == request_stream) {
        if (i < marker)
          return {};
        if (response_closed)
          continue;
        if (data->data.size() > kMaximumFrame - cumulative)
          return {};
        cumulative += data->data.size();
        response.insert(response.end(), data->data.begin(), data->data.end());
        wire::Cursor cursor(response);
        // Both drafts use their own typed response parser. The frame
        // grammar alone cannot validate FETCH-specific property scope.
        if (draft == 18) {
          const auto decoded =
              d18::decode_message(d18::StreamRole::Request, cursor, {});
          if (std::holds_alternative<d18::Message>(decoded)) {
            if (!valid_ok(draft, std::span(response).first(cursor.offset())))
              return {};
            accepted = true;
            response_closed = true;
          } else if (!std::holds_alternative<wire::NeedMore>(decoded))
            return {};
        } else {
          const auto decoded = d21::decode_successful_response(
              cursor, d21::ResponseContext::Fetch);
          if (std::holds_alternative<d21::SuccessfulResponse>(decoded)) {
            if (!valid_ok(draft, std::span(response).first(cursor.offset())))
              return {};
            accepted = true;
            response_closed = true;
          } else if (!std::holds_alternative<wire::NeedMore>(decoded))
            return {};
        }
        if (data->fin)
          response_closed = true;
      } else if ((data->stream_id & 3u) == 2u) {
        if (!streams.contains(data->stream_id)) {
          if (streams.size() >= 64)
            return {};
          streams.emplace(data->stream_id, Candidate{{}, i, false});
        }
        auto &candidate = streams.at(data->stream_id);
        if (candidate.closed)
          continue;
        if (data->data.size() > kMaximumFrame - cumulative)
          return {};
        cumulative += data->data.size();
        candidate.bytes.insert(candidate.bytes.end(), data->data.begin(),
                               data->data.end());
        candidate.closed = data->fin;
      }
    } else if (const auto *reset =
                   std::get_if<transport::PeerResetEvent>(&event)) {
      if (reset->stream_id == request_stream)
        response_closed = true;
      if ((reset->stream_id & 3u) == 2u) {
        if (!streams.contains(reset->stream_id)) {
          if (streams.size() >= 64)
            return {};
          streams.emplace(reset->stream_id, Candidate{{}, i, true});
        } else
          streams.at(reset->stream_id).closed = true;
      }
    } else if (const auto *stop =
                   std::get_if<transport::PeerStopSendingEvent>(&event);
               stop && stop->stream_id == request_stream && !accepted)
      response_closed = true;
  }
  if (!accepted)
    return {};
  const Candidate *associated = nullptr;
  for (const auto &[id, candidate] : streams) {
    (void)id;
    wire::Cursor cursor(candidate.bytes);
    const auto type = wire::read_vi64(cursor);
    const auto *value = std::get_if<std::uint64_t>(&type);
    if (!value || *value != 5)
      continue;
    const auto request = wire::read_vi64(cursor);
    const auto *request_id = std::get_if<std::uint64_t>(&request);
    if (!request_id || *request_id != kRequestId)
      continue;
    if (associated || candidate.first < marker)
      return {};
    associated = &candidate;
  }
  if (!associated)
    return {};
  wire::Cursor cursor(associated->bytes);
  wire::read_vi64(cursor);
  wire::read_vi64(cursor);
  const auto decoded_flags = wire::read_vi64(cursor);
  const auto *flags = std::get_if<std::uint64_t>(&decoded_flags);
  // A range marker is not an ordinary first Object. A later Object cannot
  // anchor this fixture's absolute first fields.
  if (!flags || *flags >= 128)
    return {};
  const auto bit = field == FetchFirstObjectField::Group ? 8u : 4u;
  if ((*flags & bit) == 0)
    return false;
  if ((*flags & 12u) != 12u)
    return {};
  const bool complete =
      draft == 18 ? complete_first_object<d18::FetchDecoder, d18::ObjectEvent>(
                        associated->bytes)
                  : complete_first_object<d21::FetchDecoder, d21::ObjectEvent>(
                        associated->bytes);
  return complete ? std::optional{true} : std::nullopt;
}
std::vector<FetchFirstObjectProbe> profiles(unsigned draft,
                                            std::chrono::milliseconds deadline,
                                            const Fixture &fixture) {
  if (deadline.count() <= 0)
    throw std::invalid_argument("invalid FETCH first-object deadline");
  const auto fetch = encode_fetch(draft, fixture);
  const auto *id =
      draft == 18 ? "fetch-known-first-object-with-nonzero-group-and-object-ids"
                  : "d21-fetch-first-object-flags";
  RawProbeDefinition definition{
      id,
      bytes({0xaf, 0, 0, 0}),
      {{RawProbeChannel::NewBidi, fetch, true}},
      true,
      [draft](auto input) { return setup_ready(draft, input); },
      deadline,
      [draft](const auto &t) {
        return observe(t, draft, FetchFirstObjectField::Group).has_value() ||
               observe(t, draft, FetchFirstObjectField::Object).has_value();
      },
      {}};
  if (draft == 18)
    return {{"D18-11-4-4-1-MUST-001",
             "first-fetch-object-has-absolute-group-id-delta", 18,
             FetchFirstObjectField::Group, definition},
            {"D18-11-4-4-1-MUST-002",
             "first-fetch-object-has-absolute-object-id-delta", 18,
             FetchFirstObjectField::Object, definition}};
  return {{"D21-11-4-1-1-MUST-551", "d21-fetch-first-group-id", 21,
           FetchFirstObjectField::Group, definition},
          {"D21-11-4-1-1-MUST-552", "d21-fetch-first-object-id", 21,
           FetchFirstObjectField::Object, definition}};
}
} // namespace
bool fetch_first_object_fixture_valid(const Namespace &track_namespace,
                                      const Bytes &track_name) {
  return valid_fixture_fields(track_namespace, track_name);
}
std::vector<FetchFirstObjectProbe>
draft18_fetch_first_object_probes(std::chrono::milliseconds deadline,
                                  Namespace ns, Bytes name) {
  return profiles(18, deadline, {std::move(ns), std::move(name)});
}
std::vector<FetchFirstObjectProbe>
draft21_fetch_first_object_probes(std::chrono::milliseconds deadline,
                                  Namespace ns, Bytes name) {
  return profiles(21, deadline, {std::move(ns), std::move(name)});
}
std::optional<bool>
evaluate_fetch_first_object_probe(const RawProbeTranscript &t,
                                  const FetchFirstObjectProbe &p) {
  if ((p.draft != 18 && p.draft != 21) || t.writes.size() != 1 ||
      p.definition.deadline.count() <= 0)
    return {};
  const auto fixture = decode_fixture(p.draft, t.writes.front().write.bytes);
  if (!fixture)
    return {};
  const auto candidates = profiles(p.draft, p.definition.deadline, *fixture);
  const auto expected =
      std::find_if(candidates.begin(), candidates.end(), [&](const auto &c) {
        return c.requirement_id == p.requirement_id &&
               c.evaluator_id == p.evaluator_id && c.field == p.field &&
               c.definition.id == p.definition.id;
      });
  if (expected == candidates.end() || !bounded_events(t))
    return {};
  // Validate the actual delivered stimulus against its pre-terminal prefix.
  // Tail events cannot extend proof past a transport terminal.
  const auto terminal =
      std::find_if(t.events.begin(), t.events.end(), session_terminal);
  RawProbeTranscript prefix;
  prefix.scenario_id = t.scenario_id;
  prefix.setup = t.setup;
  prefix.writes = t.writes;
  prefix.transport_established = t.transport_established;
  prefix.max_datagram_payload = t.max_datagram_payload;
  prefix.peer_setup_received = t.peer_setup_received;
  prefix.stimulus_delivered = t.stimulus_delivered;
  prefix.complete = t.complete;
  prefix.harness_failed = t.harness_failed;
  prefix.timed_out = t.timed_out;
  prefix.delivery_event_count = t.delivery_event_count;
  prefix.events.assign(t.events.begin(), terminal);
  if (!raw_probe_stimulus_valid(prefix, expected->definition))
    return {};
  return observe(t, p.draft, p.field);
}
} // namespace moq::interop::scenarios
