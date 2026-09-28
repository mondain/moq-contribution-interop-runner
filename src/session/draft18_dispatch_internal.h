#pragma once

#include "moq/interop/session/publisher_session.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace moq::interop::session::detail {

struct StreamTypeResult {
    std::optional<PeerStreamKind> kind;
    std::optional<std::uint64_t> raw_type;
    bool need_more{false};
    std::size_t consumed{0};
};

StreamTypeResult classify_unidirectional_stream(
    std::span<const std::byte> bytes);
bool is_known_nonrepeatable_setup_option(std::uint64_t type) noexcept;

}  // namespace moq::interop::session::detail
