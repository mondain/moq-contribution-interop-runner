#pragma once

// The moq-lite-06 scenario family in the native run manager (L1d Task 9). NativeRunManager routes a MoqLite06
// RunConfig here (only when it was constructed with the moq-lite-06 catalog): start() validates the selection with
// lite_start_refusal, binds the run's listener and stores the run; the worker thread then runs run_lite, which
// executes one LiteProbeController context per selected scenario (one fresh listener and publisher session each),
// stores the evidence the lite bindings declare and finalizes the run with the staged score.
//
// Timing: RunConfig::timeout bounds each context's wait for the publisher's connection (connect_deadline) and,
// from establishment, the probe itself (the builders' deadline). A builder that cannot fit its stated allowances
// in the timeout throws std::invalid_argument; the context then stores a harness error and runs nothing.
//
// Endpoint forms are provisional (plan decision (e), finalized in L1e against the moq CLI): native QUIC
// moql://host:port (no path: the lite raw-QUIC binding has no request URI, so the client-path rows 120/124 stay
// NotRun), WebTransport https://host:port/moq (path "/moq", no query: rows 120/124 are native-only and 124/125
// need a query, so they stay NotRun too).

#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/types.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/storage/run_store.h"
#include "moq/interop/transport/session_transport.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace moq::interop::app {

// The session URL a lite publisher is given on `transport` (see the endpoint forms above).
struct LiteSessionUrl {
    bool has_path{false};
    std::string path;
    std::string query;
};
LiteSessionUrl lite_session_url(TransportKind transport);

// moql://authority (native QUIC) or https://authority/moq (WebTransport); `authority` is host:port, an IPv6 host
// already bracketed.
std::string lite_endpoint_uri(TransportKind transport, std::string_view authority);

// Why start() refuses a lite selection before binding anything, or nullopt when it may start. InvalidConfig: no ids
// or more than 100, an empty or repeated id, an id kLiteExecutableScenarios does not list, a timeout outside
// [2 ms, 1 h], a scenario with requires_track (or a Driven run, whose driver request names the track) without a
// track fixture, or a track fixture that is not a usable broadcast path and track. The supports()/driven checks
// come before it (NativeRunManager::start).
std::optional<RunStartStatus> lite_start_refusal(const RunConfig& config);

// The probe of scenario `id` for `config` on `transport`: the scenario's builder with config.timeout as its
// deadline, connect_deadline config.timeout, the binding, the session URL fields (lite_session_url) and the track
// fixture (namespace fields joined with '/' as the broadcast path, decision (d)). Throws std::invalid_argument for
// an unknown id or when the builder cannot fit its windows in the timeout.
scenarios::LiteProbeDefinition lite_probe_for(const RunConfig& config, std::string_view id);

struct LiteRunListener {
    std::unique_ptr<transport::SessionTransport> listener;
    std::string failure;  // why `listener` is empty
};

// What the worker needs from the manager.
struct LiteRunEnvironment {
    RunId id;
    // host:port the publisher dials (the advertised address when configured).
    std::string authority;
    storage::RunStore& store;
    const requirements::RequirementCatalog& catalog;
    const NativeRunManagerConfig& config;
    const std::atomic<bool>& stop_requested;
    // A fresh listener on the run's port for every context after the first.
    std::function<LiteRunListener()> recreate_listener;
};

// The worker body: every selected scenario as one context, then evaluate_lite -> score_staged -> store.finalize.
// Never throws; always finalizes the run. The verdict is Error when any context recorded a harness_error or the
// run was stopped. A context whose publisher never connected (or never negotiated moq-lite-06) within the timeout
// stores harness_error and ends the run: the contexts after it store context_skipped.
void run_lite(LiteRunEnvironment& environment, std::unique_ptr<transport::SessionTransport> listener,
              const RunConfig& config);

}  // namespace moq::interop::app
