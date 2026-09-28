#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moq::interop::transport::test {

class QuicheTestClient {
public:
    struct Config {
        std::string host = "127.0.0.1";
        std::uint16_t port = 0;
        std::vector<std::byte> alpn;
        bool enable_datagrams = true;
        std::size_t datagram_queue = 16;
    };

    static std::unique_ptr<QuicheTestClient> create(const Config& config);
    ~QuicheTestClient();

    bool pump();
    bool established() const;
    bool send_stream(std::uint64_t stream_id,
                     std::span<const std::byte> data, bool fin);
    bool send_datagram(std::span<const std::byte> data);
    bool reset_stream(std::uint64_t stream_id, std::uint64_t application_error);
    bool stop_stream(std::uint64_t stream_id, std::uint64_t application_error);
    std::size_t available_destination_ids() const;
    std::optional<std::uint64_t> migrate_source();
    bool retire_destination_id(std::uint64_t sequence);
    bool close(std::uint64_t application_error,
               std::span<const std::byte> reason);

private:
    struct Impl;
    explicit QuicheTestClient(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace moq::interop::transport::test
