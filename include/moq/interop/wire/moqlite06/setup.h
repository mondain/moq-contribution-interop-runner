#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/moqlite06/framing.h"

namespace moq::interop::wire::moqlite06 {

// SETUP (draft 7.3): `Message Length (i)`, `Parameter Count (i)`, then per parameter
// `Parameter ID (i)`, `Parameter Length (i)`, `Parameter Value (b)`.
struct SetupParameter {
    std::uint64_t id;
    std::vector<std::byte> value;
};

// Wire-faithful: order preserved, unknown ids kept.
struct SetupMessage {
    std::vector<SetupParameter> parameters;
};

// Draft 7.3, Table 6.
inline constexpr std::uint64_t kParamProbe = 0x1;
inline constexpr std::uint64_t kParamPath = 0x2;
inline constexpr std::uint64_t kParamRole = 0x3;
inline constexpr std::uint64_t kParamCost = 0x4;
inline constexpr std::uint64_t kParamHop = 0x5;

// A Parameter Count above limits.max_parameters is LengthExceedsLimit before any parameter is read. A
// duplicate id, a count that disagrees with the body, a Parameter Length running past the Message Length and
// trailing bytes are ProtocolViolation. The cursor moves only on success.
DecodeResult<SetupMessage> decode_setup(Cursor& input, const DecodeLimits& limits = kDefaultLimits);

// Refuses duplicate ids and ids that are not varints (InvalidValue), more than limits.max_parameters, a Path
// value over limits.max_string_length or a body over limits.max_message_length (LimitExceeded), and a writer
// without room (OutputCapacity). Writes nothing on failure. This is the raw encoder: it deliberately does NOT
// validate the value shape of known parameters (a Cost with two varints, say), so malformed SETUPs stay
// buildable for probes; build_setup is the typed, validating path.
std::optional<EncodeError> encode_setup(const SetupMessage& message, ByteWriter& output,
                                        const DecodeLimits& limits = kDefaultLimits);

enum class ProbeLevel : std::uint64_t { None = 0, Report = 1, Increase = 2 };
enum class Role : std::uint64_t { Both = 0, Publisher = 1, Subscriber = 2 };

struct SetupCapabilities {
    std::optional<std::uint64_t> probe;  // raw value; use probe_level() for the clamped view
    std::optional<std::string> path;     // the Parameter Value bytes as-is (no nested length prefix)
    std::optional<std::uint64_t> role;   // raw value
    std::optional<std::uint64_t> cost;
    std::optional<std::uint64_t> hop_id;  // raw value; 0 is equivalent to absent (draft 7.3.5) but kept
};

// Typed view of the known parameters; unknown ids are ignored. A known varint parameter whose value is not
// exactly one varint is a ProtocolViolation, as is a repeated id. Path longer than limits.max_string_length
// is LengthExceedsLimit.
DecodeResult<SetupCapabilities> read_capabilities(const SetupMessage& message,
                                                  const DecodeLimits& limits = kDefaultLimits);

// Known parameters in id order. Returns nullopt when a varint capability exceeds kMaxVarint or the path is
// longer than limits.max_string_length (the typed encoder refuses inconsistent values).
std::optional<SetupMessage> build_setup(const SetupCapabilities& capabilities,
                                        const DecodeLimits& limits = kDefaultLimits);

// Absent -> None; values above 2 -> Increase (moq.dev: an unknown level decodes as the highest known one).
ProbeLevel probe_level(const SetupCapabilities& capabilities);
// Absent or unknown (above 2) -> Both (draft 7.3.3).
Role effective_role(const SetupCapabilities& capabilities);
// Absent -> 1 (draft 7.3.4); an explicit 0 stays 0.
std::uint64_t effective_cost(const SetupCapabilities& capabilities);

}  // namespace moq::interop::wire::moqlite06
