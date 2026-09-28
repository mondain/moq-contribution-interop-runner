#pragma once

#include "moq/interop/wire/cursor.h"

#include <cstddef>
#include <cstdint>
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

struct SetupMessage {
    KeyValuePairs options;
};

using Message = std::variant<SetupMessage>;

template <class T>
using DraftDecodeResult = std::variant<T, NeedMore, DecodeError, DraftAmbiguity>;

using MessageDecodeResult = DraftDecodeResult<Message>;
using KeyValueDecodeResult = DraftDecodeResult<KeyValuePairs>;
using TrackPropertiesDecodeResult = DraftDecodeResult<TrackProperties>;

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

    [[nodiscard]] bool has_value() const noexcept;
    [[nodiscard]] const EncodeError* error() const noexcept;

private:
    explicit EncodeResult(bool success, EncodeError error = {});

    bool success_;
    EncodeError error_;
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

}  // namespace moq::interop::wire::draft18
