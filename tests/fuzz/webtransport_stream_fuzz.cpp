#include "transport/webtransport_session.h"

#include <h3zero.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <span>

namespace {

int ignore_payload(picoquic_cnx_t*, std::uint8_t*, std::size_t,
                   picohttp_call_back_event_t, h3zero_stream_ctx_t*, void*) {
    return 0;
}

void check_stream(h3zero_callback_ctx_t* h3, const std::uint8_t* data,
                  std::size_t size, bool bidirectional, bool bytewise) {
    h3zero_stream_ctx_t stream{};
    stream.stream_id = bidirectional ? 4 : 2;
    stream.ps.stream_state.stream_type = std::numeric_limits<std::uint64_t>::max();
    stream.ps.stream_state.control_stream_id = std::numeric_limits<std::uint64_t>::max();
    std::size_t offset = 0;
    while (offset < size) {
        const auto chunk = bytewise ? std::size_t{1} : size - offset;
        const auto* begin = data + offset;
        const auto* payload = h3zero_parse_incoming_remote_stream(
            begin, begin + chunk, &stream, h3, nullptr);
        if (payload == nullptr) break;
        if (payload < begin || payload > begin + chunk) std::abort();
        if (stream.path_callback != nullptr &&
            stream.ps.stream_state.control_stream_id != 0) std::abort();
        offset += chunk;
    }
    h3zero_delete_data_stream_state(&stream.ps.stream_state);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > 4096) return 0;
    auto* h3 = h3zero_callback_create_context(nullptr);
    if (h3 == nullptr) return 0;
    if (h3zero_declare_stream_prefix(h3, 0, ignore_payload, nullptr) != 0)
        std::abort();
    check_stream(h3, data, size, true, true);
    check_stream(h3, data, size, false, true);
    check_stream(h3, data, size, true, false);
    check_stream(h3, data, size, false, false);

    moq::interop::transport::WebTransportSession session(
        0, {.max_events = 8, .max_event_payload_bytes = 256,
            .max_datagram_payload = 128});
    const auto payload = std::as_bytes(std::span(data, size));
    (void)session.ingest_stream(4, 0, payload, false);
    (void)session.ingest_datagram(0, payload);
    (void)session.poll(8);

    // A parser-only fixture has no connection for H3zero's deregistration callback.
    h3zero_find_stream_prefix(h3, 0)->function_call = nullptr;
    h3zero_callback_delete_context(nullptr, h3);
    return 0;
}
