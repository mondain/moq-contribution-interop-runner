#include "moq/interop/wire/draft18/messages.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <variant>
#include <vector>

namespace {

using moq::interop::wire::ByteWriter;
using moq::interop::wire::Cursor;
using moq::interop::wire::DecodeError;
using moq::interop::wire::NeedMore;
using moq::interop::wire::read_bytes;
using moq::interop::wire::read_vi64;
using namespace moq::interop::wire::draft18;

constexpr std::size_t kMaximumFrameSize = 65'535u + 9u + 2u;

[[noreturn]] void invariant_failed() { std::abort(); }

void require(bool condition) {
    if (!condition) invariant_failed();
}

bool has_duplicate_nonrepeatable_setup_option(const Message& message) {
    const auto* setup = std::get_if<SetupMessage>(&message);
    if (setup == nullptr) return false;
    for (std::size_t index = 1; index < setup->options.size(); ++index) {
        const auto type = setup->options[index].type;
        const bool known_nonrepeatable =
            type == 0x01 || type == 0x04 || type == 0x05 || type == 0x07;
        if (known_nonrepeatable && setup->options[index - 1].type == type) {
            return true;
        }
    }
    return false;
}

std::size_t framed_size(std::span<const std::byte> input) {
    Cursor framing(input);
    const auto type = read_vi64(framing);
    require(std::holds_alternative<std::uint64_t>(type));
    const auto length = read_bytes(framing, 2);
    require(std::holds_alternative<std::span<const std::byte>>(length));
    const auto length_bytes =
        std::get<std::span<const std::byte>>(length);
    const auto payload_size =
        (static_cast<std::size_t>(
             std::to_integer<std::uint8_t>(length_bytes[0]))
         << 8u) |
        static_cast<std::size_t>(
            std::to_integer<std::uint8_t>(length_bytes[1]));
    return framing.offset() + payload_size;
}

void exercise_ambiguity_classification_oracle() {
    constexpr std::array rendezvous_timeout{
        std::byte{0x03}, std::byte{0x00}, std::byte{0x08}, std::byte{0x02},
        std::byte{0x01}, std::byte{0x01}, std::byte{'n'}, std::byte{0x01},
        std::byte{'t'}, std::byte{0x01}, std::byte{0x04},
    };
    constexpr std::array fill_timeout{
        std::byte{0x16}, std::byte{0x00}, std::byte{0x06},
        std::byte{0x0a}, std::byte{0x02}, std::byte{0x02},
        std::byte{0x05}, std::byte{0x01}, std::byte{0x0a},
    };
    for (const auto frame : {std::span<const std::byte>(rendezvous_timeout),
                             std::span<const std::byte>(fill_timeout)}) {
        Cursor cursor(frame, 31);
        const auto result = decode_message(StreamRole::Request, cursor, {});
        require(std::holds_alternative<DraftAmbiguity>(result));
        require(!std::get<DraftAmbiguity>(result).detail.empty());
        require(cursor.offset() == 31u);
    }
}

void exercise_role(std::span<const std::byte> fuzz_input, StreamRole role,
                   const Limits& limits) {
    std::vector<std::byte> owned_input(fuzz_input.begin(), fuzz_input.end());
    Cursor cursor(owned_input, 17);
    const auto result = decode_message(role, cursor, limits);
    if (!std::holds_alternative<Message>(result)) {
        require(cursor.offset() == 17u);
        if (const auto* ambiguity = std::get_if<DraftAmbiguity>(&result)) {
            require(!ambiguity->detail.empty());
        }
        if (const auto* error = std::get_if<DecodeError>(&result)) {
            require(!error->detail.empty());
        }
        return;
    }

    const auto& message = std::get<Message>(result);
    const auto consumed = cursor.offset() - 17u;
    require(consumed == framed_size(owned_input));
    require(consumed <= owned_input.size());

    std::fill(owned_input.begin(), owned_input.end(), std::byte{0xa5});

    ByteWriter canonical(kMaximumFrameSize);
    const auto encoded = encode_message(message, canonical);
    if (!encoded.has_value()) {
        require(has_duplicate_nonrepeatable_setup_option(message));
        require(encoded.error() != nullptr);
        require(encoded.error()->code == EncodeErrorCode::InvalidValue);
        require(canonical.bytes().empty());
        return;
    }
    require(!canonical.bytes().empty());
    require(canonical.size() <= kMaximumFrameSize);

    Cursor round_trip(canonical.bytes(), 23);
    const auto round_trip_result = decode_message(role, round_trip, limits);
    require(std::holds_alternative<Message>(round_trip_result));
    require(round_trip.offset() == 23u + canonical.size());
    require(round_trip.remaining() == 0u);

    std::vector<std::byte> concatenated(canonical.bytes().begin(),
                                        canonical.bytes().end());
    concatenated.insert(concatenated.end(), canonical.bytes().begin(),
                        canonical.bytes().end());
    Cursor sequence(concatenated);
    const auto first = decode_message(role, sequence, limits);
    require(std::holds_alternative<Message>(first));
    require(sequence.remaining() == canonical.size());
    const auto second = decode_message(role, sequence, limits);
    require(std::holds_alternative<Message>(second));
    require(sequence.remaining() == 0u);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data,
                                      std::size_t size) {
    exercise_ambiguity_classification_oracle();
    const auto input = std::as_bytes(std::span(data, size));
    Limits limits;
    if (size != 0) {
        limits.maximum_odd_value_length =
            static_cast<std::size_t>(data[0]) * 257u;
    }
    if (size > 1) {
        limits.maximum_parameter_count =
            static_cast<std::size_t>(data[1]) * 128u + 127u;
    }
    exercise_role(input, StreamRole::Control, limits);
    exercise_role(input, StreamRole::Request, limits);
    return 0;
}
