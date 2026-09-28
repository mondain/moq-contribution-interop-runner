#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace moq::interop::wire::draft18 {

enum class StreamRole {
    Control,
    Request,
};

struct Limits {
    std::size_t maximum_odd_value_length{65'535};
    std::size_t maximum_parameter_count{32'767};
    std::size_t maximum_object_properties_length{65'535};
    std::size_t maximum_retained_payload_length{4'096};
};

struct DraftAmbiguity {
    std::size_t offset;
    std::string detail;
};

struct VarIntValue {
    std::uint64_t value;
    std::vector<std::byte> raw_bytes;
};

struct ByteValue {
    std::vector<std::byte> bytes;
};

using KeyValue = std::variant<VarIntValue, ByteValue>;

struct KeyValuePair {
    std::uint64_t type;
    KeyValue value;
};

using KeyValuePairs = std::vector<KeyValuePair>;

struct TrackProperties {
    KeyValuePairs entries;
};

struct Location {
    std::uint64_t group;
    std::uint64_t object;

    bool operator==(const Location&) const = default;
};

struct TrackNamespace {
    std::vector<std::vector<std::byte>> fields;
};

struct TrackName {
    std::vector<std::byte> bytes;
};

enum class SubscriptionFilterType : std::uint64_t {
    NextGroupStart = 1,
    LargestObject = 2,
    AbsoluteStart = 3,
    AbsoluteRange = 4,
};

struct SubscriptionFilter {
    SubscriptionFilterType type;
    std::optional<Location> start;
    std::optional<std::uint64_t> end_group_delta;
};

enum class TokenAliasType : std::uint64_t {
    Delete = 0,
    Register = 1,
    UseAlias = 2,
    UseValue = 3,
};

struct Token {
    TokenAliasType alias_type{TokenAliasType::Delete};
    std::optional<std::uint64_t> alias;
    std::optional<std::uint64_t> token_type;
    std::vector<std::byte> token_value;
};

struct VarIntParameterValue {
    std::uint64_t value;
};

struct Uint8ParameterValue {
    std::uint8_t value;
};

using ParameterValue = std::variant<VarIntParameterValue, Uint8ParameterValue,
                                    Token, Location, SubscriptionFilter,
                                    TrackNamespace>;

struct Parameter {
    std::uint64_t type;
    ParameterValue value;
};

using Parameters = std::vector<Parameter>;

enum class ParameterContext {
    Unresolved,
    Subscribe,
    SubscribeOk,
    Publish,
    PublishOk,
    Fetch,
    FetchOk,
    TrackStatus,
    TrackStatusOk,
    PublishNamespace,
    PublishNamespaceOk,
    SubscribeNamespace,
    SubscribeNamespaceOk,
    SubscribeTracks,
    SubscribeTracksOk,
    RequestUpdateSubscription,
    RequestUpdateFetch,
    RequestUpdatePublishNamespace,
    RequestUpdateSubscribeNamespace,
    RequestUpdateSubscribeTracks,
    RequestUpdateOk,
};

enum class ParameterScopeResult {
    Allowed,
    Forbidden,
    Unresolved,
};

struct SetupMessage {
    KeyValuePairs options;
};

struct ReasonPhrase {
    std::vector<std::byte> bytes;
};

struct Redirect {
    std::vector<std::byte> connect_uri;
    TrackNamespace track_namespace;
    TrackName track_name;
};

struct GoawayMessage {
    std::vector<std::byte> new_session_uri;
    std::uint64_t timeout;
    std::optional<std::uint64_t> request_id;
};

struct SubscribeOkMessage {
    std::uint64_t track_alias;
    Parameters parameters;
    TrackProperties track_properties;
};

struct RequestOkMessage {
    Parameters parameters;
    TrackProperties track_properties;
};

struct RequestErrorMessage {
    std::uint64_t error_code;
    std::uint64_t retry_interval;
    ReasonPhrase reason_phrase;
    std::optional<Redirect> redirect;
};

struct RequestUpdateMessage {
    std::uint64_t request_id;
    Parameters parameters;
};

struct PublishDoneMessage {
    std::uint64_t status_code;
    std::uint64_t stream_count;
    ReasonPhrase reason_phrase;
};

struct FetchOkMessage {
    std::uint8_t end_of_track;
    Location end_location;
    Parameters parameters;
    TrackProperties track_properties;
};

struct NamespaceMessage {
    TrackNamespace track_namespace_suffix;
};

struct NamespaceDoneMessage {
    TrackNamespace track_namespace_suffix;
};

struct PublishBlockedMessage {
    TrackNamespace track_namespace_suffix;
    TrackName track_name;
};

struct SubscribeMessage {
    std::uint64_t request_id;
    TrackNamespace track_namespace;
    TrackName track_name;
    Parameters parameters;
};

struct PublishMessage {
    std::uint64_t request_id;
    TrackNamespace track_namespace;
    TrackName track_name;
    std::uint64_t track_alias;
    Parameters parameters;
    TrackProperties track_properties;
};

struct StandaloneFetch {
    TrackNamespace track_namespace;
    TrackName track_name;
    Location start;
    Location end;
};

struct RelativeJoiningFetch {
    std::uint64_t joining_request_id;
    std::uint64_t joining_start;
};

struct AbsoluteJoiningFetch {
    std::uint64_t joining_request_id;
    std::uint64_t joining_start;
};

using Fetch = std::variant<StandaloneFetch, RelativeJoiningFetch,
                           AbsoluteJoiningFetch>;

struct FetchMessage {
    std::uint64_t request_id;
    Fetch fetch;
    Parameters parameters;
};

struct TrackStatusMessage {
    std::uint64_t request_id;
    TrackNamespace track_namespace;
    TrackName track_name;
    Parameters parameters;
};

struct PublishNamespaceMessage {
    std::uint64_t request_id;
    TrackNamespace track_namespace;
    Parameters parameters;
};

struct SubscribeNamespaceMessage {
    std::uint64_t request_id;
    TrackNamespace track_namespace_prefix;
    Parameters parameters;
};

struct SubscribeTracksMessage {
    std::uint64_t request_id;
    TrackNamespace track_namespace_prefix;
    Parameters parameters;
};

using Message = std::variant<SetupMessage, GoawayMessage, SubscribeMessage,
                             SubscribeOkMessage, PublishMessage,
                             PublishDoneMessage, FetchMessage, FetchOkMessage,
                             TrackStatusMessage, PublishNamespaceMessage,
                             SubscribeNamespaceMessage, SubscribeTracksMessage,
                             NamespaceMessage, NamespaceDoneMessage,
                             PublishBlockedMessage, RequestUpdateMessage,
                             RequestOkMessage, RequestErrorMessage>;

template <class T>
using DraftDecodeResult = std::variant<T, NeedMore, DecodeError, DraftAmbiguity>;

using MessageDecodeResult =
    std::variant<Message, NeedMore, DecodeError, DraftAmbiguity>;
using KeyValueDecodeResult = DraftDecodeResult<KeyValuePairs>;
using TrackPropertiesDecodeResult = DraftDecodeResult<TrackProperties>;
using LocationDecodeResult = DraftDecodeResult<Location>;
using TrackNamespaceDecodeResult = DraftDecodeResult<TrackNamespace>;
using TrackNameDecodeResult = DraftDecodeResult<TrackName>;
using SubscriptionFilterDecodeResult = DraftDecodeResult<SubscriptionFilter>;
using TokenDecodeResult = DraftDecodeResult<Token>;
using ParametersDecodeResult = DraftDecodeResult<Parameters>;

enum class EncodeErrorCode {
    InvalidValue,
    PayloadTooLarge,
    OutputCapacity,
};

struct EncodeError {
    EncodeErrorCode code;
    std::string detail;
};

class EncodeResult {
public:
    static EncodeResult success();
    static EncodeResult failure(EncodeErrorCode code, std::string detail);
    static EncodeResult ambiguity(std::size_t offset, std::string detail);

    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] bool is_ambiguity() const noexcept;
    [[nodiscard]] const EncodeError* error() const noexcept;
    [[nodiscard]] const DraftAmbiguity* draft_ambiguity() const noexcept;

private:
    enum class State {
        Success,
        Error,
        Ambiguity,
    };
    explicit EncodeResult(State state, EncodeError error = {},
                          DraftAmbiguity ambiguity = {});

    State state_;
    EncodeError error_;
    DraftAmbiguity ambiguity_;
};

MessageDecodeResult decode_message(StreamRole role, Cursor& input,
                                   const Limits& limits);
EncodeResult encode_message(const Message& message, ByteWriter& output);

KeyValueDecodeResult decode_key_value_pairs(Cursor& input,
                                            std::size_t payload_length,
                                            const Limits& limits);
EncodeResult encode_key_value_pairs(std::span<const KeyValuePair> entries,
                                    ByteWriter& output);

TrackPropertiesDecodeResult decode_track_properties(Cursor& input,
                                                     std::size_t payload_length,
                                                     const Limits& limits);

[[nodiscard]] bool is_mandatory_track_property(const KeyValuePair& property) noexcept;

LocationDecodeResult decode_location(Cursor& input);
EncodeResult encode_location(const Location& location, ByteWriter& output);

TrackNamespaceDecodeResult decode_track_namespace(Cursor& input,
                                                   const Limits& limits);
EncodeResult encode_track_namespace(const TrackNamespace& name_space,
                                    ByteWriter& output);

TrackNameDecodeResult decode_track_name(Cursor& input,
                                        const TrackNamespace& name_space,
                                        const Limits& limits);
EncodeResult encode_track_name(const TrackName& name,
                               const TrackNamespace& name_space,
                               ByteWriter& output);

SubscriptionFilterDecodeResult decode_subscription_filter(
    Cursor& input, std::size_t encoded_length);
EncodeResult encode_subscription_filter(const SubscriptionFilter& filter,
                                        ByteWriter& output);

TokenDecodeResult decode_token(Cursor& input, std::size_t encoded_length);
EncodeResult encode_token(const Token& token, ByteWriter& output);

[[nodiscard]] ParameterScopeResult validate_parameter_scope(
    std::uint64_t type, ParameterContext context) noexcept;
ParametersDecodeResult decode_parameters(Cursor& input, std::uint64_t count,
                                         ParameterContext context,
                                         const Limits& limits);
EncodeResult encode_parameters(std::span<const Parameter> parameters,
                               ParameterContext context, ByteWriter& output);

}  // namespace moq::interop::wire::draft18
