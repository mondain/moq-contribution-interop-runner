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
// Endpoint forms (L1e decision): native QUIC moql://host:port/moq?token=l1d, WebTransport
// https://host:port/moq?token=l1d. The session path kLiteSessionPath ("/moq") and query kLiteSessionQuery
// ("token=l1d") are fixed constants of unreserved characters only (row L06-7-3-2-MUST-120 is an exact byte match of
// path + "?" + query), defined once in src/app/lite_run.cpp. They are the driver endpoint, the WebTransport CONNECT
// :path ("/moq?token=l1d", which the listener matches exactly) and the probe's session_url_has_path/path/query on
// both transports, so on native QUIC rows 120/124 are judged and on WebTransport row 125.
//
// A refused WebTransport CONNECT is named in the context's harness_error as 'refused CONNECT: validator_status=N ...'
// (the validator's decision; the wire status is the HTTP/3 stack's). The probe engine's runner duties (FIN of the
// runner's send side after the publisher's end, PROTOCOL_VIOLATION close for a Path on WebTransport) and the
// Group-payload elision are described in docs/scenario-reference.md (moq-lite-06 session URL, runner duties).

#include "moq/interop/app/native_run_manager.h"
#include "moq/interop/app/types.h"
#include "moq/interop/requirements/catalog.h"
#include "moq/interop/scenarios/lite_probe.h"
#include "moq/interop/storage/run_store.h"
#include "moq/interop/transport/session_transport.h"

#include <atomic>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace moq::interop::app {

// The fixed session path and query of every lite endpoint (see the endpoint forms above).
extern const std::string_view kLiteSessionPath;
extern const std::string_view kLiteSessionQuery;

// The session URL a lite publisher is given on `transport` (see the endpoint forms above): the same on both.
struct LiteSessionUrl {
    bool has_path{false};
    std::string path;
    std::string query;
};
LiteSessionUrl lite_session_url(TransportKind transport);

// The request target of the session URL: path + "?" + query ("/moq?token=l1d"). The WebTransport listener of a lite
// run accepts a CONNECT to exactly this :path.
std::string lite_session_target(TransportKind transport);

// moql://authority/moq?token=l1d (native QUIC) or https://authority/moq?token=l1d (WebTransport); `authority` is
// host:port, an IPv6 host already bracketed.
std::string lite_endpoint_uri(TransportKind transport, std::string_view authority);

// The moq-lite-06 catalog for startup (src/app/main.cpp), optional: requirements_root/moq-lite-06.json loaded with
// CatalogLoadMode::AllowIncomplete against the digest-verified draft source 106 in docs_root. A missing catalog file,
// a missing draft text or digest entry, or a digest mismatch leaves moq-lite unavailable: nullptr, with one line on
// `log` saying why (the runner then serves MoQ Transport only). A catalog file that is present but does not load is a
// startup error like the other catalogs: the loader's exception propagates.
std::shared_ptr<const requirements::RequirementCatalog> load_lite_catalog_if_available(
    const std::filesystem::path& docs_root, const std::filesystem::path& requirements_root,
    const std::filesystem::path& digest_file, std::ostream& log);

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
    // Optional (tests): called on the worker thread after each context was judged, with the number of contexts the
    // run now holds verdicts for and the bytes those verdicts hold (requirements::lite_retained_bytes). A context's
    // transcript is judged (requirements::judge_lite_context) and dropped when the context ends; only the verdicts
    // stay until finalize.
    std::function<void(std::size_t contexts, std::size_t retained_bytes)> on_context_judged{};
};

// The worker body: every selected scenario as one context (each judged as it ends), then aggregate_lite ->
// score_staged -> store.finalize (the outcomes evaluate_lite gives the same transcripts).
// Never throws; always finalizes the run. The verdict is Error when any context recorded a harness_error or the
// run was stopped. A context whose publisher never connected (or never negotiated moq-lite-06) within the timeout
// stores harness_error and ends the run: the contexts after it store context_skipped.
void run_lite(LiteRunEnvironment& environment, std::unique_ptr<transport::SessionTransport> listener,
              const RunConfig& config);

}  // namespace moq::interop::app
