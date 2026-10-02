#pragma once

// Internal helpers shared by the draft-21 contribution profile sources. This
// header is private to src/scenarios and is not part of the runner API.

#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/draft21/setup.h"

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moq::interop::scenarios::d21c {

using Bytes = std::vector<std::byte>;
using Namespace = std::vector<Bytes>;

constexpr std::size_t kMaximumEvents = 4096;
constexpr std::size_t kMaximumTotalBytes = 1u << 20;

struct Fixture {
    Namespace track_namespace;
    Bytes track_name;
    // Token value the publisher's authorization policy is configured to refuse
    // (empty selects the documented default contract value).
    std::string denied_token;
};

bool fixture_valid(const Fixture& fixture);

// ---- encoding -------------------------------------------------------------
void put_vi(Bytes& output, std::uint64_t value);
void put_lp(Bytes& output, const Bytes& value);
Bytes bytes_of(std::initializer_list<unsigned> values);
Bytes frame(std::uint64_t type, const Bytes& body);

struct Param {
    std::uint64_t type;
    Bytes value;  // already encoded, including any length prefix
};
Param param_u8(std::uint64_t type, unsigned value);
Param param_vi(std::uint64_t type, std::uint64_t value);
Param param_location(std::uint64_t type, std::uint64_t group, std::uint64_t object);
Param param_lp(std::uint64_t type, const Bytes& value);
// Delta-encodes ascending parameters; the count is written separately.
Bytes encode_params(std::vector<Param> params);

void put_namespace(Bytes& output, const Namespace& track_namespace);
Bytes request_frame(std::uint64_t type, std::uint64_t request_id,
                    const Fixture& fixture, bool with_name,
                    const std::vector<Param>& params);
Bytes subscribe_frame(std::uint64_t request_id, const Fixture& fixture,
                      const std::vector<Param>& params = {});
Bytes fetch_frame(std::uint64_t request_id, const Fixture& fixture,
                  const std::vector<Param>& params = {});
Bytes track_status_frame(std::uint64_t request_id, const Fixture& fixture);
Bytes subscribe_namespace_frame(std::uint64_t request_id, const Namespace& prefix);
// SUBSCRIBE_NAMESPACE (0x50) or SUBSCRIBE_TRACKS (0x51) with Message Parameters.
Bytes discovery_frame(std::uint64_t type, std::uint64_t request_id, const Namespace& prefix,
                      const std::vector<Param>& params);
Bytes request_update_frame(std::uint64_t request_id, const std::vector<Param>& params);

// A Token that registers or uses a value; token_type 0x9D is a GREASE value.
Bytes token_value(std::uint64_t alias_type, std::optional<std::uint64_t> alias,
                  std::optional<std::uint64_t> token_type, const Bytes& value);

// ---- decoding -------------------------------------------------------------
std::optional<Fixture> recover_fixture(std::span<const std::byte> request);

struct Frame {
    std::uint64_t type{0};
    Bytes body;
    std::size_t event{0};  // transport event that completed this frame
};

struct StreamRecord {
    Bytes bytes;
    // (event index, cumulative byte count) for each data event
    std::vector<std::pair<std::size_t, std::size_t>> chunks;
    bool fin{false};
    std::optional<std::size_t> fin_event;
    bool reset{false};
    std::optional<std::size_t> reset_event;
    std::optional<std::uint64_t> reset_code;
    std::size_t first_event{0};
};

struct PeerCloseInfo {
    bool application{false};
    std::uint64_t code{0};
    std::size_t event{0};
};

struct DatagramRecord {
    std::size_t event;
    Bytes data;
};

// Immutable summary of one transcript. Construction is bounded; an
// unbounded or internally inconsistent transcript is marked invalid.
class View {
public:
    explicit View(const RawProbeTranscript& transcript);
    // Gate callbacks only see the writes accepted so far and the events.
    View(std::span<const RawProbeAcceptedWrite> writes,
         std::span<const transport::TransportEvent> events);
    bool valid() const noexcept { return valid_; }
    const std::map<transport::StreamId, StreamRecord>& streams() const noexcept { return streams_; }
    const std::vector<DatagramRecord>& datagrams() const noexcept { return datagrams_; }
    const std::optional<PeerCloseInfo>& close() const noexcept { return close_; }
    const std::optional<wire::draft21::SetupMessage>& peer_setup() const noexcept { return peer_setup_; }
    // Operator-configured denied credential; absent when the publisher's
    // authorization policy is not controllable.
    const std::optional<std::string>& denied_token() const noexcept { return denied_token_; }
    // URI of the runner-controlled second listener and what arrived there.
    const std::optional<std::string>& alternate_uri() const noexcept { return alternate_uri_; }
    std::span<const transport::TransportEvent> alternate_events() const noexcept { return alternate_events_; }
    std::optional<std::uint64_t> peer_option(std::uint64_t type) const;
    const StreamRecord* stream(transport::StreamId id) const;
    // Stream actually used by transcript write `index`, if it has one.
    const StreamRecord* write_stream(std::size_t index) const;
    std::optional<transport::StreamId> write_stream_id(std::size_t index) const;
    // Transport event count at which write `index` was fully accepted.
    std::optional<std::size_t> write_event(std::size_t index) const;
    std::vector<Frame> frames(const StreamRecord& record) const;
    std::vector<Frame> write_frames(std::size_t index) const;

private:
    std::span<const RawProbeAcceptedWrite> writes_;
    bool valid_{true};
    std::map<transport::StreamId, StreamRecord> streams_;
    std::vector<DatagramRecord> datagrams_;
    std::optional<PeerCloseInfo> close_;
    std::optional<wire::draft21::SetupMessage> peer_setup_;
    std::optional<std::string> denied_token_;
    std::optional<std::string> alternate_uri_;
    std::vector<transport::TransportEvent> alternate_events_;
};

// Splits complete framed messages; sets `malformed` for an impossible frame.
std::vector<Frame> parse_frames(const StreamRecord& record, bool& malformed);

std::optional<std::uint64_t> read_vi(wire::Cursor& cursor);
std::optional<std::span<const std::byte>> read_n(wire::Cursor& cursor, std::size_t length);

bool setup_decodes(std::span<const std::byte> input);
// Peer SETUP recovered from raw transport events (lowest peer uni stream).
std::optional<wire::draft21::SetupMessage> peer_setup_from_events(
    std::span<const transport::TransportEvent> events);
std::optional<std::uint64_t> setup_option_value(const wire::draft21::SetupMessage& setup,
                                                std::uint64_t type);
// Value of a numeric (even type) Setup Option, or nullopt when absent.
std::optional<std::uint64_t> setup_numeric_option(std::span<const std::byte> input,
                                                  std::uint64_t type);

// Gate for a stream's response bytes: first frame is a SUBSCRIBE_OK.
bool subscribe_ok_ready(std::span<const std::byte> input);

// ---- scenario specification ------------------------------------------------
struct Judgement {
    bool ready{false};
    std::optional<bool> result{};
};
using Judge = std::function<Judgement(const View&)>;
using Builder = std::function<RawProbeDefinition(const Fixture&)>;

struct RowBinding {
    const char* requirement;
    const char* evaluator;
    Judge judge{};  // overrides the scenario judge when set
};

struct Spec {
    std::string scenario;
    std::vector<RowBinding> rows;
    Builder build;
    Judge judge;
};

std::vector<Spec> session_specs();
std::vector<Spec> object_specs();
// Rows closed by the draft-21 slice-B probes (draft21_contribution_d21b.cpp).
std::vector<Spec> d21b_specs();
// Adds slice-B row bindings to scenarios defined by other profile sources.
void d21b_attach_rows(std::vector<Spec>& specs);
// Default credential value used when no denied token is configured.
constexpr const char* kDefaultDeniedToken = "interop-denied";

// Frame type constants used across profiles.
constexpr std::uint64_t kSubscribeOk = 0x4;
constexpr std::uint64_t kRequestError = 0x5;
constexpr std::uint64_t kRequestOk = 0x7;
constexpr std::uint64_t kNamespace = 0x8;
constexpr std::uint64_t kPublishDone = 0xb;
constexpr std::uint64_t kNamespaceDone = 0xe;
constexpr std::uint64_t kFetchOk = 0x18;
constexpr std::uint64_t kPublishStateNotify = 0x22;

// Default control-plane SETUP without options.
Bytes empty_setup();
RawProbeDefinition base_definition(const std::string& id);

// True when a complete SUBSCRIBE_OK or REQUEST_ERROR opens `frames`.
bool opens_with_response(const std::vector<Frame>& frames);

}  // namespace moq::interop::scenarios::d21c
