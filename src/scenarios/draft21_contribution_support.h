#pragma once

// Internal helpers shared by the draft-21 contribution profile sources. This
// header is private to src/scenarios and is not part of the runner API.

#include "moq/interop/scenarios/draft21_contribution.h"
#include "moq/interop/scenarios/location_filter_param.h"
#include "moq/interop/scenarios/raw_probe.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/draft21/key_values.h"
#include "moq/interop/wire/draft21/setup.h"
#include "moq/interop/wire/draft21/token.h"

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

constexpr std::size_t kMaximumEvents = kRawProbeMaximumEvents;
constexpr std::size_t kMaximumTotalBytes = 1u << 20;

struct Fixture {
    Namespace track_namespace;
    Bytes track_name;
    // Token value the publisher's authorization policy is configured to refuse
    // (empty selects the documented default contract value).
    std::string denied_token;
    // Operator-supplied credentials for the token rows; absent means those scenarios send nothing.
    Draft21TokenCredentials credentials{};
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
// LOCATION_FILTER (0x21) in the active wire draft's form; the drop-in for param_lp(0x21, ...).
Param filter_param(const FilterFields& fields);
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
// The first AUTHORIZATION TOKEN (0x03) parameter of a SUBSCRIBE, FETCH or TRACK_STATUS
// the runner wrote, decoded; absent when the request carries none.
std::optional<wire::draft21::Token> recover_token(std::span<const std::byte> request);

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
    // Courtesy responses the runner volunteered (empty for gate views).
    const std::vector<RawProbeCourtesyWrite>& courtesy_writes() const noexcept { return courtesy_; }
    // Transport events observed when the runner acknowledged the PUBLISH_NAMESPACE that
    // opened `stream` (RawProbeDefinition::auto_accept_*), if it did.
    std::optional<std::size_t> auto_reply_event(transport::StreamId stream) const {
        for (const auto& reply : auto_replies_)
            if (reply.stream_id == stream) return reply.delivery_event_count;
        return std::nullopt;
    }
    // REQUEST_ERROR code a compatibility profile maps to UNKNOWN_AUTH_TOKEN_ALIAS, if configured.
    std::optional<std::uint64_t> unknown_alias_code() const noexcept { return unknown_alias_code_; }
    const std::optional<wire::draft21::SetupMessage>& peer_setup() const noexcept { return peer_setup_; }
    // Operator-configured denied credential; absent when the publisher's
    // authorization policy is not controllable.
    const std::optional<std::string>& denied_token() const noexcept { return denied_token_; }
    // URI of the runner-controlled second listener and what arrived there.
    const std::optional<std::string>& replacement_uri() const noexcept { return alternate_uri_; }
    std::span<const transport::TransportEvent> replacement_events() const noexcept { return alternate_events_; }
    std::optional<std::uint64_t> peer_option(std::uint64_t type) const;
    const StreamRecord* stream(transport::StreamId id) const;
    // Stream actually used by transcript write `index`, if it has one.
    const StreamRecord* write_stream(std::size_t index) const;
    std::optional<transport::StreamId> write_stream_id(std::size_t index) const;
    // Transport event count at which write `index` was fully accepted.
    std::optional<std::size_t> write_event(std::size_t index) const;
    // The bytes the runner wrote for transcript write `index` (empty if absent).
    std::span<const std::byte> write_bytes(std::size_t index) const {
        return index < writes_.size() ? std::span<const std::byte>(writes_[index].write.bytes)
                                      : std::span<const std::byte>{};
    }
    // True when the observation window of a `Spec::window` scenario is over: the
    // context timed out or the peer ended the session, so no further evidence
    // can arrive. Always false for scenarios that must finish on evidence.
    bool window_ended() const noexcept { return window_ended_; }
    void set_window_ended(bool value) noexcept { window_ended_ = value; }
    // Arrival time of each transport event and when the window ended, when the run recorded them.
    void set_times(std::span<const RawProbeClock::time_point> event_times,
                   std::optional<RawProbeClock::time_point> window_end) noexcept {
        event_times_ = event_times;
        window_end_ = window_end;
    }
    // How long the window went on after event `index` arrived; absent when the transcript does not
    // record both times (a hand-built transcript is judged by order alone).
    std::optional<RawProbeClock::duration> window_after(std::size_t index) const {
        if (!window_end_ || index >= event_times_.size()) return std::nullopt;
        return *window_end_ - event_times_[index];
    }
    std::vector<Frame> frames(const StreamRecord& record) const;
    std::vector<Frame> write_frames(std::size_t index) const;

private:
    std::span<const RawProbeAcceptedWrite> writes_;
    bool valid_{true};
    bool window_ended_{false};
    std::span<const RawProbeClock::time_point> event_times_;
    std::optional<RawProbeClock::time_point> window_end_;
    std::optional<std::uint64_t> unknown_alias_code_;
    std::vector<RawProbeCourtesyWrite> courtesy_;
    std::vector<RawProbeAutoReply> auto_replies_;
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
    // Absence rules (nothing outside a filter, exactly N copies) need the whole
    // observation window: a timed-out or peer-closed transcript is judged on its
    // prefix with View::window_ended() set, instead of being unscorable.
    bool window{false};
};

std::vector<Spec> session_specs();
std::vector<Spec> object_specs();
// Rows closed by the draft-21 slice-B probes (draft21_contribution_d21b.cpp).
std::vector<Spec> d21b_specs();
// Adds slice-B row bindings to scenarios defined by other profile sources.
void d21b_attach_rows(std::vector<Spec>& specs);
// Default credential value used when no denied token is configured.
constexpr const char* kDefaultDeniedToken = "interop-denied";
// Rows closed by the remaining-rows slice (draft21_contribution_residual.cpp).
std::vector<Spec> residual_specs();
// The residual slice is split by what drives each row; residual_specs() concatenates
// these in the order the rows were first defined.
std::vector<Spec> residual_subscription_specs();
std::vector<Spec> residual_publisher_specs();
std::vector<Spec> residual_token_specs();

// ---- helpers shared by the object and slice-B probe sources -----------------------
// A nested namespace keeps these out of sources (residual) that still carry
// their own copies of the same names.
namespace shared {

constexpr std::uint64_t kImmutablePropertiesType = 0x0b;
constexpr std::uint64_t kGreaseValue = 0x9d;

// Object Properties: the top-level list and each Immutable Properties list.
struct Properties {
    wire::draft21::KeyValues top;
    std::vector<wire::draft21::KeyValues> immutable;
};
std::optional<Properties> parse_properties(std::span<const std::byte> block);

struct ObjectRecord {
    std::uint64_t group{0};
    std::uint64_t object{0};
    std::optional<std::uint64_t> subgroup;
    std::optional<std::uint64_t> status;
    Bytes payload;
    Properties properties;
    bool has_properties{false};
};

// One Subgroup stream: header, then every whole Object received so far.
struct SubgroupParse {
    bool header{false};
    bool invalid{false};
    std::uint64_t type{0};
    std::uint64_t alias{0};
    std::uint64_t group{0};
    std::optional<std::uint64_t> subgroup;
    std::size_t header_length{0};
    std::vector<ObjectRecord> objects;
    bool terminal_status{false};  // an End of Group or End of Track Object
    bool partial_body{false};     // bytes follow the last whole Object
};
SubgroupParse parse_subgroup(std::span<const std::byte> data);

// A request on a fresh bidirectional stream.
RawProbeWrite request_write(Bytes bytes, bool fin = false);
Spec spec(const char* scenario, std::vector<RowBinding> rows, Builder build, Judge judge = {},
          bool window = false);
Bytes text_bytes(const std::string& value);

}  // namespace shared

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
