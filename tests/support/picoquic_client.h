#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moq::interop::transport::test {

enum class ClientStreamSendStatus {
    Success,
    Partial,
    WouldBlock,
    PeerStopped,
    PeerReset,
    Error,
};

struct ClientStreamSendResult {
    ClientStreamSendStatus status = ClientStreamSendStatus::Error;
    std::size_t accepted = 0;
    std::uint64_t application_error = 0;
};

struct ClientStreamObservation {
    std::vector<std::byte> data;
    std::vector<std::size_t> chunk_sizes;
    bool fin = false;
    std::size_t fin_count = 0;
    std::optional<std::uint64_t> reset_error;
};

struct ClientCloseObservation {
    bool application = false;
    std::uint64_t error_code = 0;
    std::vector<std::byte> reason;
};

class PicoquicTestClient {
public:
    struct Config {
        std::string host = "127.0.0.1";
        std::uint16_t port = 0;
        std::vector<std::byte> alpn;
        bool enable_datagrams = true;
        std::size_t datagram_queue = 16;
        std::uint64_t initial_max_streams_bidi = 64;
        std::uint64_t initial_max_streams_uni = 64;
    };

    static std::unique_ptr<PicoquicTestClient> create(const Config& config);
    ~PicoquicTestClient();

    bool pump();
    bool established() const;
    bool send_stream(std::uint64_t stream_id,
                     std::span<const std::byte> data, bool fin);
    ClientStreamSendResult try_send_stream(std::uint64_t stream_id,
                                           std::span<const std::byte> data,
                                           bool fin);
    bool send_datagram(std::span<const std::byte> data);
    bool send_invalid_transport_frame();
    bool send_transport_close_frame();
    bool reset_stream(std::uint64_t stream_id, std::uint64_t application_error);
    bool stop_stream(std::uint64_t stream_id, std::uint64_t application_error);
    std::size_t available_destination_ids() const;
    std::optional<std::uint64_t> migrate_source();
    bool retire_destination_id(std::uint64_t sequence);
    std::optional<ClientStreamObservation> stream(
        std::uint64_t stream_id) const;
    std::vector<std::vector<std::byte>> take_datagrams();
    std::optional<ClientCloseObservation> peer_close() const;
    bool close(std::uint64_t application_error,
               std::span<const std::byte> reason);

private:
    struct Impl;
    explicit PicoquicTestClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::transport::test
