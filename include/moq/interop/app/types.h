#pragma once

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace moq::interop::app {

enum class DraftVersion : unsigned { Draft18 = 18, Draft21 = 21 };
enum class TransportKind { NativeQuic, WebTransport };
enum class RunMode { Observed, Driven };

using RunId = std::string;

struct TrackFixture {
    std::vector<std::string> namespace_fields;
    std::string track_name;
};

struct RunConfig {
    DraftVersion draft;
    TransportKind transport;
    RunMode mode;
    std::vector<std::string> scenario_ids;
    std::chrono::milliseconds timeout;
    std::optional<TrackFixture> track_fixture;
};

}  // namespace moq::interop::app
