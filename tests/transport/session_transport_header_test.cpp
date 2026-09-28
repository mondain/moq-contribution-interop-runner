#include "moq/interop/transport/session_transport.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <type_traits>

namespace moq::interop::transport {
namespace {

TEST(SessionTransportHeader, ExposesAStandardLibraryOnlyPolymorphicContract) {
    static_assert(std::is_same_v<StreamId, std::uint64_t>);
    static_assert(std::has_virtual_destructor_v<SessionTransport>);
    static_assert(std::is_move_constructible_v<TransportEvent>);
    static_assert(std::is_copy_constructible_v<TransportEvent>);
    SUCCEED();
}

}  // namespace
}  // namespace moq::interop::transport
