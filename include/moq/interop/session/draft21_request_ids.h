#pragma once

#include <cstddef>
#include <cstdint>
#include <unordered_set>

namespace moq::interop::session::draft21 {

enum class Initiator { Client, Server };

enum class RequestIdResult {
    Accepted,
    InvalidRequestId,
    HarnessLimit,
};

// Request IDs are session-wide, even when QUIC streams arrive out of order.
// The cap is a harness resource limit, never a peer protocol violation.
class RequestIds {
public:
    explicit RequestIds(std::size_t maximum_entries);

    RequestIdResult observe(std::uint64_t id, Initiator initiator);
    [[nodiscard]] std::size_t size() const noexcept;

private:
    std::size_t maximum_entries_;
    std::unordered_set<std::uint64_t> seen_;
};

}  // namespace moq::interop::session::draft21
