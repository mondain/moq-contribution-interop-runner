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

// What the publisher under test implements. A live publisher with no cache may
// omit FETCH (draft 18 Section 4 and draft 21 Section 1.5: endpoints other than
// relays MAY implement a subset), so an absent feature is a declaration, not a
// failure. Every capability defaults to present; see docs/http-api.md.
struct PublisherCapabilities {
    bool fetch{true};
    friend bool operator==(const PublisherCapabilities&, const PublisherCapabilities&) = default;
};

struct RunConfig {
    DraftVersion draft;
    TransportKind transport;
    RunMode mode;
    std::vector<std::string> scenario_ids;
    std::chrono::milliseconds timeout;
    std::optional<TrackFixture> track_fixture;
    // The effective declaration for this run (per-run value, else the startup default).
    PublisherCapabilities publisher_capabilities{};
};

}  // namespace moq::interop::app
