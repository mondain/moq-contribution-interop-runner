// Guard: a draft 22 run writes draft 22 LOCATION_FILTER parameters.
//
// Every scenario in requirements::draft21_location_filter_scenarios() that builds a filter is resolved
// exactly as NativeRunManager::start() resolves a draft 22 lineage run (the draft 21 implementation,
// built for a draft 22 peer), and every request message it writes is read back with the shared
// wire-aware parameter walk under wire draft 22. Each filter must decode as a draft 22 Location Filter
// (Type + fields, no Length), and every scenario must write at least one filter.
//
// Resolving is where a run builds these bytes, so this covers every listed scenario without a session.
// All but the residual overflow pair are shared in the draft 22 lineage and run as draft 22 runs; one of
// them is driven live through the fake-publisher harness in raw_family_driver_test.cpp
// (NativeRunManagerDraft22Lineage.SharedFetchProbeWritesTheDraft22FilterOnTheWire). Writes prepared at run
// time from the publisher's replies (RawProbeWrite::prepare_bytes) are not seen here;
// filter_source_guard_test.cpp covers their builders.
//
// Where the draft 21 and draft 22 bytes coincide (a field count that equals a draft 22 Type and
// single-byte fields) this cannot tell the two forms apart; tests/unit/filter_source_guard_test.cpp
// guards those sites statically.
#include "moq/interop/app/lineage.h"
#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/scenario_registry.h"
#include "moq/interop/requirements/lineage_policy.h"
#include "moq/interop/scenarios/parameter_walk.h"
#include "moq/interop/scenarios/wire_draft.h"
#include "moq/interop/wire/cursor.h"
#include "moq/interop/wire/draft22/location_filter.h"

#include <gtest/gtest.h>

#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <variant>
#include <vector>

namespace moq::interop::app {
namespace {

using scenarios::ParameterValueKind;
using scenarios::ParameterWalkStatus;
using scenarios::RawProbeChannel;
using scenarios::RawProbeDefinition;
using scenarios::ScopedWireDraft;
using scenarios::WalkedParameter;

constexpr std::uint64_t kLocationFilter = 0x21;
constexpr std::uint64_t kFillParameters = 0x23;

// draft21_close.cpp leaves these out of the probe list under wire 22: {u64max,0,1} overflows
// StartGroup + EndGroupDelta, which the draft 22 form cannot carry. They are the lineage residual
// (draft22_filter_building_scenarios()) and own by row D22-9-20-9-MUST-424; draft 22 probes replace them.
const std::set<std::string> kUnrepresentable{
    "d21-location-filter-end-group-overflow",
    "d21-fill-location-filter-end-group-overflow",
};

// The (b) entries: they read a received LOCATION_FILTER (parse_block / walk_parameters, covered by
// tests/unit/parameter_walk_test.cpp) and need not write one. What they do write is still checked.
const std::set<std::string> kReadersOnly{
    "d21-publish-state-notify-known-largest-object",
    "d21-publish-state-notify-before-first-object",
    "d21-publish-state-notify-preserves-subscriber-control",
    "d21-publish-state-notify-requested-forward-change",
    "d21-publisher-parameter-serialization",
    "d21-publisher-parameter-negotiation",
    "d21-publisher-parameter-multiplicity",
};

RunConfig run_config(std::string_view id) {
    return RunConfig{DraftVersion::Draft21, TransportKind::NativeQuic, RunMode::Observed,
                     {std::string(id)}, std::chrono::milliseconds(1000), TrackFixture{{"moq", "test"}, "video"}};
}

std::string hex(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : bytes) {
        out.push_back(digits[std::to_integer<unsigned>(byte) >> 4u]);
        out.push_back(digits[std::to_integer<unsigned>(byte) & 0xfu]);
    }
    return out;
}

template <class T>
bool take(wire::DecodeResult<T> result, T& out) {
    if (const auto* value = std::get_if<T>(&result)) {
        out = *value;
        return true;
    }
    return false;
}

bool read_vi(wire::Cursor& cursor, std::uint64_t& out) { return take(wire::read_vi64(cursor), out); }

bool skip(wire::Cursor& cursor, std::uint64_t size) {
    std::span<const std::byte> ignored;
    return take(wire::read_bytes(cursor, static_cast<std::size_t>(size)), ignored);
}

// Request ID, Track Namespace (field count, length-prefixed fields), Track Name: SUBSCRIBE and FETCH
// (draft 21 and draft 22 sections 9.7 and 9.11 have the same layout).
bool skip_track_request_head(wire::Cursor& cursor) {
    std::uint64_t value = 0;
    if (!read_vi(cursor, value)) return false;  // Request ID
    std::uint64_t fields = 0;
    if (!read_vi(cursor, fields) || fields > 32) return false;
    for (std::uint64_t index = 0; index < fields; ++index)
        if (!read_vi(cursor, value) || !skip(cursor, value)) return false;
    return read_vi(cursor, value) && skip(cursor, value);  // Track Name
}

// What a scenario's written request messages carry, read as a draft 22 peer reads them.
struct Findings {
    std::vector<std::string> filters;   // hex of each filter value (Type + fields), with where it was
    std::vector<std::string> problems;  // a request message whose parameters do not walk as draft 22
};

void collect_filters(const WalkedParameter& parameter, const std::string& where, Findings& findings) {
    if (parameter.type == kLocationFilter) {
        if (parameter.kind != ParameterValueKind::LocationFilter) {
            findings.problems.push_back(where + ": 0x21 not read as a draft 22 Location Filter");
            return;
        }
        // The walk accepted it; decode once more to state the guard explicitly.
        wire::Cursor cursor(parameter.value);
        const auto decoded = wire::draft22::decode_location_filter(cursor);
        if (!std::holds_alternative<wire::draft22::LocationFilter>(decoded) || cursor.remaining() != 0)
            findings.problems.push_back(where + ": filter " + hex(parameter.value) + " is not a draft 22 filter");
        else
            findings.filters.push_back(where + " " + hex(parameter.value));
        return;
    }
    if (parameter.type == kFillParameters) {
        const auto nested = scenarios::walk_nested_parameters(parameter.payload, [&](const WalkedParameter& inner) {
            collect_filters(inner, where + " in FILL_PARAMETERS", findings);
        });
        if (nested.status != ParameterWalkStatus::Complete)
            findings.problems.push_back(where + ": FILL_PARAMETERS " + hex(parameter.payload) +
                                        " does not walk as draft 22 parameters");
    }
}

// Walks the parameters of one framed request message (Type, 16-bit Length, body). Only the messages
// a subscriber sends with a LOCATION_FILTER are read: REQUEST_UPDATE 0x2, SUBSCRIBE 0x3, FETCH 0x16.
void inspect_message(std::uint64_t type, std::span<const std::byte> body, const std::string& where,
                     Findings& findings) {
    wire::Cursor cursor(body);
    if (type == 0x3 || type == 0x16) {
        if (!skip_track_request_head(cursor)) return;  // a deliberately broken head carries no filter
    } else if (type == 0x2) {
        std::uint64_t request_id = 0;
        if (!read_vi(cursor, request_id)) return;
    } else {
        return;
    }
    std::uint64_t count = 0;
    if (!read_vi(cursor, count)) return;
    Findings local;
    const auto walked = scenarios::walk_message_parameters(cursor, count, [&](const WalkedParameter& parameter) {
        collect_filters(parameter, where, local);
    });
    const bool carries_filter = !local.filters.empty() || !local.problems.empty();
    if (walked.status != ParameterWalkStatus::Complete || cursor.remaining() != 0) {
        // A walk that stops at 0x21, or that ran past a draft 21 Length it misread, is the bypass this
        // guards against. A stimulus that is malformed elsewhere and carries no filter is not ours.
        if (carries_filter || walked.failed_type == kLocationFilter || walked.failed_type == kFillParameters) {
            std::ostringstream problem;
            problem << where << ": parameters " << hex(body) << " do not walk as draft 22 (status "
                    << static_cast<int>(walked.status) << ", remaining " << cursor.remaining() << ")";
            findings.problems.push_back(problem.str());
        }
        return;
    }
    findings.filters.insert(findings.filters.end(), local.filters.begin(), local.filters.end());
    findings.problems.insert(findings.problems.end(), local.problems.begin(), local.problems.end());
}

void inspect_bytes(std::span<const std::byte> bytes, const std::string& where, Findings& findings) {
    wire::Cursor cursor(bytes);
    for (unsigned frame = 0; cursor.remaining() != 0; ++frame) {
        std::uint64_t type = 0;
        if (!read_vi(cursor, type)) return;
        std::span<const std::byte> prefix;
        if (!take(wire::read_bytes(cursor, 2), prefix)) return;
        const auto size = (std::to_integer<std::size_t>(prefix[0]) << 8u) | std::to_integer<std::size_t>(prefix[1]);
        std::span<const std::byte> body;
        if (!take(wire::read_bytes(cursor, size), body)) return;  // a truncated tail is a deliberate stimulus
        std::ostringstream label;
        label << where << " message " << frame << " (type 0x" << std::hex << type << ")";
        inspect_message(type, body, label.str(), findings);
    }
}

// Every request message a definition writes (and its liveness follow-up), read under wire draft 22.
Findings inspect(const RawProbeDefinition& definition) {
    Findings result;
    const ScopedWireDraft wire22(22);
    for (std::size_t index = 0; index < definition.writes.size(); ++index) {
        const auto& write = definition.writes[index];
        // Datagrams and new unidirectional streams carry Objects; the rest carry no bytes.
        if (write.channel != RawProbeChannel::NewBidi && write.channel != RawProbeChannel::Control &&
            write.channel != RawProbeChannel::PeerBidi)
            continue;
        inspect_bytes(write.bytes, "write " + std::to_string(index), result);
    }
    if (definition.liveness) inspect_bytes(definition.liveness->request, "liveness request", result);
    return result;
}

TEST(Draft22FilterGuard, ResolvingForAWireDraftBuildsItsFilterForm) {
    // fetch_probe's {0,0,u64max} is where the two forms differ: draft 21 writes Length 0x0b, draft 22
    // writes Type 3 (AbsoluteBounded). The wire-aware resolve start() uses must pick the peer's form.
    const std::string id = "d21-fetch-accepted";
    const auto as21 = NativeRunManager::resolve_probe({}, run_config(id), id, DraftVersion::Draft21);
    const auto as22 = NativeRunManager::resolve_probe({}, run_config(id), id, DraftVersion::Draft22);
    ASSERT_TRUE(as21 && as22);
    const auto findings21 = inspect(*as21);
    const auto findings22 = inspect(*as22);
    EXPECT_TRUE(findings22.problems.empty()) << ::testing::PrintToString(findings22.problems);
    ASSERT_EQ(findings22.filters.size(), 1u) << ::testing::PrintToString(findings22.filters);
    EXPECT_NE(findings22.filters.front().find(" 0300"), std::string::npos) << findings22.filters.front();
    // The draft 21 bytes are refused by the draft 22 reading: the guard below would catch a bypass here.
    EXPECT_FALSE(findings21.problems.empty()) << ::testing::PrintToString(findings21.filters);
    EXPECT_EQ(scenarios::current_wire_draft(), 21u) << "the resolve restores the caller's wire draft";
}

TEST(Draft22FilterGuard, OnlyTheUnrepresentableOverflowProbesStayOwnByFilter) {
    // The residual set is exactly the overflow pair, and every other listed scenario is shared: its draft
    // 22 id runs the draft 21 implementation, which this file shows writes draft 22 filters.
    const auto residual = requirements::draft22_filter_building_scenarios();
    EXPECT_EQ(residual, kUnrepresentable);
    const auto listed = requirements::draft21_location_filter_scenarios();
    EXPECT_EQ(listed.size(), 50u);
    for (const auto& id : residual) EXPECT_TRUE(listed.contains(id)) << id;
    // Own by row D22-3-3-1-MUST-NOT-069 (their rows changed their obligation), not by filter.
    const std::set<std::string> own_by_row{"d21-subscribe-bounded-location-range",
                                           "d21-update-subscription-location-range"};
    for (const auto& id : listed) {
        const auto d22 = "d22-" + id.substr(4);
        const bool own = residual.contains(id) || own_by_row.contains(id);
        EXPECT_EQ(executable_scenario(22, d22), !own) << d22;
        if (!own) EXPECT_EQ(implementation_scenario_id(d22), std::optional<std::string_view>(id)) << d22;
    }
}

TEST(Draft22FilterGuard, EveryFilterBuildingScenarioWritesDraft22Filters) {
    const auto listed = requirements::draft21_location_filter_scenarios();
    for (const auto& id : kUnrepresentable) EXPECT_TRUE(listed.contains(id)) << id << " is no longer listed";
    for (const auto& id : kReadersOnly) EXPECT_TRUE(listed.contains(id)) << id << " is no longer listed";
    for (const auto& id : listed) {
        SCOPED_TRACE(id);
        ASSERT_TRUE(executable_scenario(21, id));
        ASSERT_TRUE(raw_probe_scenario(21, id)) << "only raw probes build filters; a typed one needs a harness";
        const auto definition = NativeRunManager::resolve_probe({}, run_config(id), id, DraftVersion::Draft22);
        if (kUnrepresentable.contains(id)) {
            EXPECT_FALSE(definition.has_value()) << "an overflow probe must not run under wire draft 22";
            continue;
        }
        ASSERT_TRUE(definition.has_value());
        // Every write is built up front except where a write is prepared from the publisher's replies
        // (prepare_bytes); those builders go through filter_param_value too (filter_site_pins_test.cpp,
        // filter_source_guard_test.cpp).
        const auto findings = inspect(*definition);
        EXPECT_TRUE(findings.problems.empty())
            << "a filter is written in draft 21 form under wire draft 22; route the site through "
               "scenarios::filter_param_value: "
            << ::testing::PrintToString(findings.problems);
        if (!kReadersOnly.contains(id))
            EXPECT_FALSE(findings.filters.empty())
                << "writes no LOCATION_FILTER under wire draft 22: it no longer exercises a filter";
    }
}

}  // namespace
}  // namespace moq::interop::app
