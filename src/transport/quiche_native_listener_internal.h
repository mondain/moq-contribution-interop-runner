#pragma once

#include "moq/interop/transport/native_quic_listener.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <sys/socket.h>

namespace moq::interop::transport::detail {

struct TokenAddress {
    std::uint8_t family = 0;
    std::array<std::byte, 16> bytes{};
    std::uint8_t length = 0;
    std::uint16_t port = 0;

    bool operator==(const TokenAddress&) const = default;
};

struct RetryTokenValue {
    std::vector<std::byte> original_dcid;
    std::vector<std::byte> retry_scid;
};

class ActiveCidRoutes {
public:
    bool add(std::span<const std::uint8_t> cid);
    bool contains(std::span<const std::uint8_t> cid) const;
    bool synchronize(std::span<const std::uint8_t> current,
                     std::size_t active_count);
    std::size_t size() const noexcept { return routes_.size(); }

private:
    std::vector<std::vector<std::uint8_t>> routes_;
};

class RetryTokenCodec {
public:
    explicit RetryTokenCodec(std::array<std::byte, 32> secret);

    std::optional<std::vector<std::byte>> encode(
        std::uint64_t issue_time_seconds, const TokenAddress& address,
        std::span<const std::byte> original_dcid,
        std::span<const std::byte> retry_scid) const;
    std::optional<RetryTokenValue> validate(
        std::span<const std::byte> token, std::uint64_t now_seconds,
        std::uint64_t lifetime_seconds, const TokenAddress& address,
        std::span<const std::byte> current_dcid) const;

private:
    std::array<std::byte, 32> secret_;
};

struct NativeListenerDependencies {
    struct Socket {
        std::function<int(int, int, int)> open;
        std::function<int(int)> close;
        std::function<int(int, const sockaddr*, socklen_t)> bind;
        std::function<int(int, sockaddr*, socklen_t*)> local_address;
        std::function<ssize_t(int, msghdr*, int)> receive_message;
        std::function<ssize_t(int, const void*, std::size_t, int,
                              const sockaddr*, socklen_t)> send_datagram;
    } socket;
    std::function<bool(std::span<std::byte>)> random_bytes;
    std::function<std::uint64_t()> unix_seconds;
    std::function<std::uint64_t()> monotonic_nanoseconds;
    std::function<bool(const BoundEndpoint&)> after_bind;
};

NativeListenerDependencies default_native_listener_dependencies();
NativeQuicListenerCreateResult create_native_quic_listener(
    NativeQuicListenerConfig config, NativeListenerDependencies dependencies);

}  // namespace moq::interop::transport::detail
