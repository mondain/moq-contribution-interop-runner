#pragma once

#include "moq/interop/requirements/completeness.h"
#include "moq/interop/requirements/scoring.h"
#include "moq/interop/session/draft23_session.h"

#include <optional>
#include <string>
#include <vector>

namespace moq::interop::requirements {

// What the SETUP evaluators need: the publisher's SETUP as received (absent when none arrived) and the
// endpoint the runner advertised to the publisher. `authority` is the URI authority, `path` the path-abempty
// and `query` the query without its question mark.
struct Draft23Evidence {
    std::optional<session::Draft23PeerSetup> peer_setup;
    bool webtransport{false};
    std::string authority;
    std::string path;
    std::optional<std::string> query;
    std::vector<session::Draft23DataStream> data_streams;
    std::vector<session::Draft23RequestStream> requests;
    std::optional<std::uint64_t> subscription_alias;
    std::string scenario;  // the scenario this run executed; empty means judge every row on the evidence
    std::optional<std::string> probe_scenario;               // set on a negative probe run
    std::optional<session::Draft23ProbeResult> probe;
    std::vector<session::Draft23PublishedTrack> published_tracks;
    std::vector<std::string> announced_namespaces;
    bool connected{false};                  // the transport connection was established
    std::uint64_t max_datagram_payload{0};  // 0 with `connected` means QUIC DATAGRAM was not negotiated
    bool empty_track_name{false};
    bool empty_namespace_field{false};
    bool duplicate_parameter{false};
    bool delta_overflow{false};
    bool control_stream_closed{false};
    std::uint64_t padding_streams{0};
    bool padding_nonzero{false};
    bool direct_subscribe{false};  // the runner sent the SUBSCRIBE (d23-direct-subscribe-smoke)
    bool duplicate_request_id{false};
    bool oversize_control_message{false};
};

// One outcome per catalog row. Rows without a SETUP evaluator keep their staged state: unreviewed and
// untested rows are NotRun, NotTestable rows NotTestable, NotApplicable rows NotApplicable. The five SETUP
// rows are judged from the context; before any SETUP arrives they are NotRun.
std::vector<Outcome> evaluate_draft23(const RequirementCatalog& catalog, const Draft23Evidence& context);

// The five SETUP rows, each bound to both draft-23 smoke scenarios.
std::vector<ExecutableBinding> draft23_executable_bindings();

}  // namespace moq::interop::requirements
