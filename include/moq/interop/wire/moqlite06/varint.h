#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "moq/interop/wire/cursor.h"

namespace moq::interop::wire::moqlite06 {

// moq-lite-06 `(i)`: the QUIC variable-length integer (RFC 9000 section 16). The top two bits of the first
// byte select a 1/2/4/8 byte big-endian form carrying 6/14/30/62 value bits.
inline constexpr std::uint64_t kMaxVarint = (std::uint64_t{1} << 62) - 1;

// NeedMore on short input. A DecodeError (OffsetOverflow) is possible only when the Cursor's absolute offset is
// near SIZE_MAX; callers must check for it before taking the value. Non-minimal forms are accepted (RFC 9000 allows
// them) and the cursor advances by the wire width. The cursor moves only on success.
DecodeResult<std::uint64_t> read_varint(Cursor& input);

// Writes the minimal form. Returns false and writes nothing when value > kMaxVarint or there is no capacity.
bool write_varint(std::uint64_t value, ByteWriter& output);

// Minimal encoded size: 1, 2, 4 or 8; 0 when value > kMaxVarint.
std::size_t varint_size(std::uint64_t value);

// `(s)`: a varint byte length followed by that many bytes. The bytes are not validated as UTF-8.
// LengthExceedsLimit is reported from the length alone, before the bytes are read.
DecodeResult<std::string> read_string(Cursor& input, std::size_t max_length);
bool write_string(std::string_view value, ByteWriter& output);

}  // namespace moq::interop::wire::moqlite06
