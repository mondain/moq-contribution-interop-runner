#pragma once

#include "moq/interop/session/draft21_request_ids.h"
#include "moq/interop/transport/session_transport.h"
#include "moq/interop/wire/draft21/publish.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace moq::interop::session::draft21 {

struct PublishNamespaceOpen {
    std::uint64_t request_id{0};
    std::vector<std::vector<std::byte>> track_namespace;
};

struct PublishOpenResult {
    std::optional<wire::draft21::PublishMessage> publish;
    std::optional<PublishNamespaceOpen> publish_namespace;
    std::optional<std::uint64_t> close_error;
    bool harness_limit = false;
    bool invalid_first_message = false;
    bool unsupported_message = false;
    bool unsupported_followup = false;
    bool incomplete_request = false;
    // Wire decoder detail when close_error stems from a malformed PUBLISH or
    // PUBLISH_NAMESPACE (for example an empty namespace field).
    std::optional<std::string> decode_detail;
};

// Decodes only the first PUBLISH on client-initiated request streams.
// Unsupported known request types and later messages are surfaced, never
// silently treated as valid publisher behavior.
class PublishOpenState {
public:
    explicit PublishOpenState(std::size_t maximum_streams = 4096,
                              std::size_t maximum_buffer_bytes = 65'546);

    PublishOpenResult on_client_stream(transport::StreamId stream_id,
                                       std::span<const std::byte> data,
                                       bool fin);

private:
    struct StreamState {
        std::vector<std::byte> pending;
        bool opening_processed = false;
    };

    std::size_t maximum_streams_;
    std::size_t maximum_buffer_bytes_;
    RequestIds request_ids_;
    std::map<transport::StreamId, StreamState> streams_;
};

}  // namespace moq::interop::session::draft21
