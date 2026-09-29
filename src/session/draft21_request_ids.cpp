#include "moq/interop/session/draft21_request_ids.h"

namespace moq::interop::session::draft21 {

RequestIds::RequestIds(std::size_t maximum_entries)
    : maximum_entries_(maximum_entries) {}

RequestIdResult RequestIds::observe(std::uint64_t id, Initiator initiator) {
    const auto expected_parity = initiator == Initiator::Client ? 0u : 1u;
    if ((id & 1u) != expected_parity || seen_.contains(id)) {
        return RequestIdResult::InvalidRequestId;
    }
    if (seen_.size() >= maximum_entries_) {
        return RequestIdResult::HarnessLimit;
    }
    seen_.insert(id);
    return RequestIdResult::Accepted;
}

std::size_t RequestIds::size() const noexcept {
    return seen_.size();
}

}  // namespace moq::interop::session::draft21
