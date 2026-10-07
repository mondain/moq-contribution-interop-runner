#pragma once

#include <chrono>

namespace moq::interop::scenarios {

// Time allowances for the moq-lite-06 scenarios where a catalog row rationale gives no number (plan L1d, Global
// Constraints). Every scenario may override them for tests; the live RunConfig::timeout bounds the whole context.
inline constexpr std::chrono::milliseconds kLiteSetupAllowance{2000};
inline constexpr std::chrono::milliseconds kLiteResponseAllowance{3000};
inline constexpr std::chrono::milliseconds kLiteCloseAllowance{3000};
// Long enough to see at least two groups with the scripted peers.
inline constexpr std::chrono::milliseconds kLiteObservationWindow{6000};

}  // namespace moq::interop::scenarios
