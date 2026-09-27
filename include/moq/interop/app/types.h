#pragma once

#include <chrono>
#include <string>
#include <vector>

namespace moq::interop::app {

enum class DraftVersion : unsigned { Draft18 = 18, Draft21 = 21 };
enum class TransportKind { NativeQuic, WebTransport };
enum class RunMode { Observed, Driven };

using RunId = std::string;

struct RunConfig {
    DraftVersion draft;
    TransportKind transport;
    RunMode mode;
    std::vector<std::string> scenario_ids;
    std::chrono::milliseconds timeout;
};

}  // namespace moq::interop::app
