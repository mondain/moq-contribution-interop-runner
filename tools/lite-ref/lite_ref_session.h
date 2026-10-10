#pragma once

// The moq-lite-06 reference publisher's sessions (L2c): a draft-conforming publisher (ConformingLitePublisher, with
// the one named defect Options asks for) that DIALS the runner over native QUIC or WebTransport. The publisher logic
// reacts to an in-memory mirror (a ScriptedLitePeer) of what the runner did on the wire; what it emits is written to
// the real connection. The same drivers serve the live tests (tests/support/lite_run_live.h) and the binary.

#include "lite_ref_options.h"
#include "support/picoquic_client.h"
#include "support/scripted_lite_peer.h"

#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>

namespace moq::interop::lite_ref {

// The session a reference publisher runs. step() pumps the connection once (non-blocking); it returns false once the
// connection failed or ended.
class RefSession {
public:
    struct Summary {
        std::string transport;  // native_quic | webtransport
        std::string defect;
        std::uint64_t groups_sent{0};
        std::uint64_t datagrams_sent{0};
        std::string close_reason;
    };

    virtual ~RefSession() = default;
    virtual bool step() = 0;
    [[nodiscard]] virtual bool established() const = 0;
    [[nodiscard]] virtual Summary summary() const = 0;

    // Dials options.connect (moql:// native QUIC, https:// WebTransport). nullptr with `error` set when the endpoint
    // cannot be dialed at all (a refused or timed-out handshake shows later as step() == false before established()).
    static std::unique_ptr<RefSession> dial(const Options& options, std::string& error);
};

// Native QUIC: a picoquic client, ALPN moq-lite-06 (or `alpn`). Public because the live tests drive it directly.
class QuicDriver final : public RefSession {
public:
    using Client = transport::test::PicoquicTestClient;
    QuicDriver(std::string host, std::uint16_t port, test::lite::ConformingLitePublisherConfig config,
               std::string_view alpn = scenarios::kLiteAlpn);
    // The loopback form the tests use.
    QuicDriver(std::uint16_t port, test::lite::ConformingLitePublisherConfig config,
               std::string_view alpn = scenarios::kLiteAlpn)
        : QuicDriver("127.0.0.1", port, std::move(config), alpn) {}

    [[nodiscard]] bool valid() const { return client_ != nullptr; }
    [[nodiscard]] bool established() const override { return client_ && client_->established(); }
    [[nodiscard]] const test::lite::ConformingLitePublisher& publisher() const { return publisher_; }
    [[nodiscard]] Client& client() { return *client_; }
    bool step() override;
    [[nodiscard]] Summary summary() const override;

private:
    void mirror_runner();
    void flush();

    test::lite::ConformingLitePublisher publisher_;
    test::lite::ScriptedLitePeer mirror_;
    std::unique_ptr<Client> client_;
    std::deque<transport::TransportEvent> outbound_;
    std::map<std::uint64_t, std::size_t> forwarded_;
    std::set<std::uint64_t> fin_forwarded_, reset_forwarded_, stop_forwarded_;
    std::set<std::uint64_t> counted_groups_;
    std::uint64_t groups_sent_{0}, datagrams_sent_{0};
    std::string defect_;
    std::string close_reason_;
};

}  // namespace moq::interop::lite_ref
