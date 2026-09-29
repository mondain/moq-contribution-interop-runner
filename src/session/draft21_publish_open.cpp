#include "moq/interop/session/draft21_publish_open.h"

#include "moq/interop/wire/draft21/message_types.h"

#include <utility>
#include <variant>

namespace moq::interop::session::draft21 {
namespace {

constexpr std::uint64_t kProtocolViolation = 0x3;
constexpr std::uint64_t kInvalidRequestId = 0x4;

}  // namespace

PublishOpenState::PublishOpenState(std::size_t maximum_streams,
                                   std::size_t maximum_buffer_bytes)
    : maximum_streams_(maximum_streams),
      maximum_buffer_bytes_(maximum_buffer_bytes),
      request_ids_(maximum_streams) {}

PublishOpenResult PublishOpenState::on_client_stream(
    transport::StreamId stream_id, std::span<const std::byte> data, bool fin) {
    PublishOpenResult result;
    if ((stream_id & 3u) != 0u) {
        result.close_error = kProtocolViolation;
        return result;
    }
    auto found = streams_.find(stream_id);
    if (found == streams_.end()) {
        if (streams_.size() >= maximum_streams_) {
            result.harness_limit = true;
            return result;
        }
        found = streams_.emplace(stream_id, StreamState{}).first;
    }
    auto& stream = found->second;
    if (stream.opening_processed) {
        result.unsupported_followup = !data.empty() || fin;
        return result;
    }
    if (stream.pending.size() > maximum_buffer_bytes_ ||
        data.size() > maximum_buffer_bytes_ - stream.pending.size()) {
        result.harness_limit = true;
        return result;
    }
    stream.pending.insert(stream.pending.end(), data.begin(), data.end());
    wire::Cursor type_cursor(stream.pending);
    const auto type = wire::draft21::decode_message_type(
        type_cursor, wire::draft21::StreamRole::Request, true);
    if (std::holds_alternative<wire::NeedMore>(type)) {
        result.incomplete_request = fin;
        if (fin) stream.opening_processed = true;
        return result;
    }
    if (std::holds_alternative<wire::DecodeError>(type)) {
        result.close_error = kProtocolViolation;
        return result;
    }
    if (std::get<wire::draft21::MessageTypeInfo>(type).kind !=
        wire::draft21::MessageKind::Publish) {
        result.unsupported_message = true;
        stream.opening_processed = true;
        return result;
    }
    wire::Cursor cursor(stream.pending);
    const auto decoded = wire::draft21::decode_publish(cursor);
    if (std::holds_alternative<wire::NeedMore>(decoded)) {
        result.incomplete_request = fin;
        if (fin) stream.opening_processed = true;
        return result;
    }
    if (std::holds_alternative<wire::DecodeError>(decoded)) {
        result.close_error = kProtocolViolation;
        return result;
    }
    auto publish = std::get<wire::draft21::PublishMessage>(decoded);
    switch (request_ids_.observe(publish.request_id, Initiator::Client)) {
    case RequestIdResult::InvalidRequestId:
        result.close_error = kInvalidRequestId;
        return result;
    case RequestIdResult::HarnessLimit:
        result.harness_limit = true;
        return result;
    case RequestIdResult::Accepted:
        break;
    }
    result.publish = std::move(publish);
    result.unsupported_followup = cursor.remaining() != 0;
    stream.pending.clear();
    stream.opening_processed = true;
    return result;
}

}  // namespace moq::interop::session::draft21
