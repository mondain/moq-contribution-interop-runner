#pragma once

// The command line of moq-interop-lite-ref-publisher (L2c): the conforming moq-lite-06 reference publisher, with one
// named defect from the scripted publisher's table (tests/support/scripted_lite_peer.h) selectable for the negative
// sweep. Parsing is pure: an unknown flag, an unknown defect or a bad endpoint is an error text, never a default.

#include "support/scripted_lite_peer.h"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace moq::interop::lite_ref {

struct Options {
    std::string connect;                                   // moql://HOST:PORT/PATH?QUERY or https://HOST:PORT/PATH?QUERY
    test::lite::ConformingLitePublisherConfig publisher;   // defect, datagrams, Probe level, frames, session URL, binding
    std::string defect;                                    // as given, "none" when absent
    bool help{false};
};

struct ParseResult {
    std::optional<Options> options;
    std::string error;  // non-empty: a usage error
};

// The broadcast and track this publisher serves: the fixture the moq-lite adapters pin.
inline constexpr std::string_view kFixtureBroadcast = "interop.hang";
inline constexpr std::string_view kFixtureTrack = "0.m4s";

ParseResult parse_options(std::span<const std::string_view> args);
std::string usage();

std::string_view defect_name(test::lite::LiteDefect defect);                       // kebab-case, "none" for None
std::optional<test::lite::LiteDefect> defect_from_name(std::string_view name);     // "none" gives None
std::vector<std::string_view> all_defect_names();                                  // every enumerator except None

}  // namespace moq::interop::lite_ref
